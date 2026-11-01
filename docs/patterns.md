# Padrões do GTAV.FirstPersonExtended

Validado em **GTA V Enhanced 1.0.1158.16** (esta seção e as seguintes) e em **GTA V Legacy 1.0.3889.0** (seção própria no fim). Cada ponto tem um padrão primário (`*_full`) e um fallback (`*_tail`). Ambos foram verificados, nesta versão, com **exatamente 1 ocorrência** na `.text` da imagem em memória. Verificação offline: `tools/re/scan.py` sobre o dump. Verificação em tempo de execução: as linhas `scan …: 1 match(es)` no `GTAV.FirstPersonExtended.log`.

Regras que valem para todos os pontos:
- **Nenhum endereço ou offset fixo no código.** Os offsets de struct (metadados, campos da câmera) são lidos das próprias instruções encontradas.
- **Validação semântica depois do match.** Um padrão único não basta: cada ponto confere também constantes (10.0, 1.0, 130.0, 0.01, máscara `0x702`), registradores e o campo de destino.
- **Escrita condicionada.** O patch só é aplicado se os bytes atuais forem iguais aos lidos (`MemoryPatch::Replace`).
- **Falha isolada.** Se algo falhar, o log registra o padrão e o motivo, e **só aquele recurso** fica desligado. Exceção: o ponto "a pé" é a base do FOV. Sem ele, o mod inteiro fica inativo e o jogo não é tocado.

## Pontos

| Recurso | Padrões | O que localiza | Âncoras semânticas (validação) |
|---|---|---|---|
| FOV a pé (base) | `onfoot_fov_full` / `_tail` | `movss xmm2,[meta+max]` da fórmula `min + (max−min)·clamp(pref/10)` | antes dele: `cvtsi2ss x,[rip+pref]; divss x,[rip+10.0]` (de onde se obtém o endereço do slider) e `movss xmm1,[meta+min]` com o mesmo registrador base |
| Mira sem olhar pela mira | `hipaim_fov_*` | gravação do lerp da faixa de mira da arma no FOV-alvo da câmera | o slider lido antes é o **mesmo** endereço do ponto a pé; dele sai o offset do FOV-alvo |
| ADS | `ads_fov_*` | leitura do FOV-alvo do ADS no objeto de mira compartilhado | seguido de leitura do FOV atual da câmera, `subss`/`mulss` (interpolação) |
| ADS olhando pela mira | `sight_check_*` | `ucomiss x,[rip+1.0]; jae store` → o destino do salto é o ponto | constante 1.0; o destino é um `movss` de gravação do mesmo registrador no **mesmo** campo de FOV-alvo |
| Celular (estado de FOV fixo) | `state_fov_*` | `metadata->stateFov` gravado como FOV-alvo | carga e gravação no mesmo registrador; destino = campo de FOV-alvo |
| FOV de veículo | `vehicle_fov_*` | carga do FOV base da câmera montada (1ª pessoa no veículo) | seguida do clamp `[1.0, 130.0]`; a gravação 0x29 bytes adiante dá o offset do FOV no frame |
| Near clip de veículo | `vehicle_nearclip_*` | `maxss x,[rip+0.01]; movss [cam+near],x` da câmera montada | constante 0.01; `near` = FOV do frame + 4 |
| Tipo de veículo | `vehicle_type_check_*` | o próprio teste de aeronave da câmera montada | máscara do jogo `0x702` (avião, helicóptero, dirigível, autogiro) e tipo de entidade 3; dele saem os offsets do veículo, do tipo de entidade e do tipo de veículo |
| Near clip a pé | `camera_nearclip_*` | near clip genérico de câmera (`metadata->nearClip`) | constante 0.01; `near` = FOV do frame + 4; `mov rcx,[cam+meta]; add rcx,imm8` antes. Só age quando `rcx` = metadados da câmera a pé |

