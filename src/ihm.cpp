#include <Arduino.h>
#include <Adafruit_NeoPixel.h>
#include <Arduino_GFX_Library.h>
#include <cstdio>
#include <cstring>

#include "MAIN.HPP"
#include "armazenamento.hpp"
#include "ihm.hpp"
#include "layout.hpp"

namespace ihm {

namespace {

constexpr int16_t TFT_LARGURA = 128;
constexpr int16_t TFT_ALTURA = 160;
constexpr uint16_t COR_FUNDO = 0x0000;
constexpr uint16_t COR_CABECALHO = 0x07E0;
constexpr uint16_t COR_TITULO = 0xFFFF;
constexpr uint16_t COR_VALOR = 0xFFE0;
constexpr uint16_t COR_RODAPE = 0xC618;
constexpr uint16_t COR_TEXTO = 0xFFFF;
constexpr uint16_t COR_SELECIONADO = 0x07E0;

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
// do firmware (encoder, LEDs, sensores, MQTT, SD continuam ativos).
bool displayOk = false;

// RAII: toma o mutex do barramento SPI compartilhado (armazenamento.hpp) e
// reconfigura MOSI/SCK/MISO como GPIO simples — necessário porque o
// microSD usa o periférico de SPI de HARDWARE do ESP32 nos MESMOS pinos
// físicos (só o CS muda) e pode tê-los roteado para si desde o último
// acesso ao cartão. Sem isto, o TFT (Arduino_SWSPI, bit-bang via
// digitalWrite()) simplesmente para de responder fisicamente depois do
// primeiro SD.begin()/leitura/escrita — mesmo com o código de desenho
// certo — porque o pino deixa de obedecer digitalWrite() enquanto restar
// roteado para o periférico de SPI.
class TravaBarramentoDisplay {
 public:
  TravaBarramentoDisplay() {
    armazenamento::travarBarramentoSPI();
    pinMode(TFT_MOSI, OUTPUT);
    pinMode(TFT_SCLK, OUTPUT);
    pinMode(TFT_MISO, INPUT);
  }
  ~TravaBarramentoDisplay() { armazenamento::destravarBarramentoSPI(); }
};

Adafruit_NeoPixel pixels(NUM_LEDS, PIN_NEO, NEO_GRB + NEO_KHZ800);
Arduino_DataBus* bus = new Arduino_SWSPI(TFT_DC, TFT_CS, TFT_SCLK, TFT_MOSI,
										 TFT_MISO);
Arduino_GFX* display = new Arduino_ST7735(bus, TFT_RST, 1, false, TFT_LARGURA,
										 TFT_ALTURA, 0, 0, 0, 0);

bool textoMudou(const char* atual, const char* novoTexto) {
  if (atual == nullptr && novoTexto == nullptr) {
    return false;
  }

  if (atual == nullptr || novoTexto == nullptr) {
    return true;
  }

  return std::strcmp(atual, novoTexto) != 0;
}

void limparFaixa(int16_t y, int16_t altura) {
  display->fillRect(0, y, TFT_LARGURA, altura, COR_FUNDO);
}

void desenharTextoFaixa(int16_t y, uint8_t tamanho, uint16_t cor,
						 const char* texto) {
  limparFaixa(y, 24);
  display->setCursor(10, y);
  display->setTextSize(tamanho);
  display->setTextColor(cor);
  display->print(texto);
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
    // firmware (encoder, LEDs, sensores, MQTT, SD) continuar normalmente.
    // Todas as funções de desenho abaixo checam displayOk antes de tocar
    // no ponteiro do display.
    Serial.println("[ERRO][DISPLAY] Inicializacao falhou - display marcado como indisponivel");
    Serial.println("[DISPLAY] Demais modulos do firmware continuarao normalmente");
  } else {
    Serial.printf("[DISPLAY] Resolucao detectada: %d x %d\n", display->width(), display->height());
    Serial.printf("[DISPLAY] Heap apos inicializacao: %u bytes\n",
                  static_cast<unsigned>(ESP.getFreeHeap()));

    display->fillScreen(COR_FUNDO);
    layout::init(display->width(), display->height());
    // O teste visual (vermelho/verde/azul/"Display OK") NÃO roda mais
    // aqui: ele é uma etapa temporizada da própria sequência de boot
    // (maquina_estados::EtapaBoot::DiagnosticoDisplay), disparada via
    // ihm::executarDiagnosticoVisual() e limitada por
    // DISPLAY_DIAGNOSTIC_DURATION_MS — assim a tela sempre avança
    // sozinha para o logotipo/menu, controlada por millis(), em vez de
    // depender de quanto tempo o resto de setup() demora para rodar.
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

void executarDiagnosticoVisual() {
  if (!displayOk) {
    Serial.println("[DISPLAY] Diagnostico visual cancelado (display indisponivel)");
    return;
  }

  Serial.printf("[DISPLAY] Objeto no diagnostico: %p\n", static_cast<void*>(display));
  TravaBarramentoDisplay travaBus;
  Serial.println("[DISPLAY] Teste visual integrado iniciado");

  display->fillScreen(RGB565_RED);
  Serial.println("[DISPLAY] Fundo vermelho enviado");
  delay(300);

  display->fillScreen(RGB565_GREEN);
  Serial.println("[DISPLAY] Fundo verde enviado");
  delay(300);

  display->fillScreen(RGB565_BLUE);
  Serial.println("[DISPLAY] Fundo azul enviado");
  delay(300);

  display->fillScreen(RGB565_BLACK);
  display->setCursor(5, 10);
  display->setTextColor(RGB565_WHITE);
  display->setTextSize(1);
  display->println("HardwareFisica");
  display->setCursor(5, 25);
  display->setTextColor(RGB565_YELLOW);
  display->println("Display OK");

  Serial.println("[DISPLAY] Teste visual integrado concluido");
}

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
    display->fillRect(0, 0, TFT_LARGURA, 28, COR_CABECALHO);
    display->drawRect(0, 0, TFT_LARGURA, TFT_ALTURA, COR_CABECALHO);
    telaApp.inicializada = true;
    telaApp.titulo[0] = '\0';
    telaApp.valor[0] = '\0';
    telaApp.rodape[0] = '\0';
  }

