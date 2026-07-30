#include "bluetooth_app.hpp"

#include <ArduinoJson.h>
#include <NimBLEDevice.h>
#include <cstdio>
#include <cstring>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>

#include "MAIN.HPP"
#include "canais.hpp"
#include "comandos.hpp"
#include "configuracoes.hpp"
#include "experimentos.hpp"
#include "armazenamento.hpp"
#include "maquina_estados.hpp"

namespace bluetooth_app {

namespace {

// Serviço "Nordic UART Service" (NUS) — padrão de facto para comunicação
// serial sobre BLE, reconhecido por diversos apps/terminais BLE genéricos
// (útil para testar o firmware antes do app definitivo existir). RX é
// escrito pelo app (comandos); TX é notificado pelo firmware (estado/
// eventos/config de canais).
constexpr const char* SERVICE_UUID = "6E400001-B5A3-F393-E0A9-E50E24DCCA9E";
constexpr const char* CHARACTERISTIC_UUID_RX = "6E400002-B5A3-F393-E0A9-E50E24DCCA9E";
constexpr const char* CHARACTERISTIC_UUID_TX = "6E400003-B5A3-F393-E0A9-E50E24DCCA9E";

// Cada notificação BLE carrega no máximo (MTU do peer - 3) bytes — bem menos
// que um payload JSON típico (até 256 bytes aqui). Por isso cada mensagem é
// fragmentada em vários notify() e, do outro lado, o app deve concatenar os
// bytes recebidos até achar o '\n' de fim de mensagem — o mesmo modelo de
// stream que já era usado com Bluetooth Classic (SPP)/MQTT, só que agora
// fragmentado pela própria pilha BLE em vez de vir pronto num payload único.
constexpr uint8_t BT_COMMAND_QUEUE_LEN = 8;
constexpr size_t BT_COMMAND_MAX_LEN = 128;

struct LinhaComando {
  char texto[BT_COMMAND_MAX_LEN];
};

NimBLEServer* pServidor = nullptr;
NimBLECharacteristic* pCaracteristicaTx = nullptr;

char deviceIdBuffer[16] = "";
volatile uint16_t connHandleAtual = BLE_HS_CONN_HANDLE_NONE;
volatile bool clienteConectado = false;
bool clienteConectadoAnterior = false;
unsigned long ultimaPublicacaoEstadoMs = 0;

// Fila FreeRTOS: onWrite() (callback disparada pela tarefa própria da pilha
// NimBLE) só acumula bytes e enfileira linhas completas — quem de fato
// decodifica o JSON e chama maquina_estados::processarComando() é loop()
// (núcleo 1, mesma tarefa de IHM/Bluetooth), igual ao SPP antes. Evita
// executar lógica pesada (desenho de tela, buzzer etc.) dentro da tarefa da
// pilha BLE, que tem stack pequena.
QueueHandle_t filaComandosBt = nullptr;

// Só usado dentro de onWrite() (única tarefa que escreve aqui) — não precisa
// de mutex.
char rxAccum[BT_COMMAND_MAX_LEN];
size_t rxAccumLen = 0;

// publicarEstado/Evento/ConfiguracaoCanais/ResultadoAnalise() são chamadas
// tanto do núcleo 1 (loop(), periodicamente) quanto do núcleo 0
// (experimentos::aoReceberEventoValido() -> publicarEvento()). Mutex
// recursivo porque processarLinha() (chamada de dentro de loop()) pode
// chamar reconectar(), que também toma este mutex.
SemaphoreHandle_t mutexBt = nullptr;

class TravaBt {
 public:
  TravaBt() { xSemaphoreTakeRecursive(mutexBt, portMAX_DELAY); }
  ~TravaBt() { xSemaphoreGiveRecursive(mutexBt); }
};

// mac[0] é o primeiro octeto (mesma ordem de exibição humana); getNative()
// devolve os bytes na ordem interna do NimBLE, que é a inversa.
void obterEnderecoBLE(uint8_t mac[6]) {
  const uint8_t* nativo = NimBLEDevice::getAddress().getNative();
  for (uint8_t i = 0; i < 6; i++) mac[i] = nativo[5 - i];
}

void gerarDeviceId() {
  uint8_t mac[6];
  obterEnderecoBLE(mac);
  snprintf(deviceIdBuffer, sizeof(deviceIdBuffer), "%02X%02X%02X", mac[3], mac[4], mac[5]);
}

// Fragmenta payload+'\n' em pedaços de até (MTU do peer - 3) bytes, cada um
// enviado como uma notificação separada. Só chamar já dentro de TravaBt e
// com clienteConectado==true.
void enviarLinha(const char* payload, size_t tamanho) {
  if (pCaracteristicaTx == nullptr || connHandleAtual == BLE_HS_CONN_HANDLE_NONE) return;

  uint16_t mtu = pServidor->getPeerMTU(connHandleAtual);
  if (mtu < 23) mtu = 23;
  const size_t maxPedaco = static_cast<size_t>(mtu) - 3;

  size_t enviado = 0;
  while (enviado < tamanho) {
    const size_t restante = tamanho - enviado;
    const size_t pedaco = (restante < maxPedaco) ? restante : maxPedaco;
    pCaracteristicaTx->notify(reinterpret_cast<const uint8_t*>(payload + enviado), pedaco);
    enviado += pedaco;
  }

  const uint8_t nl = '\n';
  pCaracteristicaTx->notify(&nl, 1);
}

void processarLinha(char* linha) {
  JsonDocument doc;
  if (deserializeJson(doc, linha) != DeserializationError::Ok) return;

  const char* acao = doc["action"] | "";
  comandos::Command cmd;

  if (std::strcmp(acao, "next") == 0) {
    cmd.tipo = comandos::CommandType::Next;
  } else if (std::strcmp(acao, "previous") == 0) {
    cmd.tipo = comandos::CommandType::Previous;
  } else if (std::strcmp(acao, "confirm") == 0) {
    cmd.tipo = comandos::CommandType::Confirm;
  } else if (std::strcmp(acao, "back") == 0) {
    cmd.tipo = comandos::CommandType::Back;
  } else if (std::strcmp(acao, "set_brightness") == 0) {
    cmd.tipo = comandos::CommandType::SetBrightness;
    cmd.valor = doc["value"] | 0;
  } else if (std::strcmp(acao, "set_volume") == 0) {
    cmd.tipo = comandos::CommandType::SetVolume;
    cmd.valor = doc["value"] | 0;
  } else if (std::strcmp(acao, "set_operation_mode") == 0) {
    cmd.tipo = comandos::CommandType::SetOperationMode;
    cmd.valor = doc["value"] | 0;
  } else if (std::strcmp(acao, "set_channel_mode") == 0) {
    cmd.tipo = comandos::CommandType::SetChannelMode;
    cmd.canal = doc["channel"] | 0;
    cmd.modo = static_cast<comandos::EdgeMode>(static_cast<uint8_t>(doc["mode"] | 2));
  } else if (std::strcmp(acao, "set_all_channels_mode") == 0) {
    cmd.tipo = comandos::CommandType::SetAllChannelsMode;
    cmd.modo = static_cast<comandos::EdgeMode>(static_cast<uint8_t>(doc["mode"] | 2));
  } else if (std::strcmp(acao, "restore_channel_defaults") == 0) {
    cmd.tipo = comandos::CommandType::RestoreChannelDefaults;
  } else if (std::strcmp(acao, "start_experiment") == 0) {
    cmd.tipo = comandos::CommandType::StartExperiment;
    cmd.valor = doc["repetitions"] | 1;
  } else if (std::strcmp(acao, "stop_experiment") == 0) {
    cmd.tipo = comandos::CommandType::StopExperiment;
  } else if (std::strcmp(acao, "cancel_experiment") == 0) {
    cmd.tipo = comandos::CommandType::CancelExperiment;
  } else if (std::strcmp(acao, "finish_repetition") == 0) {
    cmd.tipo = comandos::CommandType::FinishRepetition;
  } else if (std::strcmp(acao, "reconnect") == 0) {
    reconectar();
    return;
  } else {
    return;
  }

  maquina_estados::processarComando(cmd, comandos::Origem::Bluetooth);
}

class RxCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* pCaracteristica) override {
    const NimBLEAttValue valor = pCaracteristica->getValue();
    for (size_t i = 0; i < valor.length(); i++) {
      const char c = static_cast<char>(valor.data()[i]);
      if (c == '\n' || c == '\r') {
        if (rxAccumLen > 0) {
          LinhaComando linha;
          std::memcpy(linha.texto, rxAccum, rxAccumLen);
          linha.texto[rxAccumLen] = '\0';
          xQueueSend(filaComandosBt, &linha, 0);
          rxAccumLen = 0;
        }
        continue;
      }
      if (rxAccumLen < sizeof(rxAccum) - 1) {
        rxAccum[rxAccumLen++] = c;
      } else {
        // Linha maior que o buffer: descarta e espera o próximo delimitador.
        rxAccumLen = 0;
      }
    }
  }
};

class ServerCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer* /*pServidor*/, ble_gap_conn_desc* desc) override {
    TravaBt trava;
    connHandleAtual = desc->conn_handle;
    clienteConectado = true;
  }

  void onDisconnect(NimBLEServer* /*pServidor*/, ble_gap_conn_desc* /*desc*/) override {
    TravaBt trava;
    connHandleAtual = BLE_HS_CONN_HANDLE_NONE;
    clienteConectado = false;
  }
};

RxCallbacks rxCallbacks;
ServerCallbacks serverCallbacks;

}  // namespace

void init() {
  mutexBt = xSemaphoreCreateRecursiveMutex();
  filaComandosBt = xQueueCreate(BT_COMMAND_QUEUE_LEN, sizeof(LinhaComando));

  NimBLEDevice::init(NOME_DISPOSITIVO_BT);

  pServidor = NimBLEDevice::createServer();
  pServidor->setCallbacks(&serverCallbacks);

  NimBLEService* pServico = pServidor->createService(SERVICE_UUID);
  pCaracteristicaTx = pServico->createCharacteristic(CHARACTERISTIC_UUID_TX, NIMBLE_PROPERTY::NOTIFY);
  NimBLECharacteristic* pCaracteristicaRx = pServico->createCharacteristic(
      CHARACTERISTIC_UUID_RX, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR);
  pCaracteristicaRx->setCallbacks(&rxCallbacks);
  pServico->start();

  NimBLEAdvertising* pAdvertising = NimBLEDevice::getAdvertising();
  pAdvertising->addServiceUUID(SERVICE_UUID);
  pAdvertising->setName(NOME_DISPOSITIVO_BT);
  // Ajuda clientes iOS a descobrirem o serviço durante o scan (recomendação
  // padrão do NimBLE-Arduino para compatibilidade com iPhone/iPad).
  pAdvertising->setScanResponse(true);
  pAdvertising->start();

  gerarDeviceId();
}

