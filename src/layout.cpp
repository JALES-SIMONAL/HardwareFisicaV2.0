#include "layout.hpp"

#include <algorithm>

namespace layout {

namespace {

int16_t larguraTela = UI_REFERENCE_WIDTH;
int16_t alturaTela = UI_REFERENCE_HEIGHT;
float escalaX = 1.0f;
float escalaY = 1.0f;
float escalaMinima = 1.0f;

}  // namespace

void init(int16_t larguraReal, int16_t alturaReal) {
  larguraTela = larguraReal;
  alturaTela = alturaReal;

  escalaX = static_cast<float>(larguraTela) / static_cast<float>(UI_REFERENCE_WIDTH);
  escalaY = static_cast<float>(alturaTela) / static_cast<float>(UI_REFERENCE_HEIGHT);
  escalaMinima = std::min(escalaX, escalaY);
}

int16_t uiX(int16_t valorReferencia) {
  return static_cast<int16_t>(valorReferencia * escalaX);
}

int16_t uiY(int16_t valorReferencia) {
  return static_cast<int16_t>(valorReferencia * escalaY);
}

int16_t uiWidth(int16_t valorReferencia) {
  return static_cast<int16_t>(valorReferencia * escalaX);
}

int16_t uiHeight(int16_t valorReferencia) {
  return static_cast<int16_t>(valorReferencia * escalaY);
}

uint8_t uiFontSize(uint8_t tamanhoReferencia) {
  int16_t escalado = static_cast<int16_t>(tamanhoReferencia * escalaMinima);
  return static_cast<uint8_t>(escalado < 1 ? 1 : escalado);
}

int16_t uiMargin() { return uiWidth(UI_MARGIN); }

int16_t uiCenterX() { return larguraTela / 2; }

int16_t uiCenterY() { return alturaTela / 2; }

int16_t uiHeaderHeight() { return uiHeight(UI_HEADER_HEIGHT); }

int16_t uiFooterHeight() { return uiHeight(UI_FOOTER_HEIGHT); }

int16_t uiLineSpacing() { return uiHeight(UI_LINE_SPACING); }

uint8_t uiItensVisiveis() {
  int16_t areaUtil = alturaTela - uiHeaderHeight() - uiFooterHeight();
  int16_t altura = uiLineSpacing();
  if (altura <= 0) return 1;
  int16_t itens = areaUtil / altura;
  return static_cast<uint8_t>(itens < 1 ? 1 : itens);
}

}  // namespace layout
