#pragma once

#include <stdint.h>

// Camada única de layout proporcional: todas as demais telas calculam
// posições/tamanhos a partir daqui, para permitir trocar resolução,
// rotação ou proporção do display alterando apenas estas constantes.
namespace layout {

// Resolução de referência usada para desenhar o layout original (o ST7735
// 128x160 da versão anterior). Mantida de propósito mesmo com o painel
// atual sendo 320x240: é ela que preserva as PROPORÇÕES do desenho
// original — margens, altura de cabeçalho e espaçamento continuam com o
// mesmo peso visual, só que escalados. Trocar o painel não deve mudar o
// desenho, só o tamanho.
constexpr int16_t UI_REFERENCE_WIDTH = 128;
constexpr int16_t UI_REFERENCE_HEIGHT = 160;

// Rotação inicial do display. 2 = o painel (ILI9342, deitado por natureza)
// girado em 180 graus, porque o display é montado de cabeça para baixo.
// Mantido igual a ROTACAO_DISPLAY em ihm.cpp, que é quem de fato aplica.
constexpr uint8_t UI_REFERENCE_ROTATION = 2;

// Margens, cabeçalho/rodapé e espaçamento de referência (na resolução acima).
constexpr int16_t UI_MARGIN = 4;
// Reduzido de 28 para 20 junto com o aumento do alvo de toque: os botoes e
// as linhas ficaram 30% mais altos, e sem devolver espaco em algum lugar
// caberiam so 3 itens por tela. O cabecalho so precisa acomodar uma linha
// de titulo, entao e de onde da para tirar sem custo de uso.
constexpr int16_t UI_HEADER_HEIGHT = 20;
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
