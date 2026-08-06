#include "layout.hpp"

#include <algorithm>

#include "MAIN.HPP"

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
  // Arredondado em DUAS etapas, não numa conta só — a fonte (Adafruit GFX)
  // só aceita tamanho inteiro (1x, 2x, 3x...), e nesta tela escalaMinima é
  // 0.8 (a tela real é mais "achatada" que a resolução de referência).
  // Multiplicar tudo de uma vez e arredondar só no final perde a fração do
  // multiplicador: 1 * 0.8 * 1.5 = 1.2, que arredonda pra 1 de novo — sem
  // efeito nenhum, mesmo com um multiplicador > 1. Arredondando o tamanho
  // "natural" primeiro (sem o multiplicador) e só então aplicando
  // UI_FONT_SIZE_MULTIPLICADOR sobre esse inteiro, 1.5 já produz uma
  // mudança real (ex.: tamanho natural 1 * 1.5 = 1.5, que arredonda pra 2).
  const int16_t base = static_cast<int16_t>(tamanhoReferencia * escalaMinima + 0.5f);
  const int16_t baseClampado = base < 1 ? 1 : base;

  const int16_t resultado = static_cast<int16_t>(baseClampado * UI_FONT_SIZE_MULTIPLICADOR + 0.5f);
  return static_cast<uint8_t>(resultado < 1 ? 1 : resultado);
}

int16_t uiMargin() { return uiWidth(UI_MARGIN); }

int16_t uiCenterX() { return larguraTela / 2; }

int16_t uiCenterY() { return alturaTela / 2; }

// Cabeçalho/rodapé/espaçamento entre linhas: usam o MAIOR valor entre a
// constante de referência escalada normalmente e um piso calculado a
// partir do tamanho de fonte REAL (8px por unidade de textSize na fonte
// padrão GFX, mais uma margem) — sem isto, aumentar
// UI_FONT_SIZE_MULTIPLICADOR (MAIN.HPP) deixava o texto maior sem também
// abrir mais espaço entre linhas/cabeçalho, sobrepondo o conteúdo.
int16_t uiHeaderHeight() {
  const int16_t alturaFonte = 8 * static_cast<int16_t>(uiFontSize(1));
  return std::max<int16_t>(uiHeight(UI_HEADER_HEIGHT), alturaFonte + 8);
}

int16_t uiFooterHeight() {
  const int16_t alturaFonte = 8 * static_cast<int16_t>(uiFontSize(1));
  return std::max<int16_t>(uiHeight(UI_FOOTER_HEIGHT), alturaFonte + 6);
}

int16_t uiLineSpacing() {
  const int16_t alturaFonte = 8 * static_cast<int16_t>(uiFontSize(1));
  return std::max<int16_t>(uiHeight(UI_LINE_SPACING), alturaFonte + 4);
}

uint8_t uiItensVisiveis() {
  int16_t areaUtil = alturaTela - uiHeaderHeight() - uiFooterHeight();
  int16_t altura = uiLineSpacing();
  if (altura <= 0) return 1;
  int16_t itens = areaUtil / altura;
  return static_cast<uint8_t>(itens < 1 ? 1 : itens);
}

}  // namespace layout
