# 02 — Estudo: jogador com FOV próprio ("viewmodel") no GTA V Enhanced

Build analisada: **1.0.1158.16** (dump em memória de 2026-10-31 e 6 rodadas de sonda no jogo). Os RVAs abaixo são **notas de análise**. O código do mod não usa nenhum deles: tudo será localizado por padrão de bytes.
Legenda: **[V]** verificado (dump ou sonda no jogo) · **[S]** suposição · **[W]** fonte web.

## Conclusão (TL;DR)
**É viável, e a arma entra no mesmo mecanismo do corpo, porque o próprio jogo já faz isso.** Em primeira pessoa, o jogo desenha o corpo do jogador **e** a arma (com todas as peças) com um **viewport privado**: uma cópia do viewport da câmera, relativa à câmera, montada só para esses desenhos. Basta trocar a projeção dessa cópia pelo FOV original. O mundo continua com o FOV alto, e o objeto do viewport compartilhado não é tocado.

## 1. Como o jogo desenha

- [V] **Projeção:** `grcViewport::Perspective(fov, aspect, near, far)` (`0x1c22b20`) grava os parâmetros no viewport (+0x400…). `0x1c20cd0` monta as duas matrizes de projeção (+0x80 e +0x180): `xx = 1/(tan·aspect)` e `yy = 1/tan`. A profundidade depende só de near e far.
- [V] **Envio à GPU:** uma única função (`0x1c21010`) lê o viewport atual (`TLS+0x15c48`) e envia World·View·Proj. `SetCurrent(vp, regenerar)` = `0x294560`; `SetCameraMtx(vp, …)` = `0x1c21e10`.
- [V] **Mundo:** na câmera principal, os objetos são instanciados, com a projeção vinda das constantes da view.
- [V] **Draw lists:** os comandos são despachados por uma tabela de 256 executores, por id, num objeto do heap cujo ponteiro fica num global (o construtor grava a vtable e esse global). Registro: `0x3bc7b0`, que chama `0x13faae0`.
- [V] **Corpo FPV:** comando **0x84** (executor `0x3c2760`). O bit 7 de `cmd+0xcd` vai para `TLS+0x712` durante o desenho e faz o desenho por componente (`0x10eb1c0`) pular cabeça, máscara, cabelo e dentes. O desenho do ped (`0x3d0590`), quando `drawData+0x114` tem o bit `0x80000`, **copia o viewport atual, monta uma câmera com translação zero (`SetCameraMtx`), põe a cópia como atual, desenha e restaura**.
- [V] **Arma FPV:** comando **0x82** (executor `0x3ca330`). Matrizes do objeto no comando em +0x38/+0x40 e +0x78/+0x80, a 20–35 cm à frente e 20–65 cm abaixo do olho. Pistola: 1 objeto por quadro. Carbine: várias peças. Desarmado: nenhum. Com o bit `0x40` em `drawData+0x108`, o executor faz **o mesmo**: cópia do viewport, `SetCameraMtx`, `SetCurrent`, desenho e restauração.
  - Sonda 9: na câmera principal, esse teste é atingido **só** pelos desenhos da arma (~40/s com a pistola, ~180/s com a carbine), e **sempre** com o caminho privado (`flag40=1, sil=0`).
- [V] **Sombras:** o ped é desenhado sem a flag FPV, com a projeção da luz, e não passa por esses viewports privados. Reflexos e cubemaps usam outros viewports (aspect 1, 90°).
- [W] Nenhum mod público tem viewmodel FOV no GTA V. Os mods de FOV (Rose/Flawless Widescreen, Enhanced Camera, DrDaxxy) só mexem na câmera.

## 2. Ponto de intervenção proposto

As duas chamadas `call SetCameraMtx` sobre a cópia privada têm a mesma forma: `call SetCameraMtx; mov rcx,<cópia>; mov dl,1; xor r8d,r8d; call SetCurrent`.

| Ponto | Padrão (validado: 1 ocorrência) | Validação semântica |
|---|---|---|
| Corpo (`0x3d0ed6`) | `E8 ?? ?? ?? ?? 48 89 E9 B2 01 45 31 C0 E8 ?? ?? ?? ?? 49 89 C7` | os dois `call` apontam para os mesmos `SetCameraMtx`/`SetCurrent` do ponto da arma |
| Arma (`0x3cafdc`) | `E8 ?? ?? ?? ?? 48 89 D9 B2 01 45 31 C0 E8 ?? ?? ?? ?? 48 89 84 24` | idem |

