#include <Arduino.h>
#include <esp_attr.h>
#include <esp_system.h>
#include <stdio.h>
#include <string.h>
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


// ---------------------------------------------------------------------
// Rastreador de boot na memoria do dominio RTC
// ---------------------------------------------------------------------
// PROBLEMA QUE ISTO RESOLVE: quando a placa entra em ciclo de reinicio, o
// monitor so mostra o "ESP-ROM:... / rst:0x1 (POWERON) / load: / entry ..."
// do bootloader da ROM e mais nada. Esse texto nao vem do firmware — vem da
// ROM — entao a sua presenca nao prova que o firmware rodou, e a ausencia
// das marcas [BOOT] nao diz ONDE ele morreu. Pior: "POWERON" e ambiguo. Ele
// aparece tanto quando a alimentacao caiu de verdade quanto quando o host
// pulsa o reset da placa ao abrir a porta USB — dois problemas opostos
// (hardware x ferramenta) com o mesmo sintoma na tela.
//
// COMO DISTINGUIR: RTC_NOINIT_ATTR coloca a variavel na RAM lenta do
// dominio RTC, que o bootloader nao zera. Ela ATRAVESSA reset de software,
// panico e watchdog, mas o conteudo se perde quando o dominio RTC fica sem
// energia. Essa assimetria e o teste:
//
//   assinatura intacta no boot seguinte  -> o chip nunca perdeu energia; foi
//      reset logico, e rtcUltimaEtapa diz a ultima etapa que o firmware
//      alcancou antes de morrer.
//   assinatura perdida                   -> o dominio RTC ficou sem energia:
//      ou e o primeiro boot depois de ligar, ou a alimentacao esta caindo
//      (brownout, fonte/USB sem corrente, curto). Nao e bug de software.
//
// E probabilistico, nao absoluto: apos um power-on o conteudo da RAM RTC e
// indefinido, e nada impede que caia exatamente na assinatura. Com uma
// constante de 32 bits isso e 1 em 2^32 — na pratica, conclusivo.
constexpr uint32_t RTC_ASSINATURA = 0x48465632UL;  // "HFV2"

RTC_NOINIT_ATTR uint32_t rtcAssinatura;
RTC_NOINIT_ATTR uint32_t rtcEtapa;
RTC_NOINIT_ATTR uint32_t rtcBootsIncompletos;
RTC_NOINIT_ATTR char rtcUltimaEtapa[28];

struct BootAnterior {
  bool dominioRtcPreservado;
  uint32_t etapa;
  uint32_t bootsIncompletos;
  char ultimaEtapa[28];
};

// Le o que o boot anterior deixou e JA rearma os campos para este boot.
// Chamada antes de qualquer Serial.print: se o firmware morrer antes de o
// USB enumerar, o rastro tem de estar gravado de qualquer forma.
BootAnterior lerERearmarRastroDeBoot() {
  BootAnterior anterior = {};
  anterior.dominioRtcPreservado = (rtcAssinatura == RTC_ASSINATURA);
  if (anterior.dominioRtcPreservado) {
    anterior.etapa = rtcEtapa;
    anterior.bootsIncompletos = rtcBootsIncompletos;
    rtcUltimaEtapa[sizeof(rtcUltimaEtapa) - 1] = '\0';
    snprintf(anterior.ultimaEtapa, sizeof(anterior.ultimaEtapa), "%s", rtcUltimaEtapa);
  } else {
    snprintf(anterior.ultimaEtapa, sizeof(anterior.ultimaEtapa), "%s", "(sem rastro)");
  }

  rtcAssinatura = RTC_ASSINATURA;
  rtcEtapa = 0;
  rtcBootsIncompletos = anterior.bootsIncompletos + 1;
  snprintf(rtcUltimaEtapa, sizeof(rtcUltimaEtapa), "%s", "antes da 1a etapa");
  return anterior;
}

// Registra a etapa no dominio RTC ANTES de imprimir. A ordem importa: se a
// etapa seguinte travar, o que vale e o que ficou na RAM RTC, nao o que
// chegou ao PC — o USB pode nem estar enumerado ainda.
void marcarEtapa(const char* nome) {
  ++rtcEtapa;
  snprintf(rtcUltimaEtapa, sizeof(rtcUltimaEtapa), "%s", nome);
  Serial.printf("[BOOT] etapa %u: %s\n", static_cast<unsigned>(rtcEtapa), nome);
  Serial.flush();
}

void relatarBootAnterior(const BootAnterior& anterior) {
  // 1=POWERON 3=SW 4=INT_WDT 5=TASK_WDT 6=WDT 7=DEEPSLEEP 8=BROWNOUT
  // 9=SDIO. Vindo 3 depois de um ciclo de reinicio, foi panico de software
  // (o handler chama esp_restart()); 8 seria alimentacao.
  Serial.printf("[BOOT] Motivo do reset: %d\n", static_cast<int>(esp_reset_reason()));
  Serial.printf("[BOOT] Boot numero %u desde a ultima energizacao\n",
                static_cast<unsigned>(rtcBootsIncompletos));
  if (anterior.dominioRtcPreservado) {
    Serial.printf("[BOOT] Rastro RTC INTACTO: o boot anterior parou na etapa %u (%s)\n",
                  static_cast<unsigned>(anterior.etapa), anterior.ultimaEtapa);
    Serial.println("[BOOT] Rastro intacto = o chip NAO perdeu energia: o reset");
    Serial.println("[BOOT] foi logico (panico/watchdog/reset do host), e a etapa");
    Serial.println("[BOOT] acima e o ultimo ponto que o firmware alcancou.");
  } else {
    Serial.println("[BOOT] Rastro RTC PERDIDO: o dominio RTC ficou sem energia.");
    Serial.println("[BOOT] Se este e o 1o boot depois de ligar, e o esperado.");
    Serial.println("[BOOT] Se isto repete em ciclo, a alimentacao esta caindo");
    Serial.println("[BOOT] (fonte/USB sem corrente, brownout ou curto) - nao e");
    Serial.println("[BOOT] falha de software.");
  }
  Serial.flush();
}

