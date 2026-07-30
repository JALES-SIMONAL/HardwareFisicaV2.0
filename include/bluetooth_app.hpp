#pragma once

#include <stdint.h>

// Comunicação com o aplicativo via Bluetooth Low Energy (BLE), sempre
// não-bloqueante. Comandos recebidos são convertidos em comandos::Command e
// entregues a maquina_estados::processarComando() — o mesmo caminho usado
// pelo encoder local. Uma falha/ausência de conexão Bluetooth nunca impede
// o uso local do equipamento.
//
// BLE (via NimBLE-Arduino) foi escolhido no lugar do Bluetooth Classic
// (SPP/BluetoothSerial) por consumir bem menos Flash/RAM — decisivo aqui
// porque a Flash da partição padrão já estava perto do limite. Ver
// src/bluetooth_app.cpp para o desenho do serviço GATT (padrão Nordic UART
// Service) que substitui o antigo canal serial ponto-a-ponto do SPP.
namespace bluetooth_app {

// Nome anunciado durante o escaneamento/pareamento BLE.
constexpr const char* NOME_DISPOSITIVO_BT = "Gerador_UFRN_BT";

constexpr uint32_t INTERVALO_PUBLICACAO_ESTADO_MS = 1000;

// Inicia a pilha BLE (NimBLE) como periférico/servidor GATT, anunciando
// NOME_DISPOSITIVO_BT. Não bloqueia.
void init();

// Chamada em loop pela tarefa de IHM/Bluetooth: drena os comandos
// recebidos e publica o estado periodicamente enquanto houver um
// aplicativo conectado.
void loop();

bool conectado();

const char* deviceId();
const char* enderecoMac();

// Payload/JSON idêntico ao antigo tópico MQTT correspondente, apenas com um
// campo "topico" adicional (mesmo papel dos antigos tópicos separados) para
// o app distinguir os tipos de mensagem dentro do único canal BLE.
void publicarEstado();
void publicarEvento(uint8_t canal1based, char estado, int64_t tempoRelativoUs);
void publicarConfiguracaoCanais();
void publicarResultadoAnalise(int64_t deltaTUs, float velocidadeMs);

// Derruba a conexão BLE atual (se houver), forçando o app a reconectar —
// usado pelo item "Reconectar" da tela "Conexao com app".
void reconectar();

}  // namespace bluetooth_app
