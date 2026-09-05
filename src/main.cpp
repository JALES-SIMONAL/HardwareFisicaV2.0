// Teste de TOUCH de baixa latencia + cartao SD - ESP32-S3 + XPT2046
//
// Mostra na tela exatamente onde o dedo/caneta esta tocando: desenha um
// rastro continuo sob o ponto de contato e imprime as coordenadas na barra
// superior, junto com pressao, taxa de amostragem (Hz) e tempo de resposta.
// No boot, antes do touch, testa o cartao SD e lista os arquivos.
//
// ---------------------------------------------------------------------------
// FIACAO - TRES modulos no MESMO barramento SPI
// ---------------------------------------------------------------------------
// Os tres (tela, touch e SD) compartilham SCK/MOSI/MISO e se diferenciam
// APENAS pelo CS. Sao 3 fios comuns + 1 CS para cada.
//
//   COMUM (liga nos tres):
//     SCK  / SCL / CLK ......... GPIO12
//     MOSI / SDI / DIN ......... GPIO11
//     MISO / SDO / DOUT ........ GPIO13
//     GND ...................... GND   (obrigatorio ser comum aos tres)
//
//   TELA:
//     CS  / TFT_CS ............. GPIO10
//     DC  / RS ................. GPIO9
//     RESET ...................... GPIO14
//     LED / BL ................. GPIO15
//     VCC ...................... ver a nota de alimentacao abaixo
//
//   TOUCH XPT2046 (no mesmo header do modulo):
//     T_CS  / TP_CS ............ GPIO8
//     T_CLK / TP_CLK ........... GPIO12  (mesmo fio do SCK)
//     T_DIN / TP_DIN ........... GPIO11  (mesmo fio do MOSI)
//     T_DO  / TP_DO ............ GPIO13  (mesmo fio do MISO)
//     T_IRQ .................... nao usado
//
//   CARTAO SD:
//     SD_CS .................... GPIO4
//     SD_SCK ................... GPIO12  (mesmo fio do SCK)
//     SD_MOSI / SD_DI .......... GPIO11  (mesmo fio do MOSI)
//     SD_MISO / SD_DO .......... GPIO13  (mesmo fio do MISO)
//
// ATENCAO 1: na maioria desses modulos vermelhos, os pinos SD_SCK/SD_MOSI/
// SD_MISO do header NAO sao ligados internamente aos da tela. Voce mesmo tem
// que fazer as pontes (3 fios a mais, ou 3 pontos de solda no proprio
// modulo). Sem isso o SD nunca inicializa, por mais correto que esteja o CS.
// O mesmo vale para os pinos do touch (T_CLK/T_DIN/T_DO).
//
// ATENCAO 2: cada modulo precisa do SEU proprio CS, e nenhum pode ficar no
// ar. GPIO10 (tela), GPIO8 (touch) e GPIO4 (SD) - tres pinos distintos.
//
// ATENCAO 3 (alimentacao): esses modulos tem regulador embutido com dropout
// de ~1.1V. Se o jumper J1 estiver aberto (padrao de fabrica), VCC precisa de
// 5V; alimentar com 3.3V ali entrega ~2.2V ao controlador e da corrupcao de
// imagem e leitura instavel de touch. Com J1 fechado, VCC = 3.3V direto.
// Os pinos de dados sao 3.3V nos dois casos.
//
// ---------------------------------------------------------------------------
// O QUE FOI FEITO PARA DEIXAR RAPIDO
// ---------------------------------------------------------------------------
// 1) Nao se usa tft.getTouch(). Aquela funcao tem um laco de "debounce" de
//    pressao mais tres delay() fixos por leitura (~4ms+), o que trava a taxa
//    de amostragem em algumas dezenas de Hz. Aqui a leitura e feita direto no
//    XPT2046 (getTouchRaw/getTouchRawZ), sem nenhum delay, e a conversao
//    raw->pixel e feita a mao (mesma matematica do convertRawXY da lib).
// 2) O ruido do touch resistivo e tratado por mediana de 3 amostras
//    consecutivas (nao introduz atraso, ao contrario de media movel) e por
//    histerese de pressao (limiar de toque > limiar de soltura), que evita o
//    rastro "picotar" no meio do arrasto.
// 3) A tela nunca e redesenhada inteira durante o toque: so o segmento de reta
//    entre a amostra anterior e a atual. Interpolar por reta e o que faz o
//    tracado parecer continuo mesmo com o dedo indo rapido.
// 4) O HUD (numeros da barra) so e atualizado 10x por segundo e usa
//    setTextPadding, para nao roubar tempo de SPI do desenho nem piscar.
// 5) O Serial imprime no maximo 10x por segundo. Serial.printf() a cada
//    amostra sozinho ja derrubaria a taxa para menos de 100Hz.
//
// COMO USAR
//   - A cada boot ele pede a calibracao: toque na seta de cada canto.
//     (Para pular isso depois de acertar a montagem, ponha CALIBRAR_SEMPRE
//     em 0: ai ele salva na NVS e so calibra na primeira vez.)
//   - Botao LIMPAR: apaga o rastro. Botao CALIBRAR: refaz a calibracao.
//   - Pelo Serial: 'l' limpa, 'c' recalibra.
//   - Pelo Serial: 'i' reimprime o ID do controlador.
//
// SE APARECER CHUVISCO EM PARTE DA TELA: aquela regiao e RAM do controlador
// que o firmware nunca escreve, ou seja, o painel e MAIOR que o TFT_WIDTH/
// TFT_HEIGHT configurados. Nesse caso o touch tambem fica deslocado, porque a
// calibracao mapeia para uma tela logica de tamanho errado. Ponha
// MODO_DIAGNOSTICO em 1, veja o ID impresso no Serial e ajuste o driver e a
// resolucao no platformio.ini.

