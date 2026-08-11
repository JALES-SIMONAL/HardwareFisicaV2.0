#include "bluetooth_app.hpp"

#include <ArduinoJson.h>
#include <NimBLEDevice.h>
#include <Preferences.h>
#include <cstdio>
#include <cstring>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>

#include "MAIN.HPP"
#include "analise_dados.hpp"
#include "aquisicao.hpp"
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
NimBLEAdvertising* pAdvertising = nullptr;

// true só depois de init() rodar por completo. loop() é chamada de dentro
// de maquina_estados::tick() — inclusive numa chamada manual antecipada em
// main.cpp/setup(), ANTES de bluetooth_app::init() (ver comentário lá) —
// então loop() precisa dessa guarda pra não mexer em mutexBt/filaComandosBt
// enquanto ainda são nullptr.
bool iniciado = false;

// Nome anunciado no BLE: carregado da NVS em init() (ou
// NOME_DISPOSITIVO_BT_PADRAO, na primeira vez), trocável depois em tempo de
// execução por definirNomeDispositivo().
constexpr const char* NAMESPACE_PREFS_BT = "hwfisica_bt";
char nomeDispositivoBuffer[TAMANHO_MAX_NOME_DISPOSITIVO_BT + 1] = "";

char deviceIdBuffer[16] = "";
volatile uint16_t connHandleAtual = BLE_HS_CONN_HANDLE_NONE;
volatile bool clienteConectado = false;
bool clienteConectadoAnterior = false;
unsigned long ultimaPublicacaoEstadoMs = 0;
unsigned long ultimaPublicacaoTesteCanaisMs = 0;
constexpr uint32_t INTERVALO_PUBLICACAO_TESTE_CANAIS_MS = 300;

// Rede de segurança da reconexão BLE: enquanto não há app conectado,
// confirma periodicamente que o advertising está mesmo ativo e reinicia se
// não estiver (ver loop()). Cobre qualquer cenário em que o restart
// automático da lib (ServerCallbacks::onDisconnect) não tenha pego —
// silencioso na maioria das voltas do loop, só age quando de fato preciso.
unsigned long ultimaChecagemAdvertisingMs = 0;
constexpr uint32_t INTERVALO_CHECAGEM_ADVERTISING_MS = 5000;

