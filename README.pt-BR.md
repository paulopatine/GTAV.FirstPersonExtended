# GTAV.FirstPersonExtended: FOV de primeira pessoa até 120° (GTA V Enhanced e Legacy)

[English version](README.md)

Faz o slider **nativo** "Campo de visão em primeira pessoa" (Configurações > Câmera) ir até **120° horizontais**. O mínimo e o mapeamento linear do jogo não mudam, e a posição continua salva pelo próprio jogo.

A mesma proporção do slider vale também para:
- a mira sem olhar pela mira, o ADS e o ADS olhando pela mira (exceto lunetas de sniper, que ficam como no original);
- o celular a pé;
- a câmera de primeira pessoa nos veículos.

Para não haver recorte com a visão mais aberta, o mod reduz o near clip ao mínimo do jogo (0,01 m) na câmera a pé e nos veículos com cabine (carro, avião, helicóptero, dirigível, submarino, trem). Motos, quadriciclos e barcos/jetskis ficam com o near clip nativo.

## Disparada na diagonal
Em primeira pessoa, **W+A+Shift** e **W+D+Shift** passam a manter a disparada (no original viravam corrida). A direção do personagem segue as teclas, e o som de passos e o círculo de ruído no minimapa voltam a funcionar nessa disparada.

Não mudam: A ou D sozinhos com Shift, S com Shift (o jogo original já dispara para a frente com ele), andar de costas, a câmera em terceira pessoa e o resto do movimento.

**Somente modo história.** Não use no GTA Online.

