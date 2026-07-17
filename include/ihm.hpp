#pragma once

#include <stdint.h>

namespace ihm {

void init();

int readEncoder(int maxPosition);

void controlarLED(uint16_t indice, uint8_t vermelho, uint8_t verde, uint8_t azul,
				  uint8_t brilho = 55);

void escreverTelaApp(const char* titulo, const char* valor,
					 const char* rodape = nullptr,
					 bool forcarRedesenho = false);

void escreverTextoTela(const char* texto, int16_t x = 10, int16_t y = 10,
				   uint16_t cor = 0xFFFF, uint8_t tamanho = 2,
				   bool limparTela = false);

}