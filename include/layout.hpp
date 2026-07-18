#pragma once

#include <stdint.h>

// Camada única de layout proporcional: todas as demais telas calculam
// posições/tamanhos a partir daqui, para permitir trocar resolução,
// rotação ou proporção do display alterando apenas estas constantes.
namespace layout {

// Resolução de referência usada para desenhar o layout original.
constexpr int16_t UI_REFERENCE_WIDTH = 128;
constexpr int16_t UI_REFERENCE_HEIGHT = 160;

// Rotação inicial do display (mesma convenção do Arduino_GFX_Library).
constexpr uint8_t UI_REFERENCE_ROTATION = 1;

// Margens, cabeçalho/rodapé e espaçamento de referência (na resolução acima).
constexpr int16_t UI_MARGIN = 4;
constexpr int16_t UI_HEADER_HEIGHT = 28;
constexpr int16_t UI_FOOTER_HEIGHT = 16;
constexpr int16_t UI_LINE_SPACING = 12;

// Deve ser chamada uma única vez, depois de display->begin(), informando a
// largura/altura reais do painel em uso.
void init(int16_t larguraReal, int16_t alturaReal);

// Conversão de coordenadas/tamanhos da resolução de referência para a tela real.
int16_t uiX(int16_t valorReferencia);
int16_t uiY(int16_t valorReferencia);
int16_t uiWidth(int16_t valorReferencia);
int16_t uiHeight(int16_t valorReferencia);

// Tamanho de fonte proporcional (usa a menor escala entre os eixos, para não
// deformar texto/ícones).
uint8_t uiFontSize(uint8_t tamanhoReferencia);

int16_t uiMargin();
int16_t uiCenterX();
int16_t uiCenterY();
int16_t uiHeaderHeight();
int16_t uiFooterHeight();
int16_t uiLineSpacing();

// Quantidade de itens de lista que cabem na área útil (entre cabeçalho e
// rodapé) para uma dada altura de linha proporcional — usado para decidir
// quando um menu precisa de rolagem.
uint8_t uiItensVisiveis();

}  // namespace layout
