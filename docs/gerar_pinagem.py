"""Gera a tabela de pinagem a partir da FONTE DA VERDADE do firmware.

Le as build flags do platformio.ini e as constantes de include/MAIN.HPP e
escreve docs/PINAGEM.md. Rode este script sempre que mudar um pino, em vez
de editar a tabela a mao — assim ela nao tem como divergir do que compila.

    python docs/gerar_pinagem.py
"""
import io
import re

ini = io.open('platformio.ini', encoding='utf-8').read()
hpp = io.open('include/MAIN.HPP', encoding='utf-8').read()

fl = dict(re.findall(r'-D(TFT_\w+|TOUCH_CS)=(\d+)', ini))
ct = dict(re.findall(r'constexpr uint\d+_t (\w+) = (\d+);', hpp))
canais = [c.strip() for c in
          re.search(r'CHANNEL_PINS\[NUM_CHANNELS\] = \{([^}]*)\}', hpp).group(1).split(',')]

# Ordem fisica das vias do header do ESP32-S3-DevKitC-1, de cima para baixo.
ESQ = ['3V3', '3V3', 'RST', '4', '5', '6', '7', '15', '16', '17', '18', '8',
       '19', '20', '3', '46', '9', '10', '11', '12', '13', '14']
DIR = ['GND', '43', '44', '1', '2', '42', '41', '40', '39', '38', '37', '36',
       '35', '0', '45', '48', '47', '21', 'GND', 'GND', '5V', 'GND']


def via(gpio):
    if gpio in ESQ:
        return f'esq {ESQ.index(gpio) + 1}'
    if gpio in DIR:
        return f'dir {DIR.index(gpio) + 1}'
    return '-'


# (pino, sinal impresso, destino, observacao)
MODULO = [
    (1, 'VCC', 'ver alimentacao', 'com J1 aberto = 5V; com J1 fechado = 3V3'),
    (2, 'GND', 'GND', 'comum a tudo'),
    (3, 'CS', f'GPIO{fl["TFT_CS"]}', 'chip select da TELA'),
    (4, 'RESET', f'GPIO{fl["TFT_RST"]}', ''),
    (5, 'D/C', f'GPIO{fl["TFT_DC"]}', 'dado / comando'),
    (6, 'MOSI', f'GPIO{fl["TFT_MOSI"]}', 'dado do ESP32 para o modulo'),
    (7, 'SCK', f'GPIO{fl["TFT_SCLK"]}', 'clock do barramento'),
    (8, 'LED', f'GPIO{fl["TFT_BL"]}', 'backlight, PWM no canal LEDC 2'),
    (9, 'SDO(MISO)', f'GPIO{fl["TFT_MISO"]}', 'dado do modulo para o ESP32'),
    (10, 'T_CLK', 'ponte -> pino 7', 'clock do touch'),
    (11, 'T_CS', f'GPIO{fl["TOUCH_CS"]}', 'chip select do TOUCH'),
    (12, 'T_DIN', 'ponte -> pino 6', 'MOSI do touch'),
    (13, 'T_DO', 'ponte -> pino 9', 'MISO do touch'),
    (14, 'T_IRQ', 'NAO LIGAR', 'o firmware le o toque por consulta'),
    (15, 'SD_CS', f'GPIO{ct["SD_CS_PIN"]}', 'chip select do CARTAO'),
    (16, 'SD_MOSI', 'ponte -> pino 6', ''),
    (17, 'SD_MISO', 'ponte -> pino 9', ''),
    (18, 'SD_SCK', 'ponte -> pino 7', ''),
]

OUTROS = ([(f'Canal {i}', g, 'sensor, entrada com interrupcao')
           for i, g in enumerate(canais, 1)] +
          [('NeoPixel DIN', ct['PIN_NEO'], f'{ct["NUM_LEDS"]} LEDs WS2812 em serie'),
           ('Buzzer +', ct['BUZZER_PIN'], 'passivo, acionado por tone()')])

