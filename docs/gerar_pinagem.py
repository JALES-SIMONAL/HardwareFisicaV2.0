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
# Ordem FISICA das duas fileiras do ESP32-S3-DevKitC-1, de cima para baixo
# com o conector USB voltado para cima, 22 vias cada. Conferida contra o
# simbolo do esquema do projeto.
#
# ATENCAO: GPIO19 e GPIO20 (USB nativo) ficam na fileira DIREITA, e sao os
# trilhos 5V/GND que fecham a ESQUERDA. Ter isso trocado desloca todos os
# ordinais a partir da metade da fileira — foi o que aconteceu antes, e o
# erro so aparece na conferencia contra a placa, porque a tabela continua
# coerente consigo mesma.
ESQ = ['3V3', '3V3', 'RST', '4', '5', '6', '7', '15', '16', '17', '18', '8',
       '3', '46', '9', '10', '11', '12', '13', '14', '5V', 'GND']
DIR = ['GND', '43', '44', '1', '2', '42', '41', '40', '39', '38', '37', '36',
       '35', '0', '45', '48', '47', '21', '20', '19', 'GND', 'GND']


def via(gpio):
    """Posicao FISICA do pino no header, no formato E-04 / D-17.

    E = fileira esquerda, D = fileira direita; o numero e a ordem contada
    DE CIMA PARA BAIXO com o conector USB voltado para cima. A direcao
    precisa estar dita: sem ela o ordinal nao identifica pino nenhum, e a
    coluna existe justamente para se verificar que os fios sobem na mesma
    ordem em que saem do conector (sem cruzar).
    """
    if gpio in ESQ:
        return f'E-{ESQ.index(gpio) + 1:02d}'
    if gpio in DIR:
        return f'D-{DIR.index(gpio) + 1:02d}'
    return '-'


def posicoes(nome):
    """Todas as posicoes do header que oferecem um trilho (3V3, 5V, GND)."""
    r = [f'E-{i+1:02d}' for i, p in enumerate(ESQ) if p == nome]
    r += [f'D-{i+1:02d}' for i, p in enumerate(DIR) if p == nome]
    return ', '.join(r)


# (pino, sinal impresso, destino, observacao)
MODULO = [
    (1, 'VCC', '5V', 'com J1 aberto (padrao). Com J1 fechado, ligar em 3V3'),
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
    (14, 'T_IRQ', 'deixar SOLTO', 'saida do XPT2046 — nunca amarrar a 3V3 ou GND'),
    (15, 'SD_CS', f'GPIO{ct["SD_CS_PIN"]}', 'chip select do CARTAO'),
    (16, 'SD_MOSI', 'ponte -> pino 6', ''),
    (17, 'SD_MISO', 'ponte -> pino 9', ''),
    (18, 'SD_SCK', 'ponte -> pino 7', ''),
]

# Um pino em ponte NAO fica sem GPIO: pela ponte ele chega ao mesmo GPIO do
# pino de destino. Mostrar "--" ali dava a impressao de sinal solto, quando
# na verdade e o proprio barramento compartilhado. Aqui o GPIO e resolvido a
# partir do pino alvo, para a tabela mostrar o quadro eletrico completo.
_gpio_por_pino = {pino: d[4:] for pino, _s, d, _o in MODULO if d.startswith('GPIO')}
_nome_por_pino = {pino: sinal for pino, sinal, _d, _o in MODULO}

def resolve_ponte(destino):
    """('ponte -> pino 7') -> (gpio, alvo) ; ou (None, None)."""
    m = re.match(r'ponte -> pino (\d+)$', destino)
    if not m:
        return None, None
    alvo = int(m.group(1))
    return _gpio_por_pino.get(alvo), alvo


OUTROS = ([(f'Canal {i}', g, 'sensor, entrada com interrupcao')
           for i, g in enumerate(canais, 1)] +
          [('NeoPixel DIN', ct['PIN_NEO'], f'{ct["NUM_LEDS"]} LEDs WS2812 em serie'),
           ('Buzzer +', ct['BUZZER_PIN'], 'passivo, acionado por tone()')])