// Paginação da tabela rolante de dados do arquivo (ver publicarDadosArquivo):
// cada página traz no máximo esta quantidade de linhas de dados.
constexpr uint16_t TAMANHO_PAGINA_DADOS_ARQUIVO = 20;

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
  if (deserializeJson(doc, linha) != DeserializationError::Ok) {
    Serial.printf("[DIAG][BT] JSON invalido recebido do app: \"%s\"\n", linha);
    return;
  }

  const char* acao = doc["action"] | "";
  Serial.printf("[DIAG][BT] Comando recebido do app: action=\"%s\" linha=\"%s\"\n", acao, linha);
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
  } else if (std::strcmp(acao, "restart_repetition") == 0) {
    cmd.tipo = comandos::CommandType::RestartRepetition;
  } else if (std::strcmp(acao, "reconnect") == 0) {
    reconectar();
    return;
  } else if (std::strcmp(acao, "list_files") == 0) {
    cmd.tipo = comandos::CommandType::ListFiles;
  } else if (std::strcmp(acao, "rename_file") == 0) {
    cmd.tipo = comandos::CommandType::RenameFile;
    std::strncpy(cmd.texto, doc["from"] | "", sizeof(cmd.texto) - 1);
    std::strncpy(cmd.texto2, doc["to"] | "", sizeof(cmd.texto2) - 1);
  } else if (std::strcmp(acao, "delete_file") == 0) {
    cmd.tipo = comandos::CommandType::DeleteFile;
    std::strncpy(cmd.texto, doc["nome"] | "", sizeof(cmd.texto) - 1);
  } else if (std::strcmp(acao, "delete_all_files") == 0) {
    cmd.tipo = comandos::CommandType::DeleteAllFiles;
  } else if (std::strcmp(acao, "save_measurement_name") == 0) {
    cmd.tipo = comandos::CommandType::SaveMeasurementName;
    std::strncpy(cmd.texto, doc["nome"] | "", sizeof(cmd.texto) - 1);
    cmd.valor = (doc["sobrescrever"] | false) ? 1 : 0;
  } else if (std::strcmp(acao, "load_repetition") == 0) {
    cmd.tipo = comandos::CommandType::LoadRepetition;
    std::strncpy(cmd.texto, doc["arquivo"] | "", sizeof(cmd.texto) - 1);
    cmd.valor = doc["repeticao"] | 0;
  } else if (std::strcmp(acao, "get_channels") == 0) {
    cmd.tipo = comandos::CommandType::GetChannels;
  } else if (std::strcmp(acao, "read_file_data") == 0) {
    cmd.tipo = comandos::CommandType::ReadFileData;
    std::strncpy(cmd.texto, doc["arquivo"] | "", sizeof(cmd.texto) - 1);
    cmd.valor = doc["offset"] | 0;
  } else if (std::strcmp(acao, "set_device_name") == 0) {
    // texto2 = senha: trocar o nome anunciado no BLE é uma das duas ações
    // protegidas por senha (ver configuracoes::validarSenha).
    cmd.tipo = comandos::CommandType::SetDeviceName;
    std::strncpy(cmd.texto, doc["nome"] | "", sizeof(cmd.texto) - 1);
    std::strncpy(cmd.texto2, doc["senha"] | "", sizeof(cmd.texto2) - 1);
  } else if (std::strcmp(acao, "set_datetime") == 0) {
    cmd.tipo = comandos::CommandType::SetDateTime;
    cmd.valor = doc["epoch"] | 0;
  } else if (std::strcmp(acao, "set_data_analysis_enabled") == 0) {
    cmd.tipo = comandos::CommandType::SetDataAnalysisEnabled;
    cmd.valor = (doc["habilitado"] | false) ? 1 : 0;
    std::strncpy(cmd.texto, doc["senha"] | "", sizeof(cmd.texto) - 1);
  } else if (std::strcmp(acao, "set_password") == 0) {
    cmd.tipo = comandos::CommandType::SetPassword;
    std::strncpy(cmd.texto, doc["senha_atual"] | "", sizeof(cmd.texto) - 1);
    std::strncpy(cmd.texto2, doc["nova_senha"] | "", sizeof(cmd.texto2) - 1);
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
  void onConnect(NimBLEServer* pServidor, ble_gap_conn_desc* desc) override {
    TravaBt trava;
    connHandleAtual = desc->conn_handle;
    clienteConectado = true;

    // Pede um supervision timeout curto (4s) ao central: sem isso, o
    // padrao negociado por Android/iOS/Windows costuma passar de 15-20s, e
    // só depois desse tempo o firmware percebe que o link caiu (silencio de
    // rádio, fora de alcance) e volta a anunciar — na prática, um "cair e
    // não reconectar" por quase meio minuto. min/max interval em unidades
    // de 1.25ms (24=30ms, 40=50ms), latency 0, timeout em unidades de 10ms
    // (400=4s). Valores dentro das faixas recomendadas pela Apple (timeout
    // > 2*maxInterval*(1+latency), interval >= 15ms) para não serem
    // rejeitados/renegociados pelo central.
    pServidor->updateConnParams(desc->conn_handle, 24, 40, 0, 400);
  }

  void onDisconnect(NimBLEServer* /*pServidor*/, ble_gap_conn_desc* /*desc*/) override {
    TravaBt trava;
    connHandleAtual = BLE_HS_CONN_HANDLE_NONE;
    clienteConectado = false;

    // O NimBLE-Arduino já reanuncia sozinho após uma desconexão
    // (NimBLEServer::m_advertiseOnDisconnect, default true — ver
    // handleGapEvent/BLE_GAP_EVENT_DISCONNECT na lib). Esta chamada é só
    // defensiva/explícita: garante a reconexão mesmo que esse default mude
    // numa atualização futura da lib, e é barata (startAdvertising() é
    // no-op se o advertising já estiver ativo).
    if (pAdvertising != nullptr) pAdvertising->start();
  }
};

RxCallbacks rxCallbacks;
ServerCallbacks serverCallbacks;

// Carrega o nome salvo na NVS para nomeDispositivoBuffer (ou
// NOME_DISPOSITIVO_BT_PADRAO, se nunca foi trocado).
void carregarNomeDispositivo() {
  Preferences prefs;
  prefs.begin(NAMESPACE_PREFS_BT, true);
  const String salvo = prefs.getString("nome", NOME_DISPOSITIVO_BT_PADRAO);
  prefs.end();

  std::strncpy(nomeDispositivoBuffer, salvo.c_str(), sizeof(nomeDispositivoBuffer) - 1);
  nomeDispositivoBuffer[sizeof(nomeDispositivoBuffer) - 1] = '\0';
}

}  // namespace