void loop() {
  TravaBt trava;

  const bool conectadoAgora = clienteConectado;
  if (conectadoAgora && !clienteConectadoAnterior) {
    Serial.println("[BT] Cliente conectado");
    publicarEstado();
    publicarConfiguracaoCanais();
  } else if (!conectadoAgora && clienteConectadoAnterior) {
    Serial.println("[BT] Cliente desconectado");
    rxAccumLen = 0;
    LinhaComando descarte;
    while (xQueueReceive(filaComandosBt, &descarte, 0) == pdTRUE) {
    }
  }
  clienteConectadoAnterior = conectadoAgora;

  if (!conectadoAgora) return;

  LinhaComando linha;
  while (xQueueReceive(filaComandosBt, &linha, 0) == pdTRUE) {
    processarLinha(linha.texto);
  }

  const unsigned long agora = millis();
  if (agora - ultimaPublicacaoEstadoMs >= INTERVALO_PUBLICACAO_ESTADO_MS) {
    ultimaPublicacaoEstadoMs = agora;
    publicarEstado();
  }
}

bool conectado() {
  TravaBt trava;
  return clienteConectado;
}

const char* deviceId() { return deviceIdBuffer; }

const char* enderecoMac() {
  static char buffer[18];
  uint8_t mac[6];
  obterEnderecoBLE(mac);
  snprintf(buffer, sizeof(buffer), "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3],
           mac[4], mac[5]);
  return buffer;
}

