#pragma once

#include <stdint.h>

// Captura dos canais de sensores via interrupção CHANGE. A ISR faz só o
// mínimo (nível + timestamp + envio para fila); a filtragem por modo de
// borda (canais::isTransitionEnabled) acontece fora da ISR, em
// processarFilaEventos().
namespace aquisicao {

// Configura pinMode + attachInterruptArg(CHANGE) para todos os canais.
void init();

// Drena a fila de eventos brutos e, para cada transição válida (segundo o
// modo configurado em "canais"), chama o callback registrado. Deve ser
// chamada periodicamente fora de uma ISR — hoje a partir da tarefa de IHM;
// na etapa final passa a rodar em uma tarefa dedicada no core 0.
void processarFilaEventos();

// Nível elétrico atual do canal (1..NUM_CHANNELS), independente do modo de
// borda configurado — usado pela tela de teste.
bool nivelAtual(uint8_t canal1based);

// Quantidade de mudanças de nível detectadas desde a inicialização.
uint32_t quantidadeMudancas(uint8_t canal1based);

using CallbackEventoValido = void (*)(uint8_t canal1based, bool novoEstado, int64_t tempoUs);

// Define o único consumidor de eventos válidos (o módulo de experimentos).
void definirCallbackEventoValido(CallbackEventoValido callback);

}  // namespace aquisicao
