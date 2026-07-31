#include "canais.hpp"

#include <Arduino.h>
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
  if (bruto > static_cast<uint8_t>(EdgeMode::Disabled)) return EdgeMode::Both;
  return static_cast<EdgeMode>(bruto);
}

const char* chavePreferencia(uint8_t indice0based, char* buffer, size_t tamanho) {
  snprintf(buffer, tamanho, "c%u", static_cast<unsigned>(indice0based));
  return buffer;
}

void salvarCanal(uint8_t indice0based) {
  char chave[8];
  chavePreferencia(indice0based, chave, sizeof(chave));
  const uint8_t valor = static_cast<uint8_t>(configuracaoCanais[indice0based].edgeMode);
  // Diagnóstico temporário (queixa: "configurar todos os canais só
  // salva/aplica no canal 1") — putUChar() devolve a quantidade de bytes
  // gravados; 0 indica falha silenciosa da NVS para aquela chave
  // específica.
  const size_t bytesGravados = prefs.putUChar(chave, valor);
  Serial.printf("[DIAG][CANAIS] salvarCanal: chave=\"%s\" (canal %u) valor=%u bytesGravados=%u\n", chave,
                static_cast<unsigned>(indice0based + 1), static_cast<unsigned>(valor),
                static_cast<unsigned>(bytesGravados));
}

}  // namespace

void init() {
  const bool prefsOk = prefs.begin(NAMESPACE_PREFS, false);
  Serial.printf("[DIAG][CANAIS] prefs.begin(\"%s\", false) = %d\n", NAMESPACE_PREFS,
                static_cast<int>(prefsOk));

  const uint8_t versaoSalva = prefs.getUChar("ver", 0);
  Serial.printf("[DIAG][CANAIS] versaoSalva=%u | VERSAO_ESQUEMA=%u\n",
                static_cast<unsigned>(versaoSalva), static_cast<unsigned>(VERSAO_ESQUEMA));
  if (versaoSalva != VERSAO_ESQUEMA) {
    // Primeira vez ou esquema incompatível: aplica o padrão e persiste.
    Serial.println("[DIAG][CANAIS] Esquema divergente/ausente - aplicando padrao (restaurarPadrao)");
    restaurarPadrao();
    prefs.putUChar("ver", VERSAO_ESQUEMA);
    return;
  }

  for (uint8_t i = 0; i < NUM_CHANNELS; i++) {
    char chave[8];
    chavePreferencia(i, chave, sizeof(chave));
    const uint8_t bruto = prefs.getUChar(chave, static_cast<uint8_t>(EdgeMode::Both));
    configuracaoCanais[i].edgeMode = validarModo(bruto);
    Serial.printf("[DIAG][CANAIS] init: chave=\"%s\" (canal %u) lido=%u -> validado=%u\n", chave,
                  static_cast<unsigned>(i + 1), static_cast<unsigned>(bruto),
                  static_cast<unsigned>(configuracaoCanais[i].edgeMode));
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
  Serial.printf("[DIAG][CANAIS] definirTodos(%u) chamado | NUM_CHANNELS=%u\n",
                static_cast<unsigned>(modo), static_cast<unsigned>(NUM_CHANNELS));
  for (uint8_t i = 0; i < NUM_CHANNELS; i++) {
    configuracaoCanais[i].edgeMode = modo;
    salvarCanal(i);
  }
  for (uint8_t i = 0; i < NUM_CHANNELS; i++) {
    Serial.printf("[DIAG][CANAIS] pos-definirTodos: RAM canal %u = %u\n", static_cast<unsigned>(i + 1),
                  static_cast<unsigned>(configuracaoCanais[i].edgeMode));
  }
}

void restaurarPadrao() { definirTodos(EdgeMode::Both); }

bool isTransitionEnabled(EdgeMode modo, bool estadoAnterior, bool estadoNovo) {
  if (estadoAnterior == estadoNovo) return false;
  if (modo == EdgeMode::Disabled) return false;

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
    case EdgeMode::Disabled: return "Desabilitado";
    default: return "?";
  }
}

}  // namespace canais