void init() {
  mutexBt = xSemaphoreCreateRecursiveMutex();
  filaComandosBt = xQueueCreate(BT_COMMAND_QUEUE_LEN, sizeof(LinhaComando));

  carregarNomeDispositivo();
  NimBLEDevice::init(nomeDispositivoBuffer);

  pServidor = NimBLEDevice::createServer();
  pServidor->setCallbacks(&serverCallbacks);

  NimBLEService* pServico = pServidor->createService(SERVICE_UUID);
  pCaracteristicaTx = pServico->createCharacteristic(CHARACTERISTIC_UUID_TX, NIMBLE_PROPERTY::NOTIFY);
  NimBLECharacteristic* pCaracteristicaRx = pServico->createCharacteristic(
      CHARACTERISTIC_UUID_RX, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR);
  pCaracteristicaRx->setCallbacks(&rxCallbacks);
  pServico->start();

  pAdvertising = NimBLEDevice::getAdvertising();
  pAdvertising->addServiceUUID(SERVICE_UUID);
  pAdvertising->setName(nomeDispositivoBuffer);
  // Ajuda clientes iOS a descobrirem o serviço durante o scan (recomendação
  // padrão do NimBLE-Arduino para compatibilidade com iPhone/iPad).
  pAdvertising->setScanResponse(true);
  pAdvertising->start();

  gerarDeviceId();
  iniciado = true;
}

const char* nomeDispositivo() { return nomeDispositivoBuffer; }

void definirNomeDispositivo(const char* novoNome) {
  TravaBt trava;
  Serial.printf("[DIAG][BT] definirNomeDispositivo(\"%s\") chamado\n",
                novoNome != nullptr ? novoNome : "(nullptr)");
  if (novoNome == nullptr || novoNome[0] == '\0') {
    Serial.println("[DIAG][BT] nome vazio/nulo — ignorado");
    return;
  }

  std::strncpy(nomeDispositivoBuffer, novoNome, sizeof(nomeDispositivoBuffer) - 1);
  nomeDispositivoBuffer[sizeof(nomeDispositivoBuffer) - 1] = '\0';

  Preferences prefs;
  const bool prefsOk = prefs.begin(NAMESPACE_PREFS_BT, false);
  const size_t bytesGravados = prefs.putString("nome", nomeDispositivoBuffer);
  prefs.end();
  Serial.printf(
      "[DIAG][BT] prefs.begin=%d putString(\"nome\",\"%s\") bytesGravados=%u nomeDispositivoBuffer=\"%s\"\n",
      static_cast<int>(prefsOk), nomeDispositivoBuffer, static_cast<unsigned>(bytesGravados),
      nomeDispositivoBuffer);

  // Atualiza o nome GAP (visível a um app já conectado) e o pacote de
  // advertising (visível num scan futuro) — precisa reiniciar o advertising
  // para o pacote atualizado valer, já que setName() só muda o buffer interno.
  NimBLEDevice::setDeviceName(nomeDispositivoBuffer);
  if (pAdvertising != nullptr) {
    pAdvertising->setName(nomeDispositivoBuffer);
    const bool paradaOk = pAdvertising->stop();
    const bool inicioOk = pAdvertising->start();
    Serial.printf("[DIAG][BT] advertising stop=%d start=%d\n", static_cast<int>(paradaOk),
                  static_cast<int>(inicioOk));
  } else {
    Serial.println("[DIAG][BT] pAdvertising == nullptr (nao deveria acontecer apos init())");
  }

  Serial.printf("[DIAG][BT] clienteConectado=%d — %s publicarInfoDispositivo()\n",
                static_cast<int>(clienteConectado), clienteConectado ? "chamando" : "pulando");
  if (clienteConectado) publicarInfoDispositivo();
}

