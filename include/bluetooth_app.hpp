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

// Nome padrão anunciado durante o escaneamento/pareamento BLE, usado só na
// primeira vez que o equipamento liga (sem nome salvo ainda). Depois disso o
// nome efetivo vem da NVS (ver nomeDispositivo()/definirNomeDispositivo()) e
// pode ser trocado pelo usuário — local ou pelo app.
constexpr const char* NOME_DISPOSITIVO_BT_PADRAO = "Gerador_UFRN_BT";
constexpr uint8_t TAMANHO_MAX_NOME_DISPOSITIVO_BT = 20;

constexpr uint32_t INTERVALO_PUBLICACAO_ESTADO_MS = 1000;

// Inicia a pilha BLE (NimBLE) como periférico/servidor GATT, anunciando o
// nome salvo na NVS (ou NOME_DISPOSITIVO_BT_PADRAO, na primeira vez). Não
// bloqueia.
void init();

// Chamada em loop pela tarefa de IHM/Bluetooth: drena os comandos
// recebidos e publica o estado periodicamente enquanto houver um
// aplicativo conectado.
void loop();

bool conectado();

const char* deviceId();
const char* enderecoMac();

// Nome atualmente anunciado no BLE (o mesmo enviado no campo "nome_bt" da
// mensagem "info").
const char* nomeDispositivo();

// Troca o nome anunciado no BLE: persiste na NVS, aplica na pilha NimBLE
// (GAP + advertising, reiniciando o advertising para o novo nome valer já no
// próximo escaneamento) e — se houver um app conectado — republica
// publicarInfoDispositivo() para refletir a mudança imediatamente. Usado
// tanto pelo comando remoto "set_device_name" quanto pelo editor de nome
// local (tela "Conexao com app").
void definirNomeDispositivo(const char* novoNome);

// Payload/JSON idêntico ao antigo tópico MQTT correspondente, apenas com um
// campo "topico" adicional (mesmo papel dos antigos tópicos separados) para
// o app distinguir os tipos de mensagem dentro do único canal BLE.
void publicarEstado();
void publicarEvento(uint8_t canal1based, char estado, int64_t tempoRelativoUs);
void publicarConfiguracaoCanais();

// Publicada uma única vez, logo que um app conecta (dados estáticos do
// equipamento: nome, versão, autor, MAC, device id, URL do manual) —
// equivalente aos campos fixos da tela "Sobre".
void publicarInfoDispositivo();

// Publicada periodicamente (INTERVALO_PUBLICACAO_TESTE_CANAIS_MS) enquanto
// conectado, independente da tela atual no display — nível elétrico e
// contagem de mudanças de cada canal, equivalente à tela "Teste de canais".
void publicarTesteCanais();

// Publicada sob demanda em resposta à ação "list_files" (equivalente às
// telas "Gerenciamento de arquivos"/"Selecionar arquivo" da análise).
void publicarListaArquivos();

// Publicada sob demanda em resposta à ação "load_repetition" (equivalente à
// tela "Eventos" da análise) — array vazio quando a repetição não existe ou
// não tem eventos.
void publicarEventosAnalise();

// Publicada sob demanda em resposta à ação "read_file_data": uma página (até
// TAMANHO_PAGINA_DADOS_ARQUIVO linhas) do CSV do arquivo, a partir da
// "offset"-ésima linha de dados (cabeçalho e linhas em branco não contam),
// junto com o número da repetição de cada linha — alimenta a tabela rolante
// de dados do arquivo no app (sem equivalente na tela física).
void publicarDadosArquivo(const char* nomeArquivo, uint16_t offset);

// Publicada em resposta à ação "save_measurement_name" (medição finalizada
// aguardando nome — ver experimentos::aguardandoNomeArquivo/nomeSugerido).
// "nome_existe" só é relevante quando "ok" é false: o app pode reenviar o
// mesmo comando com "sobrescrever":true para confirmar a sobrescrita.
void publicarResultadoNomeMedicao(bool ok, bool nomeExiste);

// Publicada em resposta a qualquer ação protegida por senha
// ("set_device_name", "set_data_analysis_enabled", "set_password" — ver
// configuracoes::validarSenha). "acao" repete o nome da ação BLE recebida,
// pra o app saber qual pedido falhou/teve sucesso.
void publicarResultadoAcaoProtegida(const char* acao, bool ok);

// Derruba a conexão BLE atual (se houver), forçando o app a reconectar —
// usado pelo item "Reconectar" da tela "Conexao com app".
void reconectar();

}  // namespace bluetooth_app