# Sentido da coluna, DERIVADO da tabela — nao escrito a mao. O texto sobre a
# orientacao do display ja ficou desatualizado uma vez por estar fixo.
_gpios_modulo = [int(d[4:]) for _, _, d, _ in MODULO if d.startswith('GPIO')]
# Le o ordinal do formato E-nn / D-nn (antes era 'esq nn'; mudar o
# formato sem ajustar aqui quebrou o gerador uma vez).
_vias_modulo = [int(via(str(g)).split('-')[1]) for g in _gpios_modulo]
_crescente = _vias_modulo == sorted(_vias_modulo)
_coluna = ', '.join(str(g) for g in _gpios_modulo)
if _crescente:
    _ORIENTACAO = (
        'As vias sobem junto com os pinos do conector (coluna de GPIO: '
        + _coluna + '), o que corresponde ao modulo montado na orientacao '
        'normal. Se o display for virado de cabeca para baixo, esta ordem '
        'passa a cruzar os fios em leque e a coluna precisa ser invertida.')
else:
    _ORIENTACAO = (
        'As vias sobem enquanto os pinos do conector descem (coluna de GPIO: '
        + _coluna + '), o que corresponde ao modulo montado DE CABECA PARA '
        'BAIXO. Ordenar a coluna em ordem crescente, achando que esta '
        'arrumando, cruzaria os fios em leque.')

L = []
L.append('# Pinagem — HardwareFisicaV2.0 no ESP32-S3')
L.append('')
L.append('> Arquivo GERADO por `docs/gerar_pinagem.py` a partir do')
L.append('> `platformio.ini` e do `include/MAIN.HPP`. Nao edite a mao:')
L.append('> mude o pino na fonte e rode o script de novo.')
L.append('')
L.append('Placa: **ESP32-S3-WROOM-1 N16R8** (DevKitC-1) · Display: **TJCTM24028-SPI**')
L.append('')
L.append('### Como ler a coluna *posicao*')
L.append('')
L.append('`E-04` = fileira **esquerda**, 4o pino; `D-17` = fileira **direita**, 17o pino.')
L.append('')
L.append('O numero e a ordem FISICA no header, contada **de cima para baixo com o')
L.append('conector USB voltado para cima** — cada fileira do ESP32-S3-DevKitC-1 tem 22')
L.append('pinos. Nao e o numero do GPIO nem o numero do pino do modulo: e so a posicao')
L.append('na barra de pinos.')
L.append('')
L.append('Ela existe para uma coisa so: conferir que os fios nao se cruzam. Se a coluna')
L.append('subir na mesma ordem em que os pinos saem do conector, o chicote fica')
L.append('paralelo. Se ela pular para tras em alguma linha, aqueles dois fios se cruzam.')
L.append('')
L.append('Ordem das fileiras (a de referencia para os ordinais):')
L.append('')
L.append('- **E** (esquerda): ' + ', '.join(ESQ))
L.append('- **D** (direita): ' + ', '.join(DIR))
L.append('')
L.append('> ' + _ORIENTACAO)
L.append('A ordem das vias segue a ordem dos pinos do conector, para os fios nao se')
L.append('cruzarem na PCB.')
L.append('')
L.append('## Modulo da tela (18 pinos) — coluna esquerda do header')
L.append('')
L.append('| pino | sinal | liga em | posicao | observacao |')
L.append('|-----:|-------|---------|---------|------------|')
for p, s, d, o in MODULO:
    if d.startswith('GPIO'):
        g = d[4:]
        L.append(f'| {p} | `{s}` | GPIO{g} | {via(g)} | {o} |')
        continue
    gp, alvo = resolve_ponte(d)
    if gp is not None:
        obs = f'mesma rede do pino {alvo} (`{_nome_por_pino[alvo]}`)'
        if o:
            obs += ' — ' + o
        L.append(f'| {p} | `{s}` | GPIO{gp} | {via(gp)} | {obs} |')
    else:
        L.append(f'| {p} | `{s}` | {d} | - | {o} |')