  if (titulo != nullptr && (forcarRedesenho || textoMudou(telaApp.titulo, titulo))) {
    std::strncpy(telaApp.titulo, titulo, sizeof(telaApp.titulo) - 1);
    telaApp.titulo[sizeof(telaApp.titulo) - 1] = '\0';

    display->fillRect(0, 0, TFT_LARGURA, 28, COR_CABECALHO);
    display->setCursor(10, 8);
    display->setTextSize(1);
    display->setTextColor(COR_TITULO);
    display->print(telaApp.titulo);
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
  display->setCursor(x, y);
  display->print(texto);
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
    display->setTextSize(fonte);
    display->setTextColor(COR_TITULO);
    display->setCursor(layout::uiMargin(), alturaCabecalho / 2 - 4);
    display->print(bufferTitulo);
  }

  if (rodape != nullptr) {
    char bufferRodape[24];
    truncarTexto(bufferRodape, sizeof(bufferRodape), rodape,
                 largura - 2 * layout::uiMargin(), fonte);
    display->fillRect(0, altura - alturaRodape, largura, alturaRodape, COR_FUNDO);
    display->setTextSize(fonte);
    display->setTextColor(COR_RODAPE);
    display->setCursor(layout::uiMargin(), altura - alturaRodape + 2);
    display->print(bufferRodape);
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
    display->setTextColor(selecionado ? COR_FUNDO : COR_TEXTO);
    display->setCursor(layout::uiMargin(), y);
    display->print(buffer);
  }
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
  display->setCursor(layout::uiMargin(), yPergunta);
  display->print(bufferPergunta);

  static const char* const opcoes[2] = {"Sim", "Nao"};
  const int16_t yOpcoes = yPergunta + layout::uiLineSpacing() * 2;
  for (uint8_t i = 0; i < 2; i++) {
    const bool selecionado = (i == indiceSelecionado);
    const int16_t y = yOpcoes + i * layout::uiLineSpacing();
    if (selecionado) {
      display->fillRect(0, y - 1, display->width(), layout::uiLineSpacing(), COR_SELECIONADO);
    }
    display->setTextSize(fonte);
    display->setTextColor(selecionado ? COR_FUNDO : COR_TEXTO);
    display->setCursor(layout::uiMargin(), y);
    display->print(opcoes[i]);
  }
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
  display->setCursor(layout::uiCenterX() - larguraTexto / 2, layout::uiCenterY() - 8 * fonteValor / 2);
  display->print(textoValor);

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
    display->setCursor(layout::uiMargin(), yInicial + linha * alturaLinha);
    display->print(buffer);
  }
}

void desenharGradeModulos(const char* titulo, uint8_t dimensao,
                          bool (*modulo)(uint8_t, uint8_t)) {
  if (!displayOk) return;
  TravaBarramentoDisplay travaBus;

  Serial.println("[IHM] Limpando tela em desenharGradeModulos()");
  display->fillScreen(COR_FUNDO);
  desenharCabecalhoRodape(titulo);

  if (dimensao == 0 || modulo == nullptr) return;

  const int16_t areaLargura = display->width();
  const int16_t areaAltura = display->height() - layout::uiHeaderHeight() - layout::uiFooterHeight();
  const int16_t ladoDisponivel = (areaLargura < areaAltura) ? areaLargura : areaAltura;

  const int16_t tamanhoCelula = ladoDisponivel / dimensao;
  if (tamanhoCelula <= 0) return;

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
  display->setCursor(layout::uiMargin(), layout::uiCenterY());
  display->print(buffer);
}

}  // namespace ihm