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

// Grava "chave"=valor e confere lendo de volta na mesma chamada. Retorna
// true só se a gravação E a conferência baterem.
bool gravarEConferir(const char* chave, uint8_t valor) {
  const size_t bytesGravados = prefs.putUChar(chave, valor);
  const uint8_t confirmado = prefs.getUChar(chave, 0xFF);
  const bool ok = (bytesGravados == sizeof(uint8_t)) && (confirmado == valor);
  Serial.printf(
      "[DIAG][CANAIS] gravarEConferir: chave=\"%s\" valor=%u bytesGravados=%u confirmado=%u -> %s\n",
      chave, static_cast<unsigned>(valor), static_cast<unsigned>(bytesGravados),
      static_cast<unsigned>(confirmado), ok ? "OK" : "FALHOU");
  return ok;
}

// Causa suspeita da configuração não sobreviver a um desligamento (mesmo
// com bytesGravados aparentemente OK): o handle NVS/Preferences fica
// aberto pela sessão inteira (desde init()) e recebe várias leituras/
// gravações ao longo do tempo — há relatos conhecidos do ESP32 de escritas
// num handle "usado" há muito tempo não sobreviverem de fato a um reset,
// mesmo reportando sucesso. Por isso: fecha e reabre a NVS imediatamente
// antes de CADA gravação de canal, forçando uma sessão nova por escrita
// (equivalente ao caso que já funcionava: a 1ª escrita logo após abrir a
// NVS pela primeira vez). Se mesmo assim a conferência falhar, tenta mais
// uma vez.
void salvarCanal(uint8_t indice0based) {
  char chave[8];
  chavePreferencia(indice0based, chave, sizeof(chave));
  const uint8_t valor = static_cast<uint8_t>(configuracaoCanais[indice0based].edgeMode);

  prefs.end();
  prefs.begin(NAMESPACE_PREFS, false);

  if (!gravarEConferir(chave, valor)) {
    Serial.printf("[DIAG][CANAIS] canal %u: gravacao nao confirmada, reabrindo NVS e tentando de novo\n",
                  static_cast<unsigned>(indice0based + 1));
    prefs.end();
    prefs.begin(NAMESPACE_PREFS, false);
    if (!gravarEConferir(chave, valor)) {
      Serial.printf("[DIAG][CANAIS] canal %u: segunda tentativa tambem falhou\n",
                    static_cast<unsigned>(indice0based + 1));
    }
  }
}

// Log de resumo, legível de relance (diferente das linhas [DIAG] por
// chave/byte acima) — chamado ao final de init() e após qualquer alteração
// de configuração, para conferir rapidamente o estado de todos os canais
// de uma vez só.
void logConfiguracaoAtual(const char* contexto) {
  Serial.printf("[CANAIS] Configuracao atual (%s):", contexto);
  for (uint8_t i = 0; i < NUM_CHANNELS; i++) {
    Serial.printf(" C%u=%s", static_cast<unsigned>(i + 1), nomeModo(configuracaoCanais[i].edgeMode));
  }
  Serial.println();
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
    logConfiguracaoAtual("apos boot, esquema novo");
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
  logConfiguracaoAtual("apos boot");
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
  logConfiguracaoAtual("apos configurar 1 canal");
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
  logConfiguracaoAtual("apos configurar todos os canais");
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