Ordem de dependência dentro de `FieldOfView::Apply()`:
- **a pé** fornece o slider, `min`/`max` e o ponteiro dos metadados;
- **mira sem olhar pela mira** fornece o offset do FOV-alvo, usado pela mira e pelo celular;
- **veículo** fornece o offset do FOV do frame, usado pelos dois near clips.

## Como regenerar após um update

1. **Dump da imagem em memória.** O EXE em disco tem a `.text` criptografada (entropia ~8,0), então a análise é feita na imagem carregada.
   - Compile com `make dumper`, que gera `build/GTAV.FirstPersonExtended_ImageDump.asi`.
   - Copie o `.asi` para a pasta do jogo junto com o loader e abra o modo história, com o BattlEye desligado pelo fluxo oficial (`-nobattleye` ou a opção do Rockstar Games Launcher).
   - Saem `GTAV.FirstPersonExtended_image.bin` (layout RVA = offset de arquivo) e `GTAV.FirstPersonExtended_dump.log`.
   - Remova o dumper depois.
2. **Preparo.** Disassembly linear da `.text`: `objdump -D -b binary -m i386:x86-64 -M intel --adjust-vma=0x1000 text.bin`. Use `tools/re/relib.py` para limites de função (`.pdata`) e xrefs.
3. **Teste dos padrões atuais:** `python3 tools/re/scan.py "<padrão>"`. Se algum não tiver exatamente 1 ocorrência, reencontre o ponto pelas âncoras de cada recurso:
   - **a pé:** o array `g_prefs` é a base `lea` com mais leituras diretas indexadas. `PREF_FPS_FIELD_OF_VIEW` = hash `joaat` `0xa114b6e8`; o índice dele sai da jump table do switch índice→hash (hoje é 133). Os leitores com `cvtsi2ss` desse endereço levam à fórmula;
   - **mira, ADS, mira óptica, celular:** todos os gravadores do campo de FOV-alvo da câmera a pé (`grep "+0x<alvo>\],xmm"`);
   - **veículo:** leitores dos prefs `PREF_FPS_VEH_AUTO_CENTER` e `PREF_FPS_RELATIVE_VEHICLE_CAMERA_DRIVEBY_AIMING` apontam para o `Update` da câmera montada, que chama a função de FOV (clamp 130.0) e a de near clip (máscara `0x702`);
   - **near clip a pé:** a função genérica com `maxss …0.01` gravando FOV do frame + 4, chamada por todas as câmeras.
4. **Quando a análise estática não basta,** compile `make probe`. Ele conta, por segundo, a execução das instruções do clamp de FOV (130.0) e das gravações de near clip, com o último valor gravado. Rode cenários separados por pausas (Esc) e compare. Foi assim que se separaram ADS real, celular e câmeras de veículo.
5. **Novos padrões:** copie os bytes do novo ponto, troque por `??` deslocamentos, endereços RIP-relativos e saltos, e mantenha opcodes, registradores e estrutura. Valide com `scan.py`, `make`, instalação conferida por hash e o log.

## Disparada na diagonal (W+A / W+D em primeira pessoa)

Implementada em `src/DiagonalSprint.cpp`, independente do FOV. A causa no jogo original são **três decisões** que derrubam a disparada para corrida quando A ou D está pressionado, mais uma quarta que silencia os passos. Os três primeiros patches são aplicados **juntos ou nenhum** (com menos que isso o personagem dispara reto ou deixa de disparar); o quarto é um recurso à parte.