void publicarEstado() {
  TravaBt trava;
  if (!clienteConectado) return;

  JsonDocument doc;
  doc["topico"] = "state";
  doc["modo_operacao"] =
      (configuracoes::modoOperacao() == configuracoes::ModoOperacao::App) ? "app" : "hardware";
  doc["brilho"] = configuracoes::brilho();
  doc["volume"] = configuracoes::volume();
  doc["sd_disponivel"] = armazenamento::cartaoDisponivel();
  doc["sd_erros"] = armazenamento::contadorErros();
  doc["experimento_ativo"] = experimentos::emAndamento();
  doc["repeticao_atual"] = experimentos::repeticaoAtual();
  doc["repeticoes_totais"] = experimentos::totalRepeticoes();
  doc["eventos_repeticao"] = experimentos::eventosNaRepeticaoAtual();
  doc["num_canais"] = NUM_CHANNELS;

  char payload[256];
  const size_t tamanho = serializeJson(doc, payload, sizeof(payload));
  enviarLinha(payload, tamanho);
}

void publicarEvento(uint8_t canal1based, char estado, int64_t tempoRelativoUs) {
  TravaBt trava;
  if (!clienteConectado) return;

  const char estadoStr[2] = {estado, '\0'};

  JsonDocument doc;
  doc["topico"] = "event";
  doc["canal"] = canal1based;
  doc["estado"] = estadoStr;
  doc["tempo_us"] = static_cast<long long>(tempoRelativoUs);

  char payload[96];
  const size_t tamanho = serializeJson(doc, payload, sizeof(payload));
  enviarLinha(payload, tamanho);
}

void publicarConfiguracaoCanais() {
  TravaBt trava;
  if (!clienteConectado) return;

  JsonDocument doc;
  doc["topico"] = "channels";
  JsonArray canaisArray = doc["canais"].to<JsonArray>();
  for (uint8_t i = 1; i <= NUM_CHANNELS; i++) {
    JsonObject c = canaisArray.add<JsonObject>();
    c["canal"] = i;
    c["modo"] = static_cast<uint8_t>(canais::obterModo(i));
  }

  char payload[256];
  const size_t tamanho = serializeJson(doc, payload, sizeof(payload));
  enviarLinha(payload, tamanho);
}

void publicarResultadoAnalise(int64_t deltaTUs, float velocidadeMs) {
  TravaBt trava;
  if (!clienteConectado) return;

  JsonDocument doc;
  doc["topico"] = "event";
  doc["tipo"] = "analise";
  doc["delta_t_us"] = static_cast<long long>(deltaTUs);
  doc["velocidade_ms"] = velocidadeMs;

  char payload[96];
  const size_t tamanho = serializeJson(doc, payload, sizeof(payload));
  enviarLinha(payload, tamanho);
}

void reconectar() {
  TravaBt trava;
  if (clienteConectado && pServidor != nullptr) {
    pServidor->disconnect(connHandleAtual);
  }
}

}  // namespace bluetooth_app