#include <Arduino.h>
#include <SPI.h>
#include <TFT_eSPI.h>
#include <Preferences.h>
#include <FS.h>
#include <SD.h>
#include <stdarg.h>

// 1 = testa o cartao SD no boot (antes do touch) e lista os arquivos.
#define USAR_SD 1

// CS do cartao SD. Os outros tres fios do SD sao os MESMOS da tela:
// SCK=GPIO12, MOSI=GPIO11, MISO=GPIO13 (ver a tabela de fiacao no cabecalho).
#define SD_CS_PIN 4

// 1 = nao roda o teste de touch; le o ID do controlador e fica alternando as
//     4 rotacoes com um padrao de geometria, para descobrir o tamanho real do
//     painel. 0 = programa normal de touch.
#define MODO_DIAGNOSTICO 0

// 1 = calibra a cada inicializacao e nao mexe na NVS (util enquanto a
//     montagem/pinagem ainda esta mudando, porque uma calibracao velha
//     salva na flash mascara qualquer troca de painel ou de rotacao).
// 0 = calibra so na 1a vez e guarda na NVS; nos boots seguintes sobe direto.
#define CALIBRAR_SEMPRE 1

TFT_eSPI tft = TFT_eSPI();
Preferences prefs;

// --- Orientacao. 0 = retrato 240x320 (o layout abaixo foi feito para ela) ---
static const uint8_t ROTACAO = 0;

// --- Limiares de pressao (Z do XPT2046) --------------------------------------
// Com histerese: precisa passar de Z_TOQUE para comecar, e so solta quando cai
// abaixo de Z_SOLTA. Se o toque "engasgar" no arrasto, baixe Z_SOLTA.
// Se aparecerem pontos fantasmas sem encostar, suba Z_TOQUE.
static const uint16_t Z_TOQUE = 400;
static const uint16_t Z_SOLTA = 250;

// Salto maior que isso entre duas amostras nao e ligado por reta: e leitura
// suja do XPT2046 ou o dedo voltando em outro lugar.
static const int16_t SALTO_MAX = 70;

