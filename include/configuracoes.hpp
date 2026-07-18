#pragma once

#include <stdint.h>

// Configurações de nível de dispositivo (brilho, volume, modo de operação)
// persistidas em Preferences/NVS. Usadas tanto pela IHM local quanto pelo
// MQTT — sempre pelas mesmas funções.
namespace configuracoes {

enum class ModoOperacao : uint8_t { Hardware = 0, App = 1 };

constexpr uint8_t NIVEL_MINIMO = 0;
constexpr uint8_t NIVEL_MAXIMO = 30;

constexpr const char* NOME_EQUIPAMENTO = "HardwareFisica";
constexpr const char* VERSAO_FIRMWARE = "1.0.0";
constexpr const char* AUTOR = "Wilson Douglas Jales Simonal";

// Nenhum endereço real de manual foi fornecido para o QR Code da tela
// Manual — ajuste esta constante quando o manual estiver publicado.
constexpr const char* MANUAL_URL = "http://PLACEHOLDER-CONFIRMAR/manual";

// Carrega brilho/volume/modo salvos (ou usa os padrões) e já aplica
// brilho/volume atuais na IHM.
void init();

uint8_t brilho();
uint8_t volume();
ModoOperacao modoOperacao();

void definirBrilho(uint8_t nivel);
void definirVolume(uint8_t nivel);
void definirModoOperacao(ModoOperacao modo);

}  // namespace configuracoes
