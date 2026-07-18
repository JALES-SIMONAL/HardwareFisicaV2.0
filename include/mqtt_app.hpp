#pragma once

#include <stdint.h>

// Comunicação com o aplicativo via Wi-Fi/MQTT, sempre não-bloqueante.
// Comandos recebidos são convertidos em comandos::Command e entregues a
// maquina_estados::processarComando() — o mesmo caminho usado pelo encoder
// local. Uma falha de Wi-Fi/MQTT nunca impede o uso local do equipamento.
namespace mqtt_app {

// ATENCAO: nenhuma credencial real foi fornecida. Ajuste estas constantes
// (único lugar) antes de usar em campo. A senha nunca é exibida na tela ou
// em logs normais.
constexpr const char* WIFI_SSID = "PLACEHOLDER_WIFI_SSID";
constexpr const char* WIFI_PASSWORD = "PLACEHOLDER_WIFI_PASSWORD";
constexpr const char* BROKER_HOST = "PLACEHOLDER_BROKER_HOST";
constexpr uint16_t BROKER_PORT = 1883;
constexpr const char* BROKER_USUARIO = "";
constexpr const char* BROKER_SENHA = "";

// Prefixo dos tópicos: "<prefixo>/<device_id>/{command,state,event,channels,file,status}".
constexpr const char* TOPICO_PREFIXO = "hardwarefisica";

constexpr uint32_t INTERVALO_RECONEXAO_MS = 5000;
constexpr uint32_t INTERVALO_PUBLICACAO_ESTADO_MS = 1000;

// Inicia Wi-Fi (assíncrono) e configura o cliente MQTT. Não bloqueia.
void init();

// Chamada em loop pela tarefa de IHM/MQTT. Só tenta reconectar no máximo a
// cada INTERVALO_RECONEXAO_MS; nunca deve travar o restante do equipamento
// por muito tempo mesmo com o broker inacessível.
void loop();

bool wifiConectado();
bool mqttConectado();

const char* deviceId();
const char* enderecoIP();

void publicarEstado();
void publicarEvento(uint8_t canal1based, char estado, int64_t tempoRelativoUs);
void publicarConfiguracaoCanais();
void publicarResultadoAnalise(int64_t deltaTUs, float velocidadeMs);

// Força uma nova tentativa de conexão na próxima chamada de loop().
void reconectar();

}  // namespace mqtt_app