// --- Layout -------------------------------------------------------------------
static const int16_t BARRA_H = 40;   // barra de informacoes no topo
static const int16_t BTN_H   = 36;   // botoes na base
static int16_t inkY0, inkY1;         // area de desenho (rastro)

static const uint16_t COR_BARRA  = 0x2124;  // cinza escuro
static const uint16_t COR_BTN    = 0x03EF;  // teal
static const uint16_t COR_BTN2   = 0x8000;  // vinho

struct Botao {
  int16_t x, y, w, h;
  const char *rotulo;
  uint16_t cor;
  bool contem(int16_t px, int16_t py) const {
    return px >= x && px < x + w && py >= y && py < y + h;
  }
};
static Botao btnLimpar, btnCalibrar;

// --- Calibracao ---------------------------------------------------------------
static uint16_t calData[5] = {0, 0, 0, 0, 0};
// Copia local dos parametros, para converter raw->pixel sem chamar a lib.
static uint16_t cal_x0 = 1, cal_x1 = 1, cal_y0 = 1, cal_y1 = 1;
static bool cal_rotate = false, cal_invert_x = false, cal_invert_y = false;

// --- Estado do tracado --------------------------------------------------------
static bool tocando = false;
static int16_t ultX = -1, ultY = -1;
static uint8_t corAtual = 0;
static const uint16_t PALETA[6] = {
  TFT_CYAN, TFT_GREENYELLOW, TFT_ORANGE, TFT_MAGENTA, TFT_YELLOW, TFT_SKYBLUE
};

// --- Estatisticas -------------------------------------------------------------
static uint32_t amostras = 0;       // leituras validas no segundo corrente
static uint32_t hz = 0;             // taxa medida
static uint32_t usLeitura = 0;      // tempo do ciclo ler+converter+desenhar
static uint32_t toques = 0;

// =============================================================================
// Leitura do touch
// =============================================================================

static inline uint16_t mediana3(uint16_t a, uint16_t b, uint16_t c) {
  if (a > b) { uint16_t t = a; a = b; b = t; }
  if (b > c) { uint16_t t = b; b = c; c = t; }
  if (a > b) { uint16_t t = a; a = b; b = t; }
  return b;
}

// Mesma conversao do TFT_eSPI::convertRawXY(), replicada aqui para poder
// rodar sem os delays de validTouch().
static void rawParaTela(uint16_t rx, uint16_t ry, int16_t *sx, int16_t *sy) {
  const int32_t w = tft.width();
  const int32_t h = tft.height();
  int32_t xx, yy;

  if (!cal_rotate) {
    xx = ((int32_t)rx - cal_x0) * w / cal_x1;
    yy = ((int32_t)ry - cal_y0) * h / cal_y1;
  } else {
    xx = ((int32_t)ry - cal_x0) * w / cal_x1;
    yy = ((int32_t)rx - cal_y0) * h / cal_y1;
  }
  if (cal_invert_x) xx = w - xx;
  if (cal_invert_y) yy = h - yy;

  if (xx < 0) xx = 0; else if (xx > w - 1) xx = w - 1;
  if (yy < 0) yy = 0; else if (yy > h - 1) yy = h - 1;

  *sx = (int16_t)xx;
  *sy = (int16_t)yy;
}

// Leitura rapida: 1 leitura de pressao + 3 de posicao (mediana) + 1 de
// confirmacao de pressao. Sem nenhum delay(). Custa ~200us a 2.5MHz.
static bool lerToque(int16_t *sx, int16_t *sy, uint16_t *z) {
  uint16_t limiar = tocando ? Z_SOLTA : Z_TOQUE;

  uint16_t zz = tft.getTouchRawZ();
  if (zz < limiar) return false;

  uint16_t rx[3], ry[3];
  for (int i = 0; i < 3; i++) tft.getTouchRaw(&rx[i], &ry[i]);

  // Confirma que ainda ha pressao depois de amostrar: descarta a borda de
  // soltura, que e onde o XPT2046 devolve coordenada lixo.
  uint16_t zz2 = tft.getTouchRawZ();
  if (zz2 < limiar) return false;

  rawParaTela(mediana3(rx[0], rx[1], rx[2]),
              mediana3(ry[0], ry[1], ry[2]), sx, sy);
  *z = (zz + zz2) / 2;
  return true;
}

