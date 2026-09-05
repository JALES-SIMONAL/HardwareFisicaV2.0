// IHM local — porte do ST7735 + encoder rotativo (Arduino_GFX bit-bang) para
// display SPI + touch resistivo XPT2046 via TFT_eSPI.
//
// O QUE MUDOU EM RELACAO A BRANCH main, E POR QUE
//
// 1) Biblioteca de display: Arduino_GFX (Arduino_SWSPI, bit-bang por
//    digitalWrite) -> TFT_eSPI (SPI de hardware). Consequencia importante e
//    boa: sumiu toda a disputa de barramento com o cartao SD. Na main, o
//    display bit-bangava os MESMOS pinos que o SD usava em SPI de hardware,
//    entao era preciso reconfigurar pinMode()/SPI.begin() a cada troca de
//    dono, com um comentario enorme explicando corrupcao de cartao. Aqui os
//    dois usam o MESMO periferico de hardware (ver armazenamento.cpp) e se
//    alternam so pelo CS, como o barramento SPI foi feito para funcionar.
//
// 2) Canvas em RAM: a main desenhava num Arduino_Canvas (framebuffer) e so
//    depois dava flush(), porque cada pixel via bit-bang era lentissimo e a
//    tela piscava. Com SPI de hardware o desenho vai direto ao painel; o
//    flush() deixou de existir. Se ainda incomodar o piscado no redesenho de
//    tela cheia, o caminho e subir SPI_FREQUENCY no platformio.ini (a tela
//    inteira sao ~150KB: 123ms a 10MHz, 31ms a 40MHz).
//
// 3) Entrada: encoder -> toque. A traducao esta explicada em detalhe em
//    ihm.hpp; em resumo, o toque num item enfileira a MESMA sequencia de
//    eventos que o encoder geraria (mover N passos + confirmar), entao a
//    maquina de estados nao precisou ser reescrita.
//
// 4) Rodape: na main era um texto de dica ("Gire para ajustar, KEY
//    confirma"). Aqui virou uma barra fixa de quatro botoes tocaveis —
//    Voltar, cima, baixo e OK — presentes em TODAS as telas, sempre na mesma
//    posicao. Isso e o que garante que nenhuma tela fique sem saida: com
//    encoder sempre havia o item "Voltar" na lista, mas telas como grafico e
//    mensagem dependiam de "clicar em qualquer coisa".

#include <Arduino.h>
#include <Adafruit_NeoPixel.h>
#include <Preferences.h>
#include <TFT_eSPI.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "MAIN.HPP"
#include "armazenamento.hpp"
#include "ihm.hpp"
#include "layout.hpp"

