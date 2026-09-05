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
//
// O CANAL NÃO PODE SER 0. O tone() do Arduino-ESP32 usa o canal LEDC 0 por
// padrão (Tone.cpp: "static uint8_t _channel = 0") e, a cada nota, chama
// ledcAttachPin(pinoDoBuzzer, 0). Isso REATRIBUI o canal 0 ao pino do
// buzzer — e o pino do backlight, que estava preso nesse mesmo canal, fica
// sem sinal: a tela apaga no primeiro beep e não volta mais.
//
// Na versão anterior (branch main) as duas coisas também estavam no canal
// 0, mas o bug ficava escondido porque lá FORCE_DISPLAY_BACKLIGHT_DIAGNOSTIC
// era true e o backlight nunca chegava a ser anexado ao LEDC — ficava só no
// digitalWrite(HIGH). Ao desligar esse diagnóstico neste porte, o conflito
// que já existia apareceu.
constexpr uint8_t BRILHO_PWM_CANAL = 2;
constexpr uint32_t BRILHO_PWM_FREQ_HZ = 5000;
constexpr uint8_t BRILHO_PWM_RESOLUCAO_BITS = 8;
constexpr uint8_t BRILHO_NIVEL_MAXIMO = 30;

// Canal LEDC reservado ao tone(). Definido explicitamente em init() com
// setToneChannel(), em vez de confiar no padrão: assim o canal do buzzer
// fica declarado ao lado do canal do backlight, e um não invade o outro por
// omissão.
constexpr uint8_t BUZZER_PWM_CANAL = 4;

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
  ZoomMais,    // só na tela de gráfico — tratado dentro da ihm
  ZoomMenos,   // idem
};