// =============================================================================
// Calibracao (persistida na NVS)
// =============================================================================

static void aplicarCalibracao() {
  tft.setTouch(calData);  // mantem a lib coerente (usado por calibrateTouch)

  cal_x0 = calData[0] ? calData[0] : 1;
  cal_x1 = calData[1] ? calData[1] : 1;
  cal_y0 = calData[2] ? calData[2] : 1;
  cal_y1 = calData[3] ? calData[3] : 1;
  cal_rotate   = calData[4] & 0x01;
  cal_invert_x = calData[4] & 0x02;
  cal_invert_y = calData[4] & 0x04;
}

#if !CALIBRAR_SEMPRE
// A calibracao so vale para a resolucao/rotacao em que foi feita: os valores
// convertem raw -> pixel usando tft.width()/height(). Por isso a assinatura da
// configuracao vai salva junto; se voce mudar o driver, a resolucao ou a
// rotacao no platformio.ini, a calibracao antiga e descartada sozinha.
static uint32_t assinaturaConfig() {
  return ((uint32_t)TFT_WIDTH << 20) ^ ((uint32_t)TFT_HEIGHT << 8) ^ ROTACAO;
}

static bool carregarCalibracao() {
  bool ok = false;
  prefs.begin("tela", true);
  uint32_t sig = prefs.getULong("sig", 0);
  if (sig == assinaturaConfig() && prefs.getBytesLength("cal") == sizeof(calData)) {
    prefs.getBytes("cal", calData, sizeof(calData));
    ok = (calData[1] != 0 && calData[3] != 0);
  } else if (sig != 0) {
    Serial.println("Config da tela mudou desde a ultima calibracao -> recalibrando.");
  }
  prefs.end();
  if (ok) aplicarCalibracao();
  return ok;
}

static void salvarCalibracao() {
  prefs.begin("tela", false);
  prefs.putBytes("cal", calData, sizeof(calData));
  prefs.putULong("sig", assinaturaConfig());
  prefs.end();
}
#endif  // !CALIBRAR_SEMPRE

// =============================================================================
// Cartao SD (mesmo barramento SPI da tela)
// =============================================================================
#if USAR_SD

static const uint32_t SD_FREQ_RAPIDA = 20000000;
static const uint32_t SD_FREQ_LENTA  =  4000000;
static const uint32_t SD_TEMPO_LEITURA_MS = 2500;  // tempo para ler na tela

static int16_t logY = 0;

// Escreve a mesma linha no Serial e na tela. A tela importa porque o Serial
// pela USB nativa costuma perder as primeiras linhas do boot.
static void logDuplo(const char *fmt, ...) {
  char buf[110];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);

  Serial.println(buf);

  if (logY <= tft.height() - 10) {
    tft.setTextFont(1);
    tft.setTextDatum(TL_DATUM);
    tft.setTextPadding(0);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.drawString(buf, 4, logY);
    logY += 10;
  }
}

static void listarSD() {
  File raiz = SD.open("/");
  if (!raiz || !raiz.isDirectory()) {
    logDuplo("SD: nao consegui abrir a raiz");
    if (raiz) raiz.close();
    return;
  }

  logDuplo("Arquivos na raiz:");
  int n = 0;
  File e = raiz.openNextFile();
  while (e) {
    if (e.isDirectory()) logDuplo("  [DIR] %s", e.name());
    else                 logDuplo("  %-24s %u bytes", e.name(), (unsigned)e.size());
    e.close();
    e = raiz.openNextFile();
    n++;
  }
  if (n == 0) logDuplo("  (cartao vazio)");
  else        logDuplo("Total: %d entradas", n);
  raiz.close();
}

