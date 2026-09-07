# Pinagem — HardwareFisicaV2.0 no ESP32-S3

> Arquivo GERADO por `docs/gerar_pinagem.py` a partir do
> `platformio.ini` e do `include/MAIN.HPP`. Nao edite a mao:
> mude o pino na fonte e rode o script de novo.

Placa: **ESP32-S3-WROOM-1 N16R8** (DevKitC-1) · Display: **TJCTM24028-SPI**

A coluna *via* e a posicao fisica no header (esq/dir, de cima para baixo).

> As vias sobem junto com os pinos do conector (coluna de GPIO: 5, 6, 7, 15, 16, 17, 18, 8, 9), o que corresponde ao modulo montado na orientacao normal. Se o display for virado de cabeca para baixo, esta ordem passa a cruzar os fios em leque e a coluna precisa ser invertida.
A ordem das vias segue a ordem dos pinos do conector, para os fios nao se
cruzarem na PCB.

## Modulo da tela (18 pinos) — coluna esquerda do header

| pino | sinal | liga em | via | observacao |
|-----:|-------|---------|-----|------------|
| 1 | `VCC` | ver alimentacao | - | com J1 aberto = 5V; com J1 fechado = 3V3 |
| 2 | `GND` | GND | - | comum a tudo |
| 3 | `CS` | GPIO5 | esq 5 | chip select da TELA |
| 4 | `RESET` | GPIO6 | esq 6 |  |
| 5 | `D/C` | GPIO7 | esq 7 | dado / comando |
| 6 | `MOSI` | GPIO15 | esq 8 | dado do ESP32 para o modulo |
| 7 | `SCK` | GPIO16 | esq 9 | clock do barramento |
| 8 | `LED` | GPIO17 | esq 10 | backlight, PWM no canal LEDC 2 |
| 9 | `SDO(MISO)` | GPIO18 | esq 11 | dado do modulo para o ESP32 |
| 10 | `T_CLK` | ponte -> pino 7 | - | clock do touch |
| 11 | `T_CS` | GPIO8 | esq 12 | chip select do TOUCH |
| 12 | `T_DIN` | ponte -> pino 6 | - | MOSI do touch |
| 13 | `T_DO` | ponte -> pino 9 | - | MISO do touch |
| 14 | `T_IRQ` | NAO LIGAR | - | o firmware le o toque por consulta |
| 15 | `SD_CS` | GPIO9 | esq 17 | chip select do CARTAO |
| 16 | `SD_MOSI` | ponte -> pino 6 | - |  |
| 17 | `SD_MISO` | ponte -> pino 9 | - |  |
| 18 | `SD_SCK` | ponte -> pino 7 | - |  |

### As seis pontes

Sao ligacoes **locais no proprio conector** — nao viram fio ate o ESP32.
Touch e cartao compartilham o barramento da tela e se distinguem so pelo CS.

| unir estes pinos | ao pino | sinal |
|------------------|---------|-------|
| 10 (T_CLK), 18 (SD_SCK) | 7 | SCK |
| 12 (T_DIN), 16 (SD_MOSI) | 6 | MOSI |
| 13 (T_DO), 17 (SD_MISO) | 9 | MISO |

Sem elas a tela funciona, mas o toque nao responde e o cartao nao monta.

## Sensores e indicadores — coluna direita do header

| sinal | liga em | via | observacao |
|-------|---------|-----|------------|
| Canal 1 | GPIO1 | dir 4 | sensor, entrada com interrupcao |
| Canal 2 | GPIO2 | dir 5 | sensor, entrada com interrupcao |
| Canal 3 | GPIO42 | dir 6 | sensor, entrada com interrupcao |
| Canal 4 | GPIO41 | dir 7 | sensor, entrada com interrupcao |
| Canal 5 | GPIO40 | dir 8 | sensor, entrada com interrupcao |
| Canal 6 | GPIO39 | dir 9 | sensor, entrada com interrupcao |
| NeoPixel DIN | GPIO38 | dir 10 | 6 LEDs WS2812 em serie |
| Buzzer + | GPIO47 | dir 17 | passivo, acionado por tone() |

## Chip selects (o que separa os tres dispositivos do barramento)

| dispositivo | CS |
|-------------|----|
| Tela | GPIO5 |
| Touch | GPIO8 |
| Cartao SD | GPIO9 |

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
