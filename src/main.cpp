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


// Confere a memoria externa do modulo antes de qualquer coisa alocar.
//
// Este firmware roda num ESP32-S3-WROOM-1 N16R8, com 8MB de PSRAM octal
// (platformio.ini: memory_type = qio_opi). Se essa PSRAM nao subir, dois
// efeitos aparecem longe daqui e sao dificeis de rastrear ate a causa: o
// heap disponivel cai para so o interno, e os GPIO 33 a 37 — que a PSRAM
// octal ocupa — passam a parecer livres, convidando a usa-los.
//
// psramFound() sozinho nao basta: ele diz que o controlador respondeu, nao
// que a memoria GUARDA dado. Uma linha de endereco trocada ou um bit preso
// passam nessa checagem e so se manifestam como corrupcao silenciosa depois.
// Por isso o teste escreve e confere um padrao que DEPENDE DO INDICE: se
// duas linhas de endereco estiverem trocadas, a leitura devolve o byte de
// outra posicao e o padrao denuncia; um padrao constante nao pegaria isso.
void verificarMemoria() {
  Serial.println("[MEM] Verificando memoria");
  Serial.printf("[MEM] Flash: %u MB\n",
                static_cast<unsigned>(ESP.getFlashChipSize() / (1024UL * 1024UL)));
  Serial.printf("[MEM] Heap interno: %u livres de %u bytes\n",
                static_cast<unsigned>(ESP.getFreeHeap()),
                static_cast<unsigned>(ESP.getHeapSize()));
  Serial.flush();

  if (!psramFound()) {
    Serial.println("[MEM][ERRO] PSRAM NAO DETECTADA.");
    Serial.println("[MEM][ERRO] Esperados 8MB octal (modulo N16R8).");
    Serial.println("[MEM][ERRO] Confira se o modulo e N16R8 e se o platformio.ini");
    Serial.println("[MEM][ERRO] tem board_build.arduino.memory_type = qio_opi.");
    Serial.println("[MEM][ERRO] ATENCAO: sem PSRAM, os GPIO 33 a 37 ficam livres —");
    Serial.println("[MEM][ERRO] mas a tabela de pinagem assume que estao ocupados.");
    Serial.flush();
    return;
  }

  const size_t total = ESP.getPsramSize();
  const size_t livre = ESP.getFreePsram();
  Serial.printf("[MEM] PSRAM: %u bytes (%u MB), %u livres\n",
                static_cast<unsigned>(total),
                static_cast<unsigned>(total / (1024UL * 1024UL)),
                static_cast<unsigned>(livre));
  Serial.flush();

  constexpr size_t TAM_TESTE = 64UL * 1024UL;
  volatile uint8_t* buffer = static_cast<volatile uint8_t*>(ps_malloc(TAM_TESTE));
  if (buffer == nullptr) {
    Serial.printf("[MEM][ERRO] PSRAM detectada mas ps_malloc(%u) falhou\n",
                  static_cast<unsigned>(TAM_TESTE));
    Serial.flush();
    return;
  }

  // volatile de proposito: sem ele o compilador tem todo o direito de
  // concluir que ler de volta o que acabou de escrever e redundante, apagar
  // o teste inteiro e deixar um "PSRAM OK" que nunca tocou na memoria.
  for (size_t i = 0; i < TAM_TESTE; i++) {
    buffer[i] = static_cast<uint8_t>((i * 31u) ^ (i >> 8));
  }

  size_t erros = 0;
  size_t primeiroErro = 0;
  for (size_t i = 0; i < TAM_TESTE; i++) {
    const uint8_t esperado = static_cast<uint8_t>((i * 31u) ^ (i >> 8));
    if (buffer[i] != esperado) {
      if (erros == 0) primeiroErro = i;
      erros++;
    }
  }
  free(const_cast<uint8_t*>(buffer));

  if (erros == 0) {
    Serial.printf("[MEM] PSRAM OK: %u bytes escritos e conferidos\n",
                  static_cast<unsigned>(TAM_TESTE));
  } else {
    Serial.printf("[MEM][ERRO] PSRAM CORROMPENDO: %u de %u bytes errados, "
                  "primeiro no offset %u\n",
                  static_cast<unsigned>(erros), static_cast<unsigned>(TAM_TESTE),
                  static_cast<unsigned>(primeiroErro));
    Serial.println("[MEM][ERRO] O controlador responde mas a memoria nao guarda dado.");
  }
  Serial.flush();
}

}  // namespace