// Retorna true se o cartao montou. Roda ANTES do touch, para o resultado
// aparecer na tela limpa e no Serial antes de qualquer coisa apagar.
static bool testarSD() {
  logY = 0;
  tft.fillScreen(TFT_BLACK);
  logDuplo("== Teste do cartao SD ==");
  logDuplo("CS=GPIO%d  SCK=GPIO%d  MOSI=GPIO%d  MISO=GPIO%d",
           SD_CS_PIN, TFT_SCLK, TFT_MOSI, TFT_MISO);

  pinMode(SD_CS_PIN, OUTPUT);
  digitalWrite(SD_CS_PIN, HIGH);   // solta o SD antes de falar com a tela

  // O SD TEM que usar a mesma instancia SPI do TFT_eSPI. Com USE_HSPI_PORT a
  // tela esta no SPI3; se o SD subisse no objeto "SPI" global (SPI2) nos
  // mesmos pinos, o roteamento do GPIO seria reatribuido ao SPI2 e a tela
  // pararia de responder. getSPIinstance() devolve o barramento da tela.
  SPIClass &barramento = TFT_eSPI::getSPIinstance();

  bool ok = SD.begin(SD_CS_PIN, barramento, SD_FREQ_RAPIDA);
  if (!ok) {
    logDuplo("Falhou a %u MHz, tentando %u MHz...",
             (unsigned)(SD_FREQ_RAPIDA / 1000000), (unsigned)(SD_FREQ_LENTA / 1000000));
    ok = SD.begin(SD_CS_PIN, barramento, SD_FREQ_LENTA);
  }

  if (!ok) {
    tft.setTextColor(TFT_RED, TFT_BLACK);
    logDuplo("SD: NAO INICIALIZOU.");
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    logDuplo("Confira: cartao inserido e em FAT32;");
    logDuplo("CS no GPIO%d; SD_SCK/SD_MOSI/SD_MISO", SD_CS_PIN);
    logDuplo("ligados nos mesmos GPIO12/11/13 da tela;");
    logDuplo("VCC do modulo e GND comum.");
    delay(SD_TEMPO_LEITURA_MS);
    return false;
  }

  uint8_t tipo = SD.cardType();
  const char *nomeTipo = (tipo == CARD_MMC)  ? "MMC"
                       : (tipo == CARD_SD)   ? "SDSC"
                       : (tipo == CARD_SDHC) ? "SDHC/SDXC"
                       : "desconhecido";
  if (tipo == CARD_NONE) {
    logDuplo("SD: montou, mas nao ha cartao.");
    delay(SD_TEMPO_LEITURA_MS);
    return false;
  }

  tft.setTextColor(TFT_GREEN, TFT_BLACK);
  logDuplo("SD OK - tipo %s", nomeTipo);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  logDuplo("Tamanho: %llu MB   Usado: %llu MB",
           SD.cardSize() / (1024ULL * 1024ULL),
           SD.usedBytes() / (1024ULL * 1024ULL));

  listarSD();

  delay(SD_TEMPO_LEITURA_MS);
  return true;
}
#endif  // USAR_SD

// Le o registrador de ID do controlador. So funciona com o MISO ligado.
static void imprimirIdDisplay() {
  uint8_t b1 = tft.readcommand8(0xD3, 1);
  uint8_t b2 = tft.readcommand8(0xD3, 2);
  uint8_t b3 = tft.readcommand8(0xD3, 3);
  Serial.printf("ID do controlador (reg 0xD3): %02X %02X %02X\n", b1, b2, b3);
  Serial.println("  93 41 -> ILI9341 (240x320) | 94 88 -> ILI9488 (320x480)");
  Serial.println("  00 00 ou FF FF -> sem resposta: confira o MISO (GPIO13)");
  // Fallback: alguns paineis nao respondem ao 0xD3 mas respondem ao 0x04.
  Serial.printf("ID alternativo (reg 0x04): %02X %02X %02X\n",
                tft.readcommand8(0x04, 1), tft.readcommand8(0x04, 2),
                tft.readcommand8(0x04, 3));
  // MADCTL: o bit 0x20 (MV) ligado significa que o controlador esta com
  // linha/coluna trocadas, ou seja, varrendo a tela DEITADA.
  uint8_t madctl = tft.readcommand8(0x0B, 1);
  Serial.printf("MADCTL (reg 0x0B): %02X  -> MV(0x20)=%d MX(0x40)=%d MY(0x80)=%d\n",
                madctl, (madctl & 0x20) ? 1 : 0, (madctl & 0x40) ? 1 : 0,
                (madctl & 0x80) ? 1 : 0);
  Serial.printf("Configurado no platformio.ini: %d x %d, rotacao %d -> %d x %d\n",
                TFT_WIDTH, TFT_HEIGHT, ROTACAO, tft.width(), tft.height());
}