| Patch | Padrões | O que localiza | Por que | Âncoras |
|---|---|---|---|---|
| 1. Rampa | `diagonal_ramp_full` / `_tail` | `or r12b,sil ; xor r12b,1` (7 bytes) | `noLateralInput = !(esquerda \| direita)` zera a flag "para a frente" e a rampa. Vira um stub que também aceita **W** (`MOVE_UP_ONLY`) como "sem entrada lateral" | `lea rcx,[r13+disp]` e a chamada de teste de botão logo depois |
| 1b. Leitor de botão | `control_check_left` / `_right` | a sequência do jogo que testa `MOVE_LEFT_ONLY`/`RIGHT_ONLY` (`getControl`, tabela, `isDown`) | o stub chama as **mesmas funções** do jogo para o W; os três endereços saem da própria sequência | `mov edx,0x22` (ou `0x23`), `lea rbx,[tabela]`, três chamadas; a tabela fica fora da `.text` |
| 2. Entrada | `diagonal_input_full` / `_tail` | `cmp byte [obj+disp],0 ; je forçaFrente` | o jogo troca a entrada por "reto para a frente" se um booleano interno estiver desligado. Vira um stub que mantém a entrada se o booleano está ligado **ou** a entrada aponta para a frente (`xmm9 > 0`) | `mulss xmm6,xmm6 … ucomiss … jne … jp … mov sil,1` logo depois |
| 3. Direção | `diagonal_heading_full` / `_tail` | `test sil,sil ; je pula` antes de gravar a direção desejada | o jogo vira o corpo para a câmera enquanto dispara. Vira um stub que pula essa sobrescrita se a entrada aponta para a frente. Também mantém a flag `diagonalSprinting` (frente + lateral) usada pelo patch 4 | `mov rcx,rdi ; movaps xmm1,xmm10 ; call ; movss [r14+disp],xmm10 ; test byte [rbx+disp],1` |
| 4. Passos | `diagonal_steps_full` / `_tail` | `ucomiss xmm6,[r14] ; jbe` do detector de passos | o jogo só registra o passo se o pé estiver baixo e com velocidade no mundo menor que ~4 m/s. Na diagonal o pé plantado desliza (~5,3 m/s a 45°) e nunca passa. Com `diagonalSprinting` e só para o jogador, o limite sobe para 9 m/s (`kSlideSpeedLimit`) | `or eax,r13d ; mov [rsi+rbp*4+disp],eax ; cmp byte [rsi+disp],0 ; je` |
| 4b. Teste de jogador | `diagonal_steps_player` | `mov rcx,[helper+X] … movzx eax,byte [rcx+Y] ; and al,3 ; cmp al,2` | dá os dois offsets (`X` = ped, `Y` = flags) usados no stub para restringir ao jogador; nenhum offset fixo no código | `cmp edi,1 ; jne ; movzx` |

Para os patches 2 e 3 e para a flag, o jogo usa `xmm8` = entrada lateral e `xmm9` = entrada para a frente (positivo = frente) no ponto de patch.

Regras específicas:
- Se algum ponto dos patches 1 a 3 faltar ou não validar, **nenhum dos três** é aplicado e o log diz `a site was not found or did not validate; stays original`.
- O patch 4 só é tentado depois dos três. Se faltar, o log diz `diagonal footsteps: site not found …; sprint stays silent` e a disparada diagonal funciona, só sem som.
- `S+Shift` já dispara para a frente no jogo original (a entrada é trocada por "frente"); os patches não mudam isso, porque só mantêm a entrada quando ela aponta para a frente.

### Como regenerar após um update

1. Dump da imagem como acima (`make dumper`) e `tools/re/relib.py` para limites de função.
2. **Rampa e entrada:** `make sprintprobe` conta, por combinação de teclas, as cargas das constantes de ritmo de movimento (2.0 e 3.0). O código que roda em W+Shift e para em W+A+Shift está no `.csv` gerado. As funções seguintes (decisão de rampa, `getControl`/`isDown`) saem de lá.
3. **Direção e passos:** `make gateprobe` registra, antes da gravação da direção, `sil`, a entrada lateral e a frontal, a direção da câmera e a gravada. `make footstepprobe` conta as funções da cadeia de passos e registra os **dois lados** das comparações do detector (valor do pé e velocidade do pé) por combinação de teclas.
4. `tools/re/sigmake.py <rva>` gera um padrão com curingas para o início de uma função, estendendo-o até ficar único.
5. Valide cada padrão novo com `tools/re/scan.py`, instale com hash conferido e leia o log.

### Teste de falha segura da disparada