void loop() {
  if (!iniciado) return;
  TravaBt trava;

  const bool conectadoAgora = clienteConectado;
  if (conectadoAgora && !clienteConectadoAnterior) {
    Serial.println("[BT] Cliente conectado");
    publicarInfoDispositivo();
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

  if (!conectadoAgora) {
    const unsigned long agora = millis();
    if (pAdvertising != nullptr && agora - ultimaChecagemAdvertisingMs >= INTERVALO_CHECAGEM_ADVERTISING_MS) {
      ultimaChecagemAdvertisingMs = agora;
      if (!pAdvertising->isAdvertising()) {
        Serial.println("[DIAG][BT] Advertising parado sem cliente conectado — reiniciando");
        pAdvertising->start();
      }
    }
    return;
  }

  LinhaComando linha;
  while (xQueueReceive(filaComandosBt, &linha, 0) == pdTRUE) {
    processarLinha(linha.texto);
  }

  const unsigned long agora = millis();
  if (agora - ultimaPublicacaoEstadoMs >= INTERVALO_PUBLICACAO_ESTADO_MS) {
    ultimaPublicacaoEstadoMs = agora;
    publicarEstado();
  }
  if (agora - ultimaPublicacaoTesteCanaisMs >= INTERVALO_PUBLICACAO_TESTE_CANAIS_MS) {
    ultimaPublicacaoTesteCanaisMs = agora;
    publicarTesteCanais();
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
  doc["tempo_decorrido_s"] = static_cast<int64_t>(experimentos::tempoDecorridoUs() / 1000000);
  doc["sd_usado_kb"] = static_cast<uint32_t>(armazenamento::espacoUsadoBytes() / 1024);
  doc["sd_total_kb"] = static_cast<uint32_t>(armazenamento::espacoTotalBytes() / 1024);

  // Medição finalizada (última repetição) mas ainda sem nome — arquivo
  // fechado como "_tmp_exp.csv", esperando o app (ou o menu local) chamar
  // "save_measurement_name". nome_sugerido só é preenchido nesse caso.
  const bool aguardandoNome = experimentos::aguardandoNomeArquivo();
  doc["aguardando_nome"] = aguardandoNome;
  if (aguardandoNome) doc["nome_sugerido"] = experimentos::nomeSugerido();

  doc["analise_dados_habilitada"] = configuracoes::analiseDadosHabilitada();

  char payload[384];
  const size_t tamanho = serializeJson(doc, payload, sizeof(payload));
  enviarLinha(payload, tamanho);
}

void publicarInfoDispositivo() {
  TravaBt trava;
  if (!clienteConectado) return;

  JsonDocument doc;
  doc["topico"] = "info";
  doc["equipamento"] = configuracoes::NOME_EQUIPAMENTO;
  doc["versao_firmware"] = configuracoes::VERSAO_FIRMWARE;
  doc["autor"] = configuracoes::AUTOR;
  doc["device_id"] = deviceIdBuffer;
  doc["mac"] = enderecoMac();
  doc["manual_url"] = configuracoes::MANUAL_URL;
  doc["nome_bt"] = nomeDispositivoBuffer;

  // 256 bastava antes de "nome_bt" existir; com autor + manual_url (~36
  // chars) + até 20 chars de nome BLE, o total podia passar de 256 e
  // serializeJson() truncava silenciosamente o JSON (o app então falhava ao
  // decodificar e nunca via o nome novo) — por isso o buffer maior aqui.
  char payload[384];
  const size_t tamanho = serializeJson(doc, payload, sizeof(payload));
  enviarLinha(payload, tamanho);
}

void publicarTesteCanais() {
  TravaBt trava;
  if (!clienteConectado) return;

  JsonDocument doc;
  doc["topico"] = "teste_canais";
  JsonArray canaisArray = doc["canais"].to<JsonArray>();
  for (uint8_t i = 1; i <= NUM_CHANNELS; i++) {
    JsonObject c = canaisArray.add<JsonObject>();
    c["canal"] = i;
    c["nivel"] = aquisicao::nivelAtual(i) ? "H" : "L";
    c["mudancas"] = aquisicao::quantidadeMudancas(i);
  }

  char payload[320];
  const size_t tamanho = serializeJson(doc, payload, sizeof(payload));
  enviarLinha(payload, tamanho);
}

void publicarListaArquivos() {
  TravaBt trava;
  if (!clienteConectado) return;

  constexpr uint16_t MAX_ARQUIVOS_LISTA_BT = 20;
  armazenamento::InfoArquivo arquivos[MAX_ARQUIVOS_LISTA_BT];
  const uint16_t quantidade = armazenamento::listarArquivos(arquivos, MAX_ARQUIVOS_LISTA_BT);

  JsonDocument doc;
  doc["topico"] = "files";
  JsonArray arquivosArray = doc["arquivos"].to<JsonArray>();
  for (uint16_t i = 0; i < quantidade; i++) {
    JsonObject a = arquivosArray.add<JsonObject>();
    a["nome"] = arquivos[i].nome;
    a["tamanho"] = arquivos[i].tamanhoBytes;
  }

  char payload[1024];
  const size_t tamanho = serializeJson(doc, payload, sizeof(payload));
  enviarLinha(payload, tamanho);
}

void publicarEventosAnalise() {
  TravaBt trava;
  if (!clienteConectado) return;

  const uint8_t quantidade = analise_dados::quantidadeEventosCarregados();

  JsonDocument doc;
  doc["topico"] = "analise_eventos";
  JsonArray eventosArray = doc["eventos"].to<JsonArray>();
  for (uint8_t i = 0; i < quantidade; i++) {
    const analise_dados::EventoLido& ev = analise_dados::evento(i);
    JsonObject e = eventosArray.add<JsonObject>();
    e["canal"] = ev.canal;
    const char estadoStr[2] = {ev.estado, '\0'};
    e["estado"] = estadoStr;
    e["tempo_us"] = static_cast<long long>(ev.tempoUs);
  }

  char payload[2048];
  const size_t tamanho = serializeJson(doc, payload, sizeof(payload));
  enviarLinha(payload, tamanho);
}

void publicarDadosArquivo(const char* nomeArquivo, uint16_t offset) {
  TravaBt trava;
  if (!clienteConectado) return;

  JsonDocument doc;
  doc["topico"] = "dados_arquivo";
  doc["arquivo"] = nomeArquivo;
  doc["offset"] = offset;
  JsonArray linhasArray = doc["linhas"].to<JsonArray>();
  bool temMais = false;

  if (armazenamento::abrirParaLeitura(nomeArquivo)) {
    char linha[32];
    uint16_t repeticaoAtualIdx = 0;
    bool linhaAnteriorEraDados = false;
    uint16_t linhasDadosVistas = 0;

    while (armazenamento::lerProximaLinha(linha, sizeof(linha))) {
      if (linha[0] == '\0') {
        // Linha em branco: separa repetições (mesma regra de
        // analise_dados::carregarRepeticao — só avança se a repetição
        // anterior teve alguma linha de dados válida).
        if (linhaAnteriorEraDados) repeticaoAtualIdx++;
        linhaAnteriorEraDados = false;
        continue;
      }

      unsigned canal = 0;
      char estado = '\0';
      long long tempoUs = 0;
      if (std::sscanf(linha, "%u,%c,%lld", &canal, &estado, &tempoUs) != 3) {
        continue;  // Cabeçalho ou linha corrompida: ignora.
      }
      linhaAnteriorEraDados = true;

      if (linhasDadosVistas < offset) {
        linhasDadosVistas++;
        continue;
      }

      if (linhasArray.size() >= TAMANHO_PAGINA_DADOS_ARQUIVO) {
        temMais = true;
        break;
      }

      JsonObject l = linhasArray.add<JsonObject>();
      l["repeticao"] = repeticaoAtualIdx;
      l["canal"] = static_cast<uint8_t>(canal);
      const char estadoStr[2] = {estado, '\0'};
      l["estado"] = estadoStr;
      l["tempo_us"] = static_cast<long long>(tempoUs);
      linhasDadosVistas++;
    }
    armazenamento::fecharLeitura();
  }

  doc["tem_mais"] = temMais;

  char payload[1536];
  const size_t tamanho = serializeJson(doc, payload, sizeof(payload));
  enviarLinha(payload, tamanho);
}

void publicarResultadoNomeMedicao(bool ok, bool nomeExiste) {
  TravaBt trava;
  if (!clienteConectado) return;

  JsonDocument doc;
  doc["topico"] = "resultado_nome_medicao";
  doc["ok"] = ok;
  doc["nome_existe"] = nomeExiste;

  char payload[96];
  const size_t tamanho = serializeJson(doc, payload, sizeof(payload));
  enviarLinha(payload, tamanho);
}

void publicarResultadoAcaoProtegida(const char* acao, bool ok) {
  TravaBt trava;
  if (!clienteConectado) return;

  JsonDocument doc;
  doc["topico"] = "resultado_acao_protegida";
  doc["acao"] = acao;
  doc["ok"] = ok;

  char payload[96];
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

void reconectar() {
  TravaBt trava;
  if (clienteConectado && pServidor != nullptr) {
    pServidor->disconnect(connHandleAtual);
  }
}

}  // namespace bluetooth_app