// Padrao de geometria: a moldura branca tem que coincidir com a borda fisica
// do vidro. Se sobrar area acesa fora da moldura (chuvisco ou preto), a
// resolucao configurada nao bate com o painel real.
static void testeGeometria(uint8_t rot) {
  tft.setRotation(rot);
  tft.fillScreen(TFT_BLACK);

  int16_t w = tft.width();
  int16_t h = tft.height();

  tft.drawRect(0, 0, w, h, TFT_WHITE);
  tft.drawRect(1, 1, w - 2, h - 2, TFT_WHITE);
  tft.drawLine(0, 0, w - 1, h - 1, TFT_BLUE);
  tft.drawLine(w - 1, 0, 0, h - 1, TFT_BLUE);

  // Cada canto com uma cor, para identificar orientacao e espelhamento
  tft.fillRect(0, 0, 24, 24, TFT_RED);
  tft.fillRect(w - 24, 0, 24, 24, TFT_GREEN);
  tft.fillRect(0, h - 24, 24, 24, TFT_YELLOW);
  tft.fillRect(w - 24, h - 24, 24, 24, TFT_CYAN);

  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextDatum(TL_DATUM);
  tft.setTextPadding(0);
  tft.setTextFont(2);
  tft.drawString(String("ROT ") + rot, 40, h / 2 - 24);
  tft.drawString(String(w) + " x " + String(h), 40, h / 2);

  // Regua numerada a cada 40 px, nas duas bordas. E o que mede o painel de
  // verdade: se ele for MENOR que o configurado, o endereco "da a volta" e os
  // numeros altos reaparecem no topo/esquerda. O ultimo numero que aparece na
  // sequencia certa, +40, e o tamanho real daquele eixo.
  tft.setTextFont(1);
  for (int16_t yy = 40; yy < h; yy += 40) {
    tft.drawFastHLine(0, yy, 14, TFT_WHITE);
    tft.drawString(String(yy), 16, yy - 3);
  }
  for (int16_t xx = 40; xx < w; xx += 40) {
    tft.drawFastVLine(xx, 0, 14, TFT_WHITE);
    tft.drawString(String(xx), xx + 2, 16);
  }
}

static void calibrar() {
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextFont(2);
  tft.setTextDatum(TL_DATUM);
  tft.setTextPadding(0);
  tft.drawString("Calibracao do touch", 12, 60);
  tft.setTextFont(1);
  tft.drawString("Toque na seta de cada canto", 12, 90);
  tft.drawString("com a ponta da unha ou stylus.", 12, 104);

  tft.calibrateTouch(calData, TFT_WHITE, TFT_RED, 15);
  aplicarCalibracao();
#if CALIBRAR_SEMPRE
  Serial.println("Calibracao concluida (nao salva: CALIBRAR_SEMPRE=1). Valores:");
#else
  salvarCalibracao();
  Serial.println("Calibracao salva na NVS. Valores:");
#endif
  Serial.printf("  {%u, %u, %u, %u, %u}\n",
                calData[0], calData[1], calData[2], calData[3], calData[4]);
}

// =============================================================================
// Interface
// =============================================================================

