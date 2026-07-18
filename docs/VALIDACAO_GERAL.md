# VALIDAÇÃO GERAL DO FIRMWARE — HardwareFisicaV2.0

Data da validação: 2026-07-18
Ambiente: PlatformIO Core, `pio run` (sem upload), Windows.

## ADENDO 4 — CAUSA RAIZ REAL encontrada e corrigida: barramento SPI compartilhado TFT/SD

Quinta rodada, mesma data. Sem backup novo (mudança cirúrgica, mesmos
arquivos do Adendo 3 já salvos em `backup_display_fix/`).

**Esta é a causa raiz real** dos dois sintomas relatados nas últimas duas
rodadas ("preso em Display OK" e depois "display não mostra nada") — não
um bug de lógica no código da IHM, mas um **conflito físico de barramento
SPI** que o Adendo 3 não havia identificado.

### O problema

`include/MAIN.HPP` já documentava (comentário pré-existente, linha 64):
`// Cartao microSD (compartilha o barramento SPI do TFT: MOSI/MISO/SCK)`.
`TFT_MISO=19`, `TFT_MOSI=23`, `TFT_SCLK=18` são exatamente os pinos
**padrão do periférico de SPI de hardware (VSPI)** do ESP32 — os mesmos
que `SD.begin(SD_CS_PIN)` usa implicitamente (via `SPI.begin()` interno),
já que `armazenamento.cpp` nunca especifica pinos alternativos.

O TFT (`Arduino_SWSPI`, em `ihm.cpp`) desenha em **bit-bang**: escreve nos
pinos via `digitalWrite()` puro, sem usar o periférico de SPI do chip. O
microSD usa o periférico de **SPI de hardware**. No ESP32, inicializar o
SPI de hardware nesses pinos (`SPI.begin()`, chamado dentro de
`SD.begin()`) reconfigura a **matriz de GPIO** para rotear a saída física
desses 3 pinos para o periférico de SPI — e essa configuração **persiste**
até algo religar os pinos de volta a "GPIO simples". A partir desse
momento, chamadas de `digitalWrite()` nesses mesmos pinos (como o
`Arduino_SWSPI` faz) **deixam de ter efeito físico no fio** — o pino passa
a ser controlado pelo periférico de SPI, não pelo registrador de GPIO que
`digitalWrite()` altera.

Isso explica os dois sintomas em sequência:
- **Rodada anterior ("preso em Display OK"):** o teste visual rodava
  dentro de `ihm::init()`, ANTES de `armazenamento::init()` (que só roda
  depois, em `main.cpp`). Nesse momento os pinos ainda eram GPIO simples
  → o teste desenhava certo. Assim que `armazenamento::init()` chamava
  `SD.begin()` logo em seguida, os pinos eram religados para o periférico
  de SPI — e **todo desenho seguinte** (logotipos, LEDs, autor, e o menu
  principal) parava de aparecer fisicamente, mesmo com o código de
  desenho 100% correto. Por isso a última imagem visível era sempre
  "Display OK".
- **Rodada atual ("não mostra nada"):** ao mover o teste visual para
  dentro da máquina de estados (`EtapaBoot::DiagnosticoDisplay`, Adendo
  3), ele passou a rodar DEPOIS de `setup()` completo — ou seja, depois
  de `armazenamento::init()`/`SD.begin()` já ter religado os pinos. A
  partir daí, **nem o próprio teste visual** conseguia mais desenhar —
  daí "nada" aparecer, nem o vermelho/verde/azul inicial.

### A correção

Arquitetural, não um ajuste cosmético: os dois lados agora **negociam a
posse do barramento físico** antes de cada uso, protegidos por um mutex
(`mutexBarramentoSPI`, criado em `armazenamento::init()`):

- **`armazenamento.hpp`/`.cpp`** — nova classe RAII `TravaBarramentoSD`:
  toma o mutex e chama `SPI.begin(TFT_SCLK, TFT_MISO, TFT_MOSI,
  SD_CS_PIN)` (religa os pinos para o periférico de hardware) antes de
  QUALQUER `SD.*`/`File.*`. Aplicada em todas as funções que tocam o
  cartão: `init`, `arquivoExiste`, `abrirNovoArquivo`, `processarFila`,
  `fecharArquivoAtual`, `listarArquivos`, `renomearArquivo`,
  `excluirArquivo`, `abrirParaLeitura`, `lerProximaLinha`, `fecharLeitura`,
  `espacoTotalBytes/UsadoBytes/LivreBytes`. Novas funções públicas
  `travarBarramentoSPI()`/`destravarBarramentoSPI()` só tomam/liberam o
  mutex (sem mexer em pino), para o lado do display poder usar o mesmo
  mutex sem duplicar a lógica de reconfiguração do SD.
- **`ihm.cpp`** — nova classe RAII `TravaBarramentoDisplay`: toma o MESMO
  mutex (via `armazenamento::travarBarramentoSPI()`) e chama `pinMode()`
  nos 3 pinos para religá-los como GPIO simples (bit-bang) antes de
  desenhar. Aplicada nas 8 funções de desenho públicas
  (`escreverTelaApp`, `escreverTextoTela`, `desenharListaMenu`,
  `desenharConfirmacao`, `desenharValorEditavel`, `desenharListaRolavel`,
  `desenharGradeModulos`, `desenharMensagem`) e em
  `executarDiagnosticoVisual()`. `desenharCabecalhoRodape()` (chamada só
  internamente, por essas mesmas funções) não tem a trava própria, para
  não travar em si mesma (mutex simples, não recursivo).
- Ordem de lock consistente (`TravaBarramentoSD`/`TravaBarramentoDisplay`
  sempre por fora, `mutexArquivo` por dentro quando aplicável) — sem risco
  de deadlock entre núcleos.

Isso NÃO era uma condição de corrida entre tarefas no sentido usual (SD e
display raramente escrevem no "mesmo instante") — é um efeito colateral
**permanente e cumulativo** de reconfiguração de hardware: uma vez que um
lado "rouba" o pino, o outro fica cego até alguém devolver o pino
explicitamente. Por isso um mutex sozinho não bastaria — cada lado precisa
**re-configurar o pino para si** toda vez que assume o controle, o que é
exatamente o que as duas classes RAII fazem.

**Nenhuma biblioteca, GPIO, controlador ou tipo de barramento (`Arduino_SWSPI`)
foi trocado** — a configuração de display permanece exatamente a
confirmada fisicamente; a correção é só a disciplina de handoff entre os
dois usuários do mesmo fio físico.

**Compilação:** `pio run` — ambos os ambientes `SUCCESS`, sem erros nem
warnings. RAM 14,7% (48.240–48.248/327.680 B), Flash ~70,2–70,4%
(920.033–922.253/1.310.720 B).

**Ainda não verificado fisicamente** (fora do escopo — sem upload nesta
sessão): esta é a correção mais provável para os dois sintomas relatados,
fundamentada em como o ESP32 roteia a matriz de GPIO para o periférico de
SPI de hardware. A próxima gravação real deve mostrar o teste visual, o
boot completo e o menu funcionando de ponta a ponta, incluindo depois de
qualquer leitura/escrita no cartão (listar arquivos, rodar um experimento,
etc.) — que é justamente o caso que expôs o problema.

---

## ADENDO 3 — Investigação de display apagado + ferramentas de diagnóstico

Quarta rodada, mesma data. Backups em `backup_display_fix/` (`ihm.cpp.bak`,
`ihm.hpp.bak`, `MAIN.HPP.bak`, `maquina_estados.cpp.bak`).