## Requisitos
- GTA V **Enhanced** (testado na 1.0.1158.16, Steam) ou GTA V **Legacy** (testado na 1.0.3889.0, Steam). O mod detecta a edição sozinho; todos os recursos funcionam nas duas.
- Um ASI loader. No **Legacy**: o `dinput8.dll` do Ultimate ASI Loader (o loader usual do Legacy). No **Enhanced**, pode ser um destes:
  - `xinput1_4.dll` do Ultimate ASI Loader (https://github.com/ThirteenAG/Ultimate-ASI-Loader, versão x64);
  - `xinput1_4.dll` que vem com o ScriptHookV (dev-c.com);
  - `dsound.dll` que vem com o OpenRPF.

  Use **apenas um** loader.
- BattlEye desligado pelo fluxo oficial: a opção BattlEye no Rockstar Games Launcher, ou a opção de inicialização `-nobattleye` na Steam. Com ele desligado, o Online fica inacessível, como deve ser com mods.
- O ScriptHookV **não** é necessário. O mod funciona com ou sem ele.

## Instalação
1. Copie o loader escolhido para a pasta do jogo, onde está o `GTA5_Enhanced.exe` (ou, no Legacy, o `GTA5.exe`).
2. Copie o `GTAV.FirstPersonExtended.asi` para a mesma pasta. O `GTAV.FirstPersonExtended.ini` é opcional (veja abaixo).
3. Abra o jogo no modo história e ajuste o slider em Configurações > Câmera.

## Opções (`GTAV.FirstPersonExtended.ini`)
Ao lado do `.asi`. Sem o arquivo, ou com valores inválidos (registrados no log), valem os padrões. As mudanças valem na próxima vez que o jogo abrir.

| Chave | Valores | Padrão | O que faz |
|---|---|---|---|
| `MaxFov` | inteiro de 60 a 140 | 120 | Máximo do slider de FOV de primeira pessoa, em graus **horizontais**. O mínimo é o do jogo. |
| `DiagonalSprint` | 0 ou 1 | 1 | 1: W+A / W+D mantêm a disparada. 0: comportamento original. |
| `UniversalFov` | 0 ou 1 | 1 | 1: mira sem olhar pela mira e ADS (também olhando pela mira da arma) usam exatamente o FOV do slider (no máximo `MaxFov`), em vez de ampliar o próprio. Celular e veículos continuam proporcionais; lunetas de sniper continuam nativas. Com 1, ao olhar pela mira da arma, o corpo e a arma usam o FOV original (como `PlayerFov`, só nessa câmera), para as miras continuarem alinhadas sem a arma encolher; ao recarregar, a arma sai da mira e usa o FOV do mundo até voltar à mira. 0: cada câmera mantém o seu FOV, ampliado na mesma proporção. |
| `PlayerFov` | 0 ou 1 | 0 | Experimental. 1: em primeira pessoa, o corpo do personagem e a arma nas mãos usam o FOV que a mesma câmera tinha no jogo original (braços e arma do tamanho de antes); o mundo continua com o FOV do slider. Não vale dentro de veículos. A sombra do personagem e a da arma não acompanham o tamanho maior (efeito conhecido). Só vale com o FOV do mod ativo. 0: personagem e arma usam o FOV do mundo (exceto ao olhar pela mira com `UniversalFov=1`). |

## Problemas conhecidos
Vêm de desenhar o corpo e a arma do personagem com um FOV diferente do mundo. Afetam o `PlayerFov=1` o tempo todo e o `UniversalFov=1` ao olhar pela mira da arma.
- **Efeitos de cano desalinhados.** O clarão e a fumaça do disparo são desenhados com o FOV do mundo e não ficam alinhados com a arma. Com `PlayerFov` isso aparece muito ao atirar sem mirar (hipfire). Com qualquer uma das opções, aparece ao atirar mirando com armas sem silenciador e com mira modificada.
- **Sombras desalinhadas no personagem e na arma**, principalmente no Enhanced com sombras de ray tracing. Recomenda-se desligar as sombras de ray tracing ao jogar com essas opções.

## Desinstalação
Apague `GTAV.FirstPersonExtended.asi`, `GTAV.FirstPersonExtended.ini` e `GTAV.FirstPersonExtended.log` da pasta do jogo. Se não usar outros mods `.asi`, apague também o loader. O mod não altera nenhum arquivo original do jogo.

## Solução de problemas
O mod grava o `GTAV.FirstPersonExtended.log` na pasta do jogo a cada inicialização. Confira:
- **A primeira linha mostra a versão do jogo.** Se nada aparece, o loader não está carregando o `.asi`. Verifique se há exatamente um loader instalado.
- **Cada recurso registra `scan …: 1 match(es)` e `patched at …`.** Depois de um update da Rockstar, um recurso pode aparecer como `not found; stays original`. Nesse caso só ele fica desligado, o jogo segue normal e os padrões precisam ser regenerados (veja `docs/patterns.md`).
- **`FOV feature disabled`** indica que o ponto principal (FOV a pé) não foi encontrado. Nesse caso o mod não altera nada.
- **Crash ou comportamento estranho:** remova o `.asi` para confirmar se a causa é o mod e envie o `GTAV.FirstPersonExtended.log`.

## Compilação (desenvolvedores)
Cross-compilação no Linux com mingw-w64:
- `make`: build final;
- `make probe`: diagnóstico de câmeras;
- `make sprintprobe`, `make gateprobe`, `make footstepprobe`: diagnósticos da disparada e dos passos (usados para achar os pontos; não são distribuídos);
- `make vmprobe`: diagnóstico do desenho do jogador em primeira pessoa, usado para o `PlayerFov` (não é distribuído);
- `make failtest`: teste de falha segura;
- `make dumper`: dump da imagem do jogo para análise.

Detalhes dos padrões e de como regenerá-los: `docs/patterns.md`.

## Licença e créditos
Código aberto sob a licença MIT (veja `LICENSE`). Autor: **paulopatine**. Repositório: https://github.com/paulopatine/GTAV.FirstPersonExtended

Este é um mod de fã, sem relação com a Rockstar Games nem com a Take-Two. "Grand Theft Auto" é marca dos respectivos donos.
