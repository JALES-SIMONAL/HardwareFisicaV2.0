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
#include "bluetooth_app.hpp"
#include "maquina_estados.hpp"

namespace {

// Núcleo 0: lê os canais de sensores por polling (aquisicao.cpp; sem
// interrupção nem fila — ver comentário lá) e, em seguida, descarrega a
// fila de linhas CSV para o microSD. As duas etapas ficam na mesma tarefa
// de propósito: a escrita no cartão é sempre em blocos pequenos e
// limitados (STORAGE_FLUSH_THRESHOLD linhas), então nunca atrasa a próxima
// leitura dos canais por muito tempo — sem precisar de duas tarefas
// separadas. vTaskDelay(1) (o mínimo do FreeRTOS, ~1ms) em vez de um
// intervalo maior: quanto mais frequente o polling, menor a chance de
// perder uma transição rápida entre duas leituras — só cede o núcleo o
// suficiente para não disparar o watchdog, sem atraso artificial.
void tarefaAquisicaoArmazenamento(void* /*parametro*/) {
  for (;;) {
    aquisicao::processarFilaEventos();
    armazenamento::processarFila();
    experimentos::atualizarLedsPiscando();
    vTaskDelay(1);
  }
}

}  // namespace

// main.cpp fica pequeno: só inicializa os módulos, cria a tarefa do núcleo
// 0 e entra no laço da máquina de estados. O laço padrão do Arduino-ESP32
// (loop()) já roda como "loopTask" pinada ao núcleo 1 (APP_CPU) — é, na
// prática, a tarefa de IHM/Bluetooth descrita no plano, sem precisar
// recriá-la.
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

  // ihm::init() cobre, nesta ordem: GPIO do buzzer, backlight, display,
  // calibração do touch e LEDs — tudo antes do microSD, para que uma falha
  // do cartão nunca atrase ou impeça a IHM (ver Fase 11 da validação
  // geral).
  //
  // A ordem em relação ao microSD ganhou um motivo a mais neste porte: se
  // não houver calibração de toque válida na NVS, ihm::init() executa a
  // calibração dos 4 cantos, que é bloqueante (espera o usuário). Fazer
  // isso antes do cartão evita a montagem do SD ficar pendurada esperando
  // um toque.
  Serial.println("[BOOT] Iniciando GPIOs/display/touch/LEDs (ihm)");
  ihm::init();
  Serial.println("[BOOT] GPIOs/display/touch/LEDs inicializados");

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

  Serial.println("[BOOT] Iniciando maquina de estados");
  maquina_estados::init();
  Serial.println("[BOOT] Maquina de estados inicializada");

  // Desenha a primeira tela de boot (logo Monkey Tech) JA, antes de
  // bluetooth_app::init() (pilha NimBLE — a etapa mais lenta do boot,
  // costuma levar centenas de ms). Sem isto, o display ficava preto (só
  // com o fillScreen() de ihm::init()) por toda a duração do Bluetooth
  // sendo inicializado, porque nada mais desenha na tela até
  // maquina_estados::tick() rodar pela primeira vez dentro de loop() — ou
  // seja, só depois do setup() inteiro (incluindo o Bluetooth) terminar.
  // Chamar tick() manualmente aqui adianta esse primeiro desenho: a logo
  // já fica visível enquanto o Bluetooth inicializa por baixo, escondendo
  // essa espera dentro do tempo que a logo já ficaria na tela mesmo assim
  // (BOOT_DURACAO_LOGO_MONKEY_TECH_MS). tick() chama bluetooth_app::loop()
  // internamente, mas essa chamada não faz nada antes de
  // bluetooth_app::init() rodar (ver guarda "iniciado" em bluetooth_app.cpp).
  maquina_estados::tick();

  Serial.println("[BOOT] Iniciando Bluetooth");
  bluetooth_app::init();  // BLE (NimBLE): assíncrono, não bloqueia o restante
  Serial.println("[BOOT] Bluetooth configurado");

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