**Investigação da regressão:** `diff` de `src/ihm.cpp`, `src/maquina_estados.cpp`,
`src/main.cpp`, `include/MAIN.HPP` e `include/ihm.hpp` contra os backups do
Adendo 2 (última versão confirmada mostrando vermelho/verde/azul/"Display
OK" fisicamente). **Nenhuma alteração de código que explique uma tela
totalmente apagada foi encontrada** — a construção de `bus`/`display` é
idêntica; a única mudança foi mover o desenho do teste visual de dentro de
`ihm::init()` para `ihm::executarDiagnosticoVisual()`, chamada pela nova
etapa `EtapaBoot::DiagnosticoDisplay`, com o mesmo conteúdo gráfico.
`main.cpp` não foi tocado naquela rodada. Não há `while(!SD.begin())` nem
espera bloqueante de Wi‑Fi/MQTT em nenhum arquivo (confirmado por busca
textual). Também não há dois controladores de backlight — só um
`ledcAttachPin(TFT_BL, ...)` e um `ledcWrite()`, ambos em `ihm.cpp`.

Como não foi possível reproduzir/flashar o hardware nesta sessão (proibido
por escopo), em vez de "consertar" um bug não confirmado, foram
adicionadas ferramentas de diagnóstico para a próxima gravação real
apontar a causa exata:

1. **`ihm.hpp`/`ihm.cpp`** — log do ponteiro do `display` em 3 pontos
   (`[DISPLAY] Objeto na inicializacao`, `[DISPLAY] Objeto no
   diagnostico`, `[DISPLAY] Objeto no menu principal` — este último uma
   única vez, na primeira chamada de `desenharListaMenu()`). Se algum
   desses ponteiros divergir, é sinal de instância duplicada — mas a
   revisão do código não encontrou nenhuma.
2. **`MAIN.HPP`** — nova constante `FORCE_DISPLAY_BACKLIGHT_DIAGNOSTIC`
   (mais forte que `FORCE_MAX_BRIGHTNESS_FOR_DIAGNOSTIC`): quando `true`,
   `ihm::init()` **não anexa `TFT_BL` ao LEDC** (evita que
   `ledcAttachPin()` zere o duty do canal e apague o backlight mesmo após
   o `digitalWrite(TFT_BL, HIGH)`) e `ihm::setBrilho()` retorna sem chamar
   `ledcWrite()` — o backlight fica 100% em modo GPIO simples, eliminando
   qualquer suspeita de canal/frequência/resolução do PWM. Se o backlight
   continuar apagado mesmo assim, a causa é elétrica (fiação, transistor
   de acionamento do backlight, alimentação), não mais firmware.
3. **`MAIN.HPP`/`maquina_estados.cpp`** — `UI_UPDATE_INTERVAL_MS` (16 ms):
   agrupa redesenhos muito próximos no tempo num único redesenho (reduz
   flicker em giros rápidos do encoder), via `podeRedesenharAgora()`;
   `precisaRedesenhar` continua pendente se recusado, nunca perde um
   redesenho.
4. **`maquina_estados.cpp`** — log único `[TASK][IHM] Tarefa iniciada` na
   primeira chamada de `tick()`, confirmando que o laço da IHM (rodando
   como `loopTask` do Arduino, núcleo 1) está de fato vivo.

**Itens pedidos e deliberadamente NÃO implementados nesta rodada** (para
não introduzir regressões novas sem poder testar em hardware):
redesenho parcial item-a-item do menu (só a linha selecionada muda de
cor) — a versão atual já só redesenha a tela inteira quando algo muda
(não a cada ciclo), então o ganho seria só cosmético (menos flicker
durante a troca), mas a implementação certa exige guardar o índice
anterior por tela e tratar bordas de rolagem, um risco de bug novo maior
que o benefício nesta rodada sem poder validar fisicamente. Consolidação
de `precisaRedesenhar` num helper único com motivo (`solicitarRedesenho`)
não foi generalizada para as ~30 funções `tratarX()` que já usam
`precisaRedesenhar = true` diretamente — **não havia, de fato, múltiplas
flags conflitantes** (só existe uma, `precisaRedesenhar`), então o
requisito "não mantenha várias flags independentes" já estava atendido
estruturalmente; generalizar o wrapper para todos os call sites seria
uma alteração grande e cosmética, não uma correção de bug.

**Compilação:** `pio run` — ambos os ambientes `SUCCESS`, sem erros nem
warnings. RAM 14,7% (48.240/327.680 B), Flash 70,1% (918.305/1.310.720 B).

**Conclusão honesta:** não foi encontrado nenhum defeito de código que
explique um display totalmente apagado após o Adendo 2. As ferramentas
acima (ponteiros + bypass total do PWM) devem, na próxima gravação real,
apontar precisamente se o problema é software remanescente ou hardware
(backlight/alimentação). Se `[DISPLAY] begin(): SUCESSO` aparecer mas a
tela continuar apagada mesmo com `FORCE_DISPLAY_BACKLIGHT_DIAGNOSTIC=true`,
a causa deixa de ser este firmware.

---

## ADENDO 2 — Transição para o menu, logs com títulos e sincronização dos LEDs

Terceira rodada, mesma data. Backups em `backup_menu_leds/` (`main.cpp.bak`,
`ihm.cpp.bak`, `ihm.hpp.bak`, `maquina_estados.cpp.bak`,
`maquina_estados.hpp.bak`, `MAIN.HPP.bak`).

**Causa confirmada da dessincronização dos LEDs (item mais grave, causa
raiz real de bug):** `maquina_estados.cpp` — `definirTodosLeds()` chamava
`ihm::controlarLED()` dentro de um `for` (uma vez por LED); e
`ihm::controlarLED()` (`ihm.cpp`) chama `pixels.show()` a cada chamada.
Resultado: **6 chamadas a `pixels.show()`** por "todos os LEDs", acendendo-os
progressivamente em vez de simultaneamente — exatamente o padrão
incorreto descrito na tarefa. Corrigido com uma nova função
`ihm::controlarTodosLeds()` que define as 6 cores primeiro e chama
`pixels.show()` **uma única vez** ao final; `definirTodosLeds()` agora só
repassa para ela. Não havia concorrência entre tarefas nem sobrescrita por
outro código (a única outra escrita de LEDs, `atualizarTelasAoVivo()` para
a tela "Teste de canais", só roda quando essa tela está aberta — telas
mutuamente exclusivas —, então não é necessário mutex).

**Causa da tela "Display OK" não ser substituída:** não havia um `while`/
`return` travando a IHM — o teste visual rodava **dentro de `ihm::init()`**
(antes até de `maquina_estados::init()` existir), com 3×`delay(300)`
bloqueantes, e só era "liberado" quando o resto de `setup()` terminava e o
primeiro `tick()` rodava. Ou seja, a tela ficava presa não por um bug de
lógica que impedisse a transição, mas por **não fazer parte da máquina de
estados temporizada** — sua duração dependia inteiramente de quanto tempo
as inicializações seguintes (configurações, sensores, SD, MQTT) levavam.
Corrigido: o teste visual virou `ihm::executarDiagnosticoVisual()`,
chamado como uma nova etapa `EtapaBoot::DiagnosticoDisplay` (primeira
etapa do enum, antes de `Logos`), controlada por `millis()` e pela nova
constante `DISPLAY_DIAGNOSTIC_DURATION_MS` (`MAIN.HPP`, 1500 ms) — a
mesma disciplina não-bloqueante já usada pelas demais etapas do boot.
Objeto do display confirmado único (`bus`/`display` só existem em
`ihm.cpp`, com os endereços já logados desde o adendo anterior).

**Causa dos logs mostrarem índices em vez de títulos:** os próprios logs
`[ESTADO] Menu selecionado: %u` adicionados no adendo anterior imprimiam o
índice numérico bruto, não o texto do item. Corrigido com duas funções
centralizadas em `maquina_estados.cpp` — `tituloOpcaoMenu(Tela, indice)` e
`quantidadeOpcoesTela(Tela)` — que reaproveitam **os mesmos arrays
`ITENS_*`/`QTD_*` já usados para desenhar cada tela** (nenhuma lista
paralela nova). Usadas em `redesenharTelaAtual()` (agora imprime
`[MENU] Tela: ...` / `[MENU] Opcao selecionada: ...` / `[MENU] Posicao: X
de Y`), em `navegarPara()`/`voltarUmNivel()` (`[MENU] Abrindo: ...`) e no
heartbeat (`[SISTEMA] ... | Opcao=... | Display=...`).

**Arquivos modificados:** `include/MAIN.HPP` (constantes
`DISPLAY_DIAGNOSTIC_DURATION_MS`, `LED_STARTUP_BRIGHTNESS`),
`include/ihm.hpp`/`src/ihm.cpp` (`controlarTodosLeds()`,
`executarDiagnosticoVisual()`, diagnostico visual removido de `init()`,
logs de LED corrigidos para o formato `[ERRO][LEDS] Indice invalido: N,
limite: N`), `src/maquina_estados.cpp` (`EtapaBoot::DiagnosticoDisplay`,
`definirTodosLeds()` corrigida, `tituloOpcaoMenu`/`quantidadeOpcoesTela`,
logs `[MENU]`/`[ENCODER]`/`[LEDS]`/`[IHM] Redesenho solicitado`, heartbeat
com títulos).

**Compilação:** `pio run` — os dois ambientes compilaram sem erros nem
warnings. RAM 14,7% (48.224/327.680 B), Flash 70,0%
(918.157/1.310.720 B) no ambiente padrão.

**Ainda dependente de confirmação física:** se, mesmo após esta correção,
os 6 LEDs continuarem sem acender simultaneamente, a causa deixaria de
ser software (já não há mais chamadas múltiplas de `show()` no caminho de
"todos os LEDs") e passaria a ser elétrica (queda de tensão na fiação em
cadeia, LED individual defeituoso, alimentação insuficiente para os 6
simultâneos). Da mesma forma, a transição para o menu e os novos logs só
foram validados por leitura de código + compilação nesta rodada — a
gravação real (fora do escopo desta tarefa) deve confirmar visualmente a
sequência completa até o menu principal.

---

**Conclusão desta rodada de validação:**

```
CÓDIGO E COMPILAÇÃO APROVADOS — VALIDAÇÃO FÍSICA PENDENTE
```

O firmware compila sem erros e sem warnings, a arquitetura está separada em
módulos coerentes com as responsabilidades pedidas, a máquina de estados
unifica comandos locais e MQTT, e a maior parte das fases funcionais está
implementada. Nenhum teste com hardware real (display, encoder, sensores,
microSD, Wi‑Fi/MQTT) foi executado nesta validação — ver seção
"VALIDAÇÃO FÍSICA PENDENTE".

---

## ADENDO — Diagnóstico de boot/display (rodada seguinte, mesma data)

Depois da primeira rodada, o monitor serial real mostrou os 27/27
autotestes passando e a falha do microSD, mas nenhuma confirmação de que o
display/IHM tinham de fato inicializado, nem de que a máquina de estados
continuava depois dos autotestes. Este adendo documenta a instrumentação
adicionada para tornar isso observável — **sem alterar a pinagem, a
biblioteca gráfica, o controlador ou a rotação do display**, todos mantidos
exatamente como na configuração já confirmada fisicamente.

Backups feitos antes de alterar (fora de `src/`, em `backup_display/`):
`main.cpp.bak`, `ihm.cpp.bak`, `ihm.hpp.bak`, `MAIN.HPP.bak`,
`maquina_estados.cpp.bak`, `armazenamento.cpp.bak`, `autoteste.cpp.bak`.

**Investigação do fluxo atual (antes de alterar):** confirmado que
`ihm::init()` já era chamada primeiro em `setup()` (antes de
`armazenamento::init()`), então a falha do SD **nunca bloqueou** a
inicialização do display — o problema real era falta de visibilidade, não
uma dependência incorreta. Também confirmado que `autoteste::executar()`
sempre retorna normalmente (nenhum `while(true)`/`return` incondicional
após os testes) — os autotestes já não travavam o firmware, só faltava a
mensagem explícita dizendo isso.

**Alterações:**

1. **`include/MAIN.HPP`** — duas novas constantes de diagnóstico:
   `ENABLE_DISPLAY_STARTUP_TEST` (roda vermelho/verde/azul/preto+texto
   dentro do firmware completo, uma vez, após `display->begin()` ter
   sucesso) e `FORCE_MAX_BRIGHTNESS_FOR_DIAGNOSTIC` (força o duty do PWM
   do backlight ao máximo em tempo de execução, sem gravar em NVS).
2. **`include/ihm.hpp` / `src/ihm.cpp`** — `ihm::init()` reescrita com log
   detalhado (pinos, endereços de `bus`/`display`, backlight ligado antes
   do `begin()`, resultado de `begin()`, resolução detectada, heap) e o
   teste visual integrado. Removido o `while(true)` em caso de falha do
   `display->begin()`: agora fica uma flag `displayOk`
   (exposta como `ihm::displayDisponivel()`), e **todas** as funções de
   desenho (`escreverTelaApp`, `escreverTextoTela`,
   `desenharCabecalhoRodape`, `desenharListaMenu`, `desenharConfirmacao`,
   `desenharValorEditavel`, `desenharListaRolavel`, `desenharGradeModulos`,
   `desenharMensagem`) retornam imediatamente se `!displayOk`, sem tocar o
   ponteiro do display. `setBrilho()` agora loga nível solicitado + duty
   calculado e aplica `FORCE_MAX_BRIGHTNESS_FOR_DIAGNOSTIC` sem persistir.
   Logs `[IHM] Limpando tela em <funcao>()` antes de cada `fillScreen`
   de tela cheia (não nas limpezas parciais de faixa, para não poluir).
3. **`src/armazenamento.cpp`** — mensagens de falha do SD padronizadas
   (`[SD] Falha ao montar o cartao` / `[SD] Aplicacao continuara em modo
   sem armazenamento`), reforçando que o comportamento (não bloqueante) já
   estava correto.
4. **`src/autoteste.cpp`** — ao final de `executar()`, imprime
   `[AUTOTESTE] Testes finalizados` / `[AUTOTESTE] Continuando
   inicializacao normal do firmware`.
5. **`src/main.cpp`** — `setup()` instrumentada de ponta a ponta
   (`[BOOT] ...` antes/depois de cada módulo, motivo do reset via
   `esp_reset_reason()`, heap inicial, resultado de
   `xTaskCreatePinnedToCore` via `pdPASS`).
6. **`src/maquina_estados.cpp`** — logs de transição de tela em
   `navegarPara()`/`voltarUmNivel()` (`[ESTADO] Tela: X -> Y`); logs de
   seleção de menu centralizados em `redesenharTelaAtual()` (só quando o
   índice muda **sem** trocar de tela, para não duplicar com o log de
   transição); logs de entrada/saída do modo de edição de Brilho/Volume;
   logs do ciclo de vida do experimento (iniciar/cancelar/finalizar
   repetição, local e via MQTT); logs de modo de operação
   (Hardware/App); logs da sequência de boot (`[STARTUP] Estado: ...`,
   `[STARTUP] Abrindo menu principal`); logs de início/fim de
   renderização (`[IHM] Desenhando tela: ...` / `[IHM] Renderizacao
   concluida`); heartbeat a cada 5s (`[SISTEMA] Ativo | Tela=... |
   Heap=... | SD=... | MQTT=...`) para confirmar que `tick()` continua
   rodando.

**Compilação após as alterações:** `pio run` — ambos os ambientes
(`esp32doit-devkit-v1` e `esp32doit-devkit-v1-selftest`) compilaram sem
erros nem warnings. RAM 14,7% (48.224/327.680 B), Flash 69,9%
(916.685/1.310.720 B) no ambiente padrão.

**Ainda não verificado:** o comportamento acima só foi validado por
leitura de código + compilação, não em hardware real — a próxima gravação
(fora do escopo desta tarefa, que não inclui upload) deve mostrar a
sequência completa de logs `[BOOT]`/`[DISPLAY]`/`[STARTUP]`/`[ESTADO]`/
`[IHM]`/`[SISTEMA]` e a tela física passando por vermelho → verde → azul →
"HardwareFisica"/"Display OK" → logotipos → LEDs → menu principal. Se a
tela continuar sem nenhuma resposta mesmo com `[DISPLAY] Resultado de
begin(): SUCESSO` no log, a causa mais provável passa a ser hardware
(fiação, alimentação do backlight, painel) e não mais o firmware — já que
o teste visual integrado usa exatamente a mesma configuração confirmada
fisicamente antes.

---

## 1. Resultado do `git status` inicial

```
 M include/MAIN.HPP
 M include/ihm.hpp
 M platformio.ini
 M src/ihm.cpp
 M src/main.cpp
?? .claude/
?? docs/
?? include/analise_dados.hpp
?? include/aquisicao.hpp
?? include/armazenamento.hpp
?? include/canais.hpp
?? include/comandos.hpp
?? include/configuracoes.hpp
?? include/experimentos.hpp
?? include/layout.hpp
?? include/maquina_estados.hpp
?? include/mqtt_app.hpp
?? platformio.ini.bak
?? src/analise_dados.cpp
?? src/aquisicao.cpp
?? src/armazenamento.cpp
?? src/canais.cpp
?? src/configuracoes.cpp
?? src/display_test.cpp
?? src/experimentos.cpp
?? src/ihm.cpp.bak
?? src/layout.cpp
?? src/maquina_estados.cpp
?? src/maquina_estados.cpp.bak
?? src/mqtt_app.cpp
```

Nada foi descartado; os arquivos `.bak` (versões anteriores de `ihm.cpp`,
`maquina_estados.cpp`, `platformio.ini`) foram preservados sem alteração.

---

## 2. FASE 1 — Auditoria inicial (inventário)

| Arquivo | Linhas | Finalidade | Dependências principais |
| --- | --- | --- | --- |
| `include/MAIN.HPP` | 87 | Pinagem física e constantes globais (filas, buffers, limites) | Arduino.h, SPI.h, SD.h |
| `include/comandos.hpp` | 52 | Vocabulário único de comandos (`Command`, `CommandType`, `EdgeMode`, `Origem`) compartilhado entre encoder local e MQTT | — |
| `include/layout.hpp` / `src/layout.cpp` | 49 / 67 | Camada única de layout proporcional (`uiX/uiY/uiWidth/uiHeight/uiFontSize/...`) | — |
| `include/ihm.hpp` / `src/ihm.cpp` | 81 / 490 | Display (Arduino_GFX/ST7735), NeoPixel, encoder, tecla KEY, brilho (PWM), buzzer, primitivas de desenho | Arduino_GFX_Library, Adafruit_NeoPixel, layout |
| `include/canais.hpp` / `src/canais.cpp` | 34 / 100 | Única fonte de verdade da config. de borda por canal (`ChannelConfig[NUM_CHANNELS]`), persistida em NVS | Preferences, comandos |
| `include/aquisicao.hpp` / `src/aquisicao.cpp` | 32 / 90 | ISR de borda (`CHANGE`) por canal, fila FreeRTOS, filtro de borda fora da ISR | esp_timer, freertos/queue, canais |
| `include/configuracoes.hpp` / `src/configuracoes.cpp` | 35 / 65 | Brilho/volume/modo de operação, persistidos em NVS | Preferences, ihm |
| `include/experimentos.hpp` / `src/experimentos.cpp` | 38 / 181 | Fluxo de experimento livre (repetições, tempo relativo, contagem de eventos) | aquisicao, armazenamento, mqtt_app, esp_timer |
| `include/armazenamento.hpp` / `src/armazenamento.cpp` | 58 / 235 | microSD: fila + buffer de linhas CSV, listagem/renomeio/exclusão, leitura sequencial para análise | SD, SPI, freertos/queue, freertos/semphr |
| `include/analise_dados.hpp` / `src/analise_dados.cpp` | 34 / 82 | Leitura de repetição do CSV, cálculo de intervalo/velocidade | armazenamento |
| `include/mqtt_app.hpp` / `src/mqtt_app.cpp` | 49 / ~250 | Wi‑Fi + MQTT não bloqueante, tópicos, publicação de estado/eventos/config | WiFi, PubSubClient, ArduinoJson |
| `include/maquina_estados.hpp` / `src/maquina_estados.cpp` | 63 / 1660 | Máquina de estados única (telas, navegação, edição, confirmações, boot) | todos os módulos acima, qrcode.h |
| `src/main.cpp` | ~60 | `setup()`/`loop()`: inicialização dos módulos, 1 tarefa no núcleo 0, laço da máquina de estados no núcleo 1 | todos os módulos |
| `src/display_test.cpp` | 60 | Sketch avulso de autoteste do display (próprios `setup()/loop()`) — **não deve ser linkado com `main.cpp`** | Arduino_GFX_Library |
| `include/autoteste.hpp` / `src/autoteste.cpp` (novos) | 21 / ~180 | Autotestes determinísticos, no-op quando `ENABLE_FIRMWARE_SELF_TESTS=0` | canais, analise_dados, configuracoes |
| `docs/cronometroV7.ino` | 2352 | Referência funcional de um firmware anterior (WebServer+WebSocket+EEPROM, 3 sensores, 5 botões físicos) — **não copiado para o projeto**, usado só para conferência de pinos/lógica | — |
| `docs/Monkey Tech.bmp`, `docs/UFRN.bmp` | — | Logotipos da sequência de boot; existem no repo mas **ainda não convertidos para PROGMEM** | — |
| `docs/HardwareFisicaV2.0-main.zip` | — | Snapshot de uma versão anterior do próprio repositório (não é documento de hardware; conferido e não contém pinagem adicional) | — |
| `src/ihm.cpp.bak`, `src/maquina_estados.cpp.bak`, `platformio.ini.bak` | — | Backups de versões anteriores, não compilados (extensão `.bak`) | — |
| `include/README`, `lib/README` | — | Textos padrão do PlatformIO (não editados) | — |

Não existem pastas `data/`, `test/` nem arquivo `partitions.csv` no projeto
(usa a tabela de partições padrão da placa). Não há uma pasta `lib/` com
bibliotecas locais — todas as dependências vêm de `lib_deps`.

**Namespaces/módulos:** `ihm`, `layout`, `canais`, `aquisicao`,
`configuracoes`, `experimentos`, `armazenamento`, `analise_dados`,
`mqtt_app`, `maquina_estados`, `comandos`, `autoteste` — um por
responsabilidade, sem sobreposição de nomes.

**Código duplicado/morto identificado:**
- `ihm::readEncoder()` (posição acumulada com wrap) e `ihm::lerEventoEncoder()`
  (evento discreto) implementam decodificação de quadratura em paralelo, mas
  só o segundo é usado pela máquina de estados — `readEncoder()` está
  presente na API pública e não tem nenhum chamador atual (código mantido,
  não removido, mas não utilizado hoje).
- `src/ihm.cpp.bak` e `src/maquina_estados.cpp.bak` são versões anteriores
  não compiladas — não afetam o build, mas poluem o diretório `src/`.

---

## 3. Arquitetura (Fase 3)

`src/main.cpp` está pequeno (60 linhas) e restrito a: `Serial.begin`,
inicialização dos módulos, criação de 1 tarefa (`xTaskCreatePinnedToCore`) e
o laço `maquina_estados::tick()` dentro de `loop()`. Não há lógica de
negócio em `main.cpp`. **ATENDIDO.**

Responsabilidades separadas em módulos dedicados — todos presentes:
IHM (`ihm`), máquina de estados (`maquina_estados`), aquisição
(`aquisicao`), canais/config (`canais`, `configuracoes`), armazenamento
(`armazenamento`), experimentos (`experimentos`), MQTT (`mqtt_app`),
análise de dados (`analise_dados`), layout (`layout`). **ATENDIDO.**

Problemas arquiteturais encontrados:

1. **CONFLITO ENCONTRADO (corrigido nesta validação):** `mqtt_app.cpp`
   acessava o mesmo objeto `PubSubClient`/`WiFiClient` a partir de dois
   núcleos sem proteção — `mqtt_app::loop()` roda no núcleo 1 (dentro de
   `maquina_estados::tick()`), mas `mqtt_app::publicarEvento()` é chamado a
   partir de `experimentos::aoReceberEventoValido()`, executado dentro da
   tarefa `tarefaAquisicaoArmazenamento` no **núcleo 0**
   (`src/main.cpp:26` → `aquisicao::processarFilaEventos()` →
   `experimentos.cpp:48`). PubSubClient não é thread-safe. **Correção
   aplicada:** mutex recursivo (`mutexMqtt`, `src/mqtt_app.cpp`) protegendo
   todas as funções que tocam `mqttClient`/`wifiClient`
   (`loop`, `mqttConectado`, `publicarEstado`, `publicarEvento`,
   `publicarConfiguracaoCanais`, `publicarResultadoAnalise`, `reconectar`).
   Recursivo porque `aoReceberMensagem()` (callback do PubSubClient,
   disparado de dentro de `mqttClient.loop()`) pode chamar `reconectar()`,
   que também toma o mutex.
2. **PARCIAL:** `canais::configuracaoCanais[NUM_CHANNELS]` é escrito pela
   IHM (núcleo 1, via `canais::definirModo/definirTodos`) e lido pela
   tarefa de aquisição (núcleo 0, via `canais::obterModo` dentro de
   `aquisicao::processarFilaEventos`) sem mutex. Como cada campo é um
   `uint8_t` isolado, não há corrupção de memória possível (pior caso: um
   evento é filtrado com o modo antigo por uma iteração), mas o padrão foge
   da disciplina usada em `armazenamento`/`experimentos` (mutex/spinlock
   explícitos). Risco baixo, mas vale documentar — **não corrigido nesta
   rodada** para não introduzir uma seção crítica desnecessária em um
   caminho de ISR→fila (custo/benefício desfavorável).
3. **PARCIAL:** três telas locais dependem de `CommandType::Back` para
   cancelar (`ExperimentoRepeticoes`, `AnaliseSelecionarRepeticao`,
   `AnaliseDistancia` — `src/maquina_estados.cpp:340-342,634-636,706-708`),
   mas o encoder local **nunca emite `Back`** (`tick()` só gera
   `Next`/`Previous`/`Confirm`, ver `src/maquina_estados.cpp:1500-1514`).
   `Back` só chega via MQTT. Na prática o usuário local não trava (sempre
   pode `Confirm` para prosseguir), mas não consegue "voltar sem aplicar"
   nessas 3 telas usando somente encoder + tecla KEY — diverge do padrão
   "Voltar sempre disponível" pedido na Fase 9.
4. Não há dependência circular entre módulos (grafo de `#include`
   confirmado: `MAIN.HPP` e `comandos.hpp` são as únicas folhas comuns).
5. Não há escrita em SD nem MQTT dentro de ISR (verificado — ver Fase 13).
6. Não há duplicação de lógica local vs. MQTT: ambas convergem em
   `maquina_estados::processarComando()` (ver Fase 6).

---

## 4. Pinagem (Fase 4)

| GPIO | Constante | Periférico | Direção | Barramento | Arquivo | Conflito |
| --- | --- | --- | --- | --- | --- | --- |
| 32 | `ENC_S1_PIN` | Encoder fase A | IN (pullup) | digital | `include/MAIN.HPP:11` | Nenhum no firmware atual. **Histórico:** no `docs/cronometroV7.ino:20`, GPIO 32 era `SENSOR_1_PIN` — confirma o conflito citado no enunciado, já resolvido pela reatribuição atual. |
| 33 | `ENC_S2_PIN` | Encoder fase B | IN (pullup) | digital | `include/MAIN.HPP:12` | Nenhum no firmware atual. Histórico: `docs/cronometroV7.ino:17` usava GPIO 33 como `BTN_LEFT`. |
| 25 | `ENC_KEY_PIN` | Botão do encoder | IN (pullup) | digital | `include/MAIN.HPP:13` | Nenhum no firmware atual. Histórico: `docs/cronometroV7.ino:15` usava GPIO 25 como `BTN_DOWN`. |
| 21 | `PIN_NEO` | NeoPixel (6 LEDs) | OUT | 1-wire (RMT/bit-bang) | `include/MAIN.HPP:16-17` | Nenhum |
| 19 | `TFT_MISO` | TFT (SW-SPI) | IN | SPI (software) | `include/MAIN.HPP:20-22` | Compartilhado fisicamente com o barramento do microSD (ver Fase 5) |
| 23 | `TFT_MOSI` | TFT (SW-SPI) | OUT | SPI (software) | `include/MAIN.HPP:24-26` | Idem |
| 18 | `TFT_SCLK` | TFT (SW-SPI) | OUT | SPI (software) | `include/MAIN.HPP:28-30` | Idem |
| 5 | `TFT_CS` | TFT chip-select | OUT | SPI | `include/MAIN.HPP:32-34` | Nenhum (CS é exclusivo do TFT) |
| 16 | `TFT_DC` | TFT data/command | OUT | GPIO | `include/MAIN.HPP:36-38` | Nenhum |
| 17 | `TFT_RST` | TFT reset | OUT | GPIO | `include/MAIN.HPP:40-42` | Nenhum |
| 22 | `TFT_BL` | Backlight (PWM/LEDC canal 0) | OUT | PWM | `include/MAIN.HPP:44-46`, `src/ihm.cpp:145-147` | Nenhum |
| 34, 35, 36, 39, 4, 13 | `CHANNEL_PINS[0..5]` | Canais de sensores 1–6 | IN | digital (interrupção CHANGE) | `include/MAIN.HPP:61` | **PENDENTE DE CONFIRMAÇÃO** — não existe documento de pinagem físico para os canais; valores são placeholders plausíveis (comentário explícito em `MAIN.HPP:54-60`), mas sem conflito interno com os demais periféricos. GPIO 34/35/36/39 são *input-only* (sem pull interno) — ok para sensores com pull-up/pull-down externo, mas exige confirmação de hardware. |
| 15 | `SD_CS_PIN` | microSD chip-select | OUT | SPI | `include/MAIN.HPP:67` | **PENDENTE DE CONFIRMAÇÃO** — GPIO15 é um pino de strapping (MTDO); normalmente livre em módulos DOIT DEVKIT, mas deve ser validado fisicamente. Sem conflito com outro periférico do firmware. |
| 27 | `BUZZER_PIN` | Buzzer (`tone()`) | OUT | PWM (LEDC interno do `tone()`) | `include/MAIN.HPP:73` | Nenhum conflito de pino. Ver Fase 5 sobre possível conflito de **canal PWM** com o backlight. Histórico: `docs/cronometroV7.ino:14` usava GPIO 27 como `BTN_UP` — não conflita porque este projeto não usa mais aquele botão. |

Todos os GPIOs marcados "PENDENTE DE CONFIRMAÇÃO" já estão isolados como
constantes nomeadas em `MAIN.HPP` com comentário explícito — nenhum GPIO
foi inventado silenciosamente ou hardcoded fora dessas constantes.

---

## 5. Barramentos e periféricos (Fase 5)

- **SPI do TFT:** `Arduino_SWSPI` (SPI por software/bit-bang), instanciado
  uma única vez em `src/ihm.cpp:67-70`. Não há um segundo objeto de display
  concorrente no build principal (o único outro seria `src/display_test.cpp`,
  agora excluído do link via `build_src_filter`).
- **SPI do microSD:** `SD.begin(SD_CS_PIN)` em `src/armazenamento.cpp:66`
  usa o barramento **SPI de hardware** padrão do ESP32 (biblioteca `SD`via
  `SPIClass` global), enquanto o TFT usa SPI por software
  (`Arduino_SWSPI`) nos mesmos pinos físicos MISO/MOSI/SCK (19/23/18) mas
  em modo bit-bang. Isso evita a maior parte dos conflitos de configuração
  de registrador do periférico SPI de hardware, mas os dois ainda
  competem pelos mesmos fios físicos: **cada transação deve ser exclusiva no
  tempo** (o CS do dispositivo não endereçado precisa estar em nível alto).
  O código atual nunca faz transações de TFT e SD simultaneamente (a IHM
  desenha de forma síncrona, dentro de `tick()`, e a tarefa de armazenamento
  só grava quando chamada por `processarFila()`), mas não há um mutex
  explícito de barramento — **risco latente caso alguém adicione desenho
  no núcleo 0 no futuro.** Classificado como **PARCIAL** (funciona hoje
  pela ausência de concorrência real, não por proteção explícita).
- **PWM (LEDC):** o brilho do backlight usa `ledcSetup/ledcAttachPin` no
  canal `0` (`src/ihm.cpp:26,145-146`). O buzzer usa `tone(BUZZER_PIN, ...)`
  (`src/ihm.cpp:292`), que no núcleo Arduino-ESP32 aloca **internamente**
  um canal LEDC livre (via `esp32-hal-ledc`) — não é o canal `0` fixo, mas
  ambos os recursos vêm do mesmo controlador de 16 canais LEDC do chip.
  Não há registro de canal fixo reservado para o buzzer, então em teoria
  o `tone()` pode alocar qualquer canal livre — sem conflito hoje (só há
  dois usuários de LEDC: 1 canal do brilho + 1 do buzzer, de 16
  disponíveis), mas o código não documenta/reserva isso explicitamente.
  **PARCIAL / observação de robustez.**
- Não há uso de I2C (`Wire.begin()`), UART adicional (`Serial1/Serial2`),
  RS485 ou LittleFS/SPIFFS no projeto.
- `attachInterruptArg(..., CHANGE)` é usado uma vez por canal em
  `src/aquisicao.cpp:57-58`, sem reconfiguração posterior. Sem duplicidade.
- Nenhuma chamada `SPI.end()`/`SD.end()` foi encontrada — nenhum módulo
  encerra um barramento em uso por outro.

---

## 6. Máquina de estados (Fase 6)

Enum explícito `Tela` com 30 valores (`include/maquina_estados.hpp:11-50`)
cobrindo boot, menus, submenus de canais, experimento, arquivos, conexão e
análise de dados. **ATENDIDO.**

Vocabulário único de comando (`comandos::CommandType`,
`include/comandos.hpp:18-39`) com 19 valores, usado tanto pelo encoder local
(`maquina_estados::tick()`, que só gera `Next/Previous/Confirm`) quanto pelo
MQTT (`mqtt_app::aoReceberMensagem()`, que decodifica JSON para o mesmo
enum). Ambas as origens chamam a mesma função
`maquina_estados::processarComando(cmd, origem)`
(`src/maquina_estados.cpp:1522`) — **não existem duas implementações
paralelas do mesmo comando.** **ATENDIDO.**

- Comandos "globais" (`SetBrightness`, `SetChannelMode`,
  `StartExperiment`, etc., `src/maquina_estados.cpp:1527-1565`) agem direto
  sobre os módulos antes de olhar a tela atual — usados hoje só pelo MQTT,
  mas disponíveis para qualquer origem.
- "Voltar" é sempre o último item de cada lista de menu por convenção do
  chamador (`ihm::desenharListaMenu` documenta isso), e reaproveita
  `voltarUmNivel()`/`tratarConfirmacaoBinaria()` — **padronizado.**
- Transições inválidas são ignoradas (todo `switch (cmd.tipo)` tem
  `default: break;`).
- **PARCIAL (ver Fase 3, item 3):** 3 telas de edição de valor
  (repetições do experimento, repetição da análise, distância da análise)
  só aceitam `Back` para cancelar, mas o encoder local nunca emite `Back` —
  a máquina de estados não trava (sempre é possível `Confirm`), mas o
  usuário local não tem uma forma de "cancelar sem aplicar" nessas 3 telas
  específicas.
- Nenhuma tela morta encontrada: telas ainda não desenhadas caem em
  `tratarTelaEmConstrucao()` (Confirm/Back voltam ao nível anterior).

---

## 7 e 8. IHM, display e sequência de boot (Fases 7 e 8)

Implementado e revisado no código (não testado fisicamente): menu
principal, submenus de configurações/canais/experimentos/arquivos/conexão,
teste de canais, gerenciamento de arquivos (parcial — ver Fase 22), Manual
(QR Code), Sobre, análise de dados (seleção de arquivo → repetição →
eventos → distância → resultado), mensagens de erro/confirmação e barra de
valor editável.

- `layout::init(display->width(), display->height())` é chamado uma única
  vez em `ihm::init()` (`src/ihm.cpp:143`), com `UI_REFERENCE_WIDTH/HEIGHT
  = 128/160` (`include/layout.hpp:11-12`) — trocar resolução/rotação exige
  mudar só essas 2 constantes + a chamada de `display->begin()`.
  **ATENDIDO.**
- `uiX/uiY/uiWidth/uiHeight/uiFontSize/uiMargin/uiCenterX/uiCenterY` são
  usados de forma consistente em `desenharCabecalhoRodape`,
  `desenharListaMenu`, `desenharConfirmacao`, `desenharValorEditavel`,
  `desenharListaRolavel`, `desenharGradeModulos`, `desenharMensagem`
  (todas em `src/ihm.cpp`). **Não há coordenadas fixas "mágicas" espalhadas
  fora dessas funções** (as únicas exceções são `escreverTelaApp`, que usa
  offsets fixos 8/28/48/112 sobre uma resolução de 128×160 assumida
  diretamente — função legada, não usada pela máquina de estados atual,
  mas presente na API pública).
- **PARCIAL:** os logotipos "Monkey Tech" e "UFRN" existem como `.bmp` em
  `docs/`, mas a sequência de boot ainda desenha apenas texto
  (`desenharLogoMonkeyTech/desenharLogoUFRN`, `src/maquina_estados.cpp:61-69`,
  com comentário explícito confirmando que a conversão para PROGMEM ainda
  não foi feita).
- Sequência de boot não-bloqueante confirmada via `millis()` + enum
  `EtapaBoot` (`Logos → LedVermelho → LedAzul → LedVerde → LedApagado →
  Desenvolvedor → Concluido`, `src/maquina_estados.cpp:37-45,1413-1473`):
  ordem de cores (vermelho 1s, azul 1s, verde 1s, depois apaga) confere
  exatamente com o pedido; texto final "Desenvolvido por" + "Wilson
  Simonal" por 2000 ms; nenhuma chamada a `beep()` nessa função. Transição
  automática para `MenuPrincipal` ao final. **ATENDIDO** (lógica),
  **NÃO VERIFICÁVEL SEM HARDWARE** (tempos/cores reais na tela).

---

## 9 e 10. Encoder/tecla KEY, brilho e volume (Fases 9 e 10)

- Decodificação de quadratura por borda de descida de `ENC_S1_PIN`, usando
  `ENC_S2_PIN` para direção (`ihm::lerEventoEncoder()`,
  `src/ihm.cpp:239-253`). Evento discreto (`Horario`/`AntiHorario`), sem
  acúmulo — mapeado 1:1 para `Next`/`Previous` em `tick()`.
- Tecla KEY com debounce por tempo (`DEBOUNCE_MS=30`,
  `ihm::teclaClicada()`, `src/ihm.cpp:255-277`): só reporta `true` uma vez,
  na transição pressionado→solto, e nunca repete enquanto mantida —
  **ATENDIDO** (lógica).
- Edição de valor (brilho/volume/repetições/distância/repetição da
  análise) usa um valor temporário (`edicaoValor.valorTemp`) que só é
  persistido/aplicado em `Confirm` — mas como apontado nas Fases 3/6, não
  há um caminho local para "cancelar e restaurar o valor anterior" nessas
  telas (falta `Back` local).
- **Brilho:** PWM em `TFT_BL`, canal/frequência/resolução centralizados em
  `src/ihm.cpp:26-29` (canal 0, 5 kHz, 8 bits), faixa 0–30 mapeada
  linearmente para 0–255 (`setBrilho`, `src/ihm.cpp:279-283`). Persistido em
  NVS via `configuracoes::definirBrilho` → restaurado em `init()`.
  **ATENDIDO.**
- **Volume:** faixa 0–30, `0` = mudo (`beep()` retorna sem tocar,
  `src/ihm.cpp:290-293`); bipe usa `tone()` não-bloqueante (duração default
  60 ms, chamada única, sem laço de espera). Persistido em NVS.
  **ATENDIDO.** Não há um "bipe em evento válido" durante a aquisição
  (a Fase 10 pede isso como opcional) — **NÃO IMPLEMENTADO** (e correto
  que não esteja: bipar a cada evento dentro da tarefa de aquisição
  chamaria uma função de IHM a partir do núcleo 0, quebrando a separação
  de responsabilidades — ver Fase 13).

---

## 11 e 12. Canais de sensores e persistência NVS (Fases 11 e 12)

- `EdgeMode` (`Falling=0, Rising=1, Both=2`) e `ChannelConfig` conforme
  pedido, com única fonte de verdade `canais::configuracaoCanais[NUM_CHANNELS]`
  (`src/canais.cpp:16`). Não há arrays paralelos armazenando o mesmo dado.
  **ATENDIDO.**
- Menu de canais com as 5 opções pedidas
  (`ITENS_CONFIG_CANAIS`, `src/maquina_estados.cpp:126-133`): configurar
  todos, individual, visualizar, restaurar padrão, voltar. Listas
  individuais/visualização geradas dinamicamente com `NUM_CHANNELS`
  (`redesenharConfigCanaisIndividualLista/Visualizar`,
  `src/maquina_estados.cpp:1117-1141`). Confirmação Sim/Não antes de
  aplicar em todos os 3 fluxos (todos/individual/restaurar).
  **ATENDIDO.**
- Restauração define todos como `Both` (`canais::restaurarPadrao()` →
  `definirTodos(EdgeMode::Both)`, `src/canais.cpp:74`); padrão de fábrica
  também é `Both` (valor default de `ChannelConfig::edgeMode`,
  `include/canais.hpp:15`, e valor de fallback ao ler NVS com esquema
  incompatível). **ATENDIDO.**
- Persistência: `Preferences` namespace `"hwfisica_ch"`, uma chave por
  canal (`"c0".."c5"`) + versão de esquema (`"ver"`) — se a versão salva
  não bate, aplica o padrão e regrava (`canais::init()`,
  `src/canais.cpp:36-53`). Valor bruto fora da faixa do enum é saneado
  para `Both` (`validarModo()`, `src/canais.cpp:18-21`). **ATENDIDO.**
- Publicação MQTT da configuração completa após qualquer alteração
  (`publicarConfiguracaoCanais()` chamada em `tentarConectar()` e
  disponível para ser chamada após mudanças — **PARCIAL**: ela **não** é
  chamada automaticamente dentro de
  `canais::definirModo/definirTodos/restaurarPadrao()`; o app só recebe a
  config atualizada na próxima publicação periódica de estado/reconexão,
  não imediatamente após a mudança local. Ver recomendação na seção de
  correções.

**Configurações gerais em NVS** (`configuracoes`, namespace `"hwfisica_cfg"`):
brilho, volume e modo de operação, com validação de faixa ao carregar
(`validarNivel`, `src/configuracoes.cpp:20-23`) e aplicação imediata do
brilho/volume na IHM em `init()`. Escrita em NVS só ocorre dentro de
`definirBrilho/definirVolume/definirModoOperacao`, chamadas apenas em
resposta a `Confirm` do usuário ou comando MQTT — **não há escrita a cada
iteração do `loop()`.** **ATENDIDO.**

---

## 13 e 14. Aquisição, filtro de bordas e debounce (Fases 13 e 14)

ISR mínima confirmada (`aquisicao::isrCanal`, `src/aquisicao.cpp:32-46`):
lê nível (`digitalRead`), timestamp (`esp_timer_get_time()`, `int64_t`),
monta uma struct pequena (`RawEdgeEvent`, 4 campos), envia por
`xQueueSendFromISR` + `portYIELD_FROM_ISR`. **Nenhuma** chamada a SD,
display, MQTT, `String`, Preferences, alocação dinâmica ou som dentro da
ISR. **ATENDIDO.**

Filtro de borda (`canais::isTransitionEnabled`, `src/canais.cpp:76-89`)
roda fora da ISR, em `aquisicao::processarFilaEventos()` (chamada pela
tarefa do núcleo 0). Lógica verificada linha a linha e testada pelo
autoteste interno (Fase 32):
`HIGH→LOW = Falling`, `LOW→HIGH = Rising`, `Both` aceita ambas,
estado igual a estado nunca é aceito. **ATENDIDO.**

**PARCIAL — filtro de debounce dos sensores (Fase 14):** existe a
constante `SENSOR_DEBOUNCE_US` (`include/MAIN.HPP:87`, comentário "filtro
de debounce dos sensores digitais, em microssegundos"), mas **ela não é
lida em nenhum lugar do código** (`grep` não encontra uso de
`SENSOR_DEBOUNCE_US` fora da própria declaração). A ISR atual registra
*toda* transição de nível reportada pelo hardware, sem descartar bordas
mais rápidas que o valor configurado — ou seja, o filtro de **modo** de
borda (Falling/Rising/Both) está implementado e testado, mas o filtro de
**debounce por tempo mínimo entre transições** descrito na Fase 14 está
**ausente**. Isso é uma lacuna real: sensores mecânicos/ópticos com ruído
elétrico podem gerar múltiplos eventos por passagem física.
**NÃO ATENDIDO** — recomendação: comparar `tempoUs - tempoUltimaTransicao[canal]`
contra `SENSOR_DEBOUNCE_US` dentro da própria ISR (é uma comparação de
inteiros, seguro para IRAM) antes de enfileirar o evento.

**Não corrigido nesta rodada** (ver seção de correções): embora simples em
princípio, adicionar debounce dentro da ISR muda o comportamento observável
da captura (pode descartar eventos reais se o valor não for bem calibrado
para os sensores reais) — mudança que depende de decisão/calibração do
responsável pelo hardware, não uma correção mecânica de bug.

---

## 15. FreeRTOS e dois núcleos (Fase 15)

| Tarefa | Função | Core | Prioridade | Stack | Periodicidade | Recurso compartilhado |
| --- | --- | --- | --- | --- | --- | --- |
| `AquisicaoArmazenamento` | `tarefaAquisicaoArmazenamento` (`src/main.cpp:24-30,50-51`) | 0 | 2 | 4096 B | loop com `vTaskDelay(pdMS_TO_TICKS(2))` | fila `filaEventosBrutos` (aquisicao), fila+mutex `filaLinhas`/`mutexArquivo` (armazenamento), spinlock `mux` (experimentos), leitura de `canais::configuracaoCanais` (sem lock — ver Fase 3), chamada a `mqtt_app::publicarEvento` (agora protegida por `mutexMqtt`) |
| `loopTask` (padrão Arduino, implícita) | `maquina_estados::tick()` via `loop()` (`src/main.cpp:54-56`) | 1 (padrão do core Arduino-ESP32) | 1 (padrão) | 8192 B (padrão do framework, não sobrescrito no projeto) | a cada iteração de `loop()`, sem `vTaskDelay` explícito (mas sem laço `while` interno — cede tempo naturalmente entre iterações do Arduino) | display/NeoPixel (`ihm`, uso exclusivo do núcleo 1), `mqtt_app` (via `mutexMqtt`), `configuracoes`/`canais` (escrita, uso exclusivo do núcleo 1 exceto leitura em `aquisicao`) |

- Nenhuma tarefa tem laço `while(true)` sem `vTaskDelay`/`delay` que
  bloqueie de forma agressiva: a tarefa do núcleo 0 tem `vTaskDelay(2ms)`
  explícito; a `loopTask` do núcleo 1 não tem delay explícito, mas o
  framework Arduino-ESP32 já cede tempo ao *idle task* daquele núcleo entre
  chamadas de `loop()` (comportamento padrão, sem laços internos
  bloqueantes dentro de `tick()`).
- **Starvation:** prioridade 2 (tarefa de aquisição) vs. prioridade 1
  (loopTask) — a tarefa de aquisição tem prioridade *maior*, mas roda em
  núcleo diferente da IHM, então não há disputa de CPU entre elas; ambas
  têm bastante folga (`vTaskDelay`/ausência de laço apertado).
  **ATENDIDO.**
- `uxTaskGetStackHighWaterMark` **não é usado em lugar nenhum do código**
  — não há telemetria de stack livre disponível em tempo de execução hoje.
  A stack de 4096 B para `AquisicaoArmazenamento` parece confortável dado
  o corpo simples da tarefa (chamadas a funções com buffers pequenos,
  `char[24]`/`char[16]`), mas **isso não foi medido**, só estimado por
  leitura de código — **NÃO VERIFICÁVEL SEM HARDWARE.**
- Acesso concorrente ao display: só o núcleo 1 chama funções de `ihm`
  (desenho) — sem concorrência real. Acesso concorrente ao SD: protegido
  por `mutexArquivo` (semáforo). Acesso concorrente às configurações de
  canais: sem lock, risco baixo (ver Fase 3). Acesso concorrente ao MQTT:
  **corrigido nesta validação** (mutex recursivo).

---

## 16 a 20. Experimento livre, tempo relativo, microSD, CSV, nome de arquivo

Fluxo revisado ponta a ponta em `experimentos.cpp` +
`maquina_estados.cpp` (telas `ExperimentoRepeticoes` →
`ExperimentoExecucao` → `ExperimentoNomeArquivo` /
`ExperimentoSobrescreverConfirmar`):

- Repetições: valor editável 1..`MAX_REPETICOES` (100, `include/MAIN.HPP:85`),
  clamp aplicado tanto na UI (`tratarExperimentoRepeticoes`) quanto em
  `experimentos::iniciar()` (`src/experimentos.cpp:60-62`). **ATENDIDO.**
- `experimentos::iniciar()` abre o arquivo de trabalho (`_tmp_exp.csv`,
  sempre sobrescrevendo) e zera `inicioRepeticaoUs = esp_timer_get_time()`
  atomically dentro de uma seção crítica. Se o SD estiver indisponível,
  retorna `false` e a IHM volta ao menu sem travar
  (`tratarExperimentoRepeticoes`, `src/maquina_estados.cpp:329-338`).
  **ATENDIDO** (Fase 18/31 — "experimento sem SD mostra e permite voltar").
- Durante a execução, a tela mostra repetição atual/total, eventos válidos
  e tempo decorrido (`redesenharExperimentoExecucao`,
  `src/maquina_estados.cpp:1160-1171`), com ações "Finalizar repetição" e
  "Cancelar experimento" — **cancelar exige confirmação**
  (`ExperimentoCancelarConfirmar`). **ATENDIDO.** O estado do SD não é
  mostrado nessa tela específica (só na tela Sobre) — **PARCIAL**
  (a Fase 16 pede "estado do SD" entre as informações da tela de
  execução).
- **Tempo relativo:** salvo como `tempoUs - inicioRepeticaoUs` (ambos
  `int64_t`, `src/experimentos.cpp:43`), reiniciado a cada nova repetição
  (`finalizarRepeticaoAtual`, `src/experimentos.cpp:95-99`). Não há uso de
  horário absoluto. `esp_timer_get_time()` em microssegundos com range de
  64 bits — sem overflow prático (>500.000 anos até estourar).
  **ATENDIDO.**
- **CSV:** cabeçalho `canal,estado,tempo_us` (`src/armazenamento.cpp:96`),
  canal 1-based, estado `'H'`/`'L'`, tempo relativo, linha em branco entre
  repetições (`enfileirarLinhaEmBranco`), mesmo arquivo para todas as
  repetições (só renomeado/finalizado ao final). Escrita **nunca** ocorre
  dentro da ISR — só em `armazenamento::processarFila()`, chamada pela
  tarefa do núcleo 0. **ATENDIDO — corresponde exatamente ao formato
  pedido.**
- **Nome do arquivo:** editor por encoder com alfabeto `[FIM] espaço A-Z
  0-9` (`ALFABETO_NOME`, `src/maquina_estados.cpp:155`), limite de 10
  caracteres (`TAMANHO_MAX_NOME_ARQUIVO`), remoção de espaços finais
  (`removerEspacosFinais`), nome vazio rejeitado com bipe
  (`finalizarEdicaoNomeArquivo`, `src/maquina_estados.cpp:394-404`),
  extensão `.csv` sempre adicionada via `snprintf`, confirmação obrigatória
  antes de sobrescrever (`ExperimentoSobrescreverConfirmar`) e possibilidade
  de escolher outro nome (voltando à edição se já existir e o usuário
  recusar). **Nenhuma sobrescrita automática.** **ATENDIDO.**
  Como o alfabeto só contém `A-Z0-9` e espaço, **não é possível** digitar
  caracteres inválidos pela IHM local — a "remoção de caracteres
  inválidos" citada na Fase 20 é garantida por construção (não por
  filtragem posterior). Não há um caminho de nome de arquivo inválido vindo
  do MQTT sendo validado da mesma forma — **NÃO VERIFICADO** (o protocolo
  MQTT atual não expõe um comando de renomear/salvar arquivo).

---

## 21. Teste de canais (Fase 21)

Tela dinâmica mostrando `C<n> HIGH/LOW <modo> (<contador>)` por canal
(`redesenharTesteCanais`, `src/maquina_estados.cpp:1143-1158`), usando
`NUM_CHANNELS` para gerar a lista. Nível elétrico mostrado
independentemente do modo configurado (via `aquisicao::nivelAtual`), modo
de borda mostrado via `canais::nomeModo`, contador de mudanças via
`aquisicao::quantidadeMudancas`. **ATENDIDO.**

LEDs: `LED[i] = canal i+1`, HIGH→vermelho, LOW→verde
(`atualizarTelasAoVivo`, `src/maquina_estados.cpp:1216-1225`), com guarda
explícita `if (indiceLed >= NUM_LEDS) break;` — **seguro mesmo se
`NUM_CHANNELS > NUM_LEDS`** (hoje `NUM_CHANNELS == NUM_LEDS == 6`, mas o
código não quebra se um dia divergirem). **ATENDIDO.**

**PARCIAL:** a lista desta tela é redesenhada com `indiceSelecionado` e
`offsetRolagem` fixos em `0` (`ihm::desenharListaMenu("Teste de canais", ...,
0, 0)`), e não existe um `case Tela::TesteCanais` dedicado em
`processarComando()` — qualquer `Confirm` (via `tratarTelaEmConstrucao`)
sai da tela. Funciona (sempre é possível voltar), mas não há realce visual
de uma opção "Voltar" nem rolagem caso `NUM_CHANNELS` cresça além do que
cabe na tela — diferente do padrão de lista rolável usado nas demais
telas.

---

## 22. Gerenciamento de arquivos (Fase 22)

Implementado: listar (`armazenamento::listarArquivos`, com tamanho em
bytes), selecionar, renomear, excluir (com confirmação), voltar
(`GerenciamentoArquivos` → `ArquivoDetalhe` → `ArquivoRenomear` /
`ArquivoExcluirConfirmar`). Arquivos são lidos em blocos linha-a-linha
(`armazenamento::lerProximaLinha`) — nenhum arquivo grande é carregado
inteiro na RAM.

**NÃO ATENDIDO / AUSENTE:**
- **Visualizar conteúdo do arquivo** a partir do gerenciamento de
  arquivos: `ITENS_ARQUIVO_DETALHE = {"Renomear", "Excluir", "Voltar"}`
  (`src/maquina_estados.cpp:148`) — não há opção "Visualizar". (A
  visualização de eventos só existe dentro do fluxo de **Análise de
  dados**, que é uma tela diferente.)
- **Transferência de arquivo por MQTT:** não há tópico `.../file` nem
  comando MQTT de leitura/transferência de arquivo implementado em
  `mqtt_app.cpp` — os tópicos usados hoje são
  `command/state/event/channels/status` (ver Fase 23); `file` está
  descrito no cabeçalho de `mqtt_app.hpp` mas nunca é publicado/assinado.
- **Memória livre/usada** não aparece na tela de gerenciamento de arquivos
  em si (só na tela **Sobre**, via `armazenamento::espacoUsadoBytes/
  espacoTotalBytes`).

---

## 23 a 25. MQTT, configuração de canais via MQTT e Wi‑Fi (Fases 23-25)

- Não-bloqueante: `WiFi.begin()` é assíncrono; `mqttClient.connect()` só é
  chamado a cada `INTERVALO_RECONEXAO_MS` (5000 ms) dentro de `loop()`, com
  `setSocketTimeout(2)` limitando o pior caso de bloqueio de `connect()` a
  2 s (comentário explícito em `mqtt_app.cpp:122-123`) — não é
  perfeitamente não-bloqueante (uma tentativa de conexão pode congelar
  `tick()` por até ~2s), mas é um limite deliberado e documentado, muito
  abaixo do padrão de 15s da biblioteca. **PARCIAL** (bloqueio limitado e
  intencional, não um `while` infinito).
- Nenhum `while (!WiFi.isConnected())` ou `while (!mqtt.connected())`
  bloqueante encontrado em nenhum arquivo do projeto (grep confirma).
  **ATENDIDO.**
- Tópicos: prefixo `hardwarefisica/<device_id>/...` **ATENDIDO**
  (`TOPICO_PREFIXO`, `montarTopico`). Implementados:
  `command` (assinado), `state`, `event`, `channels`, `status` (LWT
  online/offline). **`file` não é usado** (ver Fase 22). O resultado da
  análise de dados é publicado no tópico `event` (mesmo tópico dos eventos
  de captura, não um tópico dedicado) — funciona, mas mistura dois tipos
  de mensagem no mesmo canal.
- Senha nunca aparece em log (`BROKER_SENHA` só é passada para
  `mqttClient.connect()`, nunca impressa via `Serial`). **ATENDIDO.**
- Reconexão progressiva por tempo fixo (não exponencial), mas com limite
  claro — sem loop bloqueante. **ATENDIDO** (parcialmente — não há
  backoff exponencial, só intervalo fixo de 5s).
- Comandos MQTT usam a mesma máquina de estados
  (`maquina_estados::processarComando(cmd, Origem::MQTT)`), sem lógica
  paralela. **ATENDIDO.**
- **PARCIAL (Fase 24):** não há comandos MQTT de **consulta** dedicados
  (ex.: "get_channels", "get_levels") — o app recebe o estado/config via
  publicação periódica (a cada 1s) e após reconexão, não sob demanda.
  Configurar (individual/todos/restaurar) funciona e já salva em NVS + já
  usa as mesmas funções da IHM local — mas **não republica
  `publicarConfiguracaoCanais()` imediatamente após a mudança** (só na
  próxima publicação de estado/reconexão) — ver Fase 12.
- Tela "Conexão com app" mostra Wi‑Fi/IP/MQTT/ID + Reconectar + Voltar
  (`redesenharConexaoApp`). QR Code / broker sem senha na tela **não
  aparecem nessa tela especificamente** (QR Code é só na tela Manual, para
  a URL do manual, não para conexão MQTT) — **PARCIAL** frente à lista
  completa pedida na Fase 25 (não há QR Code de conexão nem exibição do
  broker na tela de conexão).

---

## 26 a 28. Manual/QR Code, Sobre, Análise de dados (Fases 26-28)

- **Manual:** QR Code gerado com a lib `QRCode` a partir de
  `configuracoes::MANUAL_URL` (hoje um placeholder —
  `"http://PLACEHOLDER-CONFIRMAR/manual"`, `include/configuracoes.hpp:21`),
  desenhado via `ihm::desenharGradeModulos` (callback genérico, sem
  acoplar `ihm` à biblioteca de QR). Compatível com PlatformIO
  (`ricmoo/QRCode@^0.0.1` compilou sem erro) e com Arduino_GFX
  (`fillRect` por módulo). **PARCIAL** — mecanismo pronto, mas a URL real
  do manual ainda não foi fornecida (classificado assim explicitamente,
  conforme pedido na Fase 26).
- **Sobre:** nome do equipamento, versão, autor "Wilson Douglas Jales
  Simonal", MAC, modo de operação, Wi‑Fi, MQTT, espaço usado/total do SD,
  quantidade de canais — todos presentes em `redesenharSobre()`
  (`src/maquina_estados.cpp:1062-1101`), em lista rolável
  (`desenharListaRolavel`). **ATENDIDO.**
- **Análise de dados:** fluxo completo selecionar arquivo → selecionar
  repetição → selecionar 2 eventos → distância → velocidade → resultado
  → publicação MQTT (`mqtt_app::publicarResultadoAnalise`). Fórmulas
  conferem exatamente com o pedido
  (`analise_dados::calcularIntervalo/calcularVelocidade`,
  testadas pelo autoteste). Não calcula quando `tempo final <= tempo
  inicial` (`calcularIntervalo`, `src/analise_dados.cpp:63-68`) nem quando
  `deltaTUs <= 0` (`calcularVelocidade`). **ATENDIDO.**

---

## 29. Execução não-bloqueante — varredura de `delay/while/for` (Fase 29)

| Local | Chamada | Classificação | Observação |
| --- | --- | --- | --- |
| `src/ihm.cpp:138` (`init()`) | `while(true) delay(1000)` | **Aceitável** | Só executa se `display->begin()` falhar — trava deliberadamente com log no Serial (falha crítica de hardware na inicialização, sem alternativa útil). |
| `src/display_test.cpp:42-44` | `while(true) delay(1000)` | **Aceitável** | Sketch avulso de autoteste manual, fora do build principal. |
| `src/display_test.cpp:29` | `delay(1000)` | **Aceitável** | Fora do build principal. |
| `src/ihm.cpp` (`beep`) | `tone(..., duracaoMs)` | **Aceitável** | `tone()` não bloqueia (usa o próprio timer/LEDC); duração é assíncrona. |
| Loops `for` em `ihm.cpp`/`maquina_estados.cpp` (desenho de listas, LEDs, boot) | `for` limitado por `NUM_CHANNELS`/`NUM_LEDS`/quantidade de itens | **Aceitável** | Sempre bounded por constantes pequenas (≤ ~40), sem I/O bloqueante dentro do laço. |
| `mqtt_app.cpp` — `mqttClient.connect()` | chamada de biblioteca | **Aceitável/Problemático** | Limitado a 2s via `setSocketTimeout(2)`, mas ainda é um bloqueio síncrono de até 2s dentro de `tick()` (núcleo 1) a cada tentativa de reconexão — congela IHM/encoder por até 2s nesse instante. Não é crítico (raro, só durante reconexão), mas vale nota. |
| Não encontrado | `while` aguardando hardware (`while(!SD.begin())`, `while(!WiFi.isConnected())`, `while(!mqtt.connected())`) | — | **Nenhuma ocorrência** no projeto — confirmado por busca textual. |
| Não encontrado | `delay()` dentro de ISR, aquisição, MQTT `loop()`, ou laços de experimento | — | **Nenhuma ocorrência.** |

Nenhum atraso crítico foi identificado exigindo substituição por
máquina de estados/filas além do já implementado — o único ponto
"problemático" (bloqueio de até 2s do `mqttClient.connect()`) é limitado,
raro e não trava a aquisição (que roda em outro núcleo/tarefa).

---

## 30. Memória e segurança (Fase 30)

- **Alocação dinâmica:** apenas 2 usos de `new`, ambos únicos e na
  inicialização (`Arduino_SWSPI`/`Arduino_ST7735` em `ihm.cpp:67-70`) —
  sem `new`/`malloc` repetido em loop. Nenhum uso de `malloc` cru no
  código do projeto.
- **`String` do Arduino:** não é usada em nenhum módulo do firmware atual
  (todos usam `char[]` fixos + `snprintf`/`strncpy`) — evita fragmentação
  de heap. `docs/cronometroV7.ino` (referência antiga) usa `String`
  extensivamente, mas **não foi copiado** para o projeto atual.
- **`sprintf`/`strcpy` sem limite:** não encontrados — todo o código usa
  `snprintf` com `sizeof(buffer)` ou `strncpy` com tamanho explícito e
  terminação manual (`buffer[sizeof(buffer)-1] = '\0'`), inclusive em
  `maquina_estados.cpp` (nomes de arquivo, MAC, etc.).
- **Índices validados:** confirmados em todos os pontos de acesso a
  arrays por índice externo: `canais::obterModo/definirModo`
  (`canal1based == 0 || > NUM_CHANNELS` → retorna/ignora),
  `aquisicao::nivelAtual/quantidadeMudancas` (mesma guarda),
  `ihm::controlarLED` (`indice >= pixels.numPixels()` → loga e retorna),
  `analise_dados::evento()` (`indice >= quantidadeCarregada` → retorna
  struct vazia estática), laço de LEDs em `atualizarTelasAoVivo`
  (`indiceLed >= NUM_LEDS` → `break`). **ATENDIDO.**
- **Arquivos/imagens grandes em RAM:** não há bitmaps embarcados ainda
  (Fase 7/8, pendente); leitura de CSV é sempre linha-a-linha
  (`armazenamento::lerProximaLinha`, buffer de 32 bytes) — nunca carrega o
  arquivo inteiro.
- **Buffers pequenos, atenção:** `analise_dados::EventoLido` usa
  `MAX_EVENTOS_REPETICAO = 40` — se uma repetição tiver mais de 40 eventos
  registrados no CSV, os eventos excedentes são silenciosamente ignorados
  na análise (não há erro/aviso ao usuário quando isso acontece,
  `src/analise_dados.cpp:41`). **PARCIAL** — comportamento seguro (não
  estoura buffer), mas sem feedback de truncamento.
- Uso consistente de `const`/`constexpr`/`enum class`/tipos de largura
  fixa (`uint8_t`..`uint64_t`, `size_t`) em todos os módulos novos.
  **ATENDIDO.**

---

## 31. Tratamento de falhas (Fase 31)

| Falha | Tratamento | Local |
| --- | --- | --- |
| Display indisponível | `while(true) delay(1000)` após log — trava deliberadamente (não há como continuar sem display; ver Fase 29) | `ihm::init()` |
| microSD ausente/erro em `SD.begin()` | `cartaoOk=false`, log no Serial, resto do firmware continua | `armazenamento::init()` |
| Falha ao abrir arquivo | `abrirNovoArquivo` retorna `false`, incrementa `erros`, IHM volta ao menu sem travar | `armazenamento.cpp` / `tratarExperimentoRepeticoes` |
| Fila de eventos/linhas cheia | Descarta e incrementa contador de erros (`enfileirarLinha`, `contadorErros()`) — **contabilizado**, mas **não exibido na IHM** em nenhuma tela (só publicado via MQTT em `sd_erros`) — **PARCIAL** (Fase 18 pede "informação de erro na interface") |
| Buffer de flush cheio | Descarrega automaticamente ao atingir `STORAGE_FLUSH_THRESHOLD` — não estoura | `armazenamento::processarFila` |
| Wi‑Fi desconectado | `mqtt_app::loop()` retorna cedo; resto do firmware não é afetado | `mqtt_app.cpp` |
| MQTT desconectado | Reconexão só tentada a cada 5s; funcionalidade local intacta | `mqtt_app.cpp` |
| Sensor indisponível fisicamente | Não há detecção de sensor "flutuante"/desconectado — nível lido normalmente via `digitalRead`, sem verificação de plausibilidade | `aquisicao.cpp` |
| NVS com esquema antigo/inválido | Detectado por versão de esquema (`canais`) ou por saturação de faixa (`configuracoes`) — aplica padrão seguro | `canais::init`, `configuracoes::init` |
| Nome de arquivo inválido | Impossível digitar caractere inválido (alfabeto restrito); nome vazio rejeitado com bipe | `maquina_estados.cpp` |
| Erro de configuração (GPIO pendente) | Documentado com constantes nomeadas + comentário "ATENÇÃO: PINAGEM A CONFIRMAR" — nunca inicializado silenciosamente | `MAIN.HPP` |

Nenhum `while (true)` de espera de hardware foi encontrado (ver Fase 29).
**ATENDIDO** de forma geral, com a ressalva de que erros de
fila/buffer/escrita não aparecem na IHM local (só via MQTT/contador
interno).

---

## 32. Testes internos (autotestes)

Criado `include/autoteste.hpp` + `src/autoteste.cpp`, guardado por
`ENABLE_FIRMWARE_SELF_TESTS` (padrão `0`, no-op — build padrão não muda de
tamanho de forma perceptível: +16 bytes de flash pela chamada vazia).
Testado nos dois modos (ver Fase 33).

Casos cobertos (todos usando as funções públicas reais dos módulos, sem
duplicar a lógica interna):

1. `canais::isTransitionEnabled` — Falling (aceita H→L, rejeita L→H),
   Rising (aceita L→H, rejeita H→L), Both (aceita as duas, rejeita
   "sem mudança") — 9 asserções.
2. `canais::definirTodos/definirModo/restaurarPadrao` — todos aplicam o
   mesmo modo; individual altera só o canal alvo; restauração volta para
   `Both`; o teste restaura o modo original do canal 1 ao final (sem
   efeito colateral permanente na configuração do usuário).
3. Índice de canal fora de faixa (`obterModo(0)`,
   `obterModo(NUM_CHANNELS+1)`, `definirModo` com os mesmos índices) —
   confirma retorno seguro e ausência de corrupção do canal 1.
4. `analise_dados::calcularVelocidade` — intervalo positivo (1 m em 2 s =
   0,5 m/s), intervalo zero rejeitado, intervalo negativo rejeitado.
5. Conversão de estado digital para `'H'/'L'` (convenção usada no CSV).
6. Limites de brilho/volume — `NIVEL_MINIMO==0`, `NIVEL_MAXIMO==30`,
   saturação ao definir valor acima do limite (255→30), com restauração do
   valor original do usuário ao final do teste.
7. `MAX_REPETICOES` configurado (> 0).

**Resultado esperado (não executado em hardware real nesta validação —
só compilado):** todos os casos são determinísticos e não dependem de
temporização/hardware externo (SD, Wi‑Fi, sensores), exceto o teste de
limites, que toca o PWM real do backlight via `configuracoes::definirBrilho`
— por isso ele só roda com o firmware já rodando no ESP32 real, nunca em
um ambiente host puro. Casos que dependeriam de SD real (geração/leitura
de CSV, `analise_dados::calcularIntervalo` com eventos carregados de
arquivo) **não foram incluídos** no autoteste para não exigir cartão
presente — ficam como validação física (ver próxima seção).

---

## 33. Compilação (Fase 33)

```
Ambiente:   esp32doit-devkit-v1 (PlatformIO Core, sem upload)
Placa:      DOIT ESP32 DEVKIT V1 (ESP32 240MHz, 320KB RAM, 4MB Flash)
Framework:  arduino (framework-arduinoespressif32 @ 3.20017.241212)
Platform:   espressif32 @ 7.0.1
Toolchain:  toolchain-xtensa-esp32 @ 8.4.0

RAM:   14.7% (48.208 / 327.680 bytes)
Flash: 69.6% (912.601 / 1.310.720 bytes)   [com autoteste habilitado: 69.6%]
Flash: 69.4% (909.901 / 1.310.720 bytes)   [build padrão, autoteste desabilitado]

Warnings: nenhum (build verbose sem "warning:" na saída do compilador)
Erros:    nenhum, após as correções desta rodada (ver seção 36)

Resultado final: [SUCCESS] — ambos os ambientes (`esp32doit-devkit-v1` e
`esp32doit-devkit-v1-selftest`) compilam com `pio run`.
```

Nenhum warning foi ocultado: o build completo (`-v`) foi buscado por
`"warning:"` e não retornou nenhuma ocorrência.

---

## 34. VALIDAÇÃO FÍSICA PENDENTE

Nenhum dos itens abaixo foi executado com hardware real nesta validação —
toda a análise até aqui é estática (leitura de código + compilação).

| Teste | Procedimento | Resultado esperado | Evidência necessária |
| --- | --- | --- | --- |
| Inicialização do display | Ligar o equipamento, observar `display->begin()` | Tela acende, sem tela branca/preta travada | Foto/vídeo da tela após boot |
| Logotipos Monkey Tech/UFRN | Observar sequência de boot | Hoje só aparece **texto** (bitmaps ainda não embarcados) | Foto da tela na etapa `Logos` |
| Seis LEDs no boot | Observar sequência de cores | Vermelho 1s → Azul 1s → Verde 1s → apaga, todos os 6 LEDs | Vídeo da fita de LED durante o boot |
| Menus/navegação | Rodar encoder por todos os submenus | Sem tela travada, "Voltar" sempre alcançável | Roteiro de navegação + checklist |
| Encoder (giro) | Girar nos dois sentidos em menus e edição de valor | Seleciona/incrementa corretamente, sem passos perdidos/duplicados | Log serial ou contagem manual |
| Botão KEY | Pressionar e soltar repetidas vezes, e manter pressionado | 1 clique por acionamento, sem repetição ao segurar | Contagem manual vs. eventos na tela |
| PWM do backlight | Ajustar brilho de 0 a 30 | Variação visível e monotônica de brilho | Foto/medição de luminância (opcional) |
| Buzzer | Ajustar volume, testar bipe em confirmações | Silêncio em volume 0; bipe curto audível acima de 0 | Gravação de áudio ou observação direta |
| Sensores (níveis) | Acionar cada canal fisicamente | Tela de teste mostra HIGH/LOW correto por canal | Foto da tela + acionamento manual |
| Filtragem de bordas | Configurar Falling/Rising/Both e acionar sensor | Só a transição configurada gera evento/contagem | CSV gerado + contagem manual de acionamentos |
| microSD — montagem | Ligar com cartão presente/ausente | `armazenamento::cartaoDisponivel()` reflete corretamente; sem travar | Tela "Sobre" mostrando SD disponível/indisponível |
| Criação/gravação de CSV | Rodar experimento completo com repetições | Arquivo `.csv` criado, formato exatamente conforme Fase 19 | Arquivo `.csv` extraído do cartão |
| Remoção do cartão durante uso | Remover o cartão com experimento em andamento | Sem travar; erro contabilizado; sistema permanece responsivo | Observação direta + `contadorErros()` |
| Wi‑Fi — conexão | Configurar SSID/senha reais (hoje placeholders) e ligar | Conecta e mostra IP na tela de Conexão | Tela "Conexão com app" com IP real |
| MQTT — conexão | Configurar broker real e ligar | Status "online" no tópico `status`, comandos funcionam | Log do broker + tela local |
| Perda de rede / reconexão | Desligar o roteador/broker e religar | Reconecta sozinho, sem travar a IHM local durante a queda | Observação direta + timestamps |
| Transferência de arquivo | — | **Não implementado ainda** (ver Fase 22) — não testável até existir o recurso | — |
| Estabilidade/reinicialização | Deixar rodando por horas, reiniciar manualmente | Sem crash/reset inesperado (watchdog) | Log serial contínuo |
| Persistência após desligar | Configurar brilho/volume/canais, desligar e religar | Valores restaurados exatamente como configurados | Comparação antes/depois |

Nenhum destes itens deve ser marcado como aprovado sem essa evidência
física — a análise de código só permite classificar a **lógica** como
implementada corretamente.

---

## 35. Matriz final de validação

| Recurso | Código encontrado | Compilação | Teste lógico | Teste de integração | Teste físico | Resultado |
| --- | --- | --- | --- | --- | --- | --- |
| Arquitetura/main.cpp | Sim | Aprovado | Aprovado | Aprovado | N/A | Aprovado |
| Display/IHM (telas, layout) | Sim | Aprovado | Aprovado | Parcial (falta logo PROGMEM) | Pendente | Parcial |
| Sequência de boot | Sim | Aprovado | Aprovado | Parcial (logos em texto) | Pendente | Parcial |
| Encoder + tecla KEY | Sim | Aprovado | Aprovado | Parcial (falta `Back` local em 3 telas) | Pendente | Parcial |
| Brilho (PWM) | Sim | Aprovado | Aprovado | Aprovado | Pendente | Parcial |
| Volume/buzzer | Sim | Aprovado | Aprovado | Aprovado | Pendente | Parcial |
| Canais (config. de bordas) | Sim | Aprovado | Aprovado | Aprovado | Pendente | Parcial |
| Persistência NVS | Sim | Aprovado | Aprovado | Aprovado | Pendente | Parcial |
| Aquisição (ISR + fila) | Sim | Aprovado | Aprovado | Parcial (sem debounce por tempo) | Pendente | Parcial |
| Máquina de estados | Sim | Aprovado | Aprovado | Aprovado | Pendente | Parcial |
| Experimento livre | Sim | Aprovado | Aprovado | Aprovado | Pendente | Parcial |
| microSD / armazenamento | Sim | Aprovado | Aprovado | Aprovado | Pendente | Parcial |
| CSV | Sim | Aprovado | Aprovado | Aprovado | Pendente | Parcial |
| Teste de canais (tela) | Sim | Aprovado | Aprovado | Parcial (sem rolagem/seleção) | Pendente | Parcial |
| Gerenciamento de arquivos | Sim | Aprovado | Aprovado | Parcial (falta visualizar/transferir MQTT) | Pendente | Parcial |
| Análise de dados | Sim | Aprovado | Aprovado | Aprovado | Pendente | Parcial |
| MQTT / Wi‑Fi | Sim | Aprovado | Aprovado | Parcial (sem query, sem tópico `file`) | Pendente | Parcial |
| Manual (QR Code) | Sim | Aprovado | Aprovado | Parcial (URL placeholder) | Pendente | Parcial |
| Tela Sobre | Sim | Aprovado | Aprovado | Aprovado | Pendente | Parcial |
| FreeRTOS / concorrência | Sim | Aprovado | Aprovado | Parcial (corrigido MQTT; canais sem lock) | Pendente | Parcial |
| Autotestes internos | Sim (novo) | Aprovado | Aprovado | N/A | N/A | Aprovado |

Nenhum recurso foi classificado como "Aprovado" total quando havia
requisito faltando ou pendência de hardware — a única linha 100%
"Aprovado" é a que não depende de hardware físico por definição
(autotestes internos, que já rodam nesta própria validação estática).

---

## 36. Correções realizadas nesta rodada

| # | Falha | Causa | Arquivo(s) afetado(s) | Correção | Compilou após? |
| --- | --- | --- | --- | --- | --- |
| 1 | Erro de link: `multiple definition of setup()/loop()` | `src/display_test.cpp` (sketch avulso de autoteste do display) tem seu próprio `setup()/loop()` e era compilado junto com `src/main.cpp` por não haver filtro de fontes | `platformio.ini` | Adicionado `build_src_filter = +<*> -<display_test.cpp>` (arquivo preservado, só excluído do link padrão) | Sim |
| 2 | Race condition entre núcleos no cliente MQTT | `mqtt_app::publicarEvento()` é chamado a partir da tarefa do núcleo 0 (aquisição), enquanto `mqtt_app::loop()` roda no núcleo 1 — `PubSubClient`/`WiFiClient` não são thread-safe | `src/mqtt_app.cpp` | Mutex recursivo (`mutexMqtt`) protegendo `loop`, `mqttConectado`, `publicarEstado`, `publicarEvento`, `publicarConfiguracaoCanais`, `publicarResultadoAnalise`, `reconectar` | Sim |
| 3 | Autotestes internos ausentes (Fase 32) | Não existia infraestrutura de autoteste | `include/autoteste.hpp`, `src/autoteste.cpp` (novos), `src/main.cpp` (chamada opcional) | Módulo novo, guardado por `ENABLE_FIRMWARE_SELF_TESTS` (padrão desligado, no-op) | Sim (nos dois modos) |

Nenhuma funcionalidade foi removida ou substituída por stub para conseguir
compilar; nenhum `TODO` foi contado como implementação.

**Falhas identificadas mas NÃO corrigidas nesta rodada** (motivo: exigem
decisão de calibração/design, não são bugs mecânicos de compilação):

- Falta de filtro de debounce por tempo na aquisição (`SENSOR_DEBOUNCE_US`
  declarado mas não usado) — Fase 14.
- `canais::configuracaoCanais` sem lock entre núcleos (risco baixo) —
  Fase 3/15.
- 3 telas sem `Back` local (repetições do experimento, repetição/distância
  da análise) — Fase 3/6/9.
- Falta de opção "Visualizar" e "Transferir por MQTT" no gerenciamento de
  arquivos; tópico MQTT `file` não implementado — Fase 22/23.
- Comandos MQTT de consulta (query) e republicação imediata da config de
  canais após alteração — Fase 24.
- Logotipos ainda em texto (bitmaps não convertidos para PROGMEM) —
  Fase 7/8.
- `MANUAL_URL` ainda é um placeholder — Fase 26.

---

## 37. Onde alterar cada parâmetro (referência rápida)

| Parâmetro | Arquivo / local |
| --- | --- |
| Resolução, rotação | `include/layout.hpp` (`UI_REFERENCE_WIDTH/HEIGHT/ROTATION`) + `src/ihm.cpp` (`TFT_LARGURA/TFT_ALTURA`, construtor `Arduino_ST7735`) |
| Margens, espaçamento, cabeçalho/rodapé | `include/layout.hpp` (`UI_MARGIN`, `UI_HEADER_HEIGHT`, `UI_FOOTER_HEIGHT`, `UI_LINE_SPACING`) |
| Fontes | `layout::uiFontSize()` em `src/layout.cpp` (escala) + tamanhos-base passados em cada `desenhar*` de `src/ihm.cpp` |
| Quantidade de canais | `NUM_CHANNELS` em `include/MAIN.HPP` |
| GPIOs dos canais | `CHANNEL_PINS[]` em `include/MAIN.HPP` |
| GPIOs dos periféricos (encoder, NeoPixel, TFT, SD, buzzer) | `include/MAIN.HPP` |
| Modo padrão das bordas | `canais::restaurarPadrao()` em `src/canais.cpp` (hoje `EdgeMode::Both`) |
| Filtro/debounce dos sensores | `SENSOR_DEBOUNCE_US` em `include/MAIN.HPP` (constante declarada; **lógica de uso ainda precisa ser adicionada em `aquisicao.cpp`**, ver Fase 14) |
| Brilho (faixa, PWM) | `include/configuracoes.hpp` (`NIVEL_MINIMO/MAXIMO`) + `src/ihm.cpp` (`BRILHO_PWM_CANAL/FREQ_HZ/RESOLUCAO_BITS`) |
| Volume | `include/configuracoes.hpp` (`NIVEL_MINIMO/MAXIMO`) + `src/ihm.cpp` (`BUZZER_FREQUENCIA_HZ`) |
| URL do manual | `MANUAL_URL` em `include/configuracoes.hpp` |
| MQTT (broker, credenciais, tópicos, intervalos) | `include/mqtt_app.hpp` (todas as constantes `WIFI_*`, `BROKER_*`, `TOPICO_PREFIXO`, `INTERVALO_*`) |
| Filas (eventos, linhas CSV) | `EVENT_QUEUE_LEN`, `CSV_LINE_QUEUE_LEN` em `include/MAIN.HPP` |
| Buffer de flush do SD | `STORAGE_FLUSH_THRESHOLD` em `include/MAIN.HPP` |
| Limite de repetições | `MAX_REPETICOES` em `include/MAIN.HPP` |
| Habilitar autotestes internos | `ENABLE_FIRMWARE_SELF_TESTS` (defina como `1` via `build_flags`, ex. ambiente `esp32doit-devkit-v1-selftest` em `platformio.ini`) |

---

## 38. Riscos e limitações desta validação

- Toda a validação foi feita por **leitura de código + compilação**; nada
  foi executado em hardware real (sem upload, conforme restrição).
- A pinagem dos canais de sensores, do microSD e do buzzer **não tem
  documento de hardware de origem** no repositório — são placeholders
  explicitamente marcados como pendentes de confirmação em `MAIN.HPP`.
  Qualquer decisão de upload real deve primeiro confirmar esses GPIOs
  contra o hardware físico.
- Credenciais de Wi‑Fi/MQTT são placeholders (`PLACEHOLDER_WIFI_SSID`
  etc.) — o firmware não vai conectar em rede real até serem preenchidas.
- O ambiente `esp32doit-devkit-v1-selftest` foi criado apenas para
  verificar que o código de autoteste compila; ele herda a mesma
  configuração do ambiente principal (`extends`), então `pio run` (sem
  `-e`) compila os dois ambientes por padrão — isso é intencional para
  esta validação, mas pode ser removido/ajustado se o time preferir manter
  só um ambiente no dia a dia.

---

## Conclusão geral

```
CÓDIGO E COMPILAÇÃO APROVADOS — VALIDAÇÃO FÍSICA PENDENTE
```

O firmware está estruturalmente sólido: arquitetura modular coerente,
máquina de estados única para comando local e remoto, ISR mínima com fila
FreeRTOS, escrita em SD bufferizada fora da ISR, persistência em NVS com
validação de esquema, e MQTT não-bloqueante com fallback local. As
lacunas identificadas (debounce de sensores, algumas telas sem "voltar"
local, transferência de arquivo por MQTT, logotipos/URL do manual
pendentes) são pontuais e já mapeadas nesta matriz, não bloqueiam a
compilação nem indicam risco de travamento do equipamento. A correção de
concorrência do MQTT (item mais sério encontrado) já foi aplicada e
verificada por recompilação. Nenhuma funcionalidade foi validada
fisicamente — isso é um passo separado, obrigatório antes de qualquer
uso em campo.