static void desenharBotao(const Botao &b) {
  tft.fillRoundRect(b.x, b.y, b.w, b.h, 5, b.cor);
  tft.drawRoundRect(b.x, b.y, b.w, b.h, 5, TFT_WHITE);
  tft.setTextFont(2);
  tft.setTextDatum(MC_DATUM);
  tft.setTextPadding(0);
  tft.setTextColor(TFT_WHITE, b.cor);
  tft.drawString(b.rotulo, b.x + b.w / 2, b.y + b.h / 2);
}

static void limparArea() {
  tft.fillRect(0, inkY0, tft.width(), inkY1 - inkY0 + 1, TFT_BLACK);
  ultX = ultY = -1;
  // marcas de referencia: cruz no centro e cantos, para conferir o alinhamento
  int16_t cx = tft.width() / 2;
  int16_t cy = (inkY0 + inkY1) / 2;
  tft.drawFastHLine(cx - 8, cy, 17, 0x39E7);
  tft.drawFastVLine(cx, cy - 8, 17, 0x39E7);
  tft.drawRect(0, inkY0, tft.width(), inkY1 - inkY0 + 1, 0x2104);
}

static void desenharHud(int16_t x, int16_t y, uint16_t z, bool ativo) {
  char linha[40];
  tft.setTextFont(2);
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(ativo ? TFT_WHITE : 0x8410, COR_BARRA);

  tft.setTextPadding(tft.width() - 8);
  if (ativo) snprintf(linha, sizeof(linha), "X:%3d  Y:%3d  Z:%4u", x, y, z);
  else       snprintf(linha, sizeof(linha), "sem toque");
  tft.drawString(linha, 4, 2);

  tft.setTextColor(TFT_GREENYELLOW, COR_BARRA);
  snprintf(linha, sizeof(linha), "%luHz  %luus  toques:%lu",
           (unsigned long)hz, (unsigned long)usLeitura, (unsigned long)toques);
  tft.drawString(linha, 4, 20);

  tft.setTextPadding(0);
}

static void montarTela() {
  tft.fillScreen(TFT_BLACK);
  tft.fillRect(0, 0, tft.width(), BARRA_H, COR_BARRA);

  const int16_t larg = (tft.width() - 12) / 2;
  btnLimpar   = {4, (int16_t)(tft.height() - BTN_H - 3), larg, BTN_H, "LIMPAR", COR_BTN};
  btnCalibrar = {(int16_t)(8 + larg), (int16_t)(tft.height() - BTN_H - 3), larg, BTN_H, "CALIBRAR", COR_BTN2};
  desenharBotao(btnLimpar);
  desenharBotao(btnCalibrar);

  limparArea();
  desenharHud(0, 0, 0, false);
}

// Rastro: reta grossa (3px) entre a amostra anterior e a atual.
static void desenharTraco(int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint16_t cor) {
  tft.drawLine(x0, y0, x1, y1, cor);
  tft.drawLine(x0 + 1, y0, x1 + 1, y1, cor);
  tft.drawLine(x0, y0 + 1, x1, y1 + 1, cor);
}

// =============================================================================

void setup() {
  Serial.begin(115200);
  unsigned long t0 = millis();
  while (!Serial && millis() - t0 < 2000) delay(10);
  Serial.println("\n== Teste de touch (baixa latencia) - ESP32-S3 ==");

  tft.init();
  tft.setRotation(ROTACAO);
  tft.fillScreen(TFT_BLACK);

  imprimirIdDisplay();

#if MODO_DIAGNOSTICO
  Serial.println("MODO_DIAGNOSTICO ligado: alternando as rotacoes a cada 5s.");
  Serial.println("A moldura branca tem que bater com a borda do vidro.");
  return;
#endif

  inkY0 = BARRA_H;
  inkY1 = tft.height() - BTN_H - 6;

#if USAR_SD
  // Antes do touch: o resultado fica visivel na tela e no Serial sem nada
  // por cima, e uma falha de SD nao fica escondida atras da calibracao.
  testarSD();
#endif

#if CALIBRAR_SEMPRE
  calibrar();
#else
  bool temCal = carregarCalibracao();

  // Dedo encostado no boot = pedido de recalibracao
  bool forcar = temCal && (tft.getTouchRawZ() > Z_TOQUE);
  if (forcar) Serial.println("Toque detectado no boot -> recalibrando.");

  if (!temCal || forcar) {
    calibrar();
  } else {
    Serial.printf("Calibracao carregada da NVS: {%u, %u, %u, %u, %u}\n",
                  calData[0], calData[1], calData[2], calData[3], calData[4]);
  }
#endif

  montarTela();
  Serial.println("Pronto. Comandos pelo Serial: 'l' = limpar, 'c' = recalibrar.");
}