L.append('')
L.append('### Por que o T_IRQ (pino 14) fica solto')
L.append('')
L.append('O T_IRQ e uma **saida** do XPT2046, nao uma entrada: e open-drain, ja tem')
L.append('pull-up no modulo, e vai a nivel BAIXO quando a tela e tocada.')
L.append('')
L.append('Amarra-lo em 3V3 curto-circuitaria a saida do controlador no momento do')
L.append('toque — o transistor interno puxa para GND enquanto o trilho segura em cima.')
L.append('Em GND seria pior ainda: o pino ficaria preso em "tocado" permanente, alem do')
L.append('mesmo conflito eletrico.')
L.append('')
L.append('A regra que se aplica aqui: pino de ENTRADA nao usado se amarra a um trilho')
L.append('para nao flutuar; pino de SAIDA nao usado se deixa DESCONECTADO. Uma saida')
L.append('nunca flutua — ela e quem manda no nivel.')
L.append('')
L.append('Ele so precisaria de fio se o firmware fosse ler o toque por interrupcao, e')
L.append('nao le: a leitura e por consulta ao controlador, a cada volta do laco.')
L.append('')
L.append('### Redes compartilhadas (o barramento SPI)')
L.append('')
L.append('Tela, touch e cartao estao no MESMO barramento SPI e se distinguem apenas')
L.append('pelo chip select. Na pratica isso quer dizer que cada uma das tres redes de')
L.append('dados sai de UM pino do ESP32 e chega a TRES pinos do modulo.')
L.append('')
L.append('Nao ha nada de "extra" a montar: e uma rede so, com tres destinos. Numa PCB')
L.append('e uma trilha ramificada; em fio solto, tres pontas no mesmo pino do ESP32.')
L.append('')
L.append('**O que costuma dar errado:** ligar apenas os pinos 6, 7 e 9 (os da tela) e')
L.append('deixar 10, 12, 13, 16, 17 e 18 sem ligacao. A tela funciona — escrever nela')
L.append('nao precisa de mais nada — e o toque e o cartao ficam mudos.')
L.append('')
L.append('| GPIO do ESP32 | sinal | pinos do modulo que recebem essa rede |')
L.append('|---------------|-------|----------------------------------------|')
for _alvo, _pontes in ((7, '10 (T_CLK), 18 (SD_SCK)'),
                       (6, '12 (T_DIN), 16 (SD_MOSI)'),
                       (9, '13 (T_DO), 17 (SD_MISO)')):
    _g = _gpio_por_pino[_alvo]
    L.append(f'| GPIO{_g} ({via(_g)}) | {_nome_por_pino[_alvo]} | '
             f'{_alvo} ({_nome_por_pino[_alvo]}), {_pontes} |')
L.append('')
L.append('Faltando qualquer um desses destinos, o dispositivo correspondente fica mudo.')
L.append('')
L.append('## Sensores e indicadores — coluna direita do header')
L.append('')
L.append('| sinal | liga em | posicao | observacao |')
L.append('|-------|---------|---------|------------|')
for s, g, o in OUTROS:
    L.append(f'| {s} | GPIO{g} | {via(g)} | {o} |')
L.append('')
L.append('## Alimentacao e terra')
L.append('')
L.append('Nao saem de GPIO — sao os trilhos do header. Um mesmo trilho atende varios')
L.append('consumidores; as posicoes abaixo sao todas equivalentes, escolha a mais')
L.append('proxima na placa.')
L.append('')
L.append('| trilho | posicoes no header | alimenta |')
L.append('|--------|--------------------|----------|')
L.append('| 3V3 | ' + posicoes('3V3') + ' | sensores dos 6 canais; NeoPixel |')
L.append('| 5V | ' + posicoes('5V') + ' | VCC do modulo da tela (pino 1) |')
L.append('| GND | ' + posicoes('GND') + ' | modulo da tela, sensores, NeoPixel, buzzer |')
L.append('')
L.append('O **NeoPixel alimentado em 3V3** e proposital: o WS2812 exige nivel logico')
L.append('alto acima de 0,7 x VDD na entrada de dados. Alimentado em 5V isso daria')
L.append('3,5V, e o ESP32 entrega no maximo 3,3V — o dado ficaria no limite, com falhas')
L.append('intermitentes. Em 3V3 o limiar cai para 2,3V e a margem fica confortavel. O')
L.append('custo e brilho maximo um pouco menor.')
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


# ---------------------------------------------------------------------
# Regenera tambem o bloco de tabela dentro de include/MAIN.HPP
# ---------------------------------------------------------------------
# Duas tabelas mantidas a mao divergem — ja aconteceu neste projeto. Aqui as
# duas saem da MESMA leitura das flags e constantes, na mesma execucao.
INI_MARCA = '// <<< PINAGEM-GERADA-INICIO'
FIM_MARCA = '// <<< PINAGEM-GERADA-FIM >>>'

