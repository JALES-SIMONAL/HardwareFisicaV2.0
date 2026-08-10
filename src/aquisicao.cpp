#include "aquisicao.hpp"

#include <Arduino.h>
#include <esp_timer.h>

#include "MAIN.HPP"
#include "canais.hpp"

namespace aquisicao {

namespace {

// Nível "cru" mais recente e contador de mudanças por canal. Escritos só
// por processarFilaEventos() (núcleo 0), lidos por outras funções deste
// módulo a partir de qualquer núcleo (ex.: tela de teste de canais, no
// núcleo 1) — volatile por causa dessa leitura entre núcleos, não por
// causa de uma ISR (não há mais nenhuma neste arquivo).
volatile bool niveisAtuais[NUM_CHANNELS] = {};
volatile uint32_t contadoresMudancas[NUM_CHANNELS] = {};

CallbackEventoValido callbackEventoValido = nullptr;

}  // namespace

void init() {
  for (uint8_t i = 0; i < NUM_CHANNELS; i++) {
    // Pull interno (ver CHANNEL_PULLUP em MAIN.HPP): estabiliza o nível
    // ocioso quando não há sensor conectado a este canal, em vez de
    // deixar o pino totalmente flutuante (sujeito a ruído/transições
    // falsas). Não atrapalha um sensor que já aciona ativamente os dois
    // níveis — é um resistor fraco, facilmente sobrescrito.
    pinMode(CHANNEL_PINS[i], CHANNEL_PULLUP ? INPUT_PULLUP : INPUT_PULLDOWN);
    niveisAtuais[i] = (digitalRead(CHANNEL_PINS[i]) == HIGH);
    contadoresMudancas[i] = 0;
  }
}

// Leitura por polling (digitalRead() comparado ao nível anterior), em vez
// de interrupção CHANGE + fila + debounce por tempo — mesmo modelo do
// firmware de referência (cronometroV7.ino), que na prática respondeu
// melhor a eventos rápidos sem registrar transições duplas por ruído. Uma
// interrupção CHANGE dispara para QUALQUER borda elétrica, inclusive
// ringing/ruído de nanossegundos que um polling periódico simplesmente
// nunca chega a amostrar; um debounce por tempo tentava compensar isso na
// ISR, mas cortar essa janela também arriscava engolir a borda de retorno
// de um evento real rápido — a troca de arquitetura evita esse dilema por
// completo. Chamada a cada iteração da tarefa do núcleo 0 (main.cpp), sem
// delay artificial além do vTaskDelay(1) mínimo entre iterações.
void processarFilaEventos() {
  for (uint8_t i = 0; i < NUM_CHANNELS; i++) {
    const bool estadoAnterior = niveisAtuais[i];
    const bool estadoNovo = digitalRead(CHANNEL_PINS[i]) == HIGH;
    if (estadoNovo == estadoAnterior) continue;

    const int64_t tempoUs = esp_timer_get_time();
    niveisAtuais[i] = estadoNovo;
    contadoresMudancas[i]++;

    const uint8_t canal1based = i + 1;
    const canais::EdgeMode modo = canais::obterModo(canal1based);
    if (canais::isTransitionEnabled(modo, estadoAnterior, estadoNovo)) {
      if (callbackEventoValido != nullptr) {
        callbackEventoValido(canal1based, estadoNovo, tempoUs);
      }
    }
  }
}

bool nivelAtual(uint8_t canal1based) {
  if (canal1based == 0 || canal1based > NUM_CHANNELS) return false;
  return niveisAtuais[canal1based - 1];
}

uint32_t quantidadeMudancas(uint8_t canal1based) {
  if (canal1based == 0 || canal1based > NUM_CHANNELS) return 0;
  return contadoresMudancas[canal1based - 1];
}

void definirCallbackEventoValido(CallbackEventoValido callback) { callbackEventoValido = callback; }

}  // namespace aquisicao