void loop() {
#if MODO_DIAGNOSTICO
  static uint8_t rot = 0;
  testeGeometria(rot);
  Serial.printf("Rotacao %d -> %d x %d\n", rot, tft.width(), tft.height());
  rot = (rot + 1) % 4;
  delay(5000);
  return;
#endif

  static uint32_t tHud = 0, tHz = 0, contaHz = 0;
  static bool sobreBotao = false;
  static uint16_t ultZ = 0;

  // ---- Comandos pelo Serial ----
  if (Serial.available()) {
    char c = Serial.read();
    if (c == 'i' || c == 'I') imprimirIdDisplay();
#if USAR_SD
    if (c == 's' || c == 'S') { listarSD(); }
#endif
    if (c == 'l' || c == 'L') limparArea();
    if (c == 'c' || c == 'C') { calibrar(); montarTela(); }
  }

  uint32_t t0 = micros();
  int16_t x = 0, y = 0;
  uint16_t z = 0;
  bool agora = lerToque(&x, &y, &z);

  if (agora) {
    contaHz++;
    ultZ = z;

    if (!tocando) {
      // ---- Borda de descida: novo toque ----
      tocando = true;
      toques++;
      corAtual = (corAtual + 1) % 6;
      ultX = ultY = -1;
      // Um toque que COMECA em cima de um botao aciona o botao. Arrastar o
      // rastro por cima dele nao aciona nada.
      sobreBotao = btnLimpar.contem(x, y) || btnCalibrar.contem(x, y);
      if (btnLimpar.contem(x, y)) {
        desenharBotao(btnLimpar);
        limparArea();
        toques = 0;
      } else if (btnCalibrar.contem(x, y)) {
        calibrar();
        montarTela();
        toques = 0;
        tocando = false;
        return;
      }
    }

    if (!sobreBotao && y >= inkY0 && y <= inkY1) {
      uint16_t cor = PALETA[corAtual];
      if (ultX >= 0 && abs(x - ultX) < SALTO_MAX && abs(y - ultY) < SALTO_MAX) {
        desenharTraco(ultX, ultY, x, y, cor);   // interpola o movimento
      } else {
        tft.fillCircle(x, y, 2, cor);
      }
      // ponto mais forte marcando exatamente a amostra atual
      tft.drawPixel(x, y, TFT_WHITE);
      ultX = x;
      ultY = y;
    }

    usLeitura = micros() - t0;

    // Serial limitado a 10Hz: imprimir a cada amostra derrubaria a taxa.
    if (millis() - tHud >= 100) {
      Serial.printf("x=%3d y=%3d z=%4u  %luHz\n", x, y, z, (unsigned long)hz);
    }
  } else if (tocando) {
    // ---- Borda de subida: dedo saiu ----
    tocando = false;
    sobreBotao = false;
    ultX = ultY = -1;
  }

  // ---- HUD a 10Hz ----
  uint32_t ms = millis();
  if (ms - tHud >= 100) {
    tHud = ms;
    desenharHud(ultX < 0 ? 0 : ultX, ultY < 0 ? 0 : ultY, ultZ, tocando);
  }
  if (ms - tHz >= 1000) {
    tHz = ms;
    hz = contaHz;
    contaHz = 0;
  }

  // Sem toque, 1ms de folga para a FreeRTOS respirar (latencia irrelevante
  // para o dedo). Durante o toque o laco roda livre, na velocidade do SPI.
  if (!tocando) delay(1);
}