// main.cpp fica pequeno: só inicializa os módulos, cria a tarefa do núcleo
// 0 e entra no laço da máquina de estados. O laço padrão do Arduino-ESP32
// (loop()) já roda como "loopTask" pinada ao núcleo 1 (APP_CPU) — é, na
// prática, a tarefa de IHM/Bluetooth descrita no plano, sem precisar
// recriá-la.
void setup() {
  Serial.begin(SERIAL_BAUD_RATE);

  // ESPERA O USB ENUMERAR ANTES DE QUALQUER COISA QUE POSSA FALHAR.
  //
  // Neste ESP32-S3 o Serial do sketch sai pela USB nativa. Se o firmware
  // trava logo no inicio e a placa entra em ciclo de reinicio, o USB nunca
  // chega a enumerar e NENHUMA linha deste setup() alcanca o PC — a tela do
  // monitor fica so com o "ESP-ROM:.../load:.../entry ..." do bootloader da
  // ROM, que fala por outro caminho. O sintoma vira "reinicia sem dizer
  // nada", que nao da para diagnosticar.
  //
  // Os 3s abaixo dao tempo ao host de reconhecer a porta antes do primeiro
  // modulo ser inicializado, entao as marcas [BOOT] passam a chegar e da
  // para ver EXATAMENTE em qual etapa o firmware morre.
  //
  // Vale saber, porque foi o que confundiu o diagnostico: a mensagem de
  // "Guru Meditation" do ESP-IDF NAO sai por aqui — o console de panico
  // deste build e a UART0 (GPIO43/44), que na placa nao esta ligada em
  // nada. Para ler o panico e preciso um conversor USB-serial no GPIO43.
  // Estas marcas [BOOT] sao o substituto pratico: a ultima que aparecer
  // indica a etapa que travou.
  const unsigned long limiteEsperaUsbMs = 3000;
  const unsigned long inicioEsperaMs = millis();
  while (!Serial && (millis() - inicioEsperaMs) < limiteEsperaUsbMs) {
    delay(10);
  }
  delay(500);

  Serial.println();
  Serial.println("========================================");
  Serial.println("[BOOT] HardwareFisicaV2.0");
  Serial.flush();
  Serial.println("[BOOT] Inicializacao completa iniciada");
  Serial.flush();
  // 1=POWERON 3=SW 4=INT_WDT 5=TASK_WDT 6=WDT 7=DEEPSLEEP 8=BROWNOUT
  // 9=SDIO. Vindo 3 depois de um ciclo de reinicio, foi panico de software
  // (o handler chama esp_restart()); 8 seria alimentacao.
  Serial.printf("[BOOT] Motivo do reset: %d\n", static_cast<int>(esp_reset_reason()));
  Serial.printf("[BOOT] Heap inicial: %u bytes\n", static_cast<unsigned>(ESP.getFreeHeap()));
  Serial.println("========================================");
  Serial.flush();

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
  verificarMemoria();

  Serial.println("[BOOT] Iniciando GPIOs/display/touch/LEDs (ihm)");
  Serial.flush();
  ihm::init();
  Serial.println("[BOOT] GPIOs/display/touch/LEDs inicializados");
  Serial.flush();

  Serial.println("[BOOT] Iniciando configuracoes (NVS: brilho/volume/modo)");
  Serial.flush();
  configuracoes::init();
  Serial.println("[BOOT] Configuracoes carregadas");
  Serial.flush();

  Serial.println("[BOOT] Iniciando sensores (canais + aquisicao)");
  Serial.flush();
  canais::init();
  aquisicao::init();
  Serial.println("[BOOT] Sensores inicializados");
  Serial.flush();

  Serial.println("[BOOT] Iniciando microSD");
  Serial.flush();
  armazenamento::init();
  if (armazenamento::cartaoDisponivel()) {
    Serial.println("[BOOT] microSD disponivel");
  Serial.flush();
  } else {
    Serial.println("[BOOT] microSD indisponivel - aplicacao continuara");
  Serial.flush();
  }

  experimentos::init();

  Serial.println("[BOOT] Iniciando maquina de estados");
  Serial.flush();
  maquina_estados::init();
  Serial.println("[BOOT] Maquina de estados inicializada");
  Serial.flush();

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
  Serial.flush();
  bluetooth_app::init();  // BLE (NimBLE): assíncrono, não bloqueia o restante
  Serial.println("[BOOT] Bluetooth configurado");
  Serial.flush();

  // No-op quando ENABLE_FIRMWARE_SELF_TESTS==0 (padrão); ver autoteste.hpp.
  // Os autotestes só verificam funções puras/isoláveis e nunca substituem
  // (nem atrasam de forma perceptível) a inicialização normal abaixo.
  autoteste::executar();

  Serial.println("[BOOT] Criando tarefa de aquisicao/armazenamento (nucleo 0)");
  Serial.flush();
  const BaseType_t resultadoTarefa = xTaskCreatePinnedToCore(
      tarefaAquisicaoArmazenamento, "AquisicaoArmazenamento", 4096, nullptr, 2, nullptr, 0);
  Serial.printf("[TASK] Resultado da criacao: %s\n", resultadoTarefa == pdPASS ? "SUCESSO" : "FALHA");

  Serial.println("[BOOT] Inicializacao concluida");
  Serial.flush();
}

void loop() {
  maquina_estados::tick();
}