`make failtest` corrompe também os padrões `diagonal_heading_*`. Resultado esperado no log:
- `scan diagonal_heading_full (corrupted on purpose): 0 match(es)` e o mesmo para o `_tail`;
- `diagonal sprint: a site was not found or did not validate; stays original`;
- nenhuma linha `diagonal sprint: ramp patched` nem `diagonal footsteps`.

No jogo: W+A+Shift volta a **correr** (não dispara), como no original; FOV e o resto seguem funcionando.

## Teste de falha segura (FOV)

`make failtest` gera um build com os padrões de ADS (e os de direção da disparada, veja acima) corrompidos de propósito. Resultado esperado no log:
- `scan ads_fov_full (corrupted on purpose): 0 match(es)` e o mesmo para o `_tail`;
- `FOV ADS: not found; stays original`;
- todos os outros pontos aplicados normalmente, sem crash.

No jogo, o ADS volta ao FOV nativo e o resto continua funcionando.

## FOV do jogador (`PlayerFov`, experimental)

Implementado em `src/PlayerFov.cpp`, opcional (`PlayerFov=1`) e dependente do FOV a pé. Em primeira pessoa, o jogo desenha o corpo (comando de draw list 0x84) e a arma nas mãos (comando 0x82) com uma **cópia privada do viewport**: copia o viewport atual, faz `SetCameraMtx(cópia, …)` (câmera relativa), `SetCurrent(cópia, regenerar)`, desenha e restaura. O mod desvia as duas chamadas `SetCameraMtx` para um stub. O stub divide os dois campos de FOV **da cópia** pela ampliação do slider (a mesma escala do resto do mod) e chama o construtor de projeção do próprio jogo. O viewport compartilhado, o mundo, as sombras, os reflexos e a terceira pessoa não passam por aí. Estudo completo: `docs/viewmodel-study.md`.

| Ponto | Padrões | O que localiza | Âncoras semânticas (validação) |
|---|---|---|---|
| Construtor de projeção | `player_projection_full` / `_tail` | `mov rsi,rcx` do construtor das duas matrizes de projeção do viewport | carrega a constante π/360 (0,00872665); dele saem os offsets de aspect, FOV e segundo FOV (`mulss xmm8,[rsi+…]`, única ocorrência); o início da função (via `.pdata`) fica a menos de 0x80 bytes |
| Corpo FPV | `player_body_full` / `_tail` | `call SetCameraMtx` sobre a cópia privada do desenho do ped | alvo é início de função; 13 bytes adiante, `call SetCurrent`; ambos iguais aos do ponto da arma |
| Arma FPV | `player_weapon_full` / `_tail` | `call SetCameraMtx` sobre a cópia privada do executor de objetos | idem |

Regras específicas:
- **Corpo e arma juntos ou nenhum.** Se a arma falhar depois do corpo aplicado, o corpo é restaurado.
- A arma só é ajustada se o corpo FPV foi desenhado há menos de 250 ms. Isso exclui a terceira pessoa, onde o corpo nunca passa por esse caminho (medido com `make vmprobe`).
- Viewports quadrados (aspect 1: cubemaps, reflexos) e FOVs fora de (1°, 130°) ficam intactos.
- **Veículos:** o stub do FOV de veículo conta cada update da câmera de primeira pessoa em veículo (`lock inc`, flags preservadas). Enquanto essa contagem muda (últimos 250 ms), o jogador fica com o FOV do mundo. Sem esse ponto, o `PlayerFov` não é aplicado.
- **Só olhando pela mira:** com `UniversalFov=1` e `PlayerFov=0`, o stub `sight` conta seus updates do mesmo jeito, e o FOV do jogador só vale enquanto essa contagem muda. Sem o ponto `sight`, esse modo não é aplicado.
- O log mostra cada transição: `player FOV: on (looking through the sight)`, `player FOV: off`.
- **FOV alvo:** olhando pela mira, o FOV da própria mira, como o jogo o definiu. O stub `sight` grava o registro antes de ajustá-lo, então o resultado é igual ao vanilla. Nas outras câmeras, o FOV da câmera dividido pela ampliação do slider.
- **Transição suave:** suaviza-se a diferença entre o FOV do mundo e o do jogador, com uma mola criticamente amortecida (`kSmoothSeconds` = 0,08 s, assenta em ~0,25 s, sem overshoot), compartilhada por todas as threads de render. Desligado, a diferença é 0 e o jogador segue o mundo exatamente. O sinal da mira fica ativo por 100 ms depois do último update (`kCameraRecentMs`); medido: sem falhas enquanto se olha pela mira.

