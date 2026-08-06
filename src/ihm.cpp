#include <Arduino.h>
#include <Adafruit_NeoPixel.h>
#include <Arduino_GFX_Library.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "MAIN.HPP"
#include "armazenamento.hpp"
#include "ihm.hpp"
#include "layout.hpp"

namespace ihm {

namespace {

// Dimensões NATIVAS (pré-rotação) do painel — usadas só para construir os
// objetos ST7735/Canvas abaixo. NUNCA usar estas duas constantes para
// limpar/desenhar: o painel opera rotacionado (paisagem), então a
// largura/altura realmente visíveis são display->width()/display->height()
// (já refletem a rotação) — ver bug corrigido em limparFaixa()/
// escreverTelaApp() (uma faixa à direita não era apagada porque essas
// funções limpavam só TFT_LARGURA_NATIVA px, menos que a largura real).
constexpr int16_t TFT_LARGURA_NATIVA = 128;
constexpr int16_t TFT_ALTURA_NATIVA = 160;
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

struct EncoderState {
  int position = 0;
  int lastA = HIGH;
};

// Estado independente do EncoderState acima: reporta só o passo mais
// recente (para navegação em listas), sem posição acumulada/wrap.
struct EventoEncoderState {
  int lastS1 = HIGH;
};

struct TeclaState {
  int leituraAnterior = HIGH;
  int estadoEstavel = HIGH;
  unsigned long ultimaMudancaMs = 0;
};

struct TelaAppState {
  char titulo[24] = "";
  char valor[24] = "";
  char rodape[24] = "";
  bool inicializada = false;
};

EncoderState encoder;
EventoEncoderState eventoEncoder;
TeclaState tecla;
TelaAppState telaApp;
uint8_t volumeAtual = BRILHO_NIVEL_MAXIMO;

// true somente se display->begin() teve sucesso. Quando false, todas as
// funções de desenho abaixo retornam sem tocar no ponteiro do display —
// evita acesso a um periférico que não respondeu, sem travar o restante
// do firmware (encoder, LEDs, sensores, Bluetooth, SD continuam ativos).
bool displayOk = false;

// RAII: toma o mutex do barramento SPI compartilhado (armazenamento.hpp) e,
// só se o dono estiver de fato mudando (do SD para o display), reconfigura
// MOSI/SCK/MISO como GPIO simples — necessário porque o microSD usa o
// periférico de SPI de HARDWARE do ESP32 nos MESMOS pinos físicos (só o CS
// muda) e pode tê-los roteado para si desde o último acesso ao cartão. Sem
// isto, o TFT (Arduino_SWSPI, bit-bang via digitalWrite()) simplesmente
// para de responder fisicamente depois do primeiro SD.begin()/leitura/
// escrita — mesmo com o código de desenho certo — porque o pino deixa de
// obedecer digitalWrite() enquanto restar roteado para o periférico de SPI.
//
// Reconfigurar incondicionalmente a cada chamada (em vez de só quando o
// dono muda) chegou a corromper o cartão na prática ao ler um BMP linha a
// linha (dezenas de reinicializações de SPI por segundo) — ver comentário
// grande em armazenamento.hpp.
class TravaBarramentoDisplay {
 public:
  TravaBarramentoDisplay() {
    armazenamento::travarBarramentoSPI();
    if (!armazenamento::donoAtualEhDisplay()) {
      armazenamento::logDiagnosticoBarramento("ANTES troca SD->Display");
      // Desseleciona o SD (CS em HIGH) ANTES de bit-bangar as linhas
      // compartilhadas — sem isto, se SD_CS_PIN ficasse em LOW durante o
      // bit-bang do TFT, o cartão interpretaria os pulsos de clock como
      // tráfego SPI real endereçado a ele, corrompendo seu estado interno
      // (observado na prática: "sdSelectCard(): Select Failed" logo após
      // o primeiro desenho no display).
      pinMode(SD_CS_PIN, OUTPUT);
      digitalWrite(SD_CS_PIN, HIGH);
      pinMode(TFT_MOSI, OUTPUT);
      pinMode(TFT_SCLK, OUTPUT);
      pinMode(TFT_MISO, INPUT);
      armazenamento::marcarDonoDisplay();
      armazenamento::logDiagnosticoBarramento("DEPOIS troca SD->Display");
    }
  }
  ~TravaBarramentoDisplay() { armazenamento::destravarBarramentoSPI(); }
};

Adafruit_NeoPixel pixels(NUM_LEDS, PIN_NEO, NEO_GRB + NEO_KHZ800);
Arduino_DataBus* bus = new Arduino_SWSPI(TFT_DC, TFT_CS, TFT_SCLK, TFT_MOSI,
										 TFT_MISO);
Arduino_GFX* displayFisico = new Arduino_ST7735(bus, TFT_RST, 1, false, TFT_LARGURA_NATIVA,
										 TFT_ALTURA_NATIVA, 0, 0, 0, 0);
// display aponta para um canvas em RAM (framebuffer), não direto para o
// TFT: todo fillScreen()/fillRect()/print() das funções abaixo escreve só
// na RAM — nada muda na tela física até display->flush() ser chamado,
// sempre como último passo de cada função desenharX()/escreverX(). Sem
// isso, cada redesenho ia direto para o SPI bit-bang (lento) e a tela
// ficava visivelmente preta entre o fillScreen() e o desenho seguinte,
// causando a sensação de "piscado" a cada atualização.
//
// IMPORTANTE: o canvas é criado com as dimensões JÁ ROTACIONADAS (largura x
// altura trocadas em relação ao painel nativo), não TFT_LARGURA_NATIVA x
// TFT_ALTURA_NATIVA — displayFisico acima já tem rotação 1 (paisagem)
// aplicada, então seu width()/height() reais são 160x128, não 128x160. Um
// canvas criado com as dimensões nativas (erro anterior) tem framebuffer
// menor que a área física visível: display->flush() manda um bitmap
// 128x160 para um painel de 160x128, deixando uma faixa de ~32px à direita
// nunca escrita/limpa (o bug de "faixa não apaga no lado direito").
Arduino_GFX* display = new Arduino_Canvas(TFT_ALTURA_NATIVA, TFT_LARGURA_NATIVA, displayFisico);

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
// verdade. Centraliza esse comportamento aqui em vez de duplicar a lógica
// em cada função desenharX()/escreverX() abaixo.
void imprimirTexto(int16_t x, int16_t y, const char* texto) {
  display->setCursor(x, y);
  display->print(texto);
  if (UI_FONTE_NEGRITO) {
    display->setCursor(x + 1, y);
    display->print(texto);
  }
}

void limparFaixa(int16_t y, int16_t altura) {
  // display->width() (não TFT_LARGURA_NATIVA): o painel é usado rotacionado
  // (paisagem) e a largura nativa é menor que a largura real visível —
  // limpar só a largura nativa deixava uma faixa à direita sem apagar.
  display->fillRect(0, y, display->width(), altura, COR_FUNDO);
}

void desenharTextoFaixa(int16_t y, uint8_t tamanho, uint16_t cor,
						 const char* texto) {
  limparFaixa(y, 24);
  display->setTextSize(tamanho);
  display->setTextColor(cor);
  imprimirTexto(10, y, texto);
}

// Trunca "origem" em "destino" para caber em "larguraDisponivelPx", usando
// "..." quando necessário (fonte padrão GFX: ~6px por caractere * tamanho).
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

}  // namespace

void init() {
  Serial.println("[DISPLAY] Inicializacao iniciada");
  Serial.printf("[DISPLAY] TFT_CS: %d\n", TFT_CS);
  Serial.printf("[DISPLAY] TFT_DC: %d\n", TFT_DC);
  Serial.printf("[DISPLAY] TFT_RST: %d\n", TFT_RST);
  Serial.printf("[DISPLAY] TFT_BL: %d\n", TFT_BL);
  Serial.printf("[DISPLAY] TFT_SCLK: %d\n", TFT_SCLK);
  Serial.printf("[DISPLAY] TFT_MOSI: %d\n", TFT_MOSI);
  Serial.printf("[DISPLAY] TFT_MISO: %d\n", TFT_MISO);
  Serial.printf("[DISPLAY] Objeto bus: %p\n", static_cast<void*>(bus));
  Serial.printf("[DISPLAY] Objeto na inicializacao: %p\n", static_cast<void*>(display));

  pinMode(ENC_S1_PIN, INPUT_PULLUP);
  pinMode(ENC_S2_PIN, INPUT_PULLUP);
  pinMode(ENC_KEY_PIN, INPUT_PULLUP);
  pinMode(BUZZER_PIN, OUTPUT);

  // Backlight ligado antes de display->begin(): confirma que o circuito do
  // backlight funciona mesmo que o controlador ST7735 não responda no SPI.
  pinMode(TFT_BL, OUTPUT);
  digitalWrite(TFT_BL, HIGH);
  Serial.printf("[DISPLAY] Backlight configurado (pino %d em HIGH). Estado GPIO: %d\n", TFT_BL,
                digitalRead(TFT_BL));

  Serial.println("[DISPLAY] Executando display->begin()");
  displayOk = display->begin();
  Serial.printf("[DISPLAY] Resultado de begin(): %s\n", displayOk ? "SUCESSO" : "FALHA");

  if (!displayOk) {
    // Sem while(true)/return: registra a falha e deixa o restante do
    // firmware (encoder, LEDs, sensores, Bluetooth, SD) continuar normalmente.
    // Todas as funções de desenho abaixo checam displayOk antes de tocar
    // no ponteiro do display.
    Serial.println("[ERRO][DISPLAY] Inicializacao falhou - display marcado como indisponivel");
    Serial.println("[DISPLAY] Demais modulos do firmware continuarao normalmente");
  } else {
    Serial.printf("[DISPLAY] Resolucao detectada: %d x %d\n", display->width(), display->height());
    Serial.printf("[DISPLAY] Heap apos inicializacao: %u bytes\n",
                  static_cast<unsigned>(ESP.getFreeHeap()));

    display->fillScreen(COR_FUNDO);
    display->flush();
    layout::init(display->width(), display->height());
  }

  if (FORCE_DISPLAY_BACKLIGHT_DIAGNOSTIC) {
    // NÃO anexa o pino ao LEDC: ledcAttachPin() assume o controle do
    // estágio de saída do GPIO e zera o duty até o primeiro ledcWrite(),
    // o que apagaria o backlight mesmo depois do digitalWrite(HIGH) acima.
    // Aqui o pino continua um GPIO simples, já em HIGH.
    Serial.println("[DISPLAY] Diagnostico: backlight em modo GPIO puro (LEDC nao anexado)");
  } else {
    ledcSetup(BRILHO_PWM_CANAL, BRILHO_PWM_FREQ_HZ, BRILHO_PWM_RESOLUCAO_BITS);
    ledcAttachPin(TFT_BL, BRILHO_PWM_CANAL);
  }
  setBrilho(BRILHO_NIVEL_MAXIMO);

  Serial.println("[LEDS] Inicializando NeoPixel");
  Serial.printf("[LEDS] GPIO: %d\n", PIN_NEO);
  Serial.printf("[LEDS] Quantidade: %d\n", NUM_LEDS);
  Serial.printf("[LEDS] Brilho de teste: %u\n", static_cast<unsigned>(LED_STARTUP_BRIGHTNESS));

  pixels.begin();
  pixels.clear();
  pixels.show();

  Serial.println("[LEDS] NeoPixel inicializado");
  Serial.println("[DISPLAY] Inicializacao concluida");
}

bool displayDisponivel() { return displayOk; }

int readEncoder(int maxPosition) {
  const int currentA = digitalRead(ENC_S1_PIN);
  const int currentB = digitalRead(ENC_S2_PIN);

  if (currentA != encoder.lastA) {
    if (encoder.lastA == HIGH && currentA == LOW) {
      if (currentB == HIGH) {
        encoder.position++;
      } else {
        encoder.position--;
      }

      if (maxPosition >= 0) {
        if (encoder.position > maxPosition) encoder.position = 0;
        if (encoder.position < 0) encoder.position = maxPosition;
      }
    }
    encoder.lastA = currentA;
  }

  return encoder.position;
}

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

void escreverTelaApp(const char* titulo, const char* valor, const char* rodape,
					 bool forcarRedesenho) {
  if (!displayOk) return;
  TravaBarramentoDisplay travaBus;

  if (forcarRedesenho || !telaApp.inicializada) {
    Serial.println("[IHM] Limpando tela em escreverTelaApp()");
    display->fillScreen(COR_FUNDO);
    display->fillRect(0, 0, display->width(), 28, COR_CABECALHO);
    display->drawRect(0, 0, display->width(), display->height(), COR_CABECALHO);
    telaApp.inicializada = true;
    telaApp.titulo[0] = '\0';
    telaApp.valor[0] = '\0';
    telaApp.rodape[0] = '\0';
  }

  if (titulo != nullptr && (forcarRedesenho || textoMudou(telaApp.titulo, titulo))) {
    std::strncpy(telaApp.titulo, titulo, sizeof(telaApp.titulo) - 1);
    telaApp.titulo[sizeof(telaApp.titulo) - 1] = '\0';

    display->fillRect(0, 0, display->width(), 28, COR_CABECALHO);
    display->setTextSize(1);
    display->setTextColor(COR_TITULO);
    imprimirTexto(10, 8, telaApp.titulo);
  }

  if (valor != nullptr && (forcarRedesenho || textoMudou(telaApp.valor, valor))) {
    std::strncpy(telaApp.valor, valor, sizeof(telaApp.valor) - 1);
    telaApp.valor[sizeof(telaApp.valor) - 1] = '\0';

    desenharTextoFaixa(48, 2, COR_VALOR, telaApp.valor);
  }

  if (rodape != nullptr && (forcarRedesenho || textoMudou(telaApp.rodape, rodape))) {
    std::strncpy(telaApp.rodape, rodape, sizeof(telaApp.rodape) - 1);
    telaApp.rodape[sizeof(telaApp.rodape) - 1] = '\0';

    desenharTextoFaixa(112, 1, COR_RODAPE, telaApp.rodape);
  }

  display->flush();
}

void escreverTextoTela(const char* texto, int16_t x, int16_t y, uint16_t cor,
                       uint8_t tamanho, bool limparTela) {
  if (!displayOk) return;
  TravaBarramentoDisplay travaBus;

  if (limparTela) {
    Serial.println("[IHM] Limpando tela em escreverTextoTela()");
    display->fillScreen(COR_FUNDO);
  }

  display->setTextColor(cor);
  display->setTextSize(tamanho);
  imprimirTexto(x, y, texto);
  display->flush();
}

EventoEncoder lerEventoEncoder() {
  const int currentS1 = digitalRead(ENC_S1_PIN);
  const int currentS2 = digitalRead(ENC_S2_PIN);

  EventoEncoder evento = EventoEncoder::Nenhum;

  if (currentS1 != eventoEncoder.lastS1) {
    if (eventoEncoder.lastS1 == HIGH && currentS1 == LOW) {
      evento = (currentS2 == HIGH) ? EventoEncoder::Horario : EventoEncoder::AntiHorario;
    }
    eventoEncoder.lastS1 = currentS1;
  }

  return evento;
}

bool teclaClicada() {
  constexpr unsigned long DEBOUNCE_MS = 30;

  const int leituraAtual = digitalRead(ENC_KEY_PIN);
  const unsigned long agora = millis();

  if (leituraAtual != tecla.leituraAnterior) {
    tecla.ultimaMudancaMs = agora;
    tecla.leituraAnterior = leituraAtual;
  }

  bool cliqueDetectado = false;
  if ((agora - tecla.ultimaMudancaMs) >= DEBOUNCE_MS && leituraAtual != tecla.estadoEstavel) {
    const bool estadoAnteriorEraPressionado = (tecla.estadoEstavel == LOW);
    tecla.estadoEstavel = leituraAtual;
    const bool estadoNovoEhSolto = (tecla.estadoEstavel == HIGH);
    if (estadoAnteriorEraPressionado && estadoNovoEhSolto) {
      cliqueDetectado = true;
    }
  }

  return cliqueDetectado;
}

void setBrilho(uint8_t nivel) {
  if (nivel > BRILHO_NIVEL_MAXIMO) nivel = BRILHO_NIVEL_MAXIMO;
  uint32_t duty = (static_cast<uint32_t>(nivel) * 255U) / BRILHO_NIVEL_MAXIMO;
  Serial.printf("[DISPLAY] Brilho solicitado: %u | Duty PWM calculado: %u | Logica invertida: nao\n",
                static_cast<unsigned>(nivel), static_cast<unsigned>(duty));

  if (FORCE_DISPLAY_BACKLIGHT_DIAGNOSTIC) {
    // Ignora o LEDC por completo: o backlight já está em HIGH via
    // digitalWrite() feito em init() — não chama ledcWrite() nesta
    // build de diagnóstico, para eliminar o canal/frequência/resolução
    // do PWM como possível causa de um backlight apagado.
    Serial.println("[DISPLAY] Diagnostico: backlight via digitalWrite HIGH, PWM ignorado");
    return;
  }

  if (FORCE_MAX_BRIGHTNESS_FOR_DIAGNOSTIC) {
    Serial.println("[DISPLAY] Diagnostico: forcando duty PWM maximo (valor nao persistido em NVS)");
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

void desenharCabecalhoRodape(const char* titulo, const char* rodape) {
  if (!displayOk) return;

  const int16_t largura = display->width();
  const int16_t altura = display->height();
  const int16_t alturaCabecalho = layout::uiHeaderHeight();
  const int16_t alturaRodape = layout::uiFooterHeight();
  const uint8_t fonte = layout::uiFontSize(1);

  display->fillRect(0, 0, largura, alturaCabecalho, COR_CABECALHO);

  if (titulo != nullptr) {
    char bufferTitulo[24];
    truncarTexto(bufferTitulo, sizeof(bufferTitulo), titulo,
                 largura - 2 * layout::uiMargin(), fonte);
    // Título do cabeçalho sempre em maiúsculo — destaca "em que tela
    // estou" — centralizado aqui em vez de escrever cada string de
    // título já em maiúsculo em cada tela/chamador. Só ASCII simples
    // (a-z); os títulos do firmware não usam acentos.
    for (char* c = bufferTitulo; *c != '\0'; c++) {
      if (*c >= 'a' && *c <= 'z') *c = static_cast<char>(*c - 'a' + 'A');
    }
    display->setTextSize(fonte);
    display->setTextColor(COR_TITULO);
    imprimirTexto(layout::uiMargin(), alturaCabecalho / 2 - 4, bufferTitulo);
  }

  if (rodape != nullptr) {
    char bufferRodape[24];
    truncarTexto(bufferRodape, sizeof(bufferRodape), rodape,
                 largura - 2 * layout::uiMargin(), fonte);
    display->fillRect(0, altura - alturaRodape, largura, alturaRodape, COR_FUNDO);
    display->setTextSize(fonte);
    display->setTextColor(COR_RODAPE);
    imprimirTexto(layout::uiMargin(), altura - alturaRodape + 2, bufferRodape);
  }
}

void desenharListaMenu(const char* titulo, const char* const* itens, uint8_t quantidade,
                       uint8_t indiceSelecionado, uint8_t offsetRolagem) {
  if (!displayOk) return;
  TravaBarramentoDisplay travaBus;

  static bool ponteiroJaLogado = false;
  if (!ponteiroJaLogado) {
    Serial.printf("[DISPLAY] Objeto no menu principal: %p\n", static_cast<void*>(display));
    ponteiroJaLogado = true;
  }

  Serial.println("[IHM] Limpando tela em desenharListaMenu()");
  display->fillScreen(COR_FUNDO);
  desenharCabecalhoRodape(titulo);

  const uint8_t itensVisiveis = layout::uiItensVisiveis();
  const int16_t yInicial = layout::uiHeaderHeight() + layout::uiMargin();
  const int16_t alturaLinha = layout::uiLineSpacing();
  const uint8_t fonte = layout::uiFontSize(1);

  for (uint8_t linha = 0; linha < itensVisiveis; linha++) {
    const uint8_t indiceItem = offsetRolagem + linha;
    if (indiceItem >= quantidade) break;

    const int16_t y = yInicial + linha * alturaLinha;
    const bool selecionado = (indiceItem == indiceSelecionado);

    if (selecionado) {
      display->fillRect(0, y - 1, display->width(), alturaLinha, COR_SELECIONADO);
    }

    char buffer[32];
    truncarTexto(buffer, sizeof(buffer), itens[indiceItem],
                 display->width() - 2 * layout::uiMargin(), fonte);

    display->setTextSize(fonte);
    display->setTextColor(selecionado ? COR_TEXTO_SELECIONADO : COR_TEXTO);
    imprimirTexto(layout::uiMargin(), y, buffer);
  }

  display->flush();
}

void desenharConfirmacao(const char* pergunta, uint8_t indiceSelecionado) {
  if (!displayOk) return;
  TravaBarramentoDisplay travaBus;

  Serial.println("[IHM] Limpando tela em desenharConfirmacao()");
  display->fillScreen(COR_FUNDO);
  desenharCabecalhoRodape("Confirmar", "KEY confirma");

  const uint8_t fonte = layout::uiFontSize(1);
  const int16_t yPergunta = layout::uiHeaderHeight() + layout::uiMargin();

  char bufferPergunta[40];
  truncarTexto(bufferPergunta, sizeof(bufferPergunta), pergunta,
               display->width() - 2 * layout::uiMargin(), fonte);
  display->setTextSize(fonte);
  display->setTextColor(COR_VALOR);
  imprimirTexto(layout::uiMargin(), yPergunta, bufferPergunta);

  static const char* const opcoes[2] = {"Sim", "Nao"};
  const int16_t yOpcoes = yPergunta + layout::uiLineSpacing() * 2;
  for (uint8_t i = 0; i < 2; i++) {
    const bool selecionado = (i == indiceSelecionado);
    const int16_t y = yOpcoes + i * layout::uiLineSpacing();
    if (selecionado) {
      display->fillRect(0, y - 1, display->width(), layout::uiLineSpacing(), COR_SELECIONADO);
    }
    display->setTextSize(fonte);
    display->setTextColor(selecionado ? COR_TEXTO_SELECIONADO : COR_TEXTO);
    imprimirTexto(layout::uiMargin(), y, opcoes[i]);
  }

  display->flush();
}

void desenharValorEditavel(const char* titulo, int32_t valor, int32_t minimo,
                           int32_t maximo, const char* unidade) {
  if (!displayOk) return;
  TravaBarramentoDisplay travaBus;

  Serial.println("[IHM] Limpando tela em desenharValorEditavel()");
  display->fillScreen(COR_FUNDO);
  desenharCabecalhoRodape(titulo, "Gire para ajustar, KEY confirma");

  const uint8_t fonteValor = layout::uiFontSize(3);
  char textoValor[16];
  if (unidade != nullptr) {
    snprintf(textoValor, sizeof(textoValor), "%ld%s", static_cast<long>(valor), unidade);
  } else {
    snprintf(textoValor, sizeof(textoValor), "%ld", static_cast<long>(valor));
  }

  const int16_t larguraTexto = static_cast<int16_t>(std::strlen(textoValor) * 6 * fonteValor);
  display->setTextSize(fonteValor);
  display->setTextColor(COR_VALOR);
  imprimirTexto(layout::uiCenterX() - larguraTexto / 2, layout::uiCenterY() - 8 * fonteValor / 2,
                textoValor);

  const int16_t barraX = layout::uiMargin();
  const int16_t barraY = display->height() - layout::uiFooterHeight() - layout::uiHeight(14);
  const int16_t barraLargura = display->width() - 2 * layout::uiMargin();
  const int16_t barraAltura = layout::uiHeight(8);

  display->drawRect(barraX, barraY, barraLargura, barraAltura, COR_RODAPE);
  const int32_t faixa = maximo - minimo;
  int16_t preenchido = 0;
  if (faixa > 0 && barraLargura > 2) {
    preenchido = static_cast<int16_t>((static_cast<int64_t>(valor - minimo) * (barraLargura - 2)) / faixa);
  }
  if (preenchido > 0) {
    display->fillRect(barraX + 1, barraY + 1, preenchido, barraAltura - 2, COR_CABECALHO);
  }

  display->flush();
}

void desenharListaRolavel(const char* titulo, const char* const* linhas,
                          uint8_t quantidade, uint8_t offsetRolagem) {
  if (!displayOk) return;
  TravaBarramentoDisplay travaBus;

  Serial.println("[IHM] Limpando tela em desenharListaRolavel()");
  display->fillScreen(COR_FUNDO);
  desenharCabecalhoRodape(titulo, "Role para ver mais");

  const uint8_t itensVisiveis = layout::uiItensVisiveis();
  const int16_t yInicial = layout::uiHeaderHeight() + layout::uiMargin();
  const int16_t alturaLinha = layout::uiLineSpacing();
  const uint8_t fonte = layout::uiFontSize(1);

  display->setTextSize(fonte);
  display->setTextColor(COR_TEXTO);

  for (uint8_t linha = 0; linha < itensVisiveis; linha++) {
    const uint8_t indice = offsetRolagem + linha;
    if (indice >= quantidade) break;

    char buffer[32];
    truncarTexto(buffer, sizeof(buffer), linhas[indice],
                 display->width() - 2 * layout::uiMargin(), fonte);
    imprimirTexto(layout::uiMargin(), yInicial + linha * alturaLinha, buffer);
  }

  display->flush();
}

void desenharGradeModulos(const char* titulo, uint8_t dimensao,
                          bool (*modulo)(uint8_t, uint8_t)) {
  if (!displayOk) return;
  TravaBarramentoDisplay travaBus;

  Serial.println("[IHM] Limpando tela em desenharGradeModulos()");
  display->fillScreen(COR_FUNDO);
  desenharCabecalhoRodape(titulo);

  if (dimensao == 0 || modulo == nullptr) {
    display->flush();
    return;
  }

  const int16_t areaLargura = display->width();
  const int16_t areaAltura = display->height() - layout::uiHeaderHeight() - layout::uiFooterHeight();
  const int16_t ladoDisponivel = (areaLargura < areaAltura) ? areaLargura : areaAltura;

  const int16_t tamanhoCelula = ladoDisponivel / dimensao;
  if (tamanhoCelula <= 0) {
    display->flush();
    return;
  }

  const int16_t ladoGrade = tamanhoCelula * dimensao;
  const int16_t offsetX = (areaLargura - ladoGrade) / 2;
  const int16_t offsetY = layout::uiHeaderHeight() + (areaAltura - ladoGrade) / 2;

  display->fillRect(offsetX, offsetY, ladoGrade, ladoGrade, 0xFFFF);
  for (uint8_t y = 0; y < dimensao; y++) {
    for (uint8_t x = 0; x < dimensao; x++) {
      if (modulo(x, y)) {
        display->fillRect(offsetX + x * tamanhoCelula, offsetY + y * tamanhoCelula, tamanhoCelula,
                           tamanhoCelula, 0x0000);
      }
    }
  }

  display->flush();
}

void desenharTecladoTexto(const char* nomeAtual, const char* const* rotulos, uint8_t quantidade,
                          uint8_t indiceSelecionado) {
  if (!displayOk) return;
  TravaBarramentoDisplay travaBus;

  Serial.println("[IHM] Limpando tela em desenharTecladoTexto()");
  display->fillScreen(COR_FUNDO);

  char titulo[40];
  snprintf(titulo, sizeof(titulo), "Nome: %s", nomeAtual != nullptr ? nomeAtual : "");
  desenharCabecalhoRodape(titulo, "Gire: mover  KEY: escolher");

  if (quantidade == 0 || rotulos == nullptr) {
    display->flush();
    return;
  }

  const uint8_t fonte = layout::uiFontSize(1);
  // ~6px de largura de caractere por unidade de textSize na fonte padrão
  // GFX (mesma estimativa usada em truncarTexto()); cada célula cabe até 2
  // caracteres (rótulos como "OK") mais uma margem interna pequena.
  const int16_t larguraCelula = 6 * fonte * 2 + layout::uiWidth(4);
  const int16_t alturaCelula = 8 * fonte + layout::uiHeight(4);

  const int16_t areaLargura = display->width() - 2 * layout::uiMargin();
  uint8_t colunas = static_cast<uint8_t>(areaLargura / larguraCelula);
  if (colunas < 1) colunas = 1;
  if (colunas > quantidade) colunas = quantidade;

  const int16_t xInicial = layout::uiMargin();
  const int16_t yInicial = layout::uiHeaderHeight() + layout::uiMargin();
  const int16_t yLimite = display->height() - layout::uiFooterHeight();

  display->setTextSize(fonte);

  for (uint8_t i = 0; i < quantidade; i++) {
    const uint8_t linha = i / colunas;
    const uint8_t coluna = i % colunas;
    const int16_t x = xInicial + coluna * larguraCelula;
    const int16_t y = yInicial + linha * alturaCelula;

    // Grade grande demais pra área disponível: corta os últimos símbolos
    // em vez de invadir o rodapé (não deveria acontecer com o alfabeto
    // atual, mas protege contra um alfabeto maior no futuro).
    if (y + alturaCelula > yLimite) break;

    const bool selecionado = (i == indiceSelecionado);
    if (selecionado) {
      display->fillRect(x, y, larguraCelula - 1, alturaCelula - 1, COR_SELECIONADO);
    }
    display->setTextColor(selecionado ? COR_TEXTO_SELECIONADO : COR_TEXTO);
    imprimirTexto(x + 2, y + 2, rotulos[i]);
  }

  display->flush();
}

void desenharMensagem(const char* titulo, const char* mensagem) {
  if (!displayOk) return;
  TravaBarramentoDisplay travaBus;

  Serial.println("[IHM] Limpando tela em desenharMensagem()");
  display->fillScreen(COR_FUNDO);
  desenharCabecalhoRodape(titulo);

  const uint8_t fonte = layout::uiFontSize(1);
  char buffer[64];
  truncarTexto(buffer, sizeof(buffer), mensagem, display->width() - 2 * layout::uiMargin(), fonte);

  display->setTextSize(fonte);
  display->setTextColor(COR_VALOR);
  imprimirTexto(layout::uiMargin(), layout::uiCenterY(), buffer);
  display->flush();
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
  // (ex.: um BMP de altíssima resolução enviado por engano). Cobre
  // confortavelmente imagens de até ~5000px de largura em 32 bits.
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
  // Buffer da imagem de SAÍDA inteira (não só uma linha): lemos todas as
  // linhas do SD primeiro e só depois desenhamos tudo de uma vez no TFT.
  // Alternar dono do barramento (SD <-> display) a cada linha — mesmo só
  // reconfigurando fisicamente quando o dono muda — ainda significava até
  // duas trocas por linha (centenas por imagem); isso corrompeu o cartão
  // na prática (falhas repetidas de CMD13/SEND_STATUS). Com o buffer
  // completo, a troca acontece só 2 vezes no total: uma vez para ler tudo,
  // uma vez para desenhar tudo.
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
    Serial.printf("[IHM] Desenhando BMP %s (leitura completa)\n", nomeComExtensao);
    TravaBarramentoDisplay travaBus;
    // Limpa a área de destino ANTES de desenhar: a imagem é centralizada
    // sem nunca ampliar (preserva proporção), então uma imagem com
    // proporção diferente da anterior pode não cobrir toda a área,
    // deixando sobras da imagem/tela anterior visíveis nas bordas (ex.:
    // a logo da UFRN "sobrepondo" a da Monkey Tech na inicialização).
    display->fillRect(x, y, larguraMaxima, alturaMaxima, COR_FUNDO);
    display->draw16bitRGBBitmap(xCentralizado, yCentralizado, framebuffer, larguraSaida, alturaSaida);
    display->flush();
  }

  free(framebuffer);
  return leituraCompleta;
}

}  // namespace ihm