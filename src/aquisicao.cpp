#include "aquisicao.hpp"

#include <Arduino.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

#include "MAIN.HPP"
#include "canais.hpp"

namespace aquisicao {

namespace {

struct RawEdgeEvent {
  uint8_t canal0based;
  bool estadoAnterior;
  bool estadoNovo;
  int64_t tempoUs;
};

QueueHandle_t filaEventosBrutos = nullptr;

// Acessados pela ISR: nível "cru" mais recente e contador de mudanças por
// canal. São contadores/flags simples de melhor esforço para a UI — não
// exigem seção crítica porque cada canal só é escrito pela sua própria ISR.
volatile bool niveisAtuais[NUM_CHANNELS] = {};
volatile uint32_t contadoresMudancas[NUM_CHANNELS] = {};

// Timestamp (esp_timer_get_time()) da última transição ACEITA de cada
// canal — usado só para o filtro de debounce abaixo, não confundir com
// niveisAtuais/contadoresMudancas (que refletem o nível bruto do pino).
volatile int64_t ultimoEventoAceitoUs[NUM_CHANNELS] = {};

CallbackEventoValido callbackEventoValido = nullptr;

void IRAM_ATTR isrCanal(void* arg) {
  const uint8_t indice0based = static_cast<uint8_t>(reinterpret_cast<uintptr_t>(arg));
  const int64_t tempoUs = esp_timer_get_time();

  // Debounce: ignora qualquer transição que aconteça a menos de
  // SENSOR_DEBOUNCE_US da última transição aceita neste canal — filtra
  // ruído elétrico rápido/bounce mecânico sem exigir um timer separado.
  // Deliberadamente simples (só compara timestamps) para manter a ISR
  // curta; uma transição real e uma de ruído próximas no tempo demais
  // para o sensor em uso podem exigir ajustar SENSOR_DEBOUNCE_US.
  if ((tempoUs - ultimoEventoAceitoUs[indice0based]) < static_cast<int64_t>(SENSOR_DEBOUNCE_US)) {
    return;
  }

  const bool estadoAnterior = niveisAtuais[indice0based];
  const bool estadoNovo = digitalRead(CHANNEL_PINS[indice0based]) == HIGH;

  niveisAtuais[indice0based] = estadoNovo;
  contadoresMudancas[indice0based]++;
  ultimoEventoAceitoUs[indice0based] = tempoUs;

  RawEdgeEvent evento{indice0based, estadoAnterior, estadoNovo, tempoUs};

  BaseType_t despertouTarefaMaisPrioritaria = pdFALSE;
  xQueueSendFromISR(filaEventosBrutos, &evento, &despertouTarefaMaisPrioritaria);
  portYIELD_FROM_ISR(despertouTarefaMaisPrioritaria);
}

}  // namespace

void init() {
  filaEventosBrutos = xQueueCreate(EVENT_QUEUE_LEN, sizeof(RawEdgeEvent));

  for (uint8_t i = 0; i < NUM_CHANNELS; i++) {
    // Pull interno (ver CHANNEL_PULLUP em MAIN.HPP): estabiliza o nível
    // ocioso quando não há sensor conectado a este canal, em vez de
    // deixar o pino totalmente flutuante (sujeito a ruído/transições
    // falsas). Não atrapalha um sensor que já aciona ativamente os dois
    // níveis — é um resistor fraco, facilmente sobrescrito.
    pinMode(CHANNEL_PINS[i], CHANNEL_PULLUP ? INPUT_PULLUP : INPUT_PULLDOWN);
    niveisAtuais[i] = (digitalRead(CHANNEL_PINS[i]) == HIGH);
    contadoresMudancas[i] = 0;
    ultimoEventoAceitoUs[i] = 0;
    attachInterruptArg(CHANNEL_PINS[i], isrCanal,
                        reinterpret_cast<void*>(static_cast<uintptr_t>(i)), CHANGE);
  }
}

void processarFilaEventos() {
  if (filaEventosBrutos == nullptr) return;

  RawEdgeEvent evento;
  while (xQueueReceive(filaEventosBrutos, &evento, 0) == pdTRUE) {
    const uint8_t canal1based = evento.canal0based + 1;
    const canais::EdgeMode modo = canais::obterModo(canal1based);

    if (canais::isTransitionEnabled(modo, evento.estadoAnterior, evento.estadoNovo)) {
      if (callbackEventoValido != nullptr) {
        callbackEventoValido(canal1based, evento.estadoNovo, evento.tempoUs);
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