### Como regenerar após um update
1. Dump (`make dumper`) e `tools/re/relib.py`.
2. **Construtor:** o único leitor de π/360 que monta duas projeções. Chamadores de `tanf` com `mulss …,[rip+π/360]`; ele começa com `cmp byte [rcx+flagZ],0` e lê near, far, aspect e FOV.
3. **`SetCurrent`:** grava o viewport em `TLS+…` (o mesmo campo que `SetWorldMtx` compara) e, se pedido, regenera. **`SetCameraMtx`:** termina chamando a composição view·proj e a regeneração.
4. **Pontos do corpo e da arma:** entre os chamadores de `SetCameraMtx`, os dois seguidos de `mov rcx,<cópia>; mov dl,1; xor r8d,r8d; call SetCurrent` em funções de desenho. O do ped testa um bit dos dados de desenho (`0x80000`) e o de objetos outro (`0x40`). Para confirmar no jogo, use `make vmprobe` (instrumenta a tabela de executores de draw list e o teste do bit `0x40`).
5. Valide com `tools/re/scan.py` (exatamente 1 ocorrência cada), instale com hash conferido e leia o log (`player FOV: body patched…`, `first body/weapon draw narrowed…`).

### Teste de falha segura
`make failtest` também corrompe `player_weapon_*`. Esperado no log (com `PlayerFov=1`): `scan player_weapon_full (corrupted on purpose): 0 match(es)`, `player FOV: a private viewport site was not found or did not validate; stays original` e nenhuma linha `player FOV: body patched`.

## GTA V Legacy (1.0.3889.0)

O mod detecta a edição pelo nome do executável (`GTA5.exe` = Legacy, `GTA5_Enhanced.exe` = Enhanced) e escreve no log, na primeira linha, `(Legacy)` ou `(Enhanced)`. O código do jogo é o mesmo nas duas edições, mas o Legacy foi compilado de outro jeito (MSVC, outros registradores e outra ordem de blocos). Por isso cada ponto tem padrões próprios com o sufixo `_legacy`, e os stubs mudam só os registradores e as instruções vizinhas. O comportamento final é igual. Todos os padrões abaixo foram validados no dump do Legacy com **exatamente 1 ocorrência** (o scanner do próprio mod, `tests/ScanDump.cpp`, roda sobre qualquer dump: ele lê a `.text` da tabela de seções). Também foram confirmados no log do jogo.

Loader no Legacy: o `dinput8.dll` do Ultimate ASI Loader (o mesmo arquivo do `xinput1_4.dll`, renomeado).

### FOV e FOV do jogador

| Ponto | Padrões | Diferença em relação ao Enhanced |
|---|---|---|
| a pé (base) | `onfoot_fov_legacy_*` | a leitura do slider fica numa função pequena (`movd`, `cvtdq2ps`, `mulss 0.1`). O ponto é a carga de `max` na fórmula `min + (max − min)·t` do chamador |
| mira, ADS, mira óptica, celular | `hipaim_fov_legacy_*`, `ads_fov_legacy_*`, `sight_store_legacy_*`, `state_fov_legacy_*` | o celular e o estado de FOV fixo usam um stub que salva e restaura `xmm0` na pilha |
| veículo, tipo, near clips | `vehicle_fov_legacy_*`, `vehicle_type_check_legacy_*`, `vehicle_nearclip_legacy_*`, `camera_nearclip_legacy_*` | o FOV do veículo é recarregado numa transição logo depois do clamp. O mesmo ajuste é aplicado também ali (`vehicle transition` no log) |
| FOV do jogador | `player_perspective_legacy_*`, `player_body_legacy_*`, `player_weapon_legacy_*` | a projeção não tem as duas matrizes do Enhanced: o viewport tem FOV, aspect, near, far e flag de perspectiva, e é montado por `Perspective(vp, fovY, aspect, near, far)`. O stub chama essa função na cópia privada |

### Disparada na diagonal

As mesmas quatro decisões do Enhanced, na mesma ordem. Funções equivalentes, conferidas lado a lado:
- despachante do movimento: Enhanced `0x18bf6e0`, Legacy `0xb09db8`;
- veto lateral: Enhanced `0xd87300`, Legacy `0x72dd80`;
- movimento com disparada: Enhanced `0x18c1b90`, Legacy `0xafa65c`.

| Patch | Padrões | O que localiza no Legacy |
|---|---|---|
| 1. Rampa | `diagonal_ramp_legacy_*` | o bloco de 16 bytes que grava `noLateralInput = !(esquerda \|\| direita)` em `r15b` (`test bl,bl ; jne ; test al,al ; jne ; mov r15b,1 ; jmp ; xor r15b,r15b`). O stub reescreve a **própria flag** (`r15b \|= isDown(MOVE_UP_ONLY)`), porque o jogo a lê **três vezes** depois: na rampa, na escolha entre corrida e disparada e na manutenção da disparada |
| 1b. Leitor de botão | `control_check_legacy_left` / `_right` | o segundo dos três testes de tecla (`getControl`, `isDown`, limiar 0,5). Os três endereços saem da sequência, e o limiar é conferido (= 0,5) |
| 2. Entrada | `diagonal_input_legacy_*` | `cmp byte [rax+disp],r12b ; je forçaFrente`. Antes do patch, o mod confere que o destino do salto é `movaps xmm15,xmm7 ; movaps xmm14,xmm12` (lateral 0, frente 1). Isso garante que `xmm14` é a entrada para a frente |
| 3. Direção | `diagonal_heading_legacy_*` | `test dil,dil ; je pula` (salto longo). A flag `diagonalSprinting` usa `xmm15` (lateral) |
| 4. Passos | `diagonal_steps_legacy_*`, `diagonal_steps_legacy_player` | `comiss xmm6,[r12] ; jbe` (11 bytes). Teste de jogador: `mov rdx,[rbx+ped] … mov al,[rdx+flags] ; test al,1 ; jne ; test al,2 ; je`. O stub usa `rax` (aqui `rcx` está em uso) |

**Lição registrada:** o bit 6 de `[ped+0x5e0]`, que o despachante testa para escolher o movimento sem disparada, **não** precisa de patch. Ele só reflete o veto: com a flag `noLateralInput` corrigida nos três usos, o jogo não liga mais esse bit nas diagonais. As primeiras tentativas no Legacy alteravam só o primeiro uso da flag, ou ignoravam o bit no despachante. Ignorar o bit fazia o S andar para a frente.

### Teste de falha segura no Legacy

`make failtest` corrompe também `diagonal_heading_legacy_*`. Resultado medido em 01/11/2026: `scan diagonal_heading_legacy_full (corrupted on purpose): 0 match(es)` (idem `_tail`), `diagonal sprint: a site was not found or did not validate; stays original`, FOV funcionando e nenhum crash.

## Recarga olhando pela mira (FOV do jogador)

Problema: recarregar segurando o botão de mirar mantém a câmera no estado "olhando pela mira". O sinal da mira continuava ativo, e o corpo e a arma ficavam com o FOV estreito da mira: a animação de recarga aparecia com zoom.

