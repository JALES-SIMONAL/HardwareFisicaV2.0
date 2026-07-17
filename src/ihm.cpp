#include <Arduino.h>
#include <Adafruit_NeoPixel.h>
#include <Arduino_GFX_Library.h>
#include <cstring>

#include "MAIN.HPP"
#include "ihm.hpp"

namespace ihm {

namespace {

constexpr int16_t TFT_LARGURA = 128;
constexpr int16_t TFT_ALTURA = 160;
constexpr uint16_t COR_FUNDO = 0x0000;
constexpr uint16_t COR_CABECALHO = 0x07E0;
constexpr uint16_t COR_TITULO = 0xFFFF;
constexpr uint16_t COR_VALOR = 0xFFE0;
constexpr uint16_t COR_RODAPE = 0xC618;

struct EncoderState {
  int position = 0;
  int lastA = HIGH;
};

struct TelaAppState {
  char titulo[24] = "";
  char valor[24] = "";
  char rodape[24] = "";
  bool inicializada = false;
};

EncoderState encoder;
TelaAppState telaApp;

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

}  // namespace

void init() {
  pinMode(TFT_BL, OUTPUT);
  digitalWrite(TFT_BL, HIGH);

  if (!display->begin()) {
    Serial.println("Falha ao iniciar o display");
    while (true) {
      delay(1000);
    }
  }

  display->fillScreen(COR_FUNDO);

  pixels.begin();
  pixels.clear();
  pixels.show();
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
    Serial.println("Indice de LED invalido");
    return;
  }

  pixels.setBrightness(brilho);
  pixels.setPixelColor(indice, pixels.Color(vermelho, verde, azul));
  pixels.show();
}

void escreverTelaApp(const char* titulo, const char* valor, const char* rodape,
					 bool forcarRedesenho) {
  if (forcarRedesenho || !telaApp.inicializada) {
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
  if (limparTela) {
    display->fillScreen(COR_FUNDO);
  }

  display->setTextColor(cor);
  display->setTextSize(tamanho);
  display->setCursor(x, y);
  display->print(texto);
}

}  // namespace ihm