#pragma once

#include <stdint.h>

#include "comandos.hpp"

// Única fonte de verdade da configuração dos canais de sensores (modo de
// borda considerado válido para registro). Usada pela IHM local, pelo
// filtro de aquisição e pelo MQTT — sempre pelas mesmas funções.
namespace canais {

using comandos::EdgeMode;

struct ChannelConfig {
  EdgeMode edgeMode = EdgeMode::Both;
};

// Carrega a configuração salva (Preferences/NVS); usa "Ambos" como padrão
// quando não há dados ou quando um valor salvo é inválido.
void init();

// canal1based vai de 1 até NUM_CHANNELS (MAIN.HPP).
EdgeMode obterModo(uint8_t canal1based);
void definirModo(uint8_t canal1based, EdgeMode modo);
void definirTodos(EdgeMode modo);
void restaurarPadrao();

// Regra central de filtragem de eventos: decide se a transição
// estadoAnterior -> estadoNovo deve ser registrada, dado o modo configurado.
bool isTransitionEnabled(EdgeMode modo, bool estadoAnterior, bool estadoNovo);

const char* nomeModo(EdgeMode modo);

}  // namespace canais