struct ZonaToque {
  int16_t x = 0, y = 0, w = 0, h = 0;
  AcaoToque acao = AcaoToque::Nenhuma;
  uint8_t indice = 0;
  // true = um toque só já confirma, sem precisar do segundo. Usado no
  // teclado de texto: ali cada célula é um caractere, o alvo é grande, e a
  // consequência de errar é apagar uma letra — não faz sentido cobrar dois
  // toques por caractere digitado. Nas listas de menu continua false,
  // porque lá o item errado pode ser "Excluir tudo".
  bool confirmaDireto = false;
  // Rótulo do botão, para repintá-lo no estado pressionado sem ter que
  // deduzir o texto a partir da ação (o mesmo botão físico é "^" numa tela
  // e "-" em outra).
  const char* rotulo = "";
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

// ---------------------------------------------------------------------
// Estado do gráfico (zoom e deslocamento)
// ---------------------------------------------------------------------
// Os pontos são COPIADOS para cá, não referenciados: o gesto redesenha o
// gráfico fora do ciclo normal da máquina de estados, e a essa altura os
// vetores originais (de analise_linear/analise_circular) podem já não estar
// mais válidos. 255 é o teto de "quantidade", que é uint8_t.
constexpr uint16_t MAX_PONTOS_GRAFICO = 255;
float graficoTempos[MAX_PONTOS_GRAFICO];
float graficoValores[MAX_PONTOS_GRAFICO];
uint8_t graficoQuantidade = 0;
char graficoTitulo[48] = "";
bool graficoAtivo = false;
// 1.0 = série inteira na tela. centro é a posição (0..1) do meio da janela
// visível dentro da faixa total de tempo.
float graficoZoom = 1.0f;
float graficoCentro = 0.5f;
constexpr float GRAFICO_ZOOM_MAX = 16.0f;


void limparZonas() {
  quantidadeZonas = 0;
  indiceSelecionadoAtual = 0;
  quantidadeItensAtual = 0;
  // Sair da tela de grafico desarma os gestos dele (zoom/arrasto lateral).
  // desenharGrafico() liga isto de novo logo depois de chamar limparZonas().
  graficoAtivo = false;
}

void registrarZona(int16_t x, int16_t y, int16_t w, int16_t h, AcaoToque acao,
                   uint8_t indice = 0, bool confirmaDireto = false,
                   const char* rotulo = "") {
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
  z.confirmaDireto = confirmaDireto;
  z.rotulo = rotulo;
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
// Gestos
// ---------------------------------------------------------------------
// O XPT2046 é resistivo e de PONTO ÚNICO: ele mede uma posição por vez, e
// com dois dedos na tela devolve um ponto no meio dos dois. Por isso não há
// (nem pode haver) pinça para ampliar — o gesto de dois dedos é
// fisicamente indetectável neste hardware. O zoom do gráfico é feito pelos
// botões - / + do rodapé, e o arrasto de um dedo faz o deslocamento
// lateral, que é a parte que dá para fazer com um ponto só.
//
// Gestos implementados:
//   - arrastar na vertical  -> rola a lista (move a seleção)
//   - arrastar na horizontal na tela de gráfico -> desloca o gráfico
//   - deslizar para a direita -> voltar
//   - toque simples -> ver enfileirarSaltoParaItem()

// Deslocamento a partir do qual o toque deixa de ser "toque" e vira gesto.
// Abaixo disso é só o tremor natural do dedo em cima do alvo.
constexpr int16_t LIMIAR_GESTO_PX = 14;

// Deslize horizontal mínimo para contar como "voltar". Exige também ser
// bem mais horizontal que vertical, senão uma rolagem meio torta viraria
// um voltar acidental — que é justamente o gesto mais irritante de
// disparar sem querer.
constexpr int16_t LIMIAR_DESLIZE_VOLTAR_PX = 70;

int16_t xInicialToque = 0;
int16_t yInicialToque = 0;
int16_t yReferenciaArrasto = 0;
int16_t xReferenciaArrasto = 0;
// true quando o dedo já passou de LIMIAR_GESTO_PX: a soltura deixa de
// acionar a zona onde encostou (senão rolar a lista também selecionaria o
// item de onde a rolagem partiu).
bool virouGesto = false;


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

// ---------------------------------------------------------------------
// Redesenho em PASSAGEM ÚNICA (o que eliminou o "piscado" entre telas)
// ---------------------------------------------------------------------
// Antes, cada tela começava com fillScreen(preto) e só então desenhava por
// cima. Isso escreve os MESMOS pixels duas vezes: primeiro tudo preto,
// depois o conteúdo. A 10MHz a tela cheia leva ~123ms, então o usuário via
// literalmente a tela apagar e reaparecer — daí a sensação de piscar, que
// não era flicker de refresh e sim o preto intermediário sendo exibido.
//
// Agora nenhuma tela apaga nada antes: cada elemento é desenhado já com o
// seu próprio fundo, por cima do que estava ali, e no fim apaga-se apenas a
// SOBRA — a faixa que o conteúdo novo não cobriu (uma lista mais curta que
// a anterior, por exemplo). Cada pixel é escrito uma vez só, na cor final:
// metade do tempo e sem estado intermediário visível.
//
// A consequência para quem escreve tela nova: todo elemento precisa pintar
// o próprio fundo. Um texto desenhado sem fundo opaco vai aparecer por cima
// do conteúdo anterior, não sobre preto.

int16_t yTopoRodape() { return tft.height() - layout::uiFooterHeight(); }

// Onde a área de conteúdo termina: acima da linha de dica, quando a tela
// tem uma.
int16_t yFimConteudo(bool comDica) {
  const uint8_t fonte = layout::uiFontSize(1);
  return comDica ? static_cast<int16_t>(yTopoRodape() - 8 * fonte - 2) : yTopoRodape();
}

// Apaga de "yDe" até o fim da área de conteúdo. É a única limpeza que
// sobrou, e cobre só o que o desenho novo não alcançou.
void limparSobra(int16_t yDe, bool comDica) {
  const int16_t ate = yFimConteudo(comDica);
  if (ate > yDe) tft.fillRect(0, yDe, tft.width(), ate - yDe, COR_FUNDO);
}

// Apaga a área de conteúdo inteira, preservando cabeçalho e rodapé. Para as
// telas de desenho ESPARSO (gráfico, QR code, mensagem), onde não dá para
// pintar o fundo elemento a elemento — uma curva não tem retângulo próprio.
// Continua melhor que fillScreen: cabeçalho e barra de botões não piscam,
// porque não são apagados para serem redesenhados iguais logo em seguida.
void limparConteudo(bool comDica) {
  const int16_t y0 = layout::uiHeaderHeight();
  const int16_t y1 = yFimConteudo(comDica);
  if (y1 > y0) tft.fillRect(0, y0, tft.width(), y1 - y0, COR_FUNDO);
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
  // Ações dos dois botões do meio. Só a tela de gráfico as troca (para
  // zoom); em todas as outras eles navegam. A POSIÇÃO nunca muda — é o que
  // permite acertar o botão sem reler a tela a cada troca.
  AcaoToque acaoAnterior = AcaoToque::Anterior;
  AcaoToque acaoProximo = AcaoToque::Proximo;
};

void desenharBotoesRodape(const RotulosRodape& rotulos, bool mostrarVoltar = true) {
  const int16_t largura = tft.width();
  const int16_t altura = tft.height();
  const int16_t alturaRodape = layout::uiFooterHeight();
  const int16_t y = altura - alturaRodape;
  const uint8_t fonte = layout::uiFontSize(1);

  const int16_t larguraBotao = largura / 4;
  const AcaoToque acoes[4] = {AcaoToque::Voltar, rotulos.acaoAnterior, rotulos.acaoProximo,
                              AcaoToque::Confirmar};
  const char* textos[4] = {rotulos.voltar, rotulos.anterior, rotulos.proximo, rotulos.confirmar};

  for (uint8_t i = 0; i < 4; i++) {
    const int16_t x = i * larguraBotao;

    // Separador de 1px entre botões vizinhos, senão a barra vira um bloco
    // só e não dá pra ver onde um termina e o outro começa (importante
    // quando o dedo cobre metade da barra). Desenhado aqui, e não por um
    // fillRect de fundo na barra inteira antes do laço: aquele fundo era
    // uma segunda passagem sobre os mesmos pixels.
    if (i > 0) tft.drawFastVLine(x - 1, y, alturaRodape, COR_FUNDO);

    if (i == 0 && !mostrarVoltar) {
      tft.fillRect(x, y, larguraBotao - 1, alturaRodape, COR_FUNDO);
      continue;
    }

    tft.fillRect(x, y, larguraBotao - 1, alturaRodape, UI_COR_BOTAO);
    tft.drawRect(x, y, larguraBotao - 1, alturaRodape, UI_COR_BORDA_TOQUE);

    const int16_t larguraTexto = static_cast<int16_t>(std::strlen(textos[i]) * 6 * fonte);
    tft.setTextSize(fonte);
    tft.setTextColor(UI_COR_TEXTO_BOTAO);
    imprimirTexto(x + (larguraBotao - larguraTexto) / 2, y + (alturaRodape - 8 * fonte) / 2,
                  textos[i]);

    registrarZona(x, y, larguraBotao, alturaRodape, acoes[i], 0, false, textos[i]);
  }
}

// Realimentação visual do botão pressionado: repinta só aquele botão na cor
// de pressionado. Sem isso o usuário não tem como saber se o toque pegou —
// o dedo cobre justamente o botão que ele está tocando.
void pintarZonaPressionada(const ZonaToque& z, bool pressionada) {
  if (z.acao == AcaoToque::ItemLista || z.acao == AcaoToque::Nenhuma) return;

  const uint8_t fonte = layout::uiFontSize(1);
  const uint16_t corFundo = pressionada ? UI_COR_BOTAO_PRESSIONADO : UI_COR_BOTAO;
  tft.fillRect(z.x, z.y, z.w - 1, z.h, corFundo);
  tft.drawRect(z.x, z.y, z.w - 1, z.h, UI_COR_BORDA_TOQUE);
  const int16_t larguraTexto = static_cast<int16_t>(std::strlen(z.rotulo) * 6 * fonte);
  tft.setTextSize(fonte);
  tft.setTextColor(pressionada ? UI_COR_TEXTO_SELECIONADO : UI_COR_TEXTO_BOTAO, corFundo);
  imprimirTexto(z.x + (z.w - larguraTexto) / 2, z.y + (z.h - 8 * fonte) / 2, z.rotulo);
}

// Toque num item de lista, em DOIS TEMPOS:
//   1o toque num item que NÃO está selecionado -> só move o cursor até ele
//                                                 (rajada de passos), sem ativar;
//   toque num item que JÁ está selecionado     -> confirma.
//
// Ou seja: um toque num item novo é sempre inofensivo, e só o segundo toque
// — agora sobre um item visivelmente destacado — executa a ação. Isso é o
// que dá chance de corrigir a mira antes de acionar qualquer coisa, que num
// touch resistivo com dedo adulto acontece o tempo todo. O preço é um toque
// a mais por escolha; a alternativa (ativar no primeiro toque) executa
// ações erradas e algumas são destrutivas — excluir arquivo, cancelar
// experimento em andamento.
//
// A tradução para o vocabulário da máquina de estados continua a mesma: só
// mudou onde entra o Confirmar. Ver ihm.hpp.
void enfileirarSaltoParaItem(uint8_t destino, bool confirmaDireto) {
  if (destino == indiceSelecionadoAtual) {
    enfileirar(EventoFila::Confirmar);
    return;
  }

  if (destino > indiceSelecionadoAtual) {
    for (uint8_t i = indiceSelecionadoAtual; i < destino; i++) enfileirar(EventoFila::Proximo);
  } else {
    for (uint8_t i = destino; i < indiceSelecionadoAtual; i++) enfileirar(EventoFila::Anterior);
  }

  // Nas listas de menu, sem Confirmar aqui: o cursor só andou, e quem
  // confirma é o toque seguinte. No teclado de texto, confirmaDireto é
  // true e o caractere sai no primeiro toque.
  if (confirmaDireto) enfileirar(EventoFila::Confirmar);

  // Todos os passos acima são consumidos NO MESMO tick pela máquina de
  // estados (ver o laço em maquina_estados::tick()), então o cursor aparece
  // direto no item tocado — sem a animação de "andar item por item" que a
  // primeira versão deste porte tinha, herdada do jeito como o encoder
  // entregava um passo por vez.
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

// Definida bem abaixo, junto das demais primitivas de desenho, mas
// declarada aqui porque atualizarToque() a chama: os gestos de zoom e
// arrasto redesenham o grafico sem passar pela maquina de estados.
void desenharGraficoInterno();

void init() {
  Serial.println("[DISPLAY] Inicializacao iniciada");
  Serial.printf("[DISPLAY] TFT_CS: %d | TFT_DC: %d | TFT_RST: %d | TFT_BL: %d\n", TFT_CS, TFT_DC,
                TFT_RST, TFT_BL);
  Serial.printf("[DISPLAY] TFT_SCLK: %d | TFT_MOSI: %d | TFT_MISO: %d | TOUCH_CS: %d\n", TFT_SCLK,
                TFT_MOSI, TFT_MISO, TOUCH_CS);

  pinMode(BUZZER_PIN, OUTPUT);
  // Tira o tone() do canal LEDC 0 (padrão dele) antes de qualquer beep —
  // ver o comentário em BRILHO_PWM_CANAL. Sem isto, o primeiro beep rouba
  // o canal do backlight e a tela apaga.
  setToneChannel(BUZZER_PWM_CANAL);

  // Backlight ligado antes do init do painel: confirma que o circuito do
  // backlight funciona mesmo que o controlador não responda no SPI.
  pinMode(TFT_BL, OUTPUT);
  digitalWrite(TFT_BL, HIGH);
  Serial.printf("[DISPLAY] Backlight configurado (pino %d em HIGH)\n", TFT_BL);

  // DESSELECIONA O CARTÃO ANTES DE FALAR COM A TELA.
  //
  // O SD compartilha SCK/MOSI/MISO com o display, e quem cuida do CS dele é
  // o SD.begin() — que só roda bem depois, em armazenamento::init(). Até
  // lá, SD_CS_PIN é uma entrada flutuante: se ela estiver em nível baixo
  // durante o tft.init(), o cartão se considera selecionado e interpreta
  // toda a sequência de inicialização do display como comandos SPI
  // endereçados a ele. O resultado é um cartão em estado inconsistente, que
  // depois recusa o SD.begin() — a falha aparece no SD, mas a causa está
  // aqui.
  //
  // O TOUCH_CS não precisa do mesmo cuidado: o próprio tft.init() o coloca
  // em HIGH (TFT_eSPI.cpp, linhas 543-546).
  pinMode(SD_CS_PIN, OUTPUT);
  digitalWrite(SD_CS_PIN, HIGH);
  Serial.printf("[DISPLAY] SD desselecionado (pino %d em HIGH) antes de iniciar o painel\n",
                SD_CS_PIN);

  tft.init();
  tft.setRotation(ROTACAO_DISPLAY);

  // O ID do controlador é lido só para DIAGNÓSTICO, e nunca para decidir se
  // a tela existe.
  //
  // O TFT_eSPI não tem um begin() com retorno de sucesso como o
  // Arduino_GFX tinha, e a leitura de registrador não serve de substituto:
  // muitos destes painéis simplesmente não respondem ao 0xD3 (ou têm o SDO
  // sem tri-state, ou compartilham o MISO com o touch/SD), e devolvem
  // 00/FF mesmo funcionando perfeitamente para escrita. Condicionar
  // displayOk a essa leitura desligava a IHM inteira — incluindo a
  // calibração do toque — num painel são, deixando a tela com o lixo de
  // RAM que ela mostra ao ligar. Escrita e leitura são caminhos
  // independentes aqui; a falha de uma não prova nada sobre a outra.
  const uint8_t id1 = tft.readcommand8(0xD3, 1);
  const uint8_t id2 = tft.readcommand8(0xD3, 2);
  const uint8_t id3 = tft.readcommand8(0xD3, 3);
  Serial.printf("[DISPLAY] ID do controlador (0xD3): %02X %02X %02X\n", id1, id2, id3);
  Serial.println("[DISPLAY]   93 41 -> ILI9341 240x320 | 93 42 -> ILI9342 320x240");
  Serial.println("[DISPLAY]   94 88 -> ILI9488 320x480 | 00/FF -> sem resposta de leitura");
  Serial.println("[DISPLAY]   (so diagnostico: o driver em uso vem do platformio.ini)");

  displayOk = true;
  Serial.printf("[DISPLAY] Resolucao configurada: %d x %d (rotacao %d)\n", tft.width(),
                tft.height(), ROTACAO_DISPLAY);
  tft.fillScreen(COR_FUNDO);
  layout::init(tft.width(), tft.height());

  // Teste visual rápido, herdado da ideia do "teste visual integrado" da
  // main: três faixas de cor por meio segundo. Se elas aparecerem, o
  // caminho de ESCRITA (SPI, CS, DC, RST e backlight) está inteiro, e
  // qualquer problema seguinte é de geometria/driver, não de fiação.
  const int16_t faixa = tft.height() / 3;
  tft.fillRect(0, 0, tft.width(), faixa, TFT_RED);
  tft.fillRect(0, faixa, tft.width(), faixa, TFT_GREEN);
  tft.fillRect(0, 2 * faixa, tft.width(), tft.height() - 2 * faixa, TFT_BLUE);
  delay(500);
  tft.fillScreen(COR_FUNDO);

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
  } else {
    Serial.println("[TOUCH] Sem calibracao valida para esta geometria - calibrando agora");
    calibrarToque();
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
    xInicialToque = x;
    yInicialToque = y;
    xReferenciaArrasto = x;
    yReferenciaArrasto = y;
    virouGesto = false;

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
  } else if (agora && tocando) {
    // ---- Dedo arrastando ----
    const int16_t dx = x - xInicialToque;
    const int16_t dy = y - yInicialToque;

    if (!virouGesto && (abs(dx) > LIMIAR_GESTO_PX || abs(dy) > LIMIAR_GESTO_PX)) {
      virouGesto = true;
      // Desfaz o destaque do botão: o toque virou gesto e não vai mais
      // acionar aquela zona, então deixá-lo aceso seria mentira visual.
      if (temZonaPressionada) {
        TravaBarramentoDisplay travaBus;
        pintarZonaPressionada(zonaPressionada, false);
      }
    }

    if (virouGesto) {
      if (graficoAtivo && abs(dx) > abs(dy)) {
        // Arrasto lateral no gráfico: desloca a janela visível. Segue o
        // dedo — arrastar para a esquerda anda para a frente no tempo.
        const int16_t passo = x - xReferenciaArrasto;
        if (passo != 0 && graficoZoom > 1.0f) {
          graficoCentro -= static_cast<float>(passo) / static_cast<float>(tft.width()) / graficoZoom;
          if (graficoCentro < 0.0f) graficoCentro = 0.0f;
          if (graficoCentro > 1.0f) graficoCentro = 1.0f;
          xReferenciaArrasto = x;
          TravaBarramentoDisplay travaBus;
          desenharGraficoInterno();
        }
      } else if (!graficoAtivo && quantidadeItensAtual > 0) {
        // Arrasto vertical numa lista: cada linha percorrida move a
        // seleção em um item. Como a rolagem acompanha a seleção
        // (offsetRolagem), mover a seleção é o que faz a lista rolar.
        const int16_t alturaLinha = layout::uiLineSpacing();
        while (y - yReferenciaArrasto >= alturaLinha) {
          enfileirar(EventoFila::Anterior);  // dedo para baixo = sobe na lista
          yReferenciaArrasto += alturaLinha;
        }
        while (yReferenciaArrasto - y >= alturaLinha) {
          enfileirar(EventoFila::Proximo);
          yReferenciaArrasto -= alturaLinha;
        }
      }
    }
  } else if (!agora && tocando) {
    // ---- Borda de subida: dedo saiu ----
    // A ação acontece aqui, não na descida: assim o usuário pode arrastar o
    // dedo para fora do botão e soltar sem acionar nada, que é o
    // comportamento esperado de qualquer interface de toque.
    tocando = false;

    const int16_t dx = ultimoXValido - xInicialToque;
    const int16_t dy = ultimoYValido - yInicialToque;

    // Deslizar para a direita = voltar. Exige ser bem mais horizontal que
    // vertical (2x) para não confundir com uma rolagem torta.
    const bool deslizouParaVoltar =
        virouGesto && dx > LIMIAR_DESLIZE_VOLTAR_PX && abs(dx) > 2 * abs(dy);

    if (temZonaPressionada) {
      {
        TravaBarramentoDisplay travaBus;
        pintarZonaPressionada(zonaPressionada, false);
      }

      // Só aciona se NÃO virou gesto e se o dedo estava dentro da mesma
      // zona na última leitura válida — é isso que faz o "arrastar para
      // fora para cancelar" funcionar, e o que impede uma rolagem de
      // também selecionar o item de onde ela partiu.
      if (!virouGesto && zonaPressionada.contem(ultimoXValido, ultimoYValido)) {
        ultimoToqueAceitoMs = ms;
        switch (zonaPressionada.acao) {
          case AcaoToque::ItemLista:
            enfileirarSaltoParaItem(zonaPressionada.indice, zonaPressionada.confirmaDireto);
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
          case AcaoToque::ZoomMais:
          case AcaoToque::ZoomMenos: {
            // Zoom não passa pela máquina de estados: é uma mudança de
            // visualização, não de estado do firmware. A ihm redesenha o
            // gráfico na hora, com os pontos que ela mesma copiou.
            if (zonaPressionada.acao == AcaoToque::ZoomMais) {
              graficoZoom *= 2.0f;
              if (graficoZoom > GRAFICO_ZOOM_MAX) graficoZoom = GRAFICO_ZOOM_MAX;
            } else {
              graficoZoom /= 2.0f;
              if (graficoZoom < 1.0f) graficoZoom = 1.0f;
              if (graficoZoom == 1.0f) graficoCentro = 0.5f;
            }
            TravaBarramentoDisplay travaBus;
            desenharGraficoInterno();
            break;
          }
          case AcaoToque::Nenhuma:
            break;
        }
      }
      temZonaPressionada = false;
    }

    if (deslizouParaVoltar) {
      ultimoToqueAceitoMs = ms;
      voltarPendente = true;
    }
    virouGesto = false;
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

  // Sem fillScreen: passagem única, ver o comentário em limparSobra().
  desenharCabecalhoRodape(titulo);

  const int16_t yInicial = layout::uiHeaderHeight() + layout::uiMargin();
  const int16_t alturaLinha = layout::uiLineSpacing();
  const uint8_t fonte = layout::uiFontSize(1);

  // Faixa fina entre o cabeçalho e a primeira linha (a margem) — precisa
  // ser apagada porque nenhum item a cobre.
  tft.fillRect(0, layout::uiHeaderHeight(), tft.width(), yInicial - layout::uiHeaderHeight(),
               COR_FUNDO);

  int16_t yFim = yInicial;
  for (uint8_t linha = 0; linha < itensVisiveis; linha++) {
    const uint8_t indiceItem = offsetRolagem + linha;
    if (indiceItem >= quantidade) break;

    const int16_t y = yInicial + linha * alturaLinha;
    const bool selecionado = (indiceItem == indiceSelecionado);

    // Toda linha pinta o próprio fundo, selecionada ou não: é o que
    // dispensa o fillScreen e apaga o que estava ali antes.
    const uint16_t corFundoLinha = selecionado ? COR_SELECIONADO : COR_FUNDO;
    tft.fillRect(0, y - 1, tft.width(), alturaLinha, corFundoLinha);
    // Contorno da area tocavel. Desenhado tambem no item selecionado: sem
    // ele, o destaque preencheria a linha inteira e se perderia a referencia
    // de onde uma linha termina e a seguinte comeca.
    tft.drawRect(UI_RECUO_BORDA_TOQUE, y - 1 + UI_RECUO_BORDA_TOQUE,
                 tft.width() - 2 * UI_RECUO_BORDA_TOQUE,
                 alturaLinha - 2 * UI_RECUO_BORDA_TOQUE,
                 selecionado ? COR_TEXTO_SELECIONADO : UI_COR_BORDA_TOQUE);

    char buffer[40];
    truncarTexto(buffer, sizeof(buffer), itens[indiceItem],
                 tft.width() - 2 * layout::uiMargin(), fonte);

    tft.setTextSize(fonte);
    tft.setTextColor(selecionado ? COR_TEXTO_SELECIONADO : COR_TEXTO, corFundoLinha);
    // Centraliza o texto na altura da linha: a linha agora é bem mais alta
    // que o texto (piso de toque, UI_ALTURA_MINIMA_ALVO_TOQUE), então
    // escrever no topo dela deixaria o texto "colado" na linha de cima.
    imprimirTexto(layout::uiMargin(), y + (alturaLinha - 8 * fonte) / 2, buffer);

    registrarZona(0, y - 1, tft.width(), alturaLinha, AcaoToque::ItemLista, indiceItem);
    yFim = y - 1 + alturaLinha;
  }

  limparSobra(yFim, false);
  desenharBotoesRodape(RotulosRodape{});
}

void desenharConfirmacao(const char* pergunta, uint8_t indiceSelecionado) {
  if (!displayOk) return;
  TravaBarramentoDisplay travaBus;
  limparZonas();
  indiceSelecionadoAtual = indiceSelecionado;
  quantidadeItensAtual = 2;

  desenharCabecalhoRodape("Confirmar", "Toque de novo na opcao para confirmar");

  const uint8_t fonte = layout::uiFontSize(1);
  const int16_t yPergunta = layout::uiHeaderHeight() + layout::uiMargin();
  const int16_t alturaLinha = layout::uiLineSpacing();

  tft.fillRect(0, layout::uiHeaderHeight(), tft.width(),
               yPergunta + alturaLinha * 2 - layout::uiHeaderHeight(), COR_FUNDO);

  char bufferPergunta[48];
  truncarTexto(bufferPergunta, sizeof(bufferPergunta), pergunta,
               tft.width() - 2 * layout::uiMargin(), fonte);
  tft.setTextSize(fonte);
  tft.setTextColor(COR_VALOR, COR_FUNDO);
  imprimirTexto(layout::uiMargin(), yPergunta, bufferPergunta);

  static const char* const opcoes[2] = {"Sim", "Nao"};
  const int16_t yOpcoes = yPergunta + alturaLinha * 2;
  int16_t yFim = yOpcoes;
  for (uint8_t i = 0; i < 2; i++) {
    const bool selecionado = (i == indiceSelecionado);
    const int16_t y = yOpcoes + i * alturaLinha;
    const uint16_t corFundoLinha = selecionado ? COR_SELECIONADO : COR_FUNDO;
    tft.fillRect(0, y - 1, tft.width(), alturaLinha, corFundoLinha);
    // Contorno da area tocavel. Desenhado tambem no item selecionado: sem
    // ele, o destaque preencheria a linha inteira e se perderia a referencia
    // de onde uma linha termina e a seguinte comeca.
    tft.drawRect(UI_RECUO_BORDA_TOQUE, y - 1 + UI_RECUO_BORDA_TOQUE,
                 tft.width() - 2 * UI_RECUO_BORDA_TOQUE,
                 alturaLinha - 2 * UI_RECUO_BORDA_TOQUE,
                 selecionado ? COR_TEXTO_SELECIONADO : UI_COR_BORDA_TOQUE);
    tft.setTextSize(fonte);
    tft.setTextColor(selecionado ? COR_TEXTO_SELECIONADO : COR_TEXTO, corFundoLinha);
    imprimirTexto(layout::uiMargin(), y + (alturaLinha - 8 * fonte) / 2, opcoes[i]);
    registrarZona(0, y - 1, tft.width(), alturaLinha, AcaoToque::ItemLista, i);
    yFim = y - 1 + alturaLinha;
  }

  limparSobra(yFim, true);
  desenharBotoesRodape(RotulosRodape{});
}

void desenharValorEditavel(const char* titulo, int32_t valor, int32_t minimo,
                           int32_t maximo, const char* unidade) {
  if (!displayOk) return;
  TravaBarramentoDisplay travaBus;
  limparZonas();

  desenharCabecalhoRodape(titulo, "- e + ajustam, OK confirma");

  const uint8_t fonteValor = layout::uiFontSize(3);
  char textoValor[16];
  if (unidade != nullptr) {
    snprintf(textoValor, sizeof(textoValor), "%ld%s", static_cast<long>(valor), unidade);
  } else {
    snprintf(textoValor, sizeof(textoValor), "%ld", static_cast<long>(valor));
  }

  // Faixa do valor apagada e reescrita: o número muda de largura (de "9"
  // para "10", de "100" para "99"), então sem apagar a faixa inteira
  // sobrariam dígitos do valor anterior nas pontas.
  const int16_t alturaValor = 8 * fonteValor;
  const int16_t yValor = layout::uiCenterY() - alturaValor / 2;
  tft.fillRect(0, layout::uiHeaderHeight(), tft.width(),
               yValor + alturaValor - layout::uiHeaderHeight(), COR_FUNDO);

  const int16_t larguraTexto = static_cast<int16_t>(std::strlen(textoValor) * 6 * fonteValor);
  tft.setTextSize(fonteValor);
  tft.setTextColor(COR_VALOR, COR_FUNDO);
  imprimirTexto(layout::uiCenterX() - larguraTexto / 2, yValor, textoValor);

  const int16_t barraX = layout::uiMargin();
  const int16_t barraY = tft.height() - layout::uiFooterHeight() - layout::uiHeight(20);
  const int16_t barraLargura = tft.width() - 2 * layout::uiMargin();
  const int16_t barraAltura = layout::uiHeight(8);

  limparSobra(yValor + alturaValor, true);

  tft.drawRect(barraX, barraY, barraLargura, barraAltura, COR_RODAPE);
  const int32_t faixa = maximo - minimo;
  int16_t preenchido = 0;
  if (faixa > 0 && barraLargura > 2) {
    preenchido = static_cast<int16_t>((static_cast<int64_t>(valor - minimo) * (barraLargura - 2)) / faixa);
  }
  // Interior da barra sempre repintado por inteiro (parte cheia + parte
  // vazia): ao DIMINUIR o valor, só pintar a parte cheia deixaria o
  // restante do preenchimento anterior na tela.
  if (preenchido > 0) {
    tft.fillRect(barraX + 1, barraY + 1, preenchido, barraAltura - 2, COR_CABECALHO);
  }
  if (preenchido < barraLargura - 2) {
    tft.fillRect(barraX + 1 + preenchido, barraY + 1, barraLargura - 2 - preenchido,
                 barraAltura - 2, COR_FUNDO);
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
  // Habilita o arrasto vertical para rolar: esta tela não tem itens
  // selecionáveis, mas a rolagem dela também é feita por Proximo/Anterior,
  // que é o que o gesto emite.
  quantidadeItensAtual = quantidade;

  desenharCabecalhoRodape(titulo, "Arraste para rolar");

  // Uma linha a menos que o layout permite: a dica acima da barra de botões
  // ocupa espaço que uiItensVisiveis() não conhece, e sem esta reserva a
  // última linha da lista seria escrita por cima dela.
  const uint8_t itensCabem = layout::uiItensVisiveis();
  const uint8_t itensVisiveis = (itensCabem > 1) ? static_cast<uint8_t>(itensCabem - 1) : 1;
  const int16_t yInicial = layout::uiHeaderHeight() + layout::uiMargin();
  const int16_t alturaLinha = layout::uiLineSpacing();
  const uint8_t fonte = layout::uiFontSize(1);

  tft.fillRect(0, layout::uiHeaderHeight(), tft.width(), yInicial - layout::uiHeaderHeight(),
               COR_FUNDO);
  tft.setTextSize(fonte);
  tft.setTextColor(COR_TEXTO, COR_FUNDO);

  int16_t yFim = yInicial;
  for (uint8_t linha = 0; linha < itensVisiveis; linha++) {
    const uint8_t indice = offsetRolagem + linha;
    if (indice >= quantidade) break;

    const int16_t y = yInicial + linha * alturaLinha;
    tft.fillRect(0, y, tft.width(), alturaLinha, COR_FUNDO);
    char buffer[40];
    truncarTexto(buffer, sizeof(buffer), linhas[indice],
                 tft.width() - 2 * layout::uiMargin(), fonte);
    imprimirTexto(layout::uiMargin(), y, buffer);
    yFim = y + alturaLinha;
  }

  limparSobra(yFim, true);
  desenharBotoesRodape(RotulosRodape{});
}

void desenharGradeModulos(const char* titulo, uint8_t dimensao,
                          bool (*modulo)(uint8_t, uint8_t)) {
  if (!displayOk) return;
  TravaBarramentoDisplay travaBus;
  limparZonas();

  limparConteudo(false);
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

  tft.fillRect(0, layout::uiHeaderHeight(), tft.width(), yInicial - layout::uiHeaderHeight(),
               COR_FUNDO);
  tft.setTextSize(fonte);
  int16_t ultimaLinhaY = yInicial;

  for (uint8_t i = 0; i < quantidade; i++) {
    const uint8_t linha = i / colunas;
    const uint8_t coluna = i % colunas;
    const int16_t x = xInicial + coluna * larguraCelula;
    const int16_t y = yInicial + linha * alturaCelula;

    // Grade grande demais pra área disponível: corta os últimos símbolos
    // em vez de invadir o rodapé.
    if (y + alturaCelula > yLimite) break;

    const bool selecionado = (i == indiceSelecionado);
    const uint16_t corFundoCelula = selecionado ? COR_SELECIONADO : COR_FUNDO;
    tft.fillRect(x, y, larguraCelula - 1, alturaCelula - 1, corFundoCelula);
    // Contorno em TODAS as teclas (antes so nas nao selecionadas): e o que
    // mostra o tamanho real do alvo de cada caractere.
    tft.drawRect(x, y, larguraCelula - 1, alturaCelula - 1,
                 selecionado ? COR_TEXTO_SELECIONADO : UI_COR_BORDA_TOQUE);
    tft.setTextColor(selecionado ? COR_TEXTO_SELECIONADO : COR_TEXTO, corFundoCelula);
    const int16_t larguraTexto = static_cast<int16_t>(std::strlen(rotulos[i]) * 6 * fonte);
    imprimirTexto(x + (larguraCelula - larguraTexto) / 2, y + (alturaCelula - 8 * fonte) / 2,
                  rotulos[i]);

    // confirmaDireto = true: no teclado, um toque ja digita o caractere.
    // Cobrar dois toques por letra tornaria digitar um nome de arquivo
    // insuportavel, e o custo de errar aqui e apagar uma letra — nada
    // parecido com o de errar um item de menu.
    registrarZona(x, y, larguraCelula, alturaCelula, AcaoToque::ItemLista, i, true);
    ultimaLinhaY = y + alturaCelula;
  }

  limparSobra(ultimaLinhaY, false);
  desenharBotoesRodape(RotulosRodape{});
}

void desenharMensagem(const char* titulo, const char* mensagem) {
  if (!displayOk) return;
  TravaBarramentoDisplay travaBus;
  limparZonas();

  limparConteudo(false);
  desenharCabecalhoRodape(titulo);

  const uint8_t fonte = layout::uiFontSize(1);
  char buffer[80];
  truncarTexto(buffer, sizeof(buffer), mensagem, tft.width() - 2 * layout::uiMargin(), fonte);

  tft.setTextSize(fonte);
  tft.setTextColor(COR_VALOR);
  imprimirTexto(layout::uiMargin(), layout::uiCenterY(), buffer);

  desenharBotoesRodape(RotulosRodape{});
}

// Desenha o gráfico a partir dos pontos JÁ COPIADOS para graficoTempos/
// graficoValores, respeitando graficoZoom/graficoCentro. Fica fora do
// namespace anônimo (foi declarada lá em cima) porque os gestos de zoom e
// arrasto, tratados em atualizarToque(), precisam redesenhar sem passar
// pela máquina de estados: zoom é mudança de visualização, não de estado do
// firmware.
void desenharGraficoInterno() {
  if (!displayOk) return;
  limparZonas();
  graficoAtivo = true;

  limparConteudo(false);
  desenharCabecalhoRodape(graficoTitulo);

  const uint8_t fonte = layout::uiFontSize(1);

  RotulosRodape rodapeGrafico;
  rodapeGrafico.anterior = "-";
  rodapeGrafico.proximo = "+";
  rodapeGrafico.acaoAnterior = AcaoToque::ZoomMenos;
  rodapeGrafico.acaoProximo = AcaoToque::ZoomMais;

  if (graficoQuantidade == 0) {
    tft.setTextSize(fonte);
    tft.setTextColor(COR_TEXTO, COR_FUNDO);
    imprimirTexto(layout::uiMargin(), layout::uiCenterY(), "Sem dados suficientes");
    desenharBotoesRodape(rodapeGrafico);
    return;
  }

  float minT = graficoTempos[0];
  float maxT = graficoTempos[0];
  for (uint8_t i = 1; i < graficoQuantidade; i++) {
    if (graficoTempos[i] < minT) minT = graficoTempos[i];
    if (graficoTempos[i] > maxT) maxT = graficoTempos[i];
  }
  // Evita divisao por zero quando todos os pontos tem o mesmo tempo (ex.:
  // um unico ponto) — nesse caso a serie vira uma linha reta.
  const float faixaTotalT = (maxT > minT) ? (maxT - minT) : 1.0f;

  // Janela visível: com zoom 1 é a série inteira; a cada zoom a janela
  // encolhe pela metade em torno de graficoCentro. O centro é preso às
  // bordas para a janela nunca sair da faixa de dados — sem isso, arrastar
  // até o fim deixaria a tela vazia.
  const float larguraJanela = faixaTotalT / graficoZoom;
  float t0 = minT + graficoCentro * faixaTotalT - larguraJanela / 2.0f;
  if (t0 < minT) t0 = minT;
  if (t0 > maxT - larguraJanela) t0 = maxT - larguraJanela;
  const float t1 = t0 + larguraJanela;

  // Escala do eixo Y recalculada para os pontos VISÍVEIS: ampliar um trecho
  // e continuar usando a escala da série inteira achataria justamente o
  // detalhe que o usuário ampliou para ver.
  bool achouVisivel = false;
  float minY = 0.0f, maxY = 0.0f;
  for (uint8_t i = 0; i < graficoQuantidade; i++) {
    if (graficoTempos[i] < t0 || graficoTempos[i] > t1) continue;
    if (!achouVisivel) {
      minY = maxY = graficoValores[i];
      achouVisivel = true;
    } else {
      if (graficoValores[i] < minY) minY = graficoValores[i];
      if (graficoValores[i] > maxY) maxY = graficoValores[i];
    }
  }
  if (!achouVisivel) {
    minY = 0.0f;
    maxY = 1.0f;
  }
  const float faixaY = (maxY > minY) ? (maxY - minY) : 1.0f;

  const int16_t plotX0 = layout::uiMargin();
  const int16_t plotX1 = tft.width() - layout::uiMargin();
  const int16_t plotY0 = layout::uiHeaderHeight() + layout::uiMargin();
  const int16_t plotY1 = tft.height() - layout::uiFooterHeight() - layout::uiMargin();
  const int16_t plotLargura = plotX1 - plotX0;
  const int16_t plotAltura = plotY1 - plotY0;
  if (plotLargura <= 1 || plotAltura <= 1) {
    desenharBotoesRodape(rodapeGrafico);
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
  bool temAnterior = false;
  for (uint8_t i = 0; i < graficoQuantidade; i++) {
    if (graficoTempos[i] < t0 || graficoTempos[i] > t1) {
      temAnterior = false;  // saiu da janela: não liga por cima do corte
      continue;
    }
    const int16_t x =
        plotX0 + static_cast<int16_t>((graficoTempos[i] - t0) / larguraJanela * (plotLargura - 1));
    const int16_t y =
        plotY1 - static_cast<int16_t>((graficoValores[i] - minY) / faixaY * (plotAltura - 1));
    if (temAnterior) {
      tft.drawLine(xAnterior, yAnterior, x, y, COR_VALOR);
    }
    tft.fillRect(x - 1, y - 1, 3, 3, COR_VALOR);
    xAnterior = x;
    yAnterior = y;
    temAnterior = true;
  }

  // Valores minimo/maximo do eixo Y, nos cantos superior/inferior esquerdos
  // da area do grafico. Com zoom > 1 mostra tambem a janela de tempo, senão
  // não haveria como saber que trecho da série está na tela.
  char bufMax[12];
  char bufMin[12];
  snprintf(bufMax, sizeof(bufMax), "%.2f", static_cast<double>(maxY));
  snprintf(bufMin, sizeof(bufMin), "%.2f", static_cast<double>(minY));
  tft.setTextSize(fonte);
  tft.setTextColor(COR_RODAPE, COR_FUNDO);
  imprimirTexto(plotX0 + 1, plotY0, bufMax);
  imprimirTexto(plotX0 + 1, plotY1 - layout::uiLineSpacing(), bufMin);

  if (graficoZoom > 1.0f) {
    char bufJanela[32];
    snprintf(bufJanela, sizeof(bufJanela), "%.2f-%.2fs  %.0fx", static_cast<double>(t0),
             static_cast<double>(t1), static_cast<double>(graficoZoom));
    const int16_t larguraJanelaTexto = static_cast<int16_t>(std::strlen(bufJanela) * 6 * fonte);
    imprimirTexto(plotX1 - larguraJanelaTexto - 2, plotY0, bufJanela);
  }

  desenharBotoesRodape(rodapeGrafico);
}

void desenharGrafico(const char* titulo, const float* temposS, const float* valoresY, uint8_t quantidade) {
  if (!displayOk) return;
  TravaBarramentoDisplay travaBus;

  // Copia os pontos em vez de guardar os ponteiros: os gestos redesenham o
  // gráfico depois, fora do ciclo da máquina de estados, e a essa altura os
  // vetores de analise_linear/analise_circular podem já não valer mais.
  graficoQuantidade = 0;
  if (temposS != nullptr && valoresY != nullptr) {
    const uint16_t limite = (quantidade < MAX_PONTOS_GRAFICO) ? quantidade : MAX_PONTOS_GRAFICO;
    for (uint16_t i = 0; i < limite; i++) {
      graficoTempos[i] = temposS[i];
      graficoValores[i] = valoresY[i];
    }
    graficoQuantidade = static_cast<uint8_t>(limite);
  }

  std::strncpy(graficoTitulo, titulo != nullptr ? titulo : "", sizeof(graficoTitulo) - 1);
  graficoTitulo[sizeof(graficoTitulo) - 1] = '\0';

  // Entrar na tela (ou trocar de repetição) sempre começa mostrando a série
  // inteira: manter o zoom da visualização anterior deixaria o usuário
  // olhando um pedaço arbitrário de uma curva que ele acabou de abrir.
  graficoZoom = 1.0f;
  graficoCentro = 0.5f;

  desenharGraficoInterno();
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

  // Escala uniforme, preservando a proporção — AMPLIA quando a imagem é
  // menor que a área disponível, ao contrário da versão anterior, que só
  // reduzia. Com as logos de boot (160x128 e 164x52) numa tela de 320x240,
  // "nunca ampliar" significava desenhá-las no tamanho original: um quarto
  // da tela e um oitavo dela, respectivamente.
  //
  // escala < 1 amplia, > 1 reduz. Toma-se o MAIOR dos dois fatores para a
  // imagem caber inteira na caixa (o menor a faria transbordar no outro
  // eixo).
  const float escalaLargura = static_cast<float>(larguraOrigem) / larguraMaxima;
  const float escalaAltura = static_cast<float>(alturaOrigem) / alturaMaxima;
  float escala = (escalaLargura > escalaAltura) ? escalaLargura : escalaAltura;
  if (escala <= 0.0f) escala = 1.0f;

  int16_t larguraSaida = static_cast<int16_t>(larguraOrigem / escala);
  int16_t alturaSaida = static_cast<int16_t>(alturaOrigem / escala);
  // Arredondamento pode estourar a caixa em 1px; prende nos limites.
  if (larguraSaida > larguraMaxima) larguraSaida = larguraMaxima;
  if (alturaSaida > alturaMaxima) alturaSaida = alturaMaxima;
  if (larguraSaida < 1 || alturaSaida < 1) {
    armazenamento::fecharBinario();
    return false;
  }

  const int16_t xCentralizado = x + (larguraMaxima - larguraSaida) / 2;
  const int16_t yCentralizado = y + (alturaMaxima - alturaSaida) / 2;

  // Desenho LINHA A LINHA, sem framebuffer da imagem inteira.
  //
  // A versão anterior lia a imagem toda para a RAM antes de desenhar, por um
  // motivo que deixou de existir: naquela época alternar o dono do barramento
  // (SD <-> display) a cada linha corrompia o cartão, porque o display era
  // bit-bang e o SD era SPI de hardware nos mesmos pinos. Hoje os dois usam o
  // mesmo periférico e só alternam o CS, então intercalar leitura e desenho
  // por linha é seguro.
  //
  // E passou a ser necessário: ampliando para a tela cheia, aquele buffer
  // seria 320*240*2 = 150KB de RAM interna — inviável ao lado do NimBLE.
  // Assim são ~1,3KB (uma linha de origem + uma de destino).
  uint8_t* linhaOrigem = static_cast<uint8_t*>(malloc(passoLinha));
  uint16_t* linhaSaida =
      static_cast<uint16_t*>(malloc(static_cast<size_t>(larguraSaida) * sizeof(uint16_t)));
  if (linhaOrigem == nullptr || linhaSaida == nullptr) {
    Serial.println("[IHM] BMP: sem memoria para buffer de linha");
    free(linhaOrigem);
    free(linhaSaida);
    armazenamento::fecharBinario();
    return false;
  }

  Serial.printf("[IHM] Lendo BMP %s do SD (%ldx%ld -> %dx%d)\n", nomeComExtensao,
                static_cast<long>(larguraOrigem), static_cast<long>(alturaOrigem), larguraSaida,
                alturaSaida);

  // Limpa a área de destino ANTES de desenhar: a imagem é centralizada
  // preservando a proporção, então uma imagem com proporção diferente da
  // anterior pode não cobrir toda a área, deixando sobras visíveis nas bordas.
  {
    TravaBarramentoDisplay travaBus;
    tft.fillRect(x, y, larguraMaxima, alturaMaxima, COR_FUNDO);
  }

  bool leituraCompleta = true;
  // Ao ampliar, várias linhas de saída vêm da MESMA linha de origem. Guardar
  // qual está no buffer evita reler e reconverter a mesma linha do cartão —
  // numa ampliação de 2x isso corta metade dos acessos ao SD.
  int32_t linhaOrigemEmBuffer = -1;

  for (int16_t linhaSaidaIdx = 0; linhaSaidaIdx < alturaSaida; linhaSaidaIdx++) {
    const int32_t linhaOrigemIdx = static_cast<int32_t>(linhaSaidaIdx * escala);
    // BMP padrão é bottom-up: a primeira linha do arquivo é a ÚLTIMA linha
    // (mais embaixo) da imagem. BITMAPINFOHEADER com altura negativa é
    // top-down (já na ordem de exibição).
    const int32_t linhaArquivo =
        origemTopoParaBase ? linhaOrigemIdx : (alturaOrigem - 1 - linhaOrigemIdx);

    if (linhaArquivo != linhaOrigemEmBuffer) {
      const uint32_t offsetLinha = offsetDados + static_cast<uint32_t>(linhaArquivo) * passoLinha;
      // Fora de qualquer TravaBarramentoDisplay: estas funções tomam o mesmo
      // mutex internamente, e ele não é recursivo — segurá-lo aqui travaria
      // o firmware.
      if (!armazenamento::posicionarBinario(offsetLinha) ||
          armazenamento::lerBinario(linhaOrigem, passoLinha) != passoLinha) {
        Serial.println("[IHM] BMP: falha de leitura no meio do arquivo, interrompendo");
        leituraCompleta = false;
        break;
      }
      linhaOrigemEmBuffer = linhaArquivo;

      for (int16_t colunaSaidaIdx = 0; colunaSaidaIdx < larguraSaida; colunaSaidaIdx++) {
        int32_t colunaOrigemIdx = static_cast<int32_t>(colunaSaidaIdx * escala);
        if (colunaOrigemIdx >= larguraOrigem) colunaOrigemIdx = larguraOrigem - 1;
        const uint8_t* pixel = linhaOrigem + static_cast<uint32_t>(colunaOrigemIdx) * bytesPorPixel;
        // BMP grava BGR(A); RGB565 = RRRRR GGGGGG BBBBB.
        const uint8_t azul = pixel[0];
        const uint8_t verde = pixel[1];
        const uint8_t vermelho = pixel[2];
        linhaSaida[colunaSaidaIdx] = static_cast<uint16_t>(((vermelho & 0xF8) << 8) |
                                                            ((verde & 0xFC) << 3) | (azul >> 3));
      }
    }

    TravaBarramentoDisplay travaBus;
    tft.pushImage(xCentralizado, yCentralizado + linhaSaidaIdx, larguraSaida, 1, linhaSaida);
  }

  free(linhaOrigem);
  free(linhaSaida);
  armazenamento::fecharBinario();
  return leituraCompleta;
}

}  // namespace ihm
