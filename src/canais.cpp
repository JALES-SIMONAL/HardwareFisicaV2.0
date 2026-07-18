#include "canais.hpp"

#include <Preferences.h>
#include <cstdio>

#include "MAIN.HPP"

namespace canais {

namespace {

constexpr const char* NAMESPACE_PREFS = "hwfisica_ch";
constexpr uint8_t VERSAO_ESQUEMA = 1;

Preferences prefs;
ChannelConfig configuracaoCanais[NUM_CHANNELS];

EdgeMode validarModo(uint8_t bruto) {
  if (bruto > static_cast<uint8_t>(EdgeMode::Both)) return EdgeMode::Both;
  return static_cast<EdgeMode>(bruto);
}

const char* chavePreferencia(uint8_t indice0based, char* buffer, size_t tamanho) {
  snprintf(buffer, tamanho, "c%u", static_cast<unsigned>(indice0based));
  return buffer;
}

void salvarCanal(uint8_t indice0based) {
  char chave[8];
  chavePreferencia(indice0based, chave, sizeof(chave));
  prefs.putUChar(chave, static_cast<uint8_t>(configuracaoCanais[indice0based].edgeMode));
}

}  // namespace

void init() {
  prefs.begin(NAMESPACE_PREFS, false);

  const uint8_t versaoSalva = prefs.getUChar("ver", 0);
  if (versaoSalva != VERSAO_ESQUEMA) {
    // Primeira vez ou esquema incompatível: aplica o padrão e persiste.
    restaurarPadrao();
    prefs.putUChar("ver", VERSAO_ESQUEMA);
    return;
  }

  for (uint8_t i = 0; i < NUM_CHANNELS; i++) {
    char chave[8];
    chavePreferencia(i, chave, sizeof(chave));
    const uint8_t bruto = prefs.getUChar(chave, static_cast<uint8_t>(EdgeMode::Both));
    configuracaoCanais[i].edgeMode = validarModo(bruto);
  }
}

EdgeMode obterModo(uint8_t canal1based) {
  if (canal1based == 0 || canal1based > NUM_CHANNELS) return EdgeMode::Both;
  return configuracaoCanais[canal1based - 1].edgeMode;
}

void definirModo(uint8_t canal1based, EdgeMode modo) {
  if (canal1based == 0 || canal1based > NUM_CHANNELS) return;
  const uint8_t indice0based = canal1based - 1;
  configuracaoCanais[indice0based].edgeMode = modo;
  salvarCanal(indice0based);
}

void definirTodos(EdgeMode modo) {
  for (uint8_t i = 0; i < NUM_CHANNELS; i++) {
    configuracaoCanais[i].edgeMode = modo;
    salvarCanal(i);
  }
}

void restaurarPadrao() { definirTodos(EdgeMode::Both); }

bool isTransitionEnabled(EdgeMode modo, bool estadoAnterior, bool estadoNovo) {
  if (estadoAnterior == estadoNovo) return false;

  switch (modo) {
    case EdgeMode::Both:
      return true;
    case EdgeMode::Rising:
      return (!estadoAnterior && estadoNovo);
    case EdgeMode::Falling:
      return (estadoAnterior && !estadoNovo);
    default:
      return false;
  }
}

const char* nomeModo(EdgeMode modo) {
  switch (modo) {
    case EdgeMode::Falling: return "H para L";
    case EdgeMode::Rising: return "L para H";
    case EdgeMode::Both: return "Ambos";
    default: return "?";
  }
}

}  // namespace canais
