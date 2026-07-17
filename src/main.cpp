#include <Arduino.h>

#include "MAIN.HPP"
#include "ihm.hpp"


void setup() {
  Serial.begin(SERIAL_BAUD_RATE);
  while (!Serial) {
    ;
  }

  pinMode(ENC_S1_PIN, INPUT_PULLUP);
  pinMode(ENC_S2_PIN, INPUT_PULLUP);
  pinMode(ENC_KEY_PIN, INPUT_PULLUP);

  ihm::init();
  ihm::escreverTelaApp("HardwareFisica", "IHM pronta",
					 "Encoder aguardando", true);

  Serial.println("Encoder pronto");
}

void loop() {
  const int posicaoAtual = ihm::readEncoder(NUM_LEDS - 1);

  static int lastPrinted = -1;
  if (posicaoAtual != lastPrinted) {
    Serial.print("Posição atual: ");
    Serial.println(posicaoAtual);

    char linha[24];
    snprintf(linha, sizeof(linha), "Encoder: %d", posicaoAtual);
    ihm::escreverTelaApp("HardwareFisica", linha, "Gire o encoder");

    for (uint16_t i = 0; i < NUM_LEDS; i++) {
      ihm::controlarLED(i, 0, 0, 0, 30);
    }
    ihm::controlarLED(static_cast<uint16_t>(posicaoAtual), 0, 250, 0, 160);

    lastPrinted = posicaoAtual;
  }
}