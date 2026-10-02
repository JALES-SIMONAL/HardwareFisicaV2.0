"""Gera uma imagem de teste com as 6 ordens de canal possiveis da UFRN.bmp.

Cada celula da grade mostra a mesma logo com uma permutacao diferente dos
canais. As marcas brancas no canto superior esquerdo numeram a celula (1 a 6)
— brancas de proposito: R=G=B, entao a marca fica branca em QUALQUER
permutacao e continua servindo de indice mesmo na celula errada.
"""
import io
import struct

ORIGEM = 'docs/UFRN.bmp'
DESTINO = 'docs/UFRN_teste_ordem.bmp'

# (rotulo, indices de origem para (R, G, B) do destino)
# 0=R, 1=G, 2=B da imagem original.
ORDENS = [
    ('1 RGB', (0, 1, 2)),
    ('2 RBG', (0, 2, 1)),
    ('3 GRB', (1, 0, 2)),
    ('4 GBR', (1, 2, 0)),
    ('5 BRG', (2, 0, 1)),
    ('6 BGR', (2, 1, 0)),
]


def ler_bmp(caminho):
    d = io.open(caminho, 'rb').read()
    assert d[:2] == b'BM', 'nao e BMP'
    offset = struct.unpack('<I', d[10:14])[0]
    larg, alt_bruta = struct.unpack('<ii', d[18:26])
    bpp = struct.unpack('<H', d[28:30])[0]
    comp = struct.unpack('<I', d[30:34])[0]
    assert comp == 0 and bpp in (24, 32), f'formato nao suportado: {bpp}bpp comp={comp}'
    topo_para_base = alt_bruta < 0
    alt = abs(alt_bruta)
    bytes_px = bpp // 8
    passo = ((larg * bytes_px + 3) // 4) * 4

    # Devolve em ordem de EXIBICAO (primeira linha = topo), como (R,G,B).
    pixels = []
    for y in range(alt):
        linha_arq = y if topo_para_base else (alt - 1 - y)
        base = offset + linha_arq * passo
        linha = []
        for x in range(larg):
            p = base + x * bytes_px
            azul, verde, vermelho = d[p], d[p + 1], d[p + 2]
            linha.append((vermelho, verde, azul))
        pixels.append(linha)
    return larg, alt, pixels


def escrever_bmp24(caminho, larg, alt, pixels):
    passo = ((larg * 3 + 3) // 4) * 4
    enchimento = passo - larg * 3
    corpo = bytearray()
    # BMP padrao e bottom-up: grava da ultima linha de exibicao para a primeira.
    for y in range(alt - 1, -1, -1):
        for (r, g, b) in pixels[y]:
            corpo += bytes((b, g, r))
        corpo += b'\x00' * enchimento

    offset = 14 + 40
    cabecalho = b'BM' + struct.pack('<IHHI', offset + len(corpo), 0, 0, offset)
    info = struct.pack('<IiiHHIIiiII', 40, larg, alt, 1, 24, 0, len(corpo), 2835, 2835, 0, 0)
    io.open(caminho, 'wb').write(cabecalho + info + bytes(corpo))


larg, alt, orig = ler_bmp(ORIGEM)
print(f'origem: {larg}x{alt}')

GAP = 4
COLS, LINHAS = 2, 3
larg_total = larg * COLS + GAP * (COLS - 1)
alt_total = alt * LINHAS + GAP * (LINHAS - 1)

# Fundo branco: separa as celulas e nao depende de canal nenhum.
grade = [[(255, 255, 255)] * larg_total for _ in range(alt_total)]

for indice, (rotulo, ordem) in enumerate(ORDENS):
    col = indice % COLS
    lin = indice // COLS
    x0 = col * (larg + GAP)
    y0 = lin * (alt + GAP)

    for y in range(alt):
        for x in range(larg):
            px = orig[y][x]
            grade[y0 + y][x0 + x] = (px[ordem[0]], px[ordem[1]], px[ordem[2]])

    # Marcas de indice: (indice+1) quadrados brancos de 5x5, com contorno
    # preto para aparecerem mesmo sobre fundo claro da logo.
    for n in range(indice + 1):
        mx = x0 + 3 + n * 8
        my = y0 + 3
        for dy in range(7):
            for dx in range(7):
                if 0 <= my + dy < alt_total and 0 <= mx + dx < larg_total:
                    borda = dy in (0, 6) or dx in (0, 6)
                    grade[my + dy][mx + dx] = (0, 0, 0) if borda else (255, 255, 255)

escrever_bmp24(DESTINO, larg_total, alt_total, grade)
print(f'gerado: {DESTINO}  ({larg_total}x{alt_total}, 24 bits)')
for i, (rotulo, _) in enumerate(ORDENS):
    print(f'  celula {i + 1} ({(i % COLS) and "direita" or "esquerda"}, '
          f'linha {i // COLS + 1}): {rotulo}')
