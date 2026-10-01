# Pinagem — HardwareFisicaV2.0 no ESP32-S3

> Arquivo GERADO por `docs/gerar_pinagem.py` a partir do
> `platformio.ini` e do `include/MAIN.HPP`. Nao edite a mao:
> mude o pino na fonte e rode o script de novo.

Placa: **ESP32-S3-WROOM-1 N16R8** (DevKitC-1) · Display: **TJCTM24028-SPI**

### Como ler a coluna *posicao*

`E-04` = fileira **esquerda**, 4o pino; `D-17` = fileira **direita**, 17o pino.

O numero e a ordem FISICA no header, contada **de cima para baixo com o
conector USB voltado para cima** — cada fileira do ESP32-S3-DevKitC-1 tem 22
pinos. Nao e o numero do GPIO nem o numero do pino do modulo: e so a posicao
na barra de pinos.

Ela existe para uma coisa so: conferir que os fios nao se cruzam. Se a coluna
subir na mesma ordem em que os pinos saem do conector, o chicote fica
paralelo. Se ela pular para tras em alguma linha, aqueles dois fios se cruzam.

Ordem das fileiras (a de referencia para os ordinais):

- **E** (esquerda): 3V3, 3V3, RST, 4, 5, 6, 7, 15, 16, 17, 18, 8, 3, 46, 9, 10, 11, 12, 13, 14, 5V, GND
- **D** (direita): GND, 43, 44, 1, 2, 42, 41, 40, 39, 38, 37, 36, 35, 0, 45, 48, 47, 21, 20, 19, GND, GND

> As vias sobem junto com os pinos do conector (coluna de GPIO: 5, 6, 7, 15, 16, 17, 18, 8, 10), o que corresponde ao modulo montado na orientacao normal. Se o display for virado de cabeca para baixo, esta ordem passa a cruzar os fios em leque e a coluna precisa ser invertida.
A ordem das vias segue a ordem dos pinos do conector, para os fios nao se
cruzarem na PCB.

## Modulo da tela (18 pinos) — coluna esquerda do header

| pino | sinal | liga em | posicao | observacao |
|-----:|-------|---------|---------|------------|
| 1 | `VCC` | 5V | - | com J1 aberto (padrao). Com J1 fechado, ligar em 3V3 |
| 2 | `GND` | GND | - | comum a tudo |
| 3 | `CS` | GPIO5 | E-05 | chip select da TELA |
| 4 | `RESET` | GPIO6 | E-06 |  |
| 5 | `D/C` | GPIO7 | E-07 | dado / comando |
| 6 | `MOSI` | GPIO15 | E-08 | dado do ESP32 para o modulo |
| 7 | `SCK` | GPIO16 | E-09 | clock do barramento |
| 8 | `LED` | GPIO17 | E-10 | backlight, PWM no canal LEDC 2 |
| 9 | `SDO(MISO)` | GPIO18 | E-11 | dado do modulo para o ESP32 |
| 10 | `T_CLK` | GPIO16 | E-09 | mesma rede do pino 7 (`SCK`) — clock do touch |
| 11 | `T_CS` | GPIO8 | E-12 | chip select do TOUCH |
| 12 | `T_DIN` | GPIO15 | E-08 | mesma rede do pino 6 (`MOSI`) — MOSI do touch |
| 13 | `T_DO` | GPIO18 | E-11 | mesma rede do pino 9 (`SDO(MISO)`) — MISO do touch |
| 14 | `T_IRQ` | deixar SOLTO | - | saida do XPT2046 — nunca amarrar a 3V3 ou GND |
| 15 | `SD_CS` | GPIO10 | E-16 | chip select do CARTAO |
| 16 | `SD_MOSI` | GPIO15 | E-08 | mesma rede do pino 6 (`MOSI`) |
| 17 | `SD_MISO` | GPIO18 | E-11 | mesma rede do pino 9 (`SDO(MISO)`) |
| 18 | `SD_SCK` | GPIO16 | E-09 | mesma rede do pino 7 (`SCK`) |

### Por que o T_IRQ (pino 14) fica solto

O T_IRQ e uma **saida** do XPT2046, nao uma entrada: e open-drain, ja tem
pull-up no modulo, e vai a nivel BAIXO quando a tela e tocada.

Amarra-lo em 3V3 curto-circuitaria a saida do controlador no momento do
toque — o transistor interno puxa para GND enquanto o trilho segura em cima.
Em GND seria pior ainda: o pino ficaria preso em "tocado" permanente, alem do
mesmo conflito eletrico.

