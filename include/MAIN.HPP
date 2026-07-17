// Pinos sugeridos do lado esquerdo (ESP32)
#pragma once

#include <Arduino.h>
#include <SPI.h>
#include <SD.h>

//velocidade comunicação serial
constexpr unsigned long SERIAL_BAUD_RATE = 115200;
// pinos do encoder rotativo
constexpr uint8_t ENC_S1_PIN = 32;
constexpr uint8_t ENC_S2_PIN = 33;
constexpr uint8_t ENC_KEY_PIN = 25;

// pino físico dos LEDs endereçáveis (NeoPixel)
constexpr uint8_t PIN_NEO = 21;
constexpr uint16_t NUM_LEDS = 6;

// pinos físicos do display TFT (SPI)
#ifndef TFT_MISO
#define TFT_MISO 19
#endif

#ifndef TFT_MOSI
#define TFT_MOSI 23
#endif

#ifndef TFT_SCLK
#define TFT_SCLK 18
#endif

#ifndef TFT_CS
#define TFT_CS 5
#endif

#ifndef TFT_DC
#define TFT_DC 16
#endif

#ifndef TFT_RST
#define TFT_RST 17
#endif

#ifndef TFT_BL
#define TFT_BL 22
#endif