// Fecha o rastro: zera o contador de boots incompletos, para que o proximo
// boot saiba que o anterior chegou ate o fim do setup().
void concluirRastroDeBoot() {
  rtcBootsIncompletos = 0;
  snprintf(rtcUltimaEtapa, sizeof(rtcUltimaEtapa), "%s", "boot completo");
}

// Relata a memoria do proprio ESP32 (flash, heap e PSRAM do modulo N16R8).
// Nao e "o modulo de memoria" do equipamento — esse e o cartao SD no
// barramento SPI, testado por armazenamento::autoTesteCartao(). Fica aqui
// porque, se a PSRAM nao subir, o heap cai para so o interno e os GPIO 33 a
// 37 que ela ocupa passam a parecer livres, contrariando a tabela de
// pinagem.
void relatarMemoriaInterna() {
  Serial.printf("[MEM] Flash: %u MB | Heap interno: %u de %u bytes\n",
                static_cast<unsigned>(ESP.getFlashChipSize() / (1024UL * 1024UL)),
                static_cast<unsigned>(ESP.getFreeHeap()),
                static_cast<unsigned>(ESP.getHeapSize()));
  if (psramFound()) {
    Serial.printf("[MEM] PSRAM: %u MB, %u bytes livres\n",
                  static_cast<unsigned>(ESP.getPsramSize() / (1024UL * 1024UL)),
                  static_cast<unsigned>(ESP.getFreePsram()));
  } else {
    Serial.println("[MEM][AVISO] PSRAM nao detectada (esperados 8MB no N16R8).");
    Serial.println("[MEM][AVISO] Sem ela os GPIO 33 a 37 ficam livres, mas a");
    Serial.println("[MEM][AVISO] tabela de pinagem assume que estao ocupados.");
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
  // PRIMEIRA instrucao do setup(), antes de Serial.begin(): se o firmware
  // morrer antes de o USB enumerar, o rastro na RAM RTC e a unica coisa que
  // sobrevive para contar onde foi.
  const BootAnterior bootAnterior = lerERearmarRastroDeBoot();

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
  relatarBootAnterior(bootAnterior);
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
  marcarEtapa("relato de memoria interna");
  relatarMemoriaInterna();

  marcarEtapa("ihm (GPIO/display/LEDs)");
  ihm::init();

  marcarEtapa("configuracoes (NVS)");
  configuracoes::init();

  marcarEtapa("sensores (canais + aquisicao)");
  canais::init();
  aquisicao::init();

  marcarEtapa("microSD (montagem)");
  armazenamento::init();
  if (armazenamento::cartaoDisponivel()) {
    Serial.println("[BOOT] microSD disponivel");
    // Montar nao prova que grava: o autoteste escreve, le de volta e
    // confere. Ver armazenamento::autoTesteCartao().
    Serial.flush();
    marcarEtapa("microSD (autoteste grava/le)");
    armazenamento::autoTesteCartao();
    Serial.flush();
  } else {
    Serial.println("[BOOT] microSD indisponivel - aplicacao continuara");
    Serial.flush();
  }

  // DEPOIS do cartao, de proposito: a calibracao do toque pode bloquear
  // esperando um toque, e a verificacao do cartao nao pode ficar atras
  // dessa espera. Ver o comentario em ihm::initToque().
  marcarEtapa("toque (diagnostico/calibracao)");
  ihm::initToque();

  experimentos::init();

  marcarEtapa("maquina de estados");
  maquina_estados::init();

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
  marcarEtapa("1o desenho (logo)");
  maquina_estados::tick();

  marcarEtapa("Bluetooth (NimBLE)");
  bluetooth_app::init();  // BLE (NimBLE): assíncrono, não bloqueia o restante

  // No-op quando ENABLE_FIRMWARE_SELF_TESTS==0 (padrão); ver autoteste.hpp.
  // Os autotestes só verificam funções puras/isoláveis e nunca substituem
  // (nem atrasam de forma perceptível) a inicialização normal abaixo.
  autoteste::executar();

  marcarEtapa("tarefa do nucleo 0");
  const BaseType_t resultadoTarefa = xTaskCreatePinnedToCore(
      tarefaAquisicaoArmazenamento, "AquisicaoArmazenamento", 4096, nullptr, 2, nullptr, 0);
  Serial.printf("[TASK] Resultado da criacao: %s\n", resultadoTarefa == pdPASS ? "SUCESSO" : "FALHA");

  concluirRastroDeBoot();
  Serial.println("[BOOT] Inicializacao concluida");
  Serial.flush();
}

void loop() {
  maquina_estados::tick();
}
