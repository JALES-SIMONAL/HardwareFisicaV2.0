#include "mqtt_app.hpp"

#include <ArduinoJson.h>
#include <PubSubClient.h>
#include <WiFi.h>
#include <cstdio>
#include <cstring>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include "MAIN.HPP"
#include "canais.hpp"
#include "comandos.hpp"
#include "configuracoes.hpp"
#include "experimentos.hpp"
#include "armazenamento.hpp"
#include "maquina_estados.hpp"

namespace mqtt_app {

namespace {

WiFiClient wifiClient;
PubSubClient mqttClient(wifiClient);

char deviceIdBuffer[16] = "";
unsigned long ultimaTentativaReconexaoMs = 0;
unsigned long ultimaPublicacaoEstadoMs = 0;

// mqttClient/wifiClient (PubSubClient) não são thread-safe: loop() roda na
// tarefa de IHM/MQTT (núcleo 1), mas publicarEvento() é chamada a partir de
// experimentos::aoReceberEventoValido(), executado na tarefa de aquisição
// (núcleo 0). Mutex recursivo porque aoReceberMensagem() (callback do
// PubSubClient, disparado de dentro de mqttClient.loop()) pode chamar
// reconectar(), que também toma este mutex.
SemaphoreHandle_t mutexMqtt = nullptr;

class TravaMqtt {
 public:
  TravaMqtt() { xSemaphoreTakeRecursive(mutexMqtt, portMAX_DELAY); }
  ~TravaMqtt() { xSemaphoreGiveRecursive(mutexMqtt); }
};

void montarTopico(char* destino, size_t tamanho, const char* sufixo) {
  snprintf(destino, tamanho, "%s/%s/%s", TOPICO_PREFIXO, deviceIdBuffer, sufixo);
}

void gerarDeviceId() {
  uint8_t mac[6];
  WiFi.macAddress(mac);
  snprintf(deviceIdBuffer, sizeof(deviceIdBuffer), "%02X%02X%02X", mac[3], mac[4], mac[5]);
}

void aoReceberMensagem(char* /*topico*/, uint8_t* payload, unsigned int comprimento) {
  char buffer[256];
  const unsigned int copiar = (comprimento < sizeof(buffer) - 1) ? comprimento : sizeof(buffer) - 1;
  std::memcpy(buffer, payload, copiar);
  buffer[copiar] = '\0';

  JsonDocument doc;
  if (deserializeJson(doc, buffer) != DeserializationError::Ok) return;

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

  maquina_estados::processarComando(cmd, comandos::Origem::MQTT);
}

void tentarConectar() {
  if (WiFi.status() != WL_CONNECTED) return;

  char topicoStatus[64];
  montarTopico(topicoStatus, sizeof(topicoStatus), "status");

  const bool ok = mqttClient.connect(deviceIdBuffer, BROKER_USUARIO, BROKER_SENHA, topicoStatus, 0,
                                      true, "offline");
  if (!ok) return;

  mqttClient.publish(topicoStatus, "online", true);

  char topicoComando[64];
  montarTopico(topicoComando, sizeof(topicoComando), "command");
  mqttClient.subscribe(topicoComando);

  publicarEstado();
  publicarConfiguracaoCanais();
}

}  // namespace

void init() {
  mutexMqtt = xSemaphoreCreateRecursiveMutex();
  gerarDeviceId();

  mqttClient.setServer(BROKER_HOST, BROKER_PORT);
  mqttClient.setCallback(aoReceberMensagem);
  // Limita o pior caso de bloqueio de mqttClient.connect() (padrão: 15s).
  mqttClient.setSocketTimeout(2);

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);  // assíncrono: não bloqueia
}

void loop() {
  TravaMqtt trava;
  if (WiFi.status() != WL_CONNECTED) return;

  if (!mqttClient.connected()) {
    const unsigned long agora = millis();
    if (agora - ultimaTentativaReconexaoMs >= INTERVALO_RECONEXAO_MS) {
      ultimaTentativaReconexaoMs = agora;
      tentarConectar();
    }
    return;
  }

  mqttClient.loop();

  const unsigned long agora = millis();
  if (agora - ultimaPublicacaoEstadoMs >= INTERVALO_PUBLICACAO_ESTADO_MS) {
    ultimaPublicacaoEstadoMs = agora;
    publicarEstado();
  }
}

bool wifiConectado() { return WiFi.status() == WL_CONNECTED; }
bool mqttConectado() {
  TravaMqtt trava;
  return mqttClient.connected();
}

const char* deviceId() { return deviceIdBuffer; }

const char* enderecoIP() {
  static char buffer[16];
  const IPAddress ip = WiFi.localIP();
  snprintf(buffer, sizeof(buffer), "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
  return buffer;
}

void publicarEstado() {
  TravaMqtt trava;
  if (!mqttClient.connected()) return;

  JsonDocument doc;
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

  char topico[64];
  montarTopico(topico, sizeof(topico), "state");
  mqttClient.publish(topico, reinterpret_cast<const uint8_t*>(payload), tamanho, false);
}

void publicarEvento(uint8_t canal1based, char estado, int64_t tempoRelativoUs) {
  TravaMqtt trava;
  if (!mqttClient.connected()) return;

  const char estadoStr[2] = {estado, '\0'};

  JsonDocument doc;
  doc["canal"] = canal1based;
  doc["estado"] = estadoStr;
  doc["tempo_us"] = static_cast<long long>(tempoRelativoUs);

  char payload[96];
  const size_t tamanho = serializeJson(doc, payload, sizeof(payload));

  char topico[64];
  montarTopico(topico, sizeof(topico), "event");
  mqttClient.publish(topico, reinterpret_cast<const uint8_t*>(payload), tamanho, false);
}

void publicarConfiguracaoCanais() {
  TravaMqtt trava;
  if (!mqttClient.connected()) return;

  JsonDocument doc;
  JsonArray canaisArray = doc["canais"].to<JsonArray>();
  for (uint8_t i = 1; i <= NUM_CHANNELS; i++) {
    JsonObject c = canaisArray.add<JsonObject>();
    c["canal"] = i;
    c["modo"] = static_cast<uint8_t>(canais::obterModo(i));
  }

  char payload[256];
  const size_t tamanho = serializeJson(doc, payload, sizeof(payload));

  char topico[64];
  montarTopico(topico, sizeof(topico), "channels");
  mqttClient.publish(topico, reinterpret_cast<const uint8_t*>(payload), tamanho, false);
}

void publicarResultadoAnalise(int64_t deltaTUs, float velocidadeMs) {
  TravaMqtt trava;
  if (!mqttClient.connected()) return;

  JsonDocument doc;
  doc["tipo"] = "analise";
  doc["delta_t_us"] = static_cast<long long>(deltaTUs);
  doc["velocidade_ms"] = velocidadeMs;

  char payload[96];
  const size_t tamanho = serializeJson(doc, payload, sizeof(payload));

  char topico[64];
  montarTopico(topico, sizeof(topico), "event");
  mqttClient.publish(topico, reinterpret_cast<const uint8_t*>(payload), tamanho, false);
}

void reconectar() {
  TravaMqtt trava;
  ultimaTentativaReconexaoMs = 0;
}

}  // namespace mqtt_app
