#include "configuracoes.hpp"

#include <Preferences.h>

#include "ihm.hpp"

namespace configuracoes {

namespace {

constexpr const char* NAMESPACE_PREFS = "hwfisica_cfg";
constexpr uint8_t BRILHO_PADRAO = 30;
constexpr uint8_t VOLUME_PADRAO = 15;

Preferences prefs;
uint8_t brilhoAtual = BRILHO_PADRAO;
uint8_t volumeAtual = VOLUME_PADRAO;
ModoOperacao modoAtual = ModoOperacao::Hardware;

uint8_t validarNivel(uint8_t valor, uint8_t padrao) {
  if (valor > NIVEL_MAXIMO) return padrao;
  return valor;
}

}  // namespace

void init() {
  prefs.begin(NAMESPACE_PREFS, false);

  brilhoAtual = validarNivel(prefs.getUChar("brilho", BRILHO_PADRAO), BRILHO_PADRAO);
  volumeAtual = validarNivel(prefs.getUChar("volume", VOLUME_PADRAO), VOLUME_PADRAO);

  const uint8_t modoBruto =
      prefs.getUChar("modo", static_cast<uint8_t>(ModoOperacao::Hardware));
  modoAtual = (modoBruto == static_cast<uint8_t>(ModoOperacao::App)) ? ModoOperacao::App
                                                                      : ModoOperacao::Hardware;

  ihm::setBrilho(brilhoAtual);
  ihm::setVolume(volumeAtual);
}

uint8_t brilho() { return brilhoAtual; }
uint8_t volume() { return volumeAtual; }
ModoOperacao modoOperacao() { return modoAtual; }

void definirBrilho(uint8_t nivel) {
  if (nivel > NIVEL_MAXIMO) nivel = NIVEL_MAXIMO;
  brilhoAtual = nivel;
  ihm::setBrilho(brilhoAtual);
  prefs.putUChar("brilho", brilhoAtual);
}

void definirVolume(uint8_t nivel) {
  if (nivel > NIVEL_MAXIMO) nivel = NIVEL_MAXIMO;
  volumeAtual = nivel;
  ihm::setVolume(volumeAtual);
  prefs.putUChar("volume", volumeAtual);
}

void definirModoOperacao(ModoOperacao modo) {
  modoAtual = modo;
  prefs.putUChar("modo", static_cast<uint8_t>(modoAtual));
}

}  // namespace configuracoes
