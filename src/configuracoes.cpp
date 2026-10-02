#include "configuracoes.hpp"

#include <Preferences.h>
#include <cctype>
#include <cstring>

#include "ihm.hpp"

namespace configuracoes {

namespace {

// Comparação sem diferenciar maiúsculas/minúsculas: o editor de texto local
// (teclado de toque) só produz letras maiúsculas (ver ALFABETO_NOME em
// maquina_estados.cpp), então a senha padrão "fisica123" (minúscula, como
// definida pelo app) precisa continuar validando mesmo digitada
// "FISICA123" no equipamento.
bool senhasIguaisSemCase(const char* a, const char* b) {
  if (a == nullptr || b == nullptr) return false;
  while (*a != '\0' && *b != '\0') {
    if (std::tolower(static_cast<unsigned char>(*a)) != std::tolower(static_cast<unsigned char>(*b))) {
      return false;
    }
    a++;
    b++;
  }
  return *a == *b;
}

constexpr const char* NAMESPACE_PREFS = "hwfisica_cfg";
constexpr uint8_t BRILHO_PADRAO = 30;
constexpr uint8_t VOLUME_PADRAO = 15;
constexpr bool ANALISE_DADOS_HABILITADA_PADRAO = true;

Preferences prefs;
uint8_t brilhoAtual = BRILHO_PADRAO;
uint8_t volumeAtual = VOLUME_PADRAO;
ModoOperacao modoAtual = ModoOperacao::Hardware;
bool analiseDadosHabilitadaAtual = ANALISE_DADOS_HABILITADA_PADRAO;
char senhaAtual[SENHA_TAMANHO_MAXIMO + 1] = "";

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

  analiseDadosHabilitadaAtual = prefs.getBool("analise_on", ANALISE_DADOS_HABILITADA_PADRAO);

  const String senhaSalva = prefs.getString("senha", SENHA_PADRAO);
  std::strncpy(senhaAtual, senhaSalva.c_str(), sizeof(senhaAtual) - 1);
  senhaAtual[sizeof(senhaAtual) - 1] = '\0';
  // Senha salva inválida (versão antiga do NVS, corrompida etc.): melhor
  // voltar pro padrão do que ficar com uma senha vazia/impossível de digitar.
  const size_t comprimento = std::strlen(senhaAtual);
  if (comprimento < SENHA_TAMANHO_MINIMO || comprimento > SENHA_TAMANHO_MAXIMO) {
    std::strncpy(senhaAtual, SENHA_PADRAO, sizeof(senhaAtual) - 1);
    senhaAtual[sizeof(senhaAtual) - 1] = '\0';
  }

  ihm::setBrilho(brilhoAtual);
  ihm::setVolume(volumeAtual);
}

uint8_t brilho() { return brilhoAtual; }
uint8_t volume() { return volumeAtual; }
ModoOperacao modoOperacao() { return modoAtual; }
bool analiseDadosHabilitada() { return analiseDadosHabilitadaAtual; }

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

void definirAnaliseDadosHabilitada(bool habilitada) {
  analiseDadosHabilitadaAtual = habilitada;
  prefs.putBool("analise_on", habilitada);
}

bool validarSenha(const char* tentativa) { return senhasIguaisSemCase(tentativa, senhaAtual); }

bool definirSenha(const char* novaSenha) {
  if (novaSenha == nullptr) return false;
  const size_t comprimento = std::strlen(novaSenha);
  if (comprimento < SENHA_TAMANHO_MINIMO || comprimento > SENHA_TAMANHO_MAXIMO) return false;

  std::strncpy(senhaAtual, novaSenha, sizeof(senhaAtual) - 1);
  senhaAtual[sizeof(senhaAtual) - 1] = '\0';
  prefs.putString("senha", senhaAtual);
  return true;
}

}  // namespace configuracoes