L = []
L.append('# Pinagem — HardwareFisicaV2.0 no ESP32-S3')
L.append('')
L.append('> Arquivo GERADO por `docs/gerar_pinagem.py` a partir do')
L.append('> `platformio.ini` e do `include/MAIN.HPP`. Nao edite a mao:')
L.append('> mude o pino na fonte e rode o script de novo.')
L.append('')
L.append('Placa: **ESP32-S3-WROOM-1 N16R8** (DevKitC-1) · Display: **TJCTM24028-SPI**')
L.append('')
L.append('A coluna *via* e a posicao fisica no header (esq/dir, de cima para baixo).')
L.append('A ordem das vias segue a ordem dos pinos do conector, para os fios nao se')
L.append('cruzarem na PCB.')
L.append('')
L.append('## Modulo da tela (18 pinos) — coluna esquerda do header')
L.append('')
L.append('| pino | sinal | liga em | via | observacao |')
L.append('|-----:|-------|---------|-----|------------|')
for p, s, d, o in MODULO:
    g = d.replace('GPIO', '') if d.startswith('GPIO') else ''
    L.append(f'| {p} | `{s}` | {d} | {via(g) if g else "-"} | {o} |')
L.append('')
L.append('### As seis pontes')
L.append('')
L.append('Sao ligacoes **locais no proprio conector** — nao viram fio ate o ESP32.')
L.append('Touch e cartao compartilham o barramento da tela e se distinguem so pelo CS.')
L.append('')
L.append('| unir estes pinos | ao pino | sinal |')
L.append('|------------------|---------|-------|')
L.append('| 10 (T_CLK), 18 (SD_SCK) | 7 | SCK |')
L.append('| 12 (T_DIN), 16 (SD_MOSI) | 6 | MOSI |')
L.append('| 13 (T_DO), 17 (SD_MISO) | 9 | MISO |')
L.append('')
L.append('Sem elas a tela funciona, mas o toque nao responde e o cartao nao monta.')
L.append('')
L.append('## Sensores e indicadores — coluna direita do header')
L.append('')
L.append('| sinal | liga em | via | observacao |')
L.append('|-------|---------|-----|------------|')
for s, g, o in OUTROS:
    L.append(f'| {s} | GPIO{g} | {via(g)} | {o} |')
L.append('')
L.append('## Chip selects (o que separa os tres dispositivos do barramento)')
L.append('')
L.append('| dispositivo | CS |')
L.append('|-------------|----|')
L.append(f'| Tela | GPIO{fl["TFT_CS"]} |')
L.append(f'| Touch | GPIO{fl["TOUCH_CS"]} |')
L.append(f'| Cartao SD | GPIO{ct["SD_CS_PIN"]} |')
L.append('')
L.append('## GPIO que NAO podem ser usados nesta placa')
L.append('')
L.append('| GPIO | motivo |')
L.append('|------|--------|')
L.append('| 26–32 | flash SPI interna |')
L.append('| 33–37 | PSRAM octal (`memory_type = qio_opi`) |')
L.append('| 19, 20 | USB nativo (D-/D+), usado pelo Serial |')
L.append('| 43, 44 | UART0 |')
L.append('| 0, 3, 45, 46 | strapping (mudam o modo de boot) |')
L.append('| 48 | LED RGB embutido na placa |')
L.append('')
L.append('GPIO25 nao existe no ESP32-S3.')
L.append('')
L.append('## Alimentacao')
L.append('')
L.append('O modulo tem regulador com dropout de ~1.1V. Com **J1 aberto** (padrao de')
L.append('fabrica) o VCC precisa de **5V**; 3.3V ali entregam ~2.2V ao controlador e')
L.append('produzem imagem corrompida e toque instavel. Com **J1 fechado**, VCC = 3.3V')
L.append('direto. Os pinos de dados sao 3.3V nos dois casos.')
L.append('')
L.append('O cartao precisa estar formatado em **FAT32**.')
L.append('')

io.open('docs/PINAGEM.md', 'w', encoding='utf-8', newline='\n').write('\n'.join(L))
print(f'docs/PINAGEM.md gerado: {len(MODULO)} pinos do modulo, {len(OUTROS)} outros sinais')
