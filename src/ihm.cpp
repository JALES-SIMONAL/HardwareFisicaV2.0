#include <Arduino.h>
#include <Adafruit_NeoPixel.h>

#include "MAIN.HPP"
#include "ihm.hpp"

namespace ihm {

namespace {

struct EncoderState {
  int position = 0;
  int lastA = HIGH;
};

EncoderState encoder;

Adafruit_NeoPixel pixels(NUM_LEDS, PIN_NEO, NEO_GRB + NEO_KHZ800);
TFT_eSPI tft = TFT_eSPI();

}  // namespace

void init() {
  SPI.begin(TFT_SCLK, TFT_MISO, TFT_MOSI, TFT_CS);
  tft.init();
  tft.setRotation(1);
  tft.fillScreen(TFT_BLACK);

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

void escreverTextoTela(const char* texto, int16_t x, int16_t y, uint16_t cor,
                       uint8_t tamanho, bool limparTela) {
  if (limparTela) {
    tft.fillScreen(TFT_BLACK);
  }

  tft.setTextColor(cor, TFT_BLACK);
  tft.setTextSize(tamanho);
  tft.setCursor(x, y);
  tft.print(texto);
}

}  // namespace ihm