Correção, em duas partes:
1. **Sinal de recarga (stub `sight`, thread do jogo).** O stub segue os ponteiros do próprio jogo: câmera → personagem → gerente de armas → objeto da arma → arma. Enquanto o estado da arma for "recarregando", ele conta um sinal próprio (`SightReloading`) no lugar do sinal da mira. Também grava a câmera e o objeto da arma para a parte 2.
2. **Volta à mira (`PlayerFov`, thread de render).** Cada recarga abre um episódio, durante o qual o FOV da mira fica desligado. O fim do estado "recarregando" não serve como critério sozinho. Medido com `make sprintprobe` (gravação da posição da arma em relação à câmera a cada 33 ms), o estado termina de 0,5 s a 1,1 s antes de a arma voltar à mira, conforme a arma. Uma recarga interrompida por rolagem pode ainda deixar o estado ligado com a arma já na mira. O episódio termina:
   - quando a arma volta a menos de 1,5 cm do lugar que ocupava na mira (aprendido enquanto se olha pela mira, só com a arma parada: menos de 1 cm em 0,15 s), depois que o estado terminou;
   - ou quando a arma assenta (parada) depois do fim do estado, mesmo sem lugar aprendido;
   - ou se a arma não sai da mira em 0,4 s, ou fica nela 0,4 s com o estado ligado (estado preso);
   - no máximo 3 s depois do fim do estado. Sem a posição da arma, vale um atraso fixo de 0,4 s.

   O log registra uma linha por recarga com o motivo (`weapon back at the sight`, `weapon settled`, `weapon never left the sight`, `stale reload state…`, `time limit…`).

| Ponto | Padrões (Enhanced / Legacy) | O que localiza | Validação |
|---|---|---|---|
| Personagem da câmera | `aim_ped_*` / `aim_ped_legacy_*` | prólogo da câmera de mira: `and byte [rcx+flags],0xDF ; mov ped,[rcx+X]` | `mov r64,[rcx+disp32]`; o fallback é único dentro da função da câmera de mira (a mesma do ponto `sight`, via `RtlLookupFunctionEntry`) |
| Gerente de armas e getter | `aim_weapon_*` / `aim_weapon_legacy_*` | `mov r,[ped+Y] ; test ; je ; mov rcx,r ; call getter` | a base é o registrador do personagem; o getter é conferido pelo formato do seu caminho rápido (`mov rax,[rcx+o] ; … mov r,[rax+w]`), de onde saem os offsets do objeto e da arma |
| Estado da arma | `weapon_reload_state_*` / `weapon_reload_state_legacy_*` | `cmp byte [rcx+S],RELOADING` do atualizador do estado de recarga (Legacy também tem a gravação `mov byte [rcx+S],3`) | o valor "recarregando" é o imediato da comparação; nos padrões completos, a gravação no mesmo campo é conferida |
| Posição e direção da câmera | `camera_frame_*` / `camera_frame_legacy_*` | fim do update da câmera a pé: `lea …,[cam+pos]` e `[cam+forward]` passados a um callback | mesma função que a do ponto abaixo; o campo de posição tem que ser o mesmo nos dois |
| Posição de entidade | `entity_position_*` / `entity_position_legacy_*` | `movaps …,[cam+pos] ; … subps …,[entity+P]` (posição da câmera em relação à entidade presa) | offsets alinhados a 16 |

Valores atuais, iguais nas duas edições exceto o estado: personagem `+0x210`, armas `+0x10b8`, objeto `+0x78`, arma `+0x320`, estado `+0x200` (Enhanced) / `+0x1f0` (Legacy) `== 2`, câmera posição `+0x60` e frente `+0x40`, entidade `+0x90`. Como o mod lê esses ponteiros do jogo a partir da thread de render, usa `ReadProcessMemory`: um ponteiro velho só faz a leitura falhar, sem travar o jogo.

**Falha segura:** `make failtest` corrompe `weapon_reload_state_*` nas duas edições. Medido no Enhanced: `0 match(es)`, `FOV sight: reload check not found; aiming while reloading keeps the sight FOV`, resto aplicado, sem crash. Sem os padrões de posição, a volta usa o atraso fixo e o log diz `weapon pose not found`.