A regra que se aplica aqui: pino de ENTRADA nao usado se amarra a um trilho
para nao flutuar; pino de SAIDA nao usado se deixa DESCONECTADO. Uma saida
nunca flutua — ela e quem manda no nivel.

Ele so precisaria de fio se o firmware fosse ler o toque por interrupcao, e
nao le: a leitura e por consulta ao controlador, a cada volta do laco.

### Redes compartilhadas (o barramento SPI)

Tela, touch e cartao estao no MESMO barramento SPI e se distinguem apenas
pelo chip select. Na pratica isso quer dizer que cada uma das tres redes de
dados sai de UM pino do ESP32 e chega a TRES pinos do modulo.

Nao ha nada de "extra" a montar: e uma rede so, com tres destinos. Numa PCB
e uma trilha ramificada; em fio solto, tres pontas no mesmo pino do ESP32.

**O que costuma dar errado:** ligar apenas os pinos 6, 7 e 9 (os da tela) e
deixar 10, 12, 13, 16, 17 e 18 sem ligacao. A tela funciona — escrever nela
nao precisa de mais nada — e o toque e o cartao ficam mudos.

| GPIO do ESP32 | sinal | pinos do modulo que recebem essa rede |
|---------------|-------|----------------------------------------|
| GPIO16 (E-09) | SCK | 7 (SCK), 10 (T_CLK), 18 (SD_SCK) |
| GPIO15 (E-08) | MOSI | 6 (MOSI), 12 (T_DIN), 16 (SD_MOSI) |
| GPIO18 (E-11) | SDO(MISO) | 9 (SDO(MISO)), 13 (T_DO), 17 (SD_MISO) |

Faltando qualquer um desses destinos, o dispositivo correspondente fica mudo.

## Sensores e indicadores — coluna direita do header

| sinal | liga em | posicao | observacao |
|-------|---------|---------|------------|
| Canal 1 | GPIO1 | D-04 | sensor, entrada com interrupcao |
| Canal 2 | GPIO2 | D-05 | sensor, entrada com interrupcao |
| Canal 3 | GPIO42 | D-06 | sensor, entrada com interrupcao |
| Canal 4 | GPIO41 | D-07 | sensor, entrada com interrupcao |
| Canal 5 | GPIO40 | D-08 | sensor, entrada com interrupcao |
| Canal 6 | GPIO39 | D-09 | sensor, entrada com interrupcao |
| NeoPixel DIN | GPIO38 | D-10 | 6 LEDs WS2812 em serie |
| Buzzer + | GPIO47 | D-17 | passivo, acionado por tone() |

## Alimentacao e terra

Nao saem de GPIO — sao os trilhos do header. Um mesmo trilho atende varios
consumidores; as posicoes abaixo sao todas equivalentes, escolha a mais
proxima na placa.

| trilho | posicoes no header | alimenta |
|--------|--------------------|----------|
| 3V3 | E-01, E-02 | sensores dos 6 canais; NeoPixel |
| 5V | E-21 | VCC do modulo da tela (pino 1) |
| GND | E-22, D-01, D-21, D-22 | modulo da tela, sensores, NeoPixel, buzzer |

O **NeoPixel alimentado em 3V3** e proposital: o WS2812 exige nivel logico
alto acima de 0,7 x VDD na entrada de dados. Alimentado em 5V isso daria
3,5V, e o ESP32 entrega no maximo 3,3V — o dado ficaria no limite, com falhas
intermitentes. Em 3V3 o limiar cai para 2,3V e a margem fica confortavel. O
custo e brilho maximo um pouco menor.

## Chip selects (o que separa os tres dispositivos do barramento)

| dispositivo | CS |
|-------------|----|
| Tela | GPIO5 |
| Touch | GPIO8 |
| Cartao SD | GPIO10 |

## GPIO que NAO podem ser usados nesta placa

| GPIO | motivo |
|------|--------|
| 26–32 | flash SPI interna |
| 33–37 | PSRAM octal (`memory_type = qio_opi`) |
| 19, 20 | USB nativo (D-/D+), usado pelo Serial |
| 43, 44 | UART0 |
| 0, 3, 45, 46 | strapping (mudam o modo de boot) |
| 48 | LED RGB embutido na placa |

GPIO25 nao existe no ESP32-S3.

## Alimentacao

O modulo tem regulador com dropout de ~1.1V. Com **J1 aberto** (padrao de
fabrica) o VCC precisa de **5V**; 3.3V ali entregam ~2.2V ao controlador e
produzem imagem corrompida e toque instavel. Com **J1 fechado**, VCC = 3.3V
direto. Os pinos de dados sao 3.3V nos dois casos.

O cartao precisa estar formatado em **FAT32**.