namespace ihm {

namespace {

constexpr uint16_t COR_FUNDO = 0x0000;
// Cabeçalho, título e seleção vêm de MAIN.HPP (UI_COR_CABECALHO/
// UI_COR_TEXTO_CABECALHO/UI_COR_SELECIONADO/UI_COR_TEXTO_SELECIONADO) —
// ajustáveis ali sem precisar mexer neste arquivo.
constexpr uint16_t COR_CABECALHO = UI_COR_CABECALHO;
constexpr uint16_t COR_TITULO = UI_COR_TEXTO_CABECALHO;
constexpr uint16_t COR_VALOR = 0xFFE0;
constexpr uint16_t COR_RODAPE = 0xC618;
constexpr uint16_t COR_TEXTO = 0xFFFF;
constexpr uint16_t COR_SELECIONADO = UI_COR_SELECIONADO;
constexpr uint16_t COR_TEXTO_SELECIONADO = UI_COR_TEXTO_SELECIONADO;

// PWM do brilho da tela (TFT_BL). Centraliza canal/frequência/resolução.
constexpr uint8_t BRILHO_PWM_CANAL = 0;
constexpr uint32_t BRILHO_PWM_FREQ_HZ = 5000;
constexpr uint8_t BRILHO_PWM_RESOLUCAO_BITS = 8;
constexpr uint8_t BRILHO_NIVEL_MAXIMO = 30;

// Frequência fixa do bipe do buzzer. O volume (0-30) só liga/desliga o som:
// um buzzer passivo controlado por tone() não tem controle analógico de
// intensidade sem amplificador externo.
constexpr uint16_t BUZZER_FREQUENCIA_HZ = 2000;

// Rotacao do painel. 0 = paisagem 320x240 neste ILI9342 (o painel e deitado
// por natureza — ver o bloco do driver no platformio.ini).
constexpr uint8_t ROTACAO_DISPLAY = 0;

// ---------------------------------------------------------------------
// Toque (XPT2046)
// ---------------------------------------------------------------------
// Limiares de pressao com histerese: precisa passar de Z_TOQUE para o
// toque comecar, e so termina quando cai abaixo de Z_SOLTA. Sem a
// histerese, um dedo parado em cima de um botao gera pressao oscilando em
// torno de um limiar unico e o firmware ve uma rajada de toques repetidos.
constexpr uint16_t Z_TOQUE = 400;
constexpr uint16_t Z_SOLTA = 250;

// Tempo minimo entre dois toques aceitos. Touch resistivo tem repique
// mecanico igual a uma chave: sem isto, um unico toque num item de lista
// as vezes conta duas vezes (e a maquina de estados avanca duas telas).
constexpr uint32_t DEBOUNCE_TOQUE_MS = 180;

// Duracao do bipe de retorno do toque. Curto de proposito: e a unica
// confirmacao de que o toque foi registrado quando o dedo cobre o botao.
constexpr uint16_t BEEP_TOQUE_MS = 15;

// ---------------------------------------------------------------------
// Indicação de conexão/desconexão BLE — máquina de estados por millis(),
// sem delay(): tick() roda em loop apertado (toque, display, Bluetooth) e
// travá-lo por 1.5s deixaria tudo isso sem resposta.
// ---------------------------------------------------------------------
enum class FaseIndicacaoBle : uint8_t {
  Nenhuma,
  ConexaoPisca1On,
  ConexaoPisca1Off,
  ConexaoPisca2On,
  ConexaoPisca2Off,
  DesconexaoOn,
};

FaseIndicacaoBle faseIndicacaoBle = FaseIndicacaoBle::Nenhuma;
unsigned long inicioFaseIndicacaoMs = 0;

// Dois ciclos on/off de 375ms = exatamente os 1.5s pedidos, terminando
// apagado. Bipe curto (não o "beep()" padrão de 60ms — 80ms fica mais
// perceptível junto com o pisca-pisca) a cada acendida.
constexpr uint32_t DURACAO_FASE_PISCA_CONEXAO_MS = 375;
constexpr uint32_t DURACAO_BEEP_CONEXAO_MS = 80;
constexpr uint32_t DURACAO_FASE_PISCA_DESCONEXAO_MS = 400;

struct TelaAppState {
  char titulo[24] = "";
  char valor[24] = "";
  char rodape[24] = "";
  bool inicializada = false;
};

TelaAppState telaApp;
uint8_t volumeAtual = BRILHO_NIVEL_MAXIMO;

bool displayOk = false;
bool toqueOk = false;

TFT_eSPI tft = TFT_eSPI();
Preferences prefsToque;

Adafruit_NeoPixel pixels(NUM_LEDS, PIN_NEO, NEO_GRB + NEO_KHZ800);

// ---------------------------------------------------------------------
// Calibração do touch (persistida na NVS)
// ---------------------------------------------------------------------
uint16_t calData[5] = {0, 0, 0, 0, 0};

// A calibração só vale para a geometria em que foi feita: os valores
// convertem raw -> pixel usando a largura/altura da tela. Guardar a
// assinatura junto faz uma troca de driver/rotação descartar sozinha uma
// calibração que ficaria silenciosamente errada.
uint32_t assinaturaCalibracao() {
  return (static_cast<uint32_t>(TFT_WIDTH) << 20) ^
         (static_cast<uint32_t>(TFT_HEIGHT) << 8) ^ ROTACAO_DISPLAY;
}

// ---------------------------------------------------------------------
// Zonas tocáveis
// ---------------------------------------------------------------------
// Cada desenharX() registra aqui as áreas que respondem ao toque na tela
// que acabou de montar. O registro é zerado no início de cada desenho: uma
// tela nunca herda as zonas da anterior (senão o usuário tocaria numa área
// vazia e ativaria o item que estava ali na tela passada).
enum class AcaoToque : uint8_t {
  Nenhuma,
  ItemLista,   // "indice" = índice do item na lista
  Confirmar,   // botão OK do rodapé
  Voltar,      // botão Voltar do rodapé
  Proximo,     // botão baixo/"+" do rodapé
  Anterior,    // botão cima/"-" do rodapé
};

struct ZonaToque {
  int16_t x = 0, y = 0, w = 0, h = 0;
  AcaoToque acao = AcaoToque::Nenhuma;
  uint8_t indice = 0;
  bool contem(int16_t px, int16_t py) const {
    return px >= x && px < x + w && py >= y && py < y + h;
  }
};

// 40 = o maior alfabeto do teclado de texto cabe, mais os 4 botões do
// rodapé. Estouro é ignorado silenciosamente (a zona simplesmente não
// responde) em vez de corromper memória.
constexpr uint8_t MAX_ZONAS = 48;
ZonaToque zonas[MAX_ZONAS];
uint8_t quantidadeZonas = 0;

// Estado da lista/grade desenhada por último — é o que permite traduzir um
// toque direto num item para a rajada de passos que o encoder geraria.
uint8_t indiceSelecionadoAtual = 0;
uint8_t quantidadeItensAtual = 0;

void limparZonas() {
  quantidadeZonas = 0;
  indiceSelecionadoAtual = 0;
  quantidadeItensAtual = 0;
}

void registrarZona(int16_t x, int16_t y, int16_t w, int16_t h, AcaoToque acao,
                   uint8_t indice = 0) {
  if (quantidadeZonas >= MAX_ZONAS) return;
  // Campo a campo em vez de inicialização por chaves: o projeto compila em
  // gnu++11, e nesse padrão um struct com inicializadores de membro
  // (os "= 0" da declaração) deixa de ser agregado, então ZonaToque{...}
  // não compila. Só a partir de C++14 isso passou a ser permitido.
  ZonaToque& z = zonas[quantidadeZonas];
  z.x = x;
  z.y = y;
  z.w = w;
  z.h = h;
  z.acao = acao;
  z.indice = indice;
  quantidadeZonas++;
}

// ---------------------------------------------------------------------
// Fila de eventos de navegação
// ---------------------------------------------------------------------
// Um toque pode gerar VÁRIOS eventos (mover N itens + confirmar). Eles são
// entregues um por tick e na ordem de entrada — ver a explicação em
// ihm.hpp. 64 cabe o maior salto possível numa lista mais o confirmar.
enum class EventoFila : uint8_t { Proximo, Anterior, Confirmar };

constexpr uint8_t TAM_FILA = 64;
EventoFila fila[TAM_FILA];
uint8_t filaInicio = 0;
uint8_t filaFim = 0;

bool filaVazia() { return filaInicio == filaFim; }

void enfileirar(EventoFila e) {
  const uint8_t proximo = static_cast<uint8_t>((filaFim + 1) % TAM_FILA);
  if (proximo == filaInicio) return;  // cheia: descarta em vez de sobrescrever
  fila[filaFim] = e;
  filaFim = proximo;
}

bool voltarPendente = false;

// Estado do toque em si.
bool tocando = false;
uint32_t ultimoToqueAceitoMs = 0;

// Cópia da zona em que o dedo encostou — cópia, e não o índice dela no
// vetor: entre a descida e a subida do dedo a tela pode ter sido
// redesenhada (a rajada de eventos de um toque anterior ainda sendo
// processada, um dado ao vivo chegando pelo BLE), e nesse caso limparZonas()
// já reconstruiu o vetor. Um índice guardado apontaria para outra zona
// qualquer, e o toque acionaria a coisa errada.
ZonaToque zonaPressionada;
bool temZonaPressionada = false;

// Última coordenada VÁLIDA lida enquanto o dedo estava na tela. Na borda de
// subida não há leitura válida (é justamente a ausência de pressão que
// define a subida), então é esta posição que diz onde o dedo estava quando
// soltou — sem ela não dá para saber se o usuário arrastou para fora do
// botão antes de soltar.
int16_t ultimoXValido = -1;
int16_t ultimoYValido = -1;

// ---------------------------------------------------------------------
// RAII do barramento SPI compartilhado com o cartão SD.
// ---------------------------------------------------------------------
// Muito mais simples que na main: aqui só serializa o acesso (o desenho
// roda no núcleo 1 e a gravação no cartão no núcleo 0). Não há mais
// reconfiguração de pinos nem SPI.begin()/end(), porque display e SD usam o
// MESMO periférico SPI de hardware e se distinguem pelo CS — ver o
// comentário no topo deste arquivo e em armazenamento.cpp.
class TravaBarramentoDisplay {
 public:
  TravaBarramentoDisplay() { armazenamento::travarBarramentoSPI(); }
  ~TravaBarramentoDisplay() { armazenamento::destravarBarramentoSPI(); }
};

bool textoMudou(const char* atual, const char* novoTexto) {
  if (atual == nullptr && novoTexto == nullptr) {
    return false;
  }

  if (atual == nullptr || novoTexto == nullptr) {
    return true;
  }

  return std::strcmp(atual, novoTexto) != 0;
}

// Desenha "texto" em (x,y) com o setTextSize()/setTextColor() já definidos
// pelo chamador. Se UI_FONTE_NEGRITO (MAIN.HPP) estiver ativo, reimprime
// 1px à direita por cima — "negrito" simulado por double-strike, já que a
// fonte embutida da biblioteca de display não tem uma variante bold de
// verdade.
void imprimirTexto(int16_t x, int16_t y, const char* texto) {
  tft.setCursor(x, y);
  tft.print(texto);
  if (UI_FONTE_NEGRITO) {
    tft.setCursor(x + 1, y);
    tft.print(texto);
  }
}

void limparFaixa(int16_t y, int16_t altura) {
  tft.fillRect(0, y, tft.width(), altura, COR_FUNDO);
}

void desenharTextoFaixa(int16_t y, uint8_t tamanho, uint16_t cor,
						 const char* texto) {
  limparFaixa(y, 24);
  tft.setTextSize(tamanho);
  tft.setTextColor(cor);
  imprimirTexto(10, y, texto);
}

// Trunca "origem" em "destino" para caber em "larguraDisponivelPx", usando
// "..." quando necessário. A conta de 6px por caractere por unidade de
// tamanho vale tanto para a fonte GLCD do Adafruit_GFX (usada na main)
// quanto para a fonte 1 do TFT_eSPI — por isso esta função veio inalterada.
void truncarTexto(char* destino, size_t tamanhoDestino, const char* origem,
				   int16_t larguraDisponivelPx, uint8_t tamanhoFonte) {
  const int16_t larguraCaractere = 6 * static_cast<int16_t>(tamanhoFonte);
  size_t maxCaracteres = tamanhoDestino - 1;
  if (larguraCaractere > 0) {
	size_t caberiam = static_cast<size_t>(larguraDisponivelPx / larguraCaractere);
	if (caberiam < maxCaracteres) maxCaracteres = caberiam;
  }
  if (maxCaracteres == 0) maxCaracteres = 1;

  const size_t comprimentoOrigem = std::strlen(origem);
  if (comprimentoOrigem <= maxCaracteres) {
	std::strncpy(destino, origem, tamanhoDestino - 1);
	destino[tamanhoDestino - 1] = '\0';
	return;
  }

  if (maxCaracteres <= 3) {
	std::strncpy(destino, origem, maxCaracteres);
	destino[maxCaracteres] = '\0';
	return;
  }

  std::strncpy(destino, origem, maxCaracteres - 3);
  destino[maxCaracteres - 3] = '\0';
  std::strcat(destino, "...");
}

// BMP grava campos numéricos em little-endian, independente da endianness
// da CPU — não usar memcpy/reinterpret_cast direto no buffer.
uint16_t leEndianCurto(const uint8_t* buffer, size_t offset) {
  return static_cast<uint16_t>(buffer[offset]) | (static_cast<uint16_t>(buffer[offset + 1]) << 8);
}

int32_t leEndianLongo(const uint8_t* buffer, size_t offset) {
  return static_cast<int32_t>(
      static_cast<uint32_t>(buffer[offset]) | (static_cast<uint32_t>(buffer[offset + 1]) << 8) |
      (static_cast<uint32_t>(buffer[offset + 2]) << 16) | (static_cast<uint32_t>(buffer[offset + 3]) << 24));
}

// ---------------------------------------------------------------------
// Barra de botões do rodapé
// ---------------------------------------------------------------------
// Quatro botões de largura igual, sempre nesta ordem e sempre no mesmo
// lugar em todas as telas: Voltar | cima | baixo | OK. Os rótulos do meio
// mudam para "-" e "+" nas telas de valor editável, mas a AÇÃO é a mesma
// (Anterior/Proximo) — a posição nunca muda, para o usuário não ter que
// reprocurar o botão a cada tela.
struct RotulosRodape {
  const char* voltar = "<";
  const char* anterior = "^";
  const char* proximo = "v";
  const char* confirmar = "OK";
};

void desenharBotoesRodape(const RotulosRodape& rotulos, bool mostrarVoltar = true) {
  const int16_t largura = tft.width();
  const int16_t altura = tft.height();
  const int16_t alturaRodape = layout::uiFooterHeight();
  const int16_t y = altura - alturaRodape;
  const uint8_t fonte = layout::uiFontSize(1);

  const int16_t larguraBotao = largura / 4;
  const AcaoToque acoes[4] = {AcaoToque::Voltar, AcaoToque::Anterior, AcaoToque::Proximo,
                              AcaoToque::Confirmar};
  const char* textos[4] = {rotulos.voltar, rotulos.anterior, rotulos.proximo, rotulos.confirmar};

  tft.fillRect(0, y, largura, alturaRodape, COR_FUNDO);

  for (uint8_t i = 0; i < 4; i++) {
    if (i == 0 && !mostrarVoltar) continue;

    const int16_t x = i * larguraBotao;
    // -1 na largura: deixa 1px de fundo entre botões vizinhos, senão a
    // barra vira um bloco só e não dá pra ver onde um termina e o outro
    // começa (importante quando o dedo cobre metade da barra).
    tft.fillRect(x, y, larguraBotao - 1, alturaRodape, UI_COR_BOTAO);

    const int16_t larguraTexto = static_cast<int16_t>(std::strlen(textos[i]) * 6 * fonte);
    tft.setTextSize(fonte);
    tft.setTextColor(UI_COR_TEXTO_BOTAO);
    imprimirTexto(x + (larguraBotao - larguraTexto) / 2, y + (alturaRodape - 8 * fonte) / 2,
                  textos[i]);

    registrarZona(x, y, larguraBotao, alturaRodape, acoes[i]);
  }
}

// Realimentação visual do botão pressionado: repinta só aquele botão na cor
// de pressionado. Sem isso o usuário não tem como saber se o toque pegou —
// o dedo cobre justamente o botão que ele está tocando.
void pintarZonaPressionada(const ZonaToque& z, bool pressionada) {
  if (z.acao == AcaoToque::ItemLista || z.acao == AcaoToque::Nenhuma) return;

  const uint8_t fonte = layout::uiFontSize(1);
  const char* texto = "";
  switch (z.acao) {
    case AcaoToque::Voltar: texto = "<"; break;
    case AcaoToque::Anterior: texto = "^"; break;
    case AcaoToque::Proximo: texto = "v"; break;
    case AcaoToque::Confirmar: texto = "OK"; break;
    default: break;
  }

  tft.fillRect(z.x, z.y, z.w - 1, z.h, pressionada ? UI_COR_BOTAO_PRESSIONADO : UI_COR_BOTAO);
  const int16_t larguraTexto = static_cast<int16_t>(std::strlen(texto) * 6 * fonte);
  tft.setTextSize(fonte);
  tft.setTextColor(pressionada ? UI_COR_TEXTO_SELECIONADO : UI_COR_TEXTO_BOTAO);
  imprimirTexto(z.x + (z.w - larguraTexto) / 2, z.y + (z.h - 8 * fonte) / 2, texto);
}

// Traduz um toque num item de lista para a sequência de eventos que o
// encoder teria gerado: N passos até o item + confirmar. Ver ihm.hpp.
void enfileirarSaltoParaItem(uint8_t destino) {
  if (destino == indiceSelecionadoAtual) {
    enfileirar(EventoFila::Confirmar);
    return;
  }

  if (destino > indiceSelecionadoAtual) {
    for (uint8_t i = indiceSelecionadoAtual; i < destino; i++) enfileirar(EventoFila::Proximo);
  } else {
    for (uint8_t i = destino; i < indiceSelecionadoAtual; i++) enfileirar(EventoFila::Anterior);
  }
  enfileirar(EventoFila::Confirmar);
}

// Leitura crua do XPT2046 com mediana de 3 amostras. Mediana em vez de
// média: filtra o pico isolado (ruído do touch resistivo) sem introduzir o
// atraso que uma média móvel introduziria.
uint16_t mediana3(uint16_t a, uint16_t b, uint16_t c) {
  if (a > b) { const uint16_t t = a; a = b; b = t; }
  if (b > c) { const uint16_t t = b; b = c; c = t; }
  if (a > b) { const uint16_t t = a; a = b; b = t; }
  return b;
}

bool lerToqueBruto(int16_t& x, int16_t& y) {
  const uint16_t limiar = tocando ? Z_SOLTA : Z_TOQUE;
  if (tft.getTouchRawZ() < limiar) return false;

  uint16_t rx[3], ry[3];
  for (uint8_t i = 0; i < 3; i++) tft.getTouchRaw(&rx[i], &ry[i]);

  // Reconfere a pressão depois de amostrar: descarta a borda de soltura,
  // que é onde o XPT2046 devolve coordenada lixo.
  if (tft.getTouchRawZ() < limiar) return false;

  uint16_t px = mediana3(rx[0], rx[1], rx[2]);
  uint16_t py = mediana3(ry[0], ry[1], ry[2]);
  tft.convertRawXY(&px, &py);

  x = static_cast<int16_t>(px);
  y = static_cast<int16_t>(py);
  return true;
}

}  // namespace

void init() {
  Serial.println("[DISPLAY] Inicializacao iniciada");
  Serial.printf("[DISPLAY] TFT_CS: %d | TFT_DC: %d | TFT_RST: %d | TFT_BL: %d\n", TFT_CS, TFT_DC,
                TFT_RST, TFT_BL);
  Serial.printf("[DISPLAY] TFT_SCLK: %d | TFT_MOSI: %d | TFT_MISO: %d | TOUCH_CS: %d\n", TFT_SCLK,
                TFT_MOSI, TFT_MISO, TOUCH_CS);

  pinMode(BUZZER_PIN, OUTPUT);

  // Backlight ligado antes do init do painel: confirma que o circuito do
  // backlight funciona mesmo que o controlador não responda no SPI.
  pinMode(TFT_BL, OUTPUT);
  digitalWrite(TFT_BL, HIGH);
  Serial.printf("[DISPLAY] Backlight configurado (pino %d em HIGH)\n", TFT_BL);

  tft.init();
  tft.setRotation(ROTACAO_DISPLAY);

  // O TFT_eSPI não tem um begin() que retorne sucesso/falha como o
  // Arduino_GFX tinha. Usa-se o registrador de ID do controlador como
  // prova de vida: se ele responde, o barramento (inclusive o MISO) está
  // funcionando de verdade. Uma resposta 00/FF significa painel mudo — o
  // firmware segue rodando sem IHM local, exatamente como na main.
  const uint8_t id1 = tft.readcommand8(0xD3, 1);
  const uint8_t id2 = tft.readcommand8(0xD3, 2);
  const uint8_t id3 = tft.readcommand8(0xD3, 3);
  Serial.printf("[DISPLAY] ID do controlador (0xD3): %02X %02X %02X\n", id1, id2, id3);
  displayOk = !((id2 == 0x00 && id3 == 0x00) || (id2 == 0xFF && id3 == 0xFF));
  Serial.printf("[DISPLAY] Painel considerado: %s\n", displayOk ? "DISPONIVEL" : "INDISPONIVEL");

  if (!displayOk) {
    // Sem while(true)/return: registra a falha e deixa o restante do
    // firmware (toque, LEDs, sensores, Bluetooth, SD) continuar. Todas as
    // funções de desenho abaixo checam displayOk antes de desenhar.
    Serial.println("[ERRO][DISPLAY] Sem resposta do controlador - display marcado como indisponivel");
    Serial.println("[DISPLAY] Demais modulos do firmware continuarao normalmente");
  } else {
    Serial.printf("[DISPLAY] Resolucao: %d x %d (rotacao %d)\n", tft.width(), tft.height(),
                  ROTACAO_DISPLAY);
    tft.fillScreen(COR_FUNDO);
    layout::init(tft.width(), tft.height());
  }

  // ---- Calibração do touch ----
  prefsToque.begin("ihm", true);
  const uint32_t assinaturaSalva = prefsToque.getULong("sigtoque", 0);
  bool temCalibracao = false;
  if (assinaturaSalva == assinaturaCalibracao() &&
      prefsToque.getBytesLength("caltoque") == sizeof(calData)) {
    prefsToque.getBytes("caltoque", calData, sizeof(calData));
    temCalibracao = (calData[1] != 0 && calData[3] != 0);
  }
  prefsToque.end();

  if (temCalibracao) {
    tft.setTouch(calData);
    toqueOk = true;
    Serial.printf("[TOUCH] Calibracao carregada da NVS: {%u, %u, %u, %u, %u}\n", calData[0],
                  calData[1], calData[2], calData[3], calData[4]);
  } else if (displayOk) {
    Serial.println("[TOUCH] Sem calibracao valida para esta geometria - calibrando agora");
    calibrarToque();
  } else {
    Serial.println("[TOUCH] Display indisponivel - calibracao adiada");
  }

  if (FORCE_DISPLAY_BACKLIGHT_DIAGNOSTIC) {
    // NÃO anexa o pino ao LEDC: ledcAttachPin() assume o controle do
    // estágio de saída do GPIO e zera o duty até o primeiro ledcWrite(),
    // o que apagaria o backlight mesmo depois do digitalWrite(HIGH) acima.
    Serial.println("[DISPLAY] Diagnostico: backlight em modo GPIO puro (LEDC nao anexado)");
  } else {
    ledcSetup(BRILHO_PWM_CANAL, BRILHO_PWM_FREQ_HZ, BRILHO_PWM_RESOLUCAO_BITS);
    ledcAttachPin(TFT_BL, BRILHO_PWM_CANAL);
  }
  setBrilho(BRILHO_NIVEL_MAXIMO);

  Serial.printf("[LEDS] Inicializando NeoPixel (GPIO %d, %d LEDs)\n", PIN_NEO, NUM_LEDS);
  pixels.begin();
  pixels.clear();
  pixels.show();

  Serial.println("[DISPLAY] Inicializacao concluida");
}

bool displayDisponivel() { return displayOk; }
bool toqueDisponivel() { return toqueOk; }

void calibrarToque() {
  if (!displayOk) return;
  TravaBarramentoDisplay travaBus;

  tft.fillScreen(COR_FUNDO);
  tft.setTextSize(layout::uiFontSize(1));
  tft.setTextColor(COR_TEXTO);
  imprimirTexto(layout::uiMargin(), layout::uiCenterY() - 20, "Calibracao do toque");
  imprimirTexto(layout::uiMargin(), layout::uiCenterY(), "Toque na seta de cada canto");

  tft.calibrateTouch(calData, COR_TEXTO, COR_FUNDO, 15);
  tft.setTouch(calData);
  toqueOk = true;

  prefsToque.begin("ihm", false);
  prefsToque.putBytes("caltoque", calData, sizeof(calData));
  prefsToque.putULong("sigtoque", assinaturaCalibracao());
  prefsToque.end();

  Serial.printf("[TOUCH] Calibracao salva na NVS: {%u, %u, %u, %u, %u}\n", calData[0], calData[1],
                calData[2], calData[3], calData[4]);

  tft.fillScreen(COR_FUNDO);
}

// ---------------------------------------------------------------------
// Entrada
// ---------------------------------------------------------------------

void atualizarToque() {
  if (!displayOk || !toqueOk) return;

  int16_t x = 0, y = 0;
  bool agora = false;
  {
    // O XPT2046 está no MESMO barramento SPI do display e do cartão, e esta
    // função é chamada do núcleo 1 enquanto a tarefa de armazenamento grava
    // no cartão a partir do núcleo 0. Sem a trava, uma leitura de toque
    // pode cair no meio de uma transferência do SD — e o resultado não é
    // uma coordenada errada, é o dado do cartão corrompido.
    TravaBarramentoDisplay travaBus;
    agora = lerToqueBruto(x, y);
  }
  const uint32_t ms = millis();

  if (agora) {
    ultimoXValido = x;
    ultimoYValido = y;
  }

  if (agora && !tocando) {
    // ---- Borda de descida: dedo encostou ----
    tocando = true;

    // Debounce: ignora um segundo toque logo depois do anterior. O repique
    // do touch resistivo chega a gerar dois toques de um encostar só, o que
    // faria a máquina de estados avançar duas telas de uma vez.
    if (ms - ultimoToqueAceitoMs < DEBOUNCE_TOQUE_MS) return;

    for (uint8_t i = 0; i < quantidadeZonas; i++) {
      if (!zonas[i].contem(x, y)) continue;
      zonaPressionada = zonas[i];
      temZonaPressionada = true;
      beep(BEEP_TOQUE_MS);
      TravaBarramentoDisplay travaBus;
      pintarZonaPressionada(zonaPressionada, true);
      break;
    }
  } else if (!agora && tocando) {
    // ---- Borda de subida: dedo saiu ----
    // A ação acontece aqui, não na descida: assim o usuário pode arrastar o
    // dedo para fora do botão e soltar sem acionar nada, que é o
    // comportamento esperado de qualquer interface de toque.
    tocando = false;

    if (temZonaPressionada) {
      {
        TravaBarramentoDisplay travaBus;
        pintarZonaPressionada(zonaPressionada, false);
      }

      // Só aciona se o dedo estava DENTRO da mesma zona na última leitura
      // válida — é isso que faz o "arrastar para fora para cancelar"
      // funcionar de verdade.
      if (zonaPressionada.contem(ultimoXValido, ultimoYValido)) {
        ultimoToqueAceitoMs = ms;
        switch (zonaPressionada.acao) {
          case AcaoToque::ItemLista:
            enfileirarSaltoParaItem(zonaPressionada.indice);
            break;
          case AcaoToque::Confirmar:
            enfileirar(EventoFila::Confirmar);
            break;
          case AcaoToque::Proximo:
            enfileirar(EventoFila::Proximo);
            break;
          case AcaoToque::Anterior:
            enfileirar(EventoFila::Anterior);
            break;
          case AcaoToque::Voltar:
            voltarPendente = true;
            break;
          case AcaoToque::Nenhuma:
            break;
        }
      }
      temZonaPressionada = false;
    }
  }
}

EventoNavegacao lerEventoNavegacao() {
  if (filaVazia()) return EventoNavegacao::Nenhum;

  // Só retira da fila se o próximo evento for de navegação — Confirmar fica
  // para confirmacaoSolicitada(), preservando a ordem original.
  const EventoFila e = fila[filaInicio];
  if (e == EventoFila::Confirmar) return EventoNavegacao::Nenhum;

  filaInicio = static_cast<uint8_t>((filaInicio + 1) % TAM_FILA);
  return (e == EventoFila::Proximo) ? EventoNavegacao::Proximo : EventoNavegacao::Anterior;
}

bool confirmacaoSolicitada() {
  if (filaVazia()) return false;
  if (fila[filaInicio] != EventoFila::Confirmar) return false;

  filaInicio = static_cast<uint8_t>((filaInicio + 1) % TAM_FILA);
  return true;
}

bool voltarSolicitado() {
  if (!voltarPendente) return false;
  voltarPendente = false;
  return true;
}

// ---------------------------------------------------------------------
// LEDs
// ---------------------------------------------------------------------

void controlarLED(uint16_t indice, uint8_t vermelho, uint8_t verde, uint8_t azul,
                  uint8_t brilho) {
  if (indice >= pixels.numPixels()) {
    Serial.printf("[ERRO][LEDS] Indice invalido: %u, limite: %u\n", static_cast<unsigned>(indice),
                  static_cast<unsigned>(pixels.numPixels()));
    return;
  }

  pixels.setBrightness(brilho);
  pixels.setPixelColor(indice, pixels.Color(vermelho, verde, azul));
  pixels.show();
}

void controlarTodosLeds(uint8_t vermelho, uint8_t verde, uint8_t azul, uint8_t brilho) {
  pixels.setBrightness(brilho);
  for (uint16_t i = 0; i < pixels.numPixels(); i++) {
    pixels.setPixelColor(i, pixels.Color(vermelho, verde, azul));
  }
  pixels.show();  // Uma única chamada, depois de definir todas as cores —
                   // garante que os LEDs acendam/mudem simultaneamente.
}

void iniciarIndicacaoConexao() {
  faseIndicacaoBle = FaseIndicacaoBle::ConexaoPisca1On;
  inicioFaseIndicacaoMs = millis();
  controlarTodosLeds(0, 0, 255, LED_STARTUP_BRIGHTNESS);
  beep(DURACAO_BEEP_CONEXAO_MS);
}

void iniciarIndicacaoDesconexao() {
  faseIndicacaoBle = FaseIndicacaoBle::DesconexaoOn;
  inicioFaseIndicacaoMs = millis();
  controlarTodosLeds(255, 200, 0, LED_STARTUP_BRIGHTNESS);
}

void atualizarIndicacoes() {
  if (faseIndicacaoBle == FaseIndicacaoBle::Nenhuma) return;

  const unsigned long decorrido = millis() - inicioFaseIndicacaoMs;

  switch (faseIndicacaoBle) {
    case FaseIndicacaoBle::ConexaoPisca1On:
      if (decorrido >= DURACAO_FASE_PISCA_CONEXAO_MS) {
        controlarTodosLeds(0, 0, 0, 0);
        faseIndicacaoBle = FaseIndicacaoBle::ConexaoPisca1Off;
        inicioFaseIndicacaoMs = millis();
      }
      break;
    case FaseIndicacaoBle::ConexaoPisca1Off:
      if (decorrido >= DURACAO_FASE_PISCA_CONEXAO_MS) {
        controlarTodosLeds(0, 0, 255, LED_STARTUP_BRIGHTNESS);
        beep(DURACAO_BEEP_CONEXAO_MS);
        faseIndicacaoBle = FaseIndicacaoBle::ConexaoPisca2On;
        inicioFaseIndicacaoMs = millis();
      }
      break;
    case FaseIndicacaoBle::ConexaoPisca2On:
      if (decorrido >= DURACAO_FASE_PISCA_CONEXAO_MS) {
        controlarTodosLeds(0, 0, 0, 0);
        faseIndicacaoBle = FaseIndicacaoBle::ConexaoPisca2Off;
        inicioFaseIndicacaoMs = millis();
      }
      break;
    case FaseIndicacaoBle::ConexaoPisca2Off:
      if (decorrido >= DURACAO_FASE_PISCA_CONEXAO_MS) {
        faseIndicacaoBle = FaseIndicacaoBle::Nenhuma;
      }
      break;
    case FaseIndicacaoBle::DesconexaoOn:
      if (decorrido >= DURACAO_FASE_PISCA_DESCONEXAO_MS) {
        controlarTodosLeds(0, 0, 0, 0);
        faseIndicacaoBle = FaseIndicacaoBle::Nenhuma;
      }
      break;
    case FaseIndicacaoBle::Nenhuma:
      break;
  }
}

// ---------------------------------------------------------------------
// Telas simples
// ---------------------------------------------------------------------

void escreverTelaApp(const char* titulo, const char* valor, const char* rodape,
					 bool forcarRedesenho) {
  if (!displayOk) return;
  TravaBarramentoDisplay travaBus;

  if (forcarRedesenho || !telaApp.inicializada) {
    // Esta tela não tem nada tocável (é a de "modo aplicativo": quem comanda
    // é o app pelo BLE). Ainda assim precisa zerar as zonas: sem isso ela
    // herdaria os alvos da tela anterior e um toque numa área visualmente
    // vazia acionaria o item que por acaso estava ali antes.
    limparZonas();
    tft.fillScreen(COR_FUNDO);
    tft.fillRect(0, 0, tft.width(), layout::uiHeaderHeight(), COR_CABECALHO);
    tft.drawRect(0, 0, tft.width(), tft.height(), COR_CABECALHO);
    telaApp.inicializada = true;
    telaApp.titulo[0] = '\0';
    telaApp.valor[0] = '\0';
    telaApp.rodape[0] = '\0';
  }

  if (titulo != nullptr && (forcarRedesenho || textoMudou(telaApp.titulo, titulo))) {
    std::strncpy(telaApp.titulo, titulo, sizeof(telaApp.titulo) - 1);
    telaApp.titulo[sizeof(telaApp.titulo) - 1] = '\0';

    tft.fillRect(0, 0, tft.width(), layout::uiHeaderHeight(), COR_CABECALHO);
    tft.setTextSize(layout::uiFontSize(1));
    tft.setTextColor(COR_TITULO);
    imprimirTexto(10, 8, telaApp.titulo);
  }

  if (valor != nullptr && (forcarRedesenho || textoMudou(telaApp.valor, valor))) {
    std::strncpy(telaApp.valor, valor, sizeof(telaApp.valor) - 1);
    telaApp.valor[sizeof(telaApp.valor) - 1] = '\0';

    desenharTextoFaixa(layout::uiCenterY() - 20, layout::uiFontSize(2), COR_VALOR, telaApp.valor);
  }

  if (rodape != nullptr && (forcarRedesenho || textoMudou(telaApp.rodape, rodape))) {
    std::strncpy(telaApp.rodape, rodape, sizeof(telaApp.rodape) - 1);
    telaApp.rodape[sizeof(telaApp.rodape) - 1] = '\0';

    desenharTextoFaixa(tft.height() - layout::uiFooterHeight() - 24, layout::uiFontSize(1),
                       COR_RODAPE, telaApp.rodape);
  }
}

void escreverTextoTela(const char* texto, int16_t x, int16_t y, uint16_t cor,
                       uint8_t tamanho, bool limparTela) {
  if (!displayOk) return;
  TravaBarramentoDisplay travaBus;

  if (limparTela) {
    // Mesmo motivo de escreverTelaApp(): apagar a tela sem apagar as zonas
    // deixaria alvos invisíveis ativos por cima do novo conteúdo. Quando
    // limparTela é false esta função é só uma sobreposição de texto na tela
    // que já está montada, e aí as zonas dela devem continuar valendo.
    limparZonas();
    tft.fillScreen(COR_FUNDO);
  }

  tft.setTextColor(cor);
  tft.setTextSize(tamanho);
  imprimirTexto(x, y, texto);
}

// ---------------------------------------------------------------------
// Brilho e som
// ---------------------------------------------------------------------

void setBrilho(uint8_t nivel) {
  if (nivel > BRILHO_NIVEL_MAXIMO) nivel = BRILHO_NIVEL_MAXIMO;
  uint32_t duty = (static_cast<uint32_t>(nivel) * 255U) / BRILHO_NIVEL_MAXIMO;

  if (FORCE_DISPLAY_BACKLIGHT_DIAGNOSTIC) {
    // Ignora o LEDC por completo: o backlight já está em HIGH via
    // digitalWrite() feito em init().
    return;
  }

  if (FORCE_MAX_BRIGHTNESS_FOR_DIAGNOSTIC) {
    duty = 255;
  }

  ledcWrite(BRILHO_PWM_CANAL, duty);
}

void setVolume(uint8_t nivel) {
  if (nivel > BRILHO_NIVEL_MAXIMO) nivel = BRILHO_NIVEL_MAXIMO;
  volumeAtual = nivel;
}

void beep(uint16_t duracaoMs) {
  if (volumeAtual == 0) return;
  tone(BUZZER_PIN, BUZZER_FREQUENCIA_HZ, duracaoMs);
}

// ---------------------------------------------------------------------
// Primitivas gráficas
// ---------------------------------------------------------------------

void desenharCabecalhoRodape(const char* titulo, const char* rodape) {
  if (!displayOk) return;

  const int16_t largura = tft.width();
  const int16_t altura = tft.height();
  const int16_t alturaCabecalho = layout::uiHeaderHeight();
  const int16_t alturaRodape = layout::uiFooterHeight();
  const uint8_t fonte = layout::uiFontSize(1);

  tft.fillRect(0, 0, largura, alturaCabecalho, COR_CABECALHO);

  if (titulo != nullptr) {
    char bufferTitulo[32];
    truncarTexto(bufferTitulo, sizeof(bufferTitulo), titulo,
                 largura - 2 * layout::uiMargin(), fonte);
    // Título do cabeçalho sempre em maiúsculo — destaca "em que tela
    // estou" — centralizado aqui em vez de escrever cada string de
    // título já em maiúsculo em cada tela/chamador. Só ASCII simples
    // (a-z); os títulos do firmware não usam acentos.
    for (char* c = bufferTitulo; *c != '\0'; c++) {
      if (*c >= 'a' && *c <= 'z') *c = static_cast<char>(*c - 'a' + 'A');
    }
    tft.setTextSize(fonte);
    tft.setTextColor(COR_TITULO);
    imprimirTexto(layout::uiMargin(), alturaCabecalho / 2 - 4 * fonte, bufferTitulo);
  }

  // O "rodape" de dica da main virou uma linha de texto LOGO ACIMA da barra
  // de botões (que ocupa o rodapé de verdade agora). Os textos herdados que
  // falavam do encoder foram reescritos nos chamadores.
  if (rodape != nullptr) {
    char bufferRodape[40];
    truncarTexto(bufferRodape, sizeof(bufferRodape), rodape,
                 largura - 2 * layout::uiMargin(), fonte);
    const int16_t y = altura - alturaRodape - 8 * fonte - 2;
    tft.fillRect(0, y, largura, 8 * fonte + 2, COR_FUNDO);
    tft.setTextSize(fonte);
    tft.setTextColor(COR_RODAPE);
    imprimirTexto(layout::uiMargin(), y, bufferRodape);
  }
}

void desenharListaMenu(const char* titulo, const char* const* itens, uint8_t quantidade,
                       uint8_t indiceSelecionado, uint8_t& offsetRolagem) {
  if (!displayOk) return;

  const uint8_t itensVisiveis = layout::uiItensVisiveis();

  // Mantem o item selecionado sempre dentro da janela visivel: rola para
  // cima se ele ficou acima do topo, ou para baixo se ficou depois da
  // ultima linha desenhada.
  if (itensVisiveis > 0) {
    if (indiceSelecionado < offsetRolagem) {
      offsetRolagem = indiceSelecionado;
    } else if (indiceSelecionado >= offsetRolagem + itensVisiveis) {
      offsetRolagem = indiceSelecionado - itensVisiveis + 1;
    }
  }

  TravaBarramentoDisplay travaBus;
  limparZonas();
  indiceSelecionadoAtual = indiceSelecionado;
  quantidadeItensAtual = quantidade;

  tft.fillScreen(COR_FUNDO);
  desenharCabecalhoRodape(titulo);

  const int16_t yInicial = layout::uiHeaderHeight() + layout::uiMargin();
  const int16_t alturaLinha = layout::uiLineSpacing();
  const uint8_t fonte = layout::uiFontSize(1);

  for (uint8_t linha = 0; linha < itensVisiveis; linha++) {
    const uint8_t indiceItem = offsetRolagem + linha;
    if (indiceItem >= quantidade) break;

    const int16_t y = yInicial + linha * alturaLinha;
    const bool selecionado = (indiceItem == indiceSelecionado);

    if (selecionado) {
      tft.fillRect(0, y - 1, tft.width(), alturaLinha, COR_SELECIONADO);
    }

    char buffer[40];
    truncarTexto(buffer, sizeof(buffer), itens[indiceItem],
                 tft.width() - 2 * layout::uiMargin(), fonte);

    tft.setTextSize(fonte);
    tft.setTextColor(selecionado ? COR_TEXTO_SELECIONADO : COR_TEXTO);
    // Centraliza o texto na altura da linha: a linha agora é bem mais alta
    // que o texto (piso de toque, UI_ALTURA_MINIMA_ALVO_TOQUE), então
    // escrever no topo dela deixaria o texto "colado" na linha de cima.
    imprimirTexto(layout::uiMargin(), y + (alturaLinha - 8 * fonte) / 2, buffer);

    registrarZona(0, y - 1, tft.width(), alturaLinha, AcaoToque::ItemLista, indiceItem);
  }

  desenharBotoesRodape(RotulosRodape{});
}

void desenharConfirmacao(const char* pergunta, uint8_t indiceSelecionado) {
  if (!displayOk) return;
  TravaBarramentoDisplay travaBus;
  limparZonas();
  indiceSelecionadoAtual = indiceSelecionado;
  quantidadeItensAtual = 2;

  tft.fillScreen(COR_FUNDO);
  desenharCabecalhoRodape("Confirmar", "Toque na opcao desejada");

  const uint8_t fonte = layout::uiFontSize(1);
  const int16_t yPergunta = layout::uiHeaderHeight() + layout::uiMargin();

  char bufferPergunta[48];
  truncarTexto(bufferPergunta, sizeof(bufferPergunta), pergunta,
               tft.width() - 2 * layout::uiMargin(), fonte);
  tft.setTextSize(fonte);
  tft.setTextColor(COR_VALOR);
  imprimirTexto(layout::uiMargin(), yPergunta, bufferPergunta);

  static const char* const opcoes[2] = {"Sim", "Nao"};
  const int16_t alturaLinha = layout::uiLineSpacing();
  const int16_t yOpcoes = yPergunta + alturaLinha * 2;
  for (uint8_t i = 0; i < 2; i++) {
    const bool selecionado = (i == indiceSelecionado);
    const int16_t y = yOpcoes + i * alturaLinha;
    if (selecionado) {
      tft.fillRect(0, y - 1, tft.width(), alturaLinha, COR_SELECIONADO);
    }
    tft.setTextSize(fonte);
    tft.setTextColor(selecionado ? COR_TEXTO_SELECIONADO : COR_TEXTO);
    imprimirTexto(layout::uiMargin(), y + (alturaLinha - 8 * fonte) / 2, opcoes[i]);
    registrarZona(0, y - 1, tft.width(), alturaLinha, AcaoToque::ItemLista, i);
  }

  desenharBotoesRodape(RotulosRodape{});
}

void desenharValorEditavel(const char* titulo, int32_t valor, int32_t minimo,
                           int32_t maximo, const char* unidade) {
  if (!displayOk) return;
  TravaBarramentoDisplay travaBus;
  limparZonas();

  tft.fillScreen(COR_FUNDO);
  desenharCabecalhoRodape(titulo, "- e + ajustam, OK confirma");

  const uint8_t fonteValor = layout::uiFontSize(3);
  char textoValor[16];
  if (unidade != nullptr) {
    snprintf(textoValor, sizeof(textoValor), "%ld%s", static_cast<long>(valor), unidade);
  } else {
    snprintf(textoValor, sizeof(textoValor), "%ld", static_cast<long>(valor));
  }

  const int16_t larguraTexto = static_cast<int16_t>(std::strlen(textoValor) * 6 * fonteValor);
  tft.setTextSize(fonteValor);
  tft.setTextColor(COR_VALOR);
  imprimirTexto(layout::uiCenterX() - larguraTexto / 2, layout::uiCenterY() - 8 * fonteValor / 2,
                textoValor);

  const int16_t barraX = layout::uiMargin();
  const int16_t barraY = tft.height() - layout::uiFooterHeight() - layout::uiHeight(20);
  const int16_t barraLargura = tft.width() - 2 * layout::uiMargin();
  const int16_t barraAltura = layout::uiHeight(8);

  tft.drawRect(barraX, barraY, barraLargura, barraAltura, COR_RODAPE);
  const int32_t faixa = maximo - minimo;
  int16_t preenchido = 0;
  if (faixa > 0 && barraLargura > 2) {
    preenchido = static_cast<int16_t>((static_cast<int64_t>(valor - minimo) * (barraLargura - 2)) / faixa);
  }
  if (preenchido > 0) {
    tft.fillRect(barraX + 1, barraY + 1, preenchido, barraAltura - 2, COR_CABECALHO);
  }

  // Mesmas posições e mesmas ações dos outros ecrãs; só os rótulos do meio
  // mudam, porque aqui "anterior/próximo" significa "diminui/aumenta".
  RotulosRodape rotulos;
  rotulos.anterior = "-";
  rotulos.proximo = "+";
  desenharBotoesRodape(rotulos);
}

void desenharListaRolavel(const char* titulo, const char* const* linhas,
                          uint8_t quantidade, uint8_t offsetRolagem) {
  if (!displayOk) return;
  TravaBarramentoDisplay travaBus;
  limparZonas();

  tft.fillScreen(COR_FUNDO);
  desenharCabecalhoRodape(titulo, "Use ^ e v para rolar");

  // Uma linha a menos que o layout permite: a dica acima da barra de botões
  // ocupa espaço que uiItensVisiveis() não conhece, e sem esta reserva a
  // última linha da lista seria escrita por cima dela.
  const uint8_t itensCabem = layout::uiItensVisiveis();
  const uint8_t itensVisiveis = (itensCabem > 1) ? static_cast<uint8_t>(itensCabem - 1) : 1;
  const int16_t yInicial = layout::uiHeaderHeight() + layout::uiMargin();
  const int16_t alturaLinha = layout::uiLineSpacing();
  const uint8_t fonte = layout::uiFontSize(1);

  tft.setTextSize(fonte);
  tft.setTextColor(COR_TEXTO);

  for (uint8_t linha = 0; linha < itensVisiveis; linha++) {
    const uint8_t indice = offsetRolagem + linha;
    if (indice >= quantidade) break;

    char buffer[40];
    truncarTexto(buffer, sizeof(buffer), linhas[indice],
                 tft.width() - 2 * layout::uiMargin(), fonte);
    imprimirTexto(layout::uiMargin(), yInicial + linha * alturaLinha, buffer);
  }

  desenharBotoesRodape(RotulosRodape{});
}

void desenharGradeModulos(const char* titulo, uint8_t dimensao,
                          bool (*modulo)(uint8_t, uint8_t)) {
  if (!displayOk) return;
  TravaBarramentoDisplay travaBus;
  limparZonas();

  tft.fillScreen(COR_FUNDO);
  desenharCabecalhoRodape(titulo);

  if (dimensao == 0 || modulo == nullptr) {
    desenharBotoesRodape(RotulosRodape{});
    return;
  }

  const int16_t areaLargura = tft.width();
  const int16_t areaAltura = tft.height() - layout::uiHeaderHeight() - layout::uiFooterHeight();
  const int16_t ladoDisponivel = (areaLargura < areaAltura) ? areaLargura : areaAltura;

  const int16_t tamanhoCelula = ladoDisponivel / dimensao;
  if (tamanhoCelula <= 0) {
    desenharBotoesRodape(RotulosRodape{});
    return;
  }

  const int16_t ladoGrade = tamanhoCelula * dimensao;
  const int16_t offsetX = (areaLargura - ladoGrade) / 2;
  const int16_t offsetY = layout::uiHeaderHeight() + (areaAltura - ladoGrade) / 2;

  tft.fillRect(offsetX, offsetY, ladoGrade, ladoGrade, 0xFFFF);
  for (uint8_t y = 0; y < dimensao; y++) {
    for (uint8_t x = 0; x < dimensao; x++) {
      if (modulo(x, y)) {
        tft.fillRect(offsetX + x * tamanhoCelula, offsetY + y * tamanhoCelula, tamanhoCelula,
                     tamanhoCelula, 0x0000);
      }
    }
  }

  desenharBotoesRodape(RotulosRodape{});
}

void desenharTecladoTexto(const char* rotuloCampo, const char* valorAtual,
                          const char* const* rotulos, uint8_t quantidade,
                          uint8_t indiceSelecionado) {
  if (!displayOk) return;
  TravaBarramentoDisplay travaBus;
  limparZonas();
  indiceSelecionadoAtual = indiceSelecionado;
  quantidadeItensAtual = quantidade;

  tft.fillScreen(COR_FUNDO);

  char titulo[48];
  snprintf(titulo, sizeof(titulo), "%s: %s", rotuloCampo != nullptr ? rotuloCampo : "",
           valorAtual != nullptr ? valorAtual : "");
  desenharCabecalhoRodape(titulo);

  if (quantidade == 0 || rotulos == nullptr) {
    desenharBotoesRodape(RotulosRodape{});
    return;
  }

  const uint8_t fonte = layout::uiFontSize(1);
  // Cada tecla precisa ser um alvo de dedo, não só um retângulo legível:
  // largura e altura têm o mesmo piso de toque das linhas de lista. Com
  // encoder, células de ~16px bastavam (a seleção vinha do giro).
  const int16_t larguraCelula = UI_ALTURA_MINIMA_ALVO_TOQUE;
  const int16_t alturaCelula = UI_ALTURA_MINIMA_ALVO_TOQUE;

  const int16_t areaLargura = tft.width() - 2 * layout::uiMargin();
  uint8_t colunas = static_cast<uint8_t>(areaLargura / larguraCelula);
  if (colunas < 1) colunas = 1;
  if (colunas > quantidade) colunas = quantidade;

  const int16_t xInicial = layout::uiMargin();
  const int16_t yInicial = layout::uiHeaderHeight() + layout::uiMargin();
  const int16_t yLimite = tft.height() - layout::uiFooterHeight();

  tft.setTextSize(fonte);

  for (uint8_t i = 0; i < quantidade; i++) {
    const uint8_t linha = i / colunas;
    const uint8_t coluna = i % colunas;
    const int16_t x = xInicial + coluna * larguraCelula;
    const int16_t y = yInicial + linha * alturaCelula;

    // Grade grande demais pra área disponível: corta os últimos símbolos
    // em vez de invadir o rodapé.
    if (y + alturaCelula > yLimite) break;

    const bool selecionado = (i == indiceSelecionado);
    if (selecionado) {
      tft.fillRect(x, y, larguraCelula - 1, alturaCelula - 1, COR_SELECIONADO);
    } else {
      tft.drawRect(x, y, larguraCelula - 1, alturaCelula - 1, UI_COR_BOTAO);
    }
    tft.setTextColor(selecionado ? COR_TEXTO_SELECIONADO : COR_TEXTO);
    const int16_t larguraTexto = static_cast<int16_t>(std::strlen(rotulos[i]) * 6 * fonte);
    imprimirTexto(x + (larguraCelula - larguraTexto) / 2, y + (alturaCelula - 8 * fonte) / 2,
                  rotulos[i]);

    registrarZona(x, y, larguraCelula, alturaCelula, AcaoToque::ItemLista, i);
  }

  desenharBotoesRodape(RotulosRodape{});
}

void desenharMensagem(const char* titulo, const char* mensagem) {
  if (!displayOk) return;
  TravaBarramentoDisplay travaBus;
  limparZonas();

  tft.fillScreen(COR_FUNDO);
  desenharCabecalhoRodape(titulo);

  const uint8_t fonte = layout::uiFontSize(1);
  char buffer[80];
  truncarTexto(buffer, sizeof(buffer), mensagem, tft.width() - 2 * layout::uiMargin(), fonte);

  tft.setTextSize(fonte);
  tft.setTextColor(COR_VALOR);
  imprimirTexto(layout::uiMargin(), layout::uiCenterY(), buffer);

  desenharBotoesRodape(RotulosRodape{});
}

void desenharGrafico(const char* titulo, const float* temposS, const float* valoresY, uint8_t quantidade) {
  if (!displayOk) return;
  TravaBarramentoDisplay travaBus;
  limparZonas();

  tft.fillScreen(COR_FUNDO);
  desenharCabecalhoRodape(titulo);

  const uint8_t fonte = layout::uiFontSize(1);

  if (quantidade == 0 || temposS == nullptr || valoresY == nullptr) {
    tft.setTextSize(fonte);
    tft.setTextColor(COR_TEXTO);
    imprimirTexto(layout::uiMargin(), layout::uiCenterY(), "Sem dados suficientes");
    desenharBotoesRodape(RotulosRodape{});
    return;
  }

  float minY = valoresY[0];
  float maxY = valoresY[0];
  float minT = temposS[0];
  float maxT = temposS[0];
  for (uint8_t i = 1; i < quantidade; i++) {
    if (valoresY[i] < minY) minY = valoresY[i];
    if (valoresY[i] > maxY) maxY = valoresY[i];
    if (temposS[i] < minT) minT = temposS[i];
    if (temposS[i] > maxT) maxT = temposS[i];
  }
  // Evita divisao por zero quando todos os pontos tem o mesmo valor/tempo
  // (ex.: um unico ponto) — nesse caso a serie fica desenhada como uma
  // linha reta no meio da area do grafico.
  const float faixaY = (maxY > minY) ? (maxY - minY) : 1.0f;
  const float faixaT = (maxT > minT) ? (maxT - minT) : 1.0f;

  const int16_t plotX0 = layout::uiMargin();
  const int16_t plotX1 = tft.width() - layout::uiMargin();
  const int16_t plotY0 = layout::uiHeaderHeight() + layout::uiMargin();
  const int16_t plotY1 = tft.height() - layout::uiFooterHeight() - layout::uiMargin();
  const int16_t plotLargura = plotX1 - plotX0;
  const int16_t plotAltura = plotY1 - plotY0;
  if (plotLargura <= 1 || plotAltura <= 1) {
    desenharBotoesRodape(RotulosRodape{});
    return;
  }

  // Linha de referencia em y=0 — só desenhada quando o zero cai dentro da
  // faixa observada (útil pra ver troca de sinal, ex.: aceleração negativa).
  if (minY < 0.0f && maxY > 0.0f) {
    const int16_t yZero = plotY1 - static_cast<int16_t>((0.0f - minY) / faixaY * (plotAltura - 1));
    tft.drawFastHLine(plotX0, yZero, plotLargura, COR_RODAPE);
  }

  int16_t xAnterior = 0;
  int16_t yAnterior = 0;
  for (uint8_t i = 0; i < quantidade; i++) {
    const int16_t x = plotX0 + static_cast<int16_t>((temposS[i] - minT) / faixaT * (plotLargura - 1));
    const int16_t y = plotY1 - static_cast<int16_t>((valoresY[i] - minY) / faixaY * (plotAltura - 1));
    if (i > 0) {
      tft.drawLine(xAnterior, yAnterior, x, y, COR_VALOR);
    }
    tft.fillRect(x - 1, y - 1, 3, 3, COR_VALOR);
    xAnterior = x;
    yAnterior = y;
  }

  // Valores minimo/maximo do eixo Y, nos cantos superior/inferior esquerdos
  // da area do grafico (unica indicacao numerica da escala vertical — o
  // eixo X so precisa caber a serie inteira, sem rotulo numerico).
  char bufMax[12];
  char bufMin[12];
  snprintf(bufMax, sizeof(bufMax), "%.2f", static_cast<double>(maxY));
  snprintf(bufMin, sizeof(bufMin), "%.2f", static_cast<double>(minY));
  tft.setTextSize(fonte);
  tft.setTextColor(COR_RODAPE);
  imprimirTexto(plotX0 + 1, plotY0, bufMax);
  imprimirTexto(plotX0 + 1, plotY1 - layout::uiLineSpacing(), bufMin);

  desenharBotoesRodape(RotulosRodape{});
}

bool desenharImagemBMP(const char* nomeComExtensao, int16_t x, int16_t y, int16_t larguraMaxima,
                       int16_t alturaMaxima) {
  if (!displayOk) return false;
  if (larguraMaxima <= 0 || alturaMaxima <= 0) return false;

  if (!armazenamento::abrirBinarioParaLeitura(nomeComExtensao)) {
    Serial.printf("[IHM] BMP nao encontrado ou SD indisponivel: %s\n", nomeComExtensao);
    return false;
  }

  uint8_t cabecalho[54];
  if (armazenamento::lerBinario(cabecalho, sizeof(cabecalho)) != sizeof(cabecalho) ||
      cabecalho[0] != 'B' || cabecalho[1] != 'M') {
    Serial.printf("[IHM] BMP invalido ou cabecalho incompleto: %s\n", nomeComExtensao);
    armazenamento::fecharBinario();
    return false;
  }

  const uint32_t offsetDados = static_cast<uint32_t>(leEndianLongo(cabecalho, 10));
  const int32_t larguraOrigem = leEndianLongo(cabecalho, 18);
  const int32_t alturaBrutaOrigem = leEndianLongo(cabecalho, 22);
  const uint16_t bpp = leEndianCurto(cabecalho, 28);
  const uint32_t compressao = static_cast<uint32_t>(leEndianLongo(cabecalho, 30));

  const bool origemTopoParaBase = (alturaBrutaOrigem < 0);
  const int32_t alturaOrigem = origemTopoParaBase ? -alturaBrutaOrigem : alturaBrutaOrigem;

  // Só BI_RGB (sem compressão) de 24 ou 32 bits — cobre o caso comum de
  // exportação simples; qualquer outro formato cai no retrocesso do
  // chamador (texto), sem tentar decodificar.
  if (compressao != 0 || (bpp != 24 && bpp != 32) || larguraOrigem <= 0 || alturaOrigem <= 0) {
    Serial.printf("[IHM] BMP formato nao suportado (%s): bpp=%u compressao=%u\n", nomeComExtensao,
                  static_cast<unsigned>(bpp), static_cast<unsigned>(compressao));
    armazenamento::fecharBinario();
    return false;
  }

  const uint32_t bytesPorPixel = bpp / 8;
  const uint32_t passoLinha = ((static_cast<uint32_t>(larguraOrigem) * bytesPorPixel + 3) / 4) * 4;

  // Limite de sanidade: evita alocar um buffer de linha desproporcional
  // (ex.: um BMP de altíssima resolução enviado por engano).
  constexpr uint32_t LIMITE_BYTES_LINHA = 20000;
  if (passoLinha > LIMITE_BYTES_LINHA) {
    Serial.printf("[IHM] BMP linha grande demais (%s): %u bytes (limite %u)\n", nomeComExtensao,
                  static_cast<unsigned>(passoLinha), static_cast<unsigned>(LIMITE_BYTES_LINHA));
    armazenamento::fecharBinario();
    return false;
  }

  // Escala uniforme (preserva proporção), só reduz — nunca amplia além do
  // tamanho original da imagem.
  float escala = 1.0f;
  if (larguraOrigem > larguraMaxima) escala = static_cast<float>(larguraOrigem) / larguraMaxima;
  if ((alturaOrigem / escala) > alturaMaxima) escala = static_cast<float>(alturaOrigem) / alturaMaxima;

  const int16_t larguraSaida = static_cast<int16_t>(larguraOrigem / escala);
  const int16_t alturaSaida = static_cast<int16_t>(alturaOrigem / escala);
  const int16_t xCentralizado = x + (larguraMaxima - larguraSaida) / 2;
  const int16_t yCentralizado = y + (alturaMaxima - alturaSaida) / 2;

  uint8_t* linhaOrigem = static_cast<uint8_t*>(malloc(passoLinha));
  // Buffer da imagem de SAÍDA inteira (não só uma linha): lê-se tudo do SD
  // primeiro e só depois desenha de uma vez. Isso vinha da main, onde
  // alternar dono do barramento por linha corrompia o cartão. Aqui o
  // barramento não troca mais de dono, mas o buffer completo continua
  // valendo a pena: uma única transferência para o painel em vez de uma
  // por linha.
  uint16_t* framebuffer = static_cast<uint16_t*>(
      malloc(static_cast<size_t>(larguraSaida) * static_cast<size_t>(alturaSaida) * sizeof(uint16_t)));
  if (linhaOrigem == nullptr || framebuffer == nullptr) {
    Serial.println("[IHM] BMP: sem memoria para buffer de imagem");
    free(linhaOrigem);
    free(framebuffer);
    armazenamento::fecharBinario();
    return false;
  }

  Serial.printf("[IHM] Lendo BMP %s do SD (%ldx%ld -> %dx%d)\n", nomeComExtensao,
                static_cast<long>(larguraOrigem), static_cast<long>(alturaOrigem), larguraSaida, alturaSaida);

  bool leituraCompleta = true;
  for (int16_t linhaSaidaIdx = 0; linhaSaidaIdx < alturaSaida; linhaSaidaIdx++) {
    const int32_t linhaOrigemIdx = static_cast<int32_t>(linhaSaidaIdx * escala);
    // BMP padrão é bottom-up: a primeira linha do arquivo é a ÚLTIMA linha
    // (mais embaixo) da imagem. BITMAPINFOHEADER com altura negativa é
    // top-down (já na ordem de exibição).
    const int32_t linhaArquivo =
        origemTopoParaBase ? linhaOrigemIdx : (alturaOrigem - 1 - linhaOrigemIdx);
    const uint32_t offsetLinha = offsetDados + static_cast<uint32_t>(linhaArquivo) * passoLinha;

    if (!armazenamento::posicionarBinario(offsetLinha) ||
        armazenamento::lerBinario(linhaOrigem, passoLinha) != passoLinha) {
      Serial.println("[IHM] BMP: falha de leitura no meio do arquivo, interrompendo");
      leituraCompleta = false;
      break;
    }

    uint16_t* linhaSaidaBuffer = framebuffer + static_cast<size_t>(linhaSaidaIdx) * larguraSaida;
    for (int16_t colunaSaidaIdx = 0; colunaSaidaIdx < larguraSaida; colunaSaidaIdx++) {
      const int32_t colunaOrigemIdx = static_cast<int32_t>(colunaSaidaIdx * escala);
      const uint8_t* pixel = linhaOrigem + static_cast<uint32_t>(colunaOrigemIdx) * bytesPorPixel;
      // BMP grava BGR(A); RGB565 = RRRRR GGGGGG BBBBB.
      const uint8_t azul = pixel[0];
      const uint8_t verde = pixel[1];
      const uint8_t vermelho = pixel[2];
      linhaSaidaBuffer[colunaSaidaIdx] = static_cast<uint16_t>(((vermelho & 0xF8) << 8) |
                                                                ((verde & 0xFC) << 3) | (azul >> 3));
    }
  }

  free(linhaOrigem);
  armazenamento::fecharBinario();

  if (leituraCompleta) {
    TravaBarramentoDisplay travaBus;
    // Limpa a área de destino ANTES de desenhar: a imagem é centralizada
    // sem nunca ampliar (preserva proporção), então uma imagem com
    // proporção diferente da anterior pode não cobrir toda a área,
    // deixando sobras da imagem/tela anterior visíveis nas bordas.
    tft.fillRect(x, y, larguraMaxima, alturaMaxima, COR_FUNDO);
    tft.pushImage(xCentralizado, yCentralizado, larguraSaida, alturaSaida, framebuffer);
  }

  free(framebuffer);
  return leituraCompleta;
}

}  // namespace ihm