H = []
H.append('// ---------------------------------------------------------------------')
H.append('// MAPA DE PINOS — ordenado para MINIMIZAR CRUZAMENTO DE FIOS NA PCB')
H.append('// ---------------------------------------------------------------------')
H.append('// Tabela completa, com as pontes e os motivos: docs/PINAGEM.md')
H.append('//')
H.append('// O criterio nao e o numero do GPIO, e a POSICAO FISICA da via no header')
H.append('// do DevKitC-1. Dois fios so se cruzam quando a ordem em que saem do')
H.append('// conector difere da ordem em que chegam ao header — entao cada grupo sobe')
H.append('// pelo header na sequencia do seu proprio conector. Por isso os GPIO')
H.append('// parecem fora de ordem: e a ordem das POSICOES que importa aqui.')
H.append('//')
H.append('// Posicao: E-04 = fileira esquerda, 4o pino; D-17 = fileira direita, 17o.')
H.append('// Contagem DE CIMA PARA BAIXO com o conector USB voltado para cima; cada')
H.append('// fileira tem 22 pinos. Nao e o numero do GPIO nem o do pino do modulo.')
H.append('//')
for _l in __import__('textwrap').wrap(_ORIENTACAO, 68):
    H.append('// ' + _l)
H.append('//')
H.append('// Modulo da tela (TJCTM24028-SPI) — coluna esquerda:')
H.append('//')
H.append('//   pino  sinal        GPIO   posicao no header')
H.append('//   ----  -----------  -----  -----------------')
for pino, sinal, destino, _obs in MODULO:
    if destino.startswith('GPIO'):
        g = destino[4:]
        H.append(f'//   {pino:>4d}  {sinal:<11s}  {g:>5s}  {via(g)}')
        continue
    gp, alvo = resolve_ponte(destino)
    if gp is not None:
        H.append(f'//   {pino:>4d}  {sinal:<11s}  {gp:>5s}  {via(gp)}  '
                 f'mesma rede do pino {alvo}')
    else:
        H.append(f'//   {pino:>4d}  {sinal:<11s}  {"--":>5s}  {destino}')
H.append('//')
H.append('// Sensores e indicadores — coluna direita:')
H.append('//')
for sinal, g, _o in OUTROS:
    H.append(f'//   {sinal:<14s} GPIO{g:<4s} {via(g)}')
H.append('//')
H.append('// T_IRQ (pino 14) fica SOLTO. E uma SAIDA do XPT2046 (open-drain,')
H.append('// com pull-up no modulo, vai a nivel baixo ao tocar) — amarra-la a')
H.append('// 3V3 ou GND curto-circuita a saida do controlador. Pino de saida')
H.append('// nao usado fica desconectado; quem se amarra a trilho e entrada.')
H.append('//')
H.append('// Alimentacao (trilhos do header, nao saem de GPIO):')
H.append('//   3V3  ' + posicoes('3V3') + '   sensores dos canais, NeoPixel')
H.append('//   5V   ' + posicoes('5V') + '   VCC do modulo da tela (pino 1)')
H.append('//   GND  ' + posicoes('GND') + '')
H.append('//        tudo: modulo, sensores, NeoPixel, buzzer')
H.append('//')
H.append('// NeoPixel em 3V3 de proposito: o WS2812 pede nivel alto acima de')
H.append('// 0,7 x VDD no dado. Em 5V isso seria 3,5V e o ESP32 entrega 3,3V —')
H.append('// ficaria no limite. Em 3V3 o limiar cai para 2,3V.')
H.append('//')
H.append('// GPIO reservados nesta placa: 26..32 (flash), 33..37 (PSRAM octal),')
H.append('// 19/20 (USB nativo), 43/44 (UART0), 0/3/45/46 (strapping), 48 (LED da')
H.append('// placa). GPIO25 nao existe no S3.')
H.append('//')
H.append('// Alimentacao do modulo: com J1 aberto (padrao) VCC = 5V; com J1 fechado,')
H.append('// 3V3. Os pinos de dados sao 3.3V nos dois casos. Cartao em FAT32.')

hpp_novo = io.open('include/MAIN.HPP', encoding='utf-8').read()
a = hpp_novo.index(INI_MARCA)
a = hpp_novo.index(chr(10), a) + 1
b = hpp_novo.index(FIM_MARCA)
io.open('include/MAIN.HPP', 'w', encoding='utf-8', newline='').write(
    hpp_novo[:a] + chr(10).join(H) + chr(10) + hpp_novo[b:])
print('include/MAIN.HPP: bloco de tabela regenerado')