Cada `call` (5 bytes, só o rel32 muda) passa para um stub. O stub, quando as condições valem, reescreve a projeção **da cópia** (FOV vertical do jogador = FOV da cópia ÷ escala do mod, via `Perspective` do próprio jogo ou `xx/yy` diretamente) e segue para `SetCameraMtx`. Condições: primeira pessoa ativa, viewport da câmera principal (aspect da tela, FOV igual ao aplicado pelo mod), escala do mod > 1. Fallbacks: padrões com mais contexto antes da chamada (a fazer na implementação).

Qualquer padrão faltando desliga só o recurso, e o resto do mod segue (regra do projeto).

## 3. O que ainda não foi medido (vai para os testes da implementação)
- **Veículos:** não sei se a cabine também passa pelo caminho privado. Se não passar, as mãos sairiam do volante, e a proposta é desligar dentro de veículos (o mod já sabe quando a câmera de veículo está ativa).
- **Celular e objetos na mão:** a hipótese [S] é que usem o mesmo comando 0x82 com o bit 0x40, como a arma.
- **Iluminação:** a posição reconstruída da profundidade fica deslocada lateralmente ×k (k = 1,87 a 120°). Isso desloca um pouco sombras e luzes locais sobre as mãos e a arma; o sol fica praticamente certo. É inerente a qualquer viewmodel num renderizador deferred. Só dá para avaliar vendo.
- **Pernas e pés ao olhar para baixo:** ampliados em torno do centro da tela, eles não coincidem com o chão. Proposta: reduzir k → 1 conforme o pitch passa de ~45° para baixo.
- **Cobertura, escada, escalada:** as mãos tocam o mundo. Proposta: k → 1 com rampa nesses estados.
- **TAA/DLSS:** mudanças bruscas de k podem gerar um quadro de rastro. Por isso as rampas.

## 4. Alternativas descartadas
- **Passe separado** (limpar a profundidade): exige G-buffer e iluminação extras no renderizador DX12. Custo e risco desproporcionais.
- **Escalar o modelo** em torno de um ponto da linha de mira: o mesmo defeito da tentativa anterior (mãos crescem mais que a ponta da arma).
- **Escala (k, k, 1) no espaço da view:** equivale à projeção, mas distorce as normais (~34° de erro a 45°).
- **Tirar a arma do lote instanciado:** desnecessário, porque o jogo já desenha a arma FPV pelo caminho privado.

## 5. Resultado da implementação (testado pelo usuário)
- Miras alinhadas e mãos na arma: **perfeito**. Terceira pessoa intacta. Celular funciona.
- **Veículos:** as mãos saíam do volante e do guidão, então o FOV do jogador fica desligado em veículos (sinal da câmera de veículo do mod).
- **Sombras:** desalinhadas (no chão e na arma), como previsto. Decisão do usuário: deixar como está e documentar.
- **Pés ao olhar para baixo:** difícil de avaliar com o FOV vertical atual. Considerado ok.
- **Escopo (decisão do usuário):** `PlayerFov=1` vale para todas as câmeras de primeira pessoa. Com `UniversalFov=1`, mesmo com `PlayerFov=0`, vale **ao olhar pela mira**, para as miras continuarem alinhadas sem a arma encolher.
- **Transição:** de 0,25 s, suave. Aprovada.
- **Olhando pela mira:** o jogador usa o FOV da própria mira (o vanilla, guardado pelo stub `sight` antes do ajuste do mod). A arma fica igual ao vanilla, com o mundo largo. Aprovado pelo usuário com `UniversalFov=1` (`PlayerFov=0`) e com `PlayerFov=1`. A transição entre alvos é suavizada (mola criticamente amortecida sobre a diferença entre o FOV do mundo e o do jogador).

## 6. Recomendação original
Implementar a **projeção do jogador nos dois viewports privados** (corpo e arma), atrás de uma opção nova no `.ini`, desligada por padrão e com falha segura. Começar sem as rampas (pitch, cobertura, veículo desligado por regra simples) e avaliar no jogo: alinhamento das miras, mãos na arma, iluminação e pés.

## Sondas usadas (nunca distribuídas)
`make vmprobe` (`src/ViewmodelProbe.cpp`, versão final): instrumenta a tabela de executores de draw list (troca das entradas por stubs que contam e procuram matrizes perto da câmera) e o teste do bit 0x40 no executor de objetos. Rodadas anteriores: detour na função de envio (por viewport e flag FPV). Logs: `D:\FirstPersonExtended\backup\2026-10-31_viewmodel\vmprobe{1,2,5,7,8,9}.log`.
