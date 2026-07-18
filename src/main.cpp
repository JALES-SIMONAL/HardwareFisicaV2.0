#include <Arduino.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "MAIN.HPP"
#include "aquisicao.hpp"
#include "armazenamento.hpp"
#include "autoteste.hpp"
#include "canais.hpp"
#include "configuracoes.hpp"
#include "experimentos.hpp"
#include "ihm.hpp"
#include "maquina_estados.hpp"
#include "mqtt_app.hpp"

namespace {

// Núcleo 0: drena a fila de eventos brutos da ISR de aquisição (aplicando
// o filtro de borda) e, em seguida, descarrega a fila de linhas CSV para o
// microSD. As duas etapas ficam na mesma tarefa de propósito: ambas usam
// filas FreeRTOS (thread-safe) para se desacoplar da ISR e da IHM, e a
// escrita no cartão é sempre em blocos pequenos e limitados
// (STORAGE_FLUSH_THRESHOLD linhas), então nunca atrasa a próxima leitura de
// evento por muito tempo — sem precisar de duas tarefas separadas.
void tarefaAquisicaoArmazenamento(void* /*parametro*/) {
  for (;;) {
    aquisicao::processarFilaEventos();
    armazenamento::processarFila();
    vTaskDelay(pdMS_TO_TICKS(2));
  }
}

}  // namespace

// main.cpp fica pequeno: só inicializa os módulos, cria a tarefa do núcleo
// 0 e entra no laço da máquina de estados. O laço padrão do Arduino-ESP32
// (loop()) já roda como "loopTask" pinada ao núcleo 1 (APP_CPU) — é, na
// prática, a tarefa de IHM/MQTT descrita no plano, sem precisar recriá-la.
void setup() {
  Serial.begin(SERIAL_BAUD_RATE);
  delay(300);

  Serial.println();
  Serial.println("========================================");
  Serial.println("[BOOT] HardwareFisicaV2.0");
  Serial.println("[BOOT] Inicializacao completa iniciada");
  Serial.printf("[BOOT] Motivo do reset: %d\n", static_cast<int>(esp_reset_reason()));
  Serial.printf("[BOOT] Heap inicial: %u bytes\n", static_cast<unsigned>(ESP.getFreeHeap()));
  Serial.println("========================================");

  // ihm::init() cobre, nesta ordem: GPIOs do encoder/tecla/buzzer,
  // backlight, display (com teste visual integrado, ver ihm.cpp) e LEDs —
  // tudo antes do microSD, para que uma falha do cartão nunca atrase ou
  // impeça a IHM (ver Fase 11 da validação geral).
  Serial.println("[BOOT] Iniciando GPIOs/display/encoder/LEDs (ihm)");
  ihm::init();
  Serial.println("[BOOT] GPIOs/display/encoder/LEDs inicializados");

  Serial.println("[BOOT] Iniciando configuracoes (NVS: brilho/volume/modo)");
  configuracoes::init();
  Serial.println("[BOOT] Configuracoes carregadas");

  Serial.println("[BOOT] Iniciando sensores (canais + aquisicao)");
  canais::init();
  aquisicao::init();
  Serial.println("[BOOT] Sensores inicializados");

  Serial.println("[BOOT] Iniciando microSD");
  armazenamento::init();
  if (armazenamento::cartaoDisponivel()) {
    Serial.println("[BOOT] microSD disponivel");
  } else {
    Serial.println("[BOOT] microSD indisponivel - aplicacao continuara");
  }

  experimentos::init();

  Serial.println("[BOOT] Iniciando MQTT");
  mqtt_app::init();  // Wi-Fi/MQTT: assíncrono, não bloqueia o restante
  Serial.println("[BOOT] MQTT configurado");

  Serial.println("[BOOT] Iniciando maquina de estados");
  maquina_estados::init();
  Serial.println("[BOOT] Maquina de estados inicializada");

  // No-op quando ENABLE_FIRMWARE_SELF_TESTS==0 (padrão); ver autoteste.hpp.
  // Os autotestes só verificam funções puras/isoláveis e nunca substituem
  // (nem atrasam de forma perceptível) a inicialização normal abaixo.
  autoteste::executar();

  Serial.println("[BOOT] Criando tarefa de aquisicao/armazenamento (nucleo 0)");
  const BaseType_t resultadoTarefa = xTaskCreatePinnedToCore(
      tarefaAquisicaoArmazenamento, "AquisicaoArmazenamento", 4096, nullptr, 2, nullptr, 0);
  Serial.printf("[TASK] Resultado da criacao: %s\n", resultadoTarefa == pdPASS ? "SUCESSO" : "FALHA");

  Serial.println("[BOOT] Inicializacao concluida");
}

void loop() {
  maquina_estados::tick();
}
