#include <WiFi.h>
#include <WebSocketsServer.h>
#include <ArduinoJson.h>
#include <WebServer.h>
#include <Adafruit_NeoPixel.h>
#include <EEPROM.h>

// ============================================================
//                       CONFIGURAÇÕES
// ============================================================
const char* WIFI_SSID = "CONTROLE_ESP32";
const char* WIFI_PASSWORD = "12345678";

#define BTN_UP 27
#define BTN_DOWN 25
#define BTN_OK 26
#define BTN_LEFT 33
#define BTN_RIGHT 14

const int SENSOR_1_PIN = 32;
const int SENSOR_2_PIN = 35;
const int SENSOR_3_PIN = 34;

#define PIN_NEO 21
#define NUM_LEDS 3
Adafruit_NeoPixel pixels(NUM_LEDS, PIN_NEO, NEO_GRB + NEO_KHZ800);

#define EEPROM_SIZE 4096

// ============================================================
//                    SISTEMA DE ARQUIVOS EEPROM
// ============================================================
#define STORAGE_MAGIC 0x31534645UL  // "EFS1"
#define STORAGE_VERSION 1

#define MAX_FILES 30
#define MAX_EVENT_RECORDS 250
#define MAX_CAPTURE_EVENTS 500
#define FILE_NAME_LEN 15

enum FileType : uint8_t {
  FILE_TYPE_CHRONO = 1,
  FILE_TYPE_COUNTER = 2
};

enum DetectMode : uint8_t {
  DETECT_RISING = 0,
  DETECT_FALLING = 1,
  DETECT_BOTH = 2
};

struct __attribute__((packed)) StorageHeader {
  uint32_t magic;
  uint8_t version;
  uint8_t sensorMode[3];
  uint8_t fileCount;
  uint16_t usedEvents;
  uint16_t nextChronoNumber;
  uint16_t nextCounterNumber;
  uint8_t reserved[3];
};

struct __attribute__((packed)) FileRecord {
  uint16_t id;
  uint8_t type; // 1=cronometro, 2=contador
  char name[16];
  uint32_t timestampSec;
  uint32_t durationMs;
  uint16_t eventStart;
  uint16_t eventCount;
  uint8_t reserved;
};

struct __attribute__((packed)) EventRecord {
  uint8_t sensor;      // 1..3
  uint32_t timestampMs;
  uint8_t estado;      // HIGH/LOW
  int32_t counterPulses; // para contador
};

static_assert(sizeof(StorageHeader) <= 32, "StorageHeader muito grande");
static_assert(sizeof(FileRecord) == 32, "FileRecord esperado em 32 bytes");
static_assert(sizeof(EventRecord) == 10, "EventRecord esperado em 10 bytes");

StorageHeader gStorage;
FileRecord gFiles[MAX_FILES];
EventRecord gEvents[MAX_EVENT_RECORDS];

const uint16_t EEPROM_ADDR_HEADER = 0;
const uint16_t EEPROM_ADDR_FILES = EEPROM_ADDR_HEADER + sizeof(StorageHeader);
const uint16_t EEPROM_ADDR_EVENTS = EEPROM_ADDR_FILES + sizeof(FileRecord) * MAX_FILES;

// ============================================================
//                       ESTADOS DO SISTEMA
// ============================================================
enum States {
  STATE_MENU = 0,
  STATE_CRONOMETRO = 1,
  STATE_CONTADOR = 2,
  STATE_ARQUIVOS = 3,
  STATE_CONFIG = 4,
  STATE_TESTE = 5,
  STATE_DADOS = 6
};

// ============================================================
//                    INSTÂNCIAS DE SERVIDOR
// ============================================================
WebServer server(80);
WebSocketsServer webSocket = WebSocketsServer(81);

// ============================================================
//                  VARIÁVEIS GLOBAIS DE EXECUÇÃO
// ============================================================
volatile int currentState = STATE_MENU;
volatile int menuSelection = 0;
volatile bool stateChanged = false;

// Cronômetro
volatile unsigned long chronoStartTime = 0;
volatile unsigned long chronoTime = 0;
volatile unsigned long chronoPausedTime = 0;
volatile bool chronoRunning = false;
volatile bool chronoActive = false;
String draftChronoName = "";

// Contador
volatile int counterValues[3] = {0, 0, 0};
#define counterValue (counterValues[0] + counterValues[1] + counterValues[2])
volatile bool counterActive = false;
volatile unsigned long counterStartTime = 0;
String draftCounterName = "";

// Captura de eventos em RAM (sessão atual)
EventRecord captureEvents[MAX_CAPTURE_EVENTS];
uint16_t captureEventCount = 0;

int prevSensor1 = LOW;
int prevSensor2 = LOW;
int prevSensor3 = LOW;

String lastWarning = "";

// Botões
struct Button {
  int pin;
  String command;
  bool pressed;
  unsigned long pressTime;
};

Button buttons[5] = {
  {BTN_UP, "up", false, 0},
  {BTN_DOWN, "down", false, 0},
  {BTN_OK, "ok", false, 0},
  {BTN_LEFT, "left", false, 0},
  {BTN_RIGHT, "right", false, 0}
};

const unsigned long MIN_PRESS_TIME = 30;
const unsigned long MAX_PRESS_TIME = 2000;

// LED flash não-bloqueante por sensor
uint8_t ledFlashState[NUM_LEDS] = {0, 0, 0};      // 0=idle, 1=on, 2=off
uint32_t ledFlashColor[NUM_LEDS] = {0, 0, 0};
uint8_t ledFlashCount[NUM_LEDS] = {0, 0, 0};      // ciclos on/off restantes
unsigned long ledFlashStart[NUM_LEDS] = {0, 0, 0};
uint16_t ledFlashDelayMs[NUM_LEDS] = {40, 40, 40};

// Navegação e sincronização por botões físicos
int configSelectedSensor = 0;
int selectedFileIndex = -1;
uint16_t selectedFileId = 0;
uint8_t lastClientCount = 0;
unsigned long noClientSinceMs = 0;
unsigned long counterElapsedMs = 0;

// ============================================================
//                    PROTÓTIPOS DE FUNÇÕES
// ============================================================
void initWiFi();
void initButtons();
void initServers();
void initSensors();
void initEEPROM();

void handleRoot();
void handleDownloadFile();
void handleWebSocketEvent(uint8_t num, WStype_t type, uint8_t* payload, size_t length);

void processCommand(const String& cmd);
void processJsonCommand(uint8_t num, const String& payload);
void handleMenuInput(const String& cmd);
void handleChronoInput(const String& cmd);
void handleContadorInput(const String& cmd);
void handleConfigInput(const String& cmd);
void handleFilesInput(const String& cmd);

void checkButtons();
void updateChrono();
void checkSensoresChrono();
void checkSensoresContador();
void updateLedFlashes();
void checkAutoResetOnDisconnect();
void resetSessionToMenuOnDisconnect();

void sendStateToClients();
void sendFilesList(uint8_t clientNum = 255);
void sendFileView(uint8_t clientNum, uint16_t fileId);
void sendInfoMessage(const String& msg, bool warning = false);

void turnOffAllLeds();
void setLEDColor(uint32_t color);
void piscarLED(int ledIndex, uint32_t color, int vezes, int delayMs);
void updateNeoPixelsFromSensors();

void changeState(States newState, int newMenuSelection = 0);
int readSensor1();
int readSensor2();
int readSensor3();

void controlarCronometro(bool iniciar);
void resetarCronometro();
void finalizarCronometro(const String& suggestedName);
void iniciarContador();
void finalizarContador(const String& suggestedName);

bool shouldRegisterByMode(int prevState, int currState, uint8_t mode);
void registerCaptureEvent(uint8_t sensor, uint32_t timestampMs, uint8_t estado, int32_t counterPulses);

void storageInitOrLoad();
void storageFactoryReset();
bool storageCommit();
uint16_t storageUsedBytes();
uint16_t storageFreeBytes();

String sanitizeFileName(const String& raw, const String& fallback);
String defaultFileName(uint8_t fileType);
int findFileIndexById(uint16_t id);

bool saveSessionFile(uint8_t fileType, const String& requestedName, uint32_t durationMs,
                     EventRecord* events, uint16_t eventCount, uint16_t& outFileId);
bool deleteFileById(uint16_t id);
bool renameFileById(uint16_t id, const String& newName);
String csvFromFile(const FileRecord& rec);
String fileTypeLabel(uint8_t type);
const char* detectModeLabel(uint8_t mode);

// ============================================================
//                      HTML EMBARCADO
// ============================================================
const char* HTML_PAGE = R"rawliteral(
<!DOCTYPE html>
<html lang="pt-BR">
<head>
  <meta charset="UTF-8" />
  <meta name="viewport" content="width=device-width, initial-scale=1.0" />
  <title>Controle ESP32</title>
  <style>
    :root {
      --bg1: #0f172a;
      --bg2: #111827;
      --card: #1f2937;
      --card2: #111827;
      --text: #e5e7eb;
      --muted: #9ca3af;
      --accent: #22d3ee;
      --accent2: #60a5fa;
      --ok: #22c55e;
      --warn: #f59e0b;
      --danger: #ef4444;
      --shadow: 0 10px 30px rgba(0,0,0,.35);
      --radius: 18px;
    }

    * { box-sizing: border-box; }
    html, body { width: 100%; height: 100%; margin: 0; }

    body {
      font-family: Inter, system-ui, -apple-system, Segoe UI, Roboto, Arial, sans-serif;
      color: var(--text);
      background: radial-gradient(circle at 10% 10%, #1e3a8a 0%, transparent 35%),
                  radial-gradient(circle at 90% 20%, #0e7490 0%, transparent 35%),
                  linear-gradient(140deg, var(--bg1), var(--bg2));
      display: flex;
      justify-content: center;
      align-items: center;
      padding: 10px;
      overflow: hidden;
    }

    .app {
      width: min(100%, 460px);
      height: min(100vh - 12px, 860px);
      border-radius: var(--radius);
      background: linear-gradient(180deg, rgba(255,255,255,.06), rgba(255,255,255,.02));
      border: 1px solid rgba(255,255,255,.12);
      box-shadow: var(--shadow);
      backdrop-filter: blur(7px);
      display: grid;
      grid-template-rows: auto 1fr auto;
      overflow: hidden;
    }

    .header {
      display: flex;
      justify-content: space-between;
      align-items: center;
      padding: 12px 14px;
      border-bottom: 1px solid rgba(255,255,255,.12);
      background: linear-gradient(90deg, rgba(34,211,238,.2), rgba(96,165,250,.15));
      font-weight: 700;
      letter-spacing: .3px;
    }

    .header small { color: var(--muted); font-weight: 600; }

    .content {
      overflow: auto;
      padding: 12px;
      height: 100%;
      /* espaço fixo para pad virtual sempre visível */
      max-height: calc(100vh - 220px);
    }

    .state { display: none; animation: fade .22s ease; }
    .state.show { display: block; }

    @keyframes fade {
      from { opacity: 0; transform: translateY(8px); }
      to { opacity: 1; transform: translateY(0); }
    }

    .grid-menu {
      display: grid;
      grid-template-columns: repeat(2, minmax(0, 1fr));
      gap: 10px;
    }

    .menu-item {
      border: 1px solid rgba(255,255,255,.12);
      background: linear-gradient(180deg, rgba(255,255,255,.06), rgba(255,255,255,.02));
      padding: 14px;
      border-radius: 14px;
      min-height: 84px;
      display: flex;
      flex-direction: column;
      justify-content: center;
      gap: 4px;
      cursor: pointer;
      transition: .2s ease;
      user-select: none;
    }

    .menu-item .icon { font-size: 22px; }
    .menu-item .title { font-weight: 700; }
    .menu-item .desc { color: var(--muted); font-size: 12px; }

    .menu-item.active, .menu-item:hover {
      transform: translateY(-2px);
      border-color: rgba(34,211,238,.75);
      box-shadow: 0 0 0 1px rgba(34,211,238,.3), 0 10px 24px rgba(34,211,238,.18);
    }

    .card {
      background: linear-gradient(180deg, rgba(255,255,255,.06), rgba(255,255,255,.02));
      border: 1px solid rgba(255,255,255,.1);
      border-radius: 14px;
      padding: 12px;
      margin-bottom: 10px;
    }

    .display-big {
      text-align: center;
      font-size: clamp(34px, 8vw, 54px);
      font-weight: 800;
      letter-spacing: 2px;
      margin: 8px 0;
      text-shadow: 0 0 24px rgba(96,165,250,.55);
    }

    .status {
      text-align: center;
      color: var(--muted);
      font-weight: 600;
      margin-bottom: 8px;
    }

    .row { display: flex; gap: 8px; flex-wrap: wrap; }
    .row > * { flex: 1; min-width: 120px; }

    .btn {
      border: 1px solid rgba(255,255,255,.16);
      background: linear-gradient(180deg, rgba(34,211,238,.2), rgba(34,211,238,.08));
      color: #ecfeff;
      border-radius: 10px;
      padding: 10px;
      font-weight: 700;
      cursor: pointer;
      transition: .15s ease;
    }

    .btn:hover { transform: translateY(-1px); box-shadow: 0 8px 18px rgba(34,211,238,.25); }
    .btn.warn { background: linear-gradient(180deg, rgba(245,158,11,.3), rgba(245,158,11,.12)); }
    .btn.danger { background: linear-gradient(180deg, rgba(239,68,68,.3), rgba(239,68,68,.12)); }
    .btn.ok { background: linear-gradient(180deg, rgba(34,197,94,.3), rgba(34,197,94,.12)); }

    input, select {
      width: 100%;
      border: 1px solid rgba(255,255,255,.14);
      border-radius: 10px;
      background: rgba(17,24,39,.8);
      color: var(--text);
      padding: 8px 10px;
      font-size: 14px;
      outline: none;
    }

    .kpi {
      display: grid;
      grid-template-columns: repeat(2, minmax(0, 1fr));
      gap: 10px;
    }

    .kpi .item {
      background: rgba(17,24,39,.55);
      border: 1px solid rgba(255,255,255,.1);
      border-radius: 12px;
      padding: 10px;
      font-size: 13px;
    }

    .kpi .item strong { display: block; font-size: 18px; margin-top: 3px; }

    .pill {
      display: inline-block;
      font-size: 12px;
      padding: 4px 8px;
      border-radius: 999px;
      border: 1px solid rgba(255,255,255,.14);
      background: rgba(17,24,39,.7);
      margin-right: 4px;
      margin-bottom: 4px;
    }

    .sensor-status-wrap {
      display: flex;
      gap: 14px;
      flex-wrap: wrap;
      margin-top: 8px;
    }

    .sensor-dot {
      width: 56px;
      height: 56px;
      border-radius: 50%;
      display: flex;
      align-items: center;
      justify-content: center;
      font-weight: 800;
      font-size: 19px;
      color: #fff;
      border: 2px solid rgba(255,255,255,.25);
      text-shadow: 0 1px 3px rgba(0,0,0,.45);
      box-shadow: inset 0 0 0 1px rgba(255,255,255,.12);
    }

    .sensor-dot.high {
      background: linear-gradient(180deg, rgba(239,68,68,.95), rgba(185,28,28,.95));
    }

    .sensor-dot.low {
      background: linear-gradient(180deg, rgba(34,197,94,.95), rgba(22,163,74,.95));
    }

    .sensor-item {
      display: flex;
      flex-direction: column;
      align-items: center;
      gap: 6px;
      min-width: 64px;
    }

    .sensor-label {
      font-size: 12px;
      color: var(--muted);
    }

    #filesTable .file-row { cursor: pointer; }
    #filesTable .file-row.selected { background: rgba(34,211,238,.08); }
    #filesTable td.file-name-cell { min-width: 180px; }
    #filesTable input.file-name-input {
      min-width: 17ch;
      width: 100%;
      font-family: ui-monospace, SFMono-Regular, Menlo, Consolas, monospace;
    }
    #filesTable .actions-cell { background: rgba(2,6,23,.45); }

    table {
      width: 100%;
      border-collapse: collapse;
      font-size: 12px;
    }

    th, td {
      border-bottom: 1px solid rgba(255,255,255,.09);
      text-align: left;
      padding: 6px 4px;
      vertical-align: top;
    }

    .table-wrap {
      max-height: 230px;
      overflow: auto;
      border: 1px solid rgba(255,255,255,.08);
      border-radius: 10px;
    }

    .muted { color: var(--muted); }

    .bottom-pad {
      border-top: 1px solid rgba(255,255,255,.12);
      padding: 8px 10px;
      background: rgba(2,6,23,.45);
    }

    .pad-grid {
      display: grid;
      grid-template-columns: repeat(3, 54px);
      justify-content: center;
      gap: 8px;
    }

    .pad-btn {
      width: 54px;
      height: 54px;
      border-radius: 12px;
      border: 1px solid rgba(255,255,255,.2);
      background: linear-gradient(180deg, rgba(96,165,250,.22), rgba(96,165,250,.08));
      color: white;
      font-size: 20px;
      cursor: pointer;
    }

    .pad-btn:active { transform: scale(.96); }
    .empty { visibility: hidden; }

    .toast {
      position: fixed;
      bottom: 90px;
      left: 50%;
      transform: translateX(-50%);
      background: rgba(17,24,39,.96);
      border: 1px solid rgba(34,211,238,.6);
      color: #e0f2fe;
      padding: 10px 14px;
      border-radius: 10px;
      font-size: 13px;
      box-shadow: var(--shadow);
      display: none;
      z-index: 15;
    }

    @media (max-width: 420px), (max-height: 720px) {
      .app { height: 100vh; border-radius: 0; }
      .content { max-height: calc(100vh - 210px); }
      .pad-grid { grid-template-columns: repeat(3, 48px); }
      .pad-btn { width: 48px; height: 48px; }
      .menu-item { min-height: 76px; padding: 10px; }
    }
  </style>
</head>
<body>
  <div class="app">
    <div class="header">
      <div>⚡ Controle ESP32</div>
      <small id="memInfo">Memória: --</small>
    </div>

    <div class="content">
      <div id="toast" class="toast"></div>

      <!-- MENU -->
      <section id="stateMenu" class="state show">
        <div class="grid-menu" id="menuGrid">
          <div class="menu-item active" data-index="0"><div class="icon">⏱️</div><div class="title">Cronômetro</div><div class="desc">Captura por sensores</div></div>
          <div class="menu-item" data-index="1"><div class="icon">🔢</div><div class="title">Contador</div><div class="desc">Pulso + eventos</div></div>
          <div class="menu-item" data-index="2"><div class="icon">📁</div><div class="title">Arquivos</div><div class="desc">Visualizar e baixar</div></div>
          <div class="menu-item" data-index="3"><div class="icon">⚙️</div><div class="title">Configuração</div><div class="desc">Modo dos sensores</div></div>
          <div class="menu-item" data-index="4"><div class="icon">🧪</div><div class="title">Teste</div><div class="desc">Leitura em tempo real</div></div>
          <div class="menu-item" data-index="5"><div class="icon">ℹ️</div><div class="title">Dados</div><div class="desc">Rede e memória</div></div>
        </div>
        <p class="muted" style="margin-top:10px">[⬆/⬇] Navegar · [◉] Selecionar</p>
      </section>

      <!-- CRONÔMETRO -->
      <section id="stateChrono" class="state">
        <div class="card">
          <div class="display-big" id="chronoDisplay">00:00:00</div>
          <div class="status" id="chronoStatus">Parado</div>
          <div class="row">
            <button class="btn" id="chronoPlay">[◉] Iniciar</button>
            <button class="btn warn" id="chronoStart">[➡] Iniciar</button>
          </div>
          <div style="margin-top:8px">
            <label class="muted">Nome do arquivo (até 15 chars)</label>
            <input id="chronoName" maxlength="15" placeholder="Cronometro_1" />
          </div>
          <div class="row" style="margin-top:8px">
            <button class="btn ok" id="chronoSaveBack">[⬇] Salvar e voltar</button>
            <button class="btn" id="chronoBack">[⬅] Voltar</button>
          </div>
        </div>

        <div class="card">
          <strong>Eventos da sessão</strong>
          <div class="table-wrap" style="margin-top:8px">
            <table>
              <thead><tr><th>#</th><th>Sensor</th><th>Tempo (ms)</th><th>Estado</th></tr></thead>
              <tbody id="chronoEvents"></tbody>
            </table>
          </div>
        </div>
      </section>

      <!-- CONTADOR -->
      <section id="stateCounter" class="state">
        <div class="card">
          <div class="row" style="gap:8px; flex-wrap:wrap; margin-bottom:8px">
            <div style="flex:1; min-width:120px">
              <label class="muted">Sensor 1</label>
              <div class="display-big" id="counterDisplay1">0</div>
            </div>
            <div style="flex:1; min-width:120px">
              <label class="muted">Sensor 2</label>
              <div class="display-big" id="counterDisplay2">0</div>
            </div>
            <div style="flex:1; min-width:120px">
              <label class="muted">Sensor 3</label>
              <div class="display-big" id="counterDisplay3">0</div>
            </div>
          </div>
          <label class="muted">Total</label>
          <div class="display-big" id="counterDisplayTotal">0</div>
          <p class="muted" style="margin:2px 0 10px 0">O contador incrementa automaticamente por pulsos dos sensores.</p>
          <div class="row" style="margin-top:8px">
            <button class="btn" id="counterToggle">[◉] Iniciar/Parar</button>
          </div>
          <div style="margin-top:8px">
            <label class="muted">Nome do arquivo (até 15 chars)</label>
            <input id="counterName" maxlength="15" placeholder="Contador_1" />
          </div>
          <div class="row" style="margin-top:8px">
            <button class="btn ok" id="counterSaveBack">[⬇] Salvar e voltar</button>
            <button class="btn" id="counterBack">[⬅] Voltar</button>
          </div>
        </div>

        <div class="card">
          <strong>Eventos (com contador_pulsos)</strong>
          <div class="table-wrap" style="margin-top:8px">
            <table>
              <thead><tr><th>#</th><th>Sensor</th><th>Tempo</th><th>Estado</th><th>contador_pulsos</th></tr></thead>
              <tbody id="counterEvents"></tbody>
            </table>
          </div>
        </div>
      </section>

      <!-- ARQUIVOS -->
      <section id="stateFiles" class="state">
        <div class="card">
          <div class="row" style="align-items:center">
            <strong style="flex:1">Arquivos salvos</strong>
            <button class="btn" id="refreshFiles" style="min-width:110px">Atualizar</button>
            <button class="btn" id="backFromFiles" style="min-width:110px">[⬅] Voltar</button>
          </div>
          <p class="muted" style="margin:8px 0 0 0">[⬆/⬇] Navegar · [◉] Abrir · [⬅] Voltar</p>
          <div class="table-wrap" style="margin-top:8px">
            <table>
              <thead>
                <tr><th>ID</th><th>Nome</th><th>Tipo</th><th>Eventos</th><th>Ações</th></tr>
              </thead>
              <tbody id="filesTable"></tbody>
            </table>
          </div>
        </div>

        <div class="card" id="fileViewCard" style="display:none">
          <strong id="fileViewTitle">Visualização</strong>
          <div id="fileMeta" class="muted" style="margin:6px 0"></div>
          <div class="table-wrap">
            <table>
              <thead id="fileViewHead"></thead>
              <tbody id="fileViewBody"></tbody>
            </table>
          </div>
        </div>
      </section>

      <!-- CONFIG -->
      <section id="stateConfig" class="state">
        <div class="card">
          <strong>Modo de detecção por sensor</strong>
          <p class="muted">Escolha 0→1, 1→0 ou AMBAS e salve na EEPROM.</p>
          <div class="row" style="margin-top:8px">
            <div>
              <label>S1</label>
              <select id="mode1">
                <option value="0">0→1 (Rising)</option>
                <option value="1">1→0 (Falling)</option>
                <option value="2">AMBAS</option>
              </select>
            </div>
            <div>
              <label>S2</label>
              <select id="mode2">
                <option value="0">0→1 (Rising)</option>
                <option value="1">1→0 (Falling)</option>
                <option value="2">AMBAS</option>
              </select>
            </div>
            <div>
              <label>S3</label>
              <select id="mode3">
                <option value="0">0→1 (Rising)</option>
                <option value="1">1→0 (Falling)</option>
                <option value="2">AMBAS</option>
              </select>
            </div>
          </div>
          <div class="row" style="margin-top:10px">
            <button class="btn ok" id="saveModes">[◉] Salvar config</button>
            <button class="btn" id="backFromConfig">[⬅] Voltar</button>
          </div>
        </div>
      </section>

      <!-- TESTE -->
      <section id="stateTest" class="state">
        <div class="card">
          <strong>Sensores em tempo real</strong>
          <div class="sensor-status-wrap">
            <div class="sensor-item">
              <div class="sensor-dot low" id="s1">L</div>
              <div class="sensor-label">S1</div>
            </div>
            <div class="sensor-item">
              <div class="sensor-dot low" id="s2">L</div>
              <div class="sensor-label">S2</div>
            </div>
            <div class="sensor-item">
              <div class="sensor-dot low" id="s3">L</div>
              <div class="sensor-label">S3</div>
            </div>
          </div>
          <div class="row" style="margin-top:10px">
            <button class="btn" id="backFromTest">[⬅] Voltar</button>
          </div>
        </div>
      </section>

      <!-- DADOS -->
      <section id="stateData" class="state">
        <div class="card kpi">
          <div class="item">IP do AP<strong id="ipVal">192.168.4.1</strong></div>
          <div class="item">WebSocket<strong>81</strong></div>
          <div class="item">Arquivos<strong id="filesCount">0</strong></div>
          <div class="item">Eventos usados<strong id="eventsCount">0</strong></div>
          <div class="item">Memória livre<strong id="freeMem">0 B</strong></div>
          <div class="item">Memória usada<strong id="usedMem">0 B</strong></div>
        </div>
        <div class="card">
          <strong>Informações do sistema</strong>
          <div style="margin-top:8px">Autor: <strong>Wilson Douglas Jales Simonal</strong></div>
          <div style="margin-top:4px">Versão: <strong>1.0.0</strong></div>
           <div style="margin-top:4px">objetivo: <strong>Popularizar o ensino da física experimetal com uso de hardware baratos amplamente disponíveis comercialmente</strong></div>
        </div>
        <div class="card" style="text-align:center">
          <strong>GIT do projeto</strong>
          <div style="margin-top:8px">
            <img alt="QR Code" style="max-width:180px; width:100%; height:auto; border-radius:8px" src="data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAANwAAADcAQAAAAAzIfIsAAACJklEQVR42u1YS46dQBBzwZOKXXOD5iTT72LhMycD5SLFDZpdI/HaWZBZTrKKpiJNLfHGUpeNy0J8OkeDP8w3+HfwEBHp9hnAKdLtzTm8RETEIVswJ7KqCVnVRmUFLiW5MrtkC7wplwIgssSqdumK4JHtB7jPwERjsPf/YKnjq4sN9qVgOoavJ/T8HCRnQMr+koEyLMGAxPx1bDeRBvsUDGrQrd76l6dL2d9jUjACUjCVj28ehXQ8pd8kVWUD/OiidHj1eBxeLbXNiTaqSQHUpMQLyadJgTkhrIgscaGNakBsM9oMl2y5IrBgVJvIhaxK5tRydbq3bWYwnAO7vZ7SnPLAhuPp9Adw9j+PAcoGsVHOygsJSF4dLL8BQFzIirjQLiQE+txbXki0ERgRWSAlIqw+2TbA9sDWxbbERm0JxmAALpfBBSSvsNEmGpQVGAGE1auD5XSFVUlyKRjVoNZmXG73ljkVQG1UzogsQFjp1W9F+q2L9+lFGpTMSXq/gVtoOO+cvTfnIL1jlSGsaiMARJLL7WCAUwdjlhIXcimgSAWuY2pJr4mRVVkBKGdlVft9m+cvPQ+mEhfaVDDRyLX1K3uRDu+6VUTpgHN49bicBpcMAGoAhJzvl+YK3I+dyOqwcbkzNxBvcpfvxmXqYgNI2dsSmdPDd40o3Oo5jGo/gr16tDk5ZUuuiM05CCmSZuwPbBc21yq7S03lUmLL1WfMku8+/p+BvwBGfJP3lmmsagAAAABJRU5ErkJggg==" />
          </div>
        </div>
        <div class="card">
          <button class="btn" id="backFromData">[⬅] Voltar</button>
        </div>
      </section>
    </div>

    <div class="bottom-pad">
      <div class="pad-grid">
        <button class="pad-btn empty"></button>
        <button class="pad-btn" id="padUp">⬆️</button>
        <button class="pad-btn empty"></button>
        <button class="pad-btn" id="padLeft">⬅️</button>
        <button class="pad-btn" id="padOk">◉</button>
        <button class="pad-btn" id="padRight">➡️</button>
        <button class="pad-btn empty"></button>
        <button class="pad-btn" id="padDown">⬇️</button>
        <button class="pad-btn empty"></button>
      </div>
    </div>
  </div>

  <script>
    const STATES = { MENU:0, CRONO:1, COUNTER:2, FILES:3, CONFIG:4, TEST:5, DATA:6 };
    let ws = null;
    let wsConnected = false;

    let ui = {
      currentState: STATES.MENU,
      menuSelection: 0,
      chronoTime: 0,
      chronoRunning: false,
      counterActive: false,
      counterValue: 0,
      counter1: 0,
      counter2: 0,
      counter3: 0,
      sensorModes: [2,2,2],
      sensor: [0,0,0],
      files: [],
      captureEvents: [],
      memFree: 0,
      memUsed: 0,
      usedEvents: 0,
      fileCount: 0,
      selectedFileId: null,
      configDraftModes: [2,2,2],
      configDirty: false,
      lastState: STATES.MENU,
    };

    function showToast(msg) {
      const t = document.getElementById('toast');
      t.textContent = msg;
      t.style.display = 'block';
      clearTimeout(showToast._timer);
      showToast._timer = setTimeout(() => t.style.display = 'none', 3000);
    }

    function wsSendRaw(raw) {
      if (ws && wsConnected && ws.readyState === WebSocket.OPEN) ws.send(raw);
    }

    function wsSend(obj) { wsSendRaw(JSON.stringify(obj)); }

    function initWebSocket() {
      const protocol = location.protocol === 'https:' ? 'wss:' : 'ws:';
      ws = new WebSocket(`${protocol}//${location.hostname}:81`);

      ws.onopen = () => {
        wsConnected = true;
        wsSendRaw('connected');
        wsSend({ action:'list_files' });
      };

      ws.onclose = () => {
        wsConnected = false;
        setTimeout(initWebSocket, 2200);
      };

      ws.onerror = () => { wsConnected = false; };

      ws.onmessage = (e) => {
        const raw = e.data;
        try {
          const data = JSON.parse(raw);
          handleMessage(data);
        } catch (err) {
          console.warn('Mensagem não-JSON', raw);
        }
      };
    }

    function handleMessage(msg) {
      if (msg.type === 'state') {
        ui.lastState = ui.currentState;
        ui.currentState = msg.state;
        ui.menuSelection = msg.menuSelection;
        ui.chronoTime = msg.chrono_time;
        ui.chronoRunning = msg.chrono_running;
        ui.counterActive = !!msg.counter_active;
        ui.counterValue = msg.counter_value;
        ui.counter1 = msg.counter1 || 0;
        ui.counter2 = msg.counter2 || 0;
        ui.counter3 = msg.counter3 || 0;
        ui.sensor = [msg.sensor1, msg.sensor2, msg.sensor3];
        ui.sensorModes = [msg.mode1, msg.mode2, msg.mode3];
        ui.captureEvents = msg.capture_events || [];
        ui.memFree = msg.mem_free || 0;
        ui.memUsed = msg.mem_used || 0;
        ui.usedEvents = msg.events_used || 0;
        ui.fileCount = msg.files_count || 0;
        ui.selectedFileId = (msg.selected_file_id && msg.selected_file_id > 0) ? msg.selected_file_id : null;
        ui.defaultChronoName = msg.default_chrono_name || 'Cronometro_1';
        ui.defaultCounterName = msg.default_counter_name || 'Contador_1';

        if (ui.currentState === STATES.CONFIG && ui.lastState !== STATES.CONFIG) {
          ui.configDraftModes = [...ui.sensorModes];
          ui.configDirty = false;
        }

        render();
      } else if (msg.type === 'files_list') {
        ui.files = msg.files || [];
        if (!ui.files.some(f => f.id === ui.selectedFileId)) {
          ui.selectedFileId = null;
        }
        renderFilesTable();
      } else if (msg.type === 'file_view') {
        renderFileView(msg);
      } else if (msg.type === 'info') {
        showToast(msg.message || 'Info');
      } else if (msg.type === 'warning') {
        showToast(msg.message || 'Aviso');
      }
    }

    function formatTime(ms) {
      const total = Math.floor(ms / 1000);
      const h = String(Math.floor(total / 3600)).padStart(2,'0');
      const m = String(Math.floor((total % 3600)/60)).padStart(2,'0');
      const s = String(total % 60).padStart(2,'0');
      return `${h}:${m}:${s}`;
    }

    function render() {
      // Estados
      const map = {
        [STATES.MENU]: 'stateMenu',
        [STATES.CRONO]: 'stateChrono',
        [STATES.COUNTER]: 'stateCounter',
        [STATES.FILES]: 'stateFiles',
        [STATES.CONFIG]: 'stateConfig',
        [STATES.TEST]: 'stateTest',
        [STATES.DATA]: 'stateData'
      };
      document.querySelectorAll('.state').forEach(el => el.classList.remove('show'));
      const active = document.getElementById(map[ui.currentState]);
      if (active) active.classList.add('show');

      // Menu active
      document.querySelectorAll('#menuGrid .menu-item').forEach((el, idx) => {
        el.classList.toggle('active', idx === ui.menuSelection);
      });

      // Crono
      document.getElementById('chronoDisplay').textContent = formatTime(ui.chronoTime);
      document.getElementById('chronoStatus').textContent = ui.chronoRunning ? 'Executando' : 'Parado';
      document.getElementById('chronoPlay').textContent = ui.chronoRunning ? '[◉] Parar' : '[◉] Iniciar';
      if (!document.getElementById('chronoName').value) {
        document.getElementById('chronoName').placeholder = ui.defaultChronoName || 'Cronometro_1';
      }

      // Counter
      document.getElementById('counterDisplay1').textContent = ui.counter1;
      document.getElementById('counterDisplay2').textContent = ui.counter2;
      document.getElementById('counterDisplay3').textContent = ui.counter3;
      document.getElementById('counterDisplayTotal').textContent = ui.counterValue;
      document.getElementById('counterToggle').textContent = ui.counterActive ? '[◉] Parar' : '[◉] Iniciar';
      if (!document.getElementById('counterName').value) {
        document.getElementById('counterName').placeholder = ui.defaultCounterName || 'Contador_1';
      }

      // Config (evita sobrescrever seleção local antes do salvar)
      if (!(ui.currentState === STATES.CONFIG && ui.configDirty)) {
        ui.configDraftModes = [...ui.sensorModes];
      }
      document.getElementById('mode1').value = String(ui.configDraftModes[0] ?? 2);
      document.getElementById('mode2').value = String(ui.configDraftModes[1] ?? 2);
      document.getElementById('mode3').value = String(ui.configDraftModes[2] ?? 2);

      // Teste - H/L dentro de círculo
      const s1El = document.getElementById('s1');
      const s2El = document.getElementById('s2');
      const s3El = document.getElementById('s3');
      [
        { el: s1El, high: !!ui.sensor[0] },
        { el: s2El, high: !!ui.sensor[1] },
        { el: s3El, high: !!ui.sensor[2] }
      ].forEach(item => {
        item.el.textContent = item.high ? 'H' : 'L';
        item.el.classList.toggle('high', item.high);
        item.el.classList.toggle('low', !item.high);
      });

      // Dados
      document.getElementById('ipVal').textContent = location.hostname;
      document.getElementById('filesCount').textContent = ui.fileCount;
      document.getElementById('eventsCount').textContent = ui.usedEvents;
      document.getElementById('freeMem').textContent = `${ui.memFree} B`;
      document.getElementById('usedMem').textContent = `${ui.memUsed} B`;
      document.getElementById('memInfo').textContent = `Memória livre: ${ui.memFree} B`;

      if (ui.currentState === STATES.FILES) {
        renderFilesTable();
      }

      renderCaptureEvents();
    }

    function renderCaptureEvents() {
      const cBody = document.getElementById('chronoEvents');
      const kBody = document.getElementById('counterEvents');
      cBody.innerHTML = '';
      kBody.innerHTML = '';
      (ui.captureEvents || []).forEach((ev, i) => {
        const estado = ev.estado ? 'HIGH' : 'LOW';
        const tr1 = document.createElement('tr');
        tr1.innerHTML = `<td>${i+1}</td><td>S${ev.sensor}</td><td>${ev.ts}</td><td>${estado}</td>`;
        cBody.appendChild(tr1);

        const tr2 = document.createElement('tr');
        tr2.innerHTML = `<td>${i+1}</td><td>S${ev.sensor}</td><td>${ev.ts}</td><td>${estado}</td><td>${ev.counter}</td>`;
        kBody.appendChild(tr2);
      });
    }

    function renderFilesTable() {
      const tb = document.getElementById('filesTable');
      tb.innerHTML = '';

      if (!ui.files.length) {
        const tr = document.createElement('tr');
        tr.innerHTML = `<td colspan="5" class="muted">Nenhum arquivo salvo.</td>`;
        tb.appendChild(tr);
        return;
      }

      ui.files.forEach(f => {
        const isSelected = ui.selectedFileId === f.id;
        const mainTr = document.createElement('tr');
        mainTr.className = `file-row ${isSelected ? 'selected' : ''}`;
        mainTr.innerHTML = `
          <td>${f.id}</td>
          <td class="file-name-cell">${f.name}</td>
          <td>${f.typeLabel}</td>
          <td>${f.eventCount}</td>
          <td>${isSelected ? '▼' : '▶'} Selecionar</td>`;
        mainTr.onclick = () => {
          ui.selectedFileId = isSelected ? null : f.id;
          renderFilesTable();
        };
        tb.appendChild(mainTr);

        if (!isSelected) return;

        const detailTr = document.createElement('tr');
        const downloadHref = `/download?id=${f.id}`;
        detailTr.innerHTML = `
          <td colspan="5" class="actions-cell">
            <div class="row" style="margin-bottom:8px">
              <input class="file-name-input" maxlength="15" value="${f.name}" data-rename-id="${f.id}" />
            </div>
            <div class="row">
              <button class="btn" data-view="${f.id}">Ver</button>
              <a class="btn" href="${downloadHref}" style="text-decoration:none;text-align:center">Baixar</a>
              <button class="btn warn" data-rename="${f.id}">Renomear</button>
              <button class="btn danger" data-del="${f.id}">Excluir</button>
            </div>
          </td>`;
        tb.appendChild(detailTr);
      });

      tb.querySelectorAll('[data-view]').forEach(b => b.onclick = () => wsSend({ action:'view_file', id: Number(b.dataset.view) }));
      tb.querySelectorAll('[data-del]').forEach(b => b.onclick = () => wsSend({ action:'delete_file', id: Number(b.dataset.del) }));
      tb.querySelectorAll('[data-rename]').forEach(b => b.onclick = () => {
        const id = Number(b.dataset.rename);
        const input = tb.querySelector(`[data-rename-id="${id}"]`);
        wsSend({ action:'rename_file', id, name: input ? input.value : '' });
      });
    }

    function renderFileView(msg) {
      const card = document.getElementById('fileViewCard');
      card.style.display = 'block';
      document.getElementById('fileViewTitle').textContent = `Arquivo #${msg.id} - ${msg.name}`;
      document.getElementById('fileMeta').textContent = `${msg.typeLabel} | duração ${msg.durationMs} ms | eventos ${msg.eventCount}`;

      const head = document.getElementById('fileViewHead');
      const body = document.getElementById('fileViewBody');
      head.innerHTML = '';
      body.innerHTML = '';

      if (msg.file_type === 2) {
        head.innerHTML = '<tr><th>#</th><th>Sensor</th><th>timestamp_ms</th><th>estado</th><th>contador_pulsos</th></tr>';
      } else {
        head.innerHTML = '<tr><th>#</th><th>Sensor</th><th>timestamp_ms</th><th>estado</th></tr>';
      }

      (msg.events || []).forEach((ev, i) => {
        const tr = document.createElement('tr');
        if (msg.file_type === 2) {
          tr.innerHTML = `<td>${i+1}</td><td>S${ev.sensor}</td><td>${ev.timestamp_ms}</td><td>${ev.estado}</td><td>${ev.contador_pulsos}</td>`;
        } else {
          tr.innerHTML = `<td>${i+1}</td><td>S${ev.sensor}</td><td>${ev.timestamp_ms}</td><td>${ev.estado}</td>`;
        }
        body.appendChild(tr);
      });
    }

    function bindEvents() {
      // teclado
      document.addEventListener('keydown', (e) => {
        const map = { ArrowUp:'up', ArrowDown:'down', ArrowLeft:'left', ArrowRight:'right', Enter:'ok' };
        if (map[e.key]) {
          wsSendRaw(map[e.key]);
          e.preventDefault();
        }
      });

      // pad virtual
      document.getElementById('padUp').onclick = () => wsSendRaw('up');
      document.getElementById('padDown').onclick = () => wsSendRaw('down');
      document.getElementById('padLeft').onclick = () => wsSendRaw('left');
      document.getElementById('padRight').onclick = () => wsSendRaw('right');
      document.getElementById('padOk').onclick = () => wsSendRaw('ok');

      // menu clicável direto
      document.querySelectorAll('#menuGrid .menu-item').forEach(item => {
        item.addEventListener('click', () => {
          wsSend({ action:'menu_select', index: Number(item.dataset.index) });
          wsSendRaw('ok');
        });
      });

      // cronômetro
      document.getElementById('chronoPlay').onclick = () => wsSendRaw('ok');
      document.getElementById('chronoStart').onclick = () => wsSendRaw('right');
      document.getElementById('chronoBack').onclick = () => wsSendRaw('left');
      document.getElementById('chronoSaveBack').onclick = () => wsSendRaw('down');
      document.getElementById('chronoName').addEventListener('change', (e) => {
        wsSend({ action:'set_draft_name', target:'chrono', name:e.target.value || '' });
      });

      // contador
      document.getElementById('counterToggle').onclick = () => wsSendRaw('ok');
      document.getElementById('counterBack').onclick = () => wsSendRaw('left');
      document.getElementById('counterSaveBack').onclick = () => wsSendRaw('down');
      document.getElementById('counterName').addEventListener('change', (e) => {
        wsSend({ action:'set_draft_name', target:'counter', name:e.target.value || '' });
      });

      // arquivos
      document.getElementById('refreshFiles').onclick = () => wsSend({ action:'list_files' });
      document.getElementById('backFromFiles').onclick = () => wsSendRaw('left');

      // config
      ['mode1','mode2','mode3'].forEach((id, idx) => {
        document.getElementById(id).addEventListener('change', (e) => {
          ui.configDraftModes[idx] = Number(e.target.value);
          ui.configDirty = true;
        });
      });

      document.getElementById('saveModes').onclick = () => {
        wsSend({ action:'set_sensor_mode', sensor:1, mode:ui.configDraftModes[0] });
        wsSend({ action:'set_sensor_mode', sensor:2, mode:ui.configDraftModes[1] });
        wsSend({ action:'set_sensor_mode', sensor:3, mode:ui.configDraftModes[2] });
        wsSend({ action:'save_sensor_config' });
        ui.configDirty = false;
      };
      document.getElementById('backFromConfig').onclick = () => wsSendRaw('left');

      // outros voltar
      document.getElementById('backFromTest').onclick = () => wsSendRaw('left');
      document.getElementById('backFromData').onclick = () => wsSendRaw('left');
    }

    window.addEventListener('load', () => {
      bindEvents();
      initWebSocket();
      render();
    });
  </script>
</body>
</html>
)rawliteral";

// ============================================================
//                            SETUP
// ============================================================
void setup() {
  Serial.begin(115200);

  Serial.println("\n\n╔══════════════════════════════════════════════╗");
  Serial.println("║        ESP32 Controle Web + EEPROM FS       ║");
  Serial.println("╚══════════════════════════════════════════════╝");

  initButtons();
  initSensors();
  initEEPROM();
  storageInitOrLoad();

  pixels.begin();
  pixels.setBrightness(55);
  turnOffAllLeds();

  initWiFi();
  initServers();

  Serial.println("✓ Sistema iniciado com sucesso");
  Serial.println("IP: " + WiFi.softAPIP().toString());
}

// ============================================================
//                            LOOP
// ============================================================
void loop() {
  server.handleClient();
  webSocket.loop();
  checkButtons();
  updateChrono();

  if (currentState == STATE_TESTE) {
    updateNeoPixelsFromSensors();
  }

  if (currentState == STATE_CRONOMETRO && chronoRunning) {
    checkSensoresChrono();
  }

  if (currentState == STATE_CONTADOR && counterActive) {
    checkSensoresContador();
  }

  updateLedFlashes();
  checkAutoResetOnDisconnect();

  static unsigned long lastPush = 0;
  if (millis() - lastPush > 120) {
    lastPush = millis();
    sendStateToClients();
  }

  yield();
}

// ============================================================
//                  INICIALIZAÇÕES DE HARDWARE
// ============================================================
void initEEPROM() {
  EEPROM.begin(EEPROM_SIZE);
  Serial.println("✓ EEPROM inicializada");
}

void initWiFi() {
  WiFi.mode(WIFI_AP);
  if (!WiFi.softAP(WIFI_SSID, WIFI_PASSWORD)) {
    Serial.println("✗ Falha ao iniciar AP");
    while (true) {
      yield();
    }
  }
  IPAddress localIP(192, 168, 4, 1);
  IPAddress gateway(192, 168, 4, 1);
  IPAddress subnet(255, 255, 255, 0);
  WiFi.softAPConfig(localIP, gateway, subnet);

  Serial.println("✓ Access Point ativo");
  Serial.println("SSID: " + String(WIFI_SSID));
  Serial.println("IP: " + WiFi.softAPIP().toString());
}

void initButtons() {
  for (int i = 0; i < 5; i++) {
    pinMode(buttons[i].pin, INPUT_PULLUP);
  }
  Serial.println("✓ Botões configurados");
}

void initSensors() {
  pinMode(SENSOR_1_PIN, INPUT);
  pinMode(SENSOR_2_PIN, INPUT);
  pinMode(SENSOR_3_PIN, INPUT);
  Serial.println("✓ Sensores configurados");
}

void initServers() {
  server.on("/", handleRoot);
  server.on("/download", handleDownloadFile);
  server.begin();

  webSocket.begin();
  webSocket.onEvent(handleWebSocketEvent);

  Serial.println("✓ WebServer na porta 80");
  Serial.println("✓ WebSocket na porta 81");
}

// ============================================================
//                     MANIPULADORES HTTP/WS
// ============================================================
void handleRoot() {
  server.send(200, "text/html; charset=utf-8", HTML_PAGE);
}

void handleDownloadFile() {
  if (!server.hasArg("id")) {
    server.send(400, "text/plain", "Parametro id obrigatorio");
    return;
  }

  uint16_t id = (uint16_t)server.arg("id").toInt();
  int idx = findFileIndexById(id);
  if (idx < 0) {
    server.send(404, "text/plain", "Arquivo nao encontrado");
    return;
  }

  String csv = csvFromFile(gFiles[idx]);
  String fileName = String(gFiles[idx].name) + ".csv";
  server.sendHeader("Content-Disposition", "attachment; filename=" + fileName);
  server.send(200, "text/csv; charset=utf-8", csv);
}

void handleWebSocketEvent(uint8_t num, WStype_t type, uint8_t* payload, size_t length) {
  switch (type) {
    case WStype_CONNECTED:
      Serial.printf("[WS %u] conectado\n", num);
      lastClientCount = webSocket.connectedClients();
      noClientSinceMs = 0;
      sendStateToClients();
      sendFilesList(num);
      break;

    case WStype_DISCONNECTED:
      Serial.printf("[WS %u] desconectado\n", num);
      lastClientCount = webSocket.connectedClients();
      if (lastClientCount == 0) {
        noClientSinceMs = millis();
      }
      break;

    case WStype_TEXT: {
      String msg = String((char*)payload).substring(0, length);
      msg.trim();
      if (msg.length() == 0) return;

      if (msg[0] == '{') {
        processJsonCommand(num, msg);
      } else {
        if (msg == "up" || msg == "down" || msg == "left" || msg == "right" || msg == "ok") {
          processCommand(msg);
        }
      }
      break;
    }

    default:
      break;
  }
}

void processJsonCommand(uint8_t num, const String& payload) {
  DynamicJsonDocument doc(1024);
  auto err = deserializeJson(doc, payload);
  if (err) {
    sendInfoMessage("JSON invalido", true);
    return;
  }

  String action = doc["action"] | "";

  if (action == "menu_select") {
    menuSelection = constrain((int)(doc["index"] | 0), 0, 5);
    stateChanged = true;
    return;
  }

  if (action == "set_draft_name") {
    String target = doc["target"] | "";
    String name = doc["name"] | "";
    if (target == "chrono") draftChronoName = sanitizeFileName(name, "");
    if (target == "counter") draftCounterName = sanitizeFileName(name, "");
    return;
  }

  if (action == "finalize_chrono") {
    String name = doc["name"] | "";
    finalizarCronometro(name);
    return;
  }

  if (action == "finalize_counter") {
    String name = doc["name"] | "";
    finalizarContador(name);
    return;
  }

  if (action == "set_sensor_mode") {
    int sensor = doc["sensor"] | 0;
    int mode = doc["mode"] | 2;
    if (sensor >= 1 && sensor <= 3) {
      uint8_t normalized = (uint8_t)constrain(mode, 0, 2);
      gStorage.sensorMode[sensor - 1] = normalized;
      Serial.printf("[CONFIG] S%d modo temporário => %u (%s)\n", sensor, normalized, detectModeLabel(normalized));
      stateChanged = true;
    }
    return;
  }

  if (action == "save_sensor_config") {
    Serial.printf("[CONFIG] Salvando EEPROM S1=%u(%s) S2=%u(%s) S3=%u(%s)\n",
                  gStorage.sensorMode[0], detectModeLabel(gStorage.sensorMode[0]),
                  gStorage.sensorMode[1], detectModeLabel(gStorage.sensorMode[1]),
                  gStorage.sensorMode[2], detectModeLabel(gStorage.sensorMode[2]));
    if (storageCommit()) {
      sendInfoMessage("Configuração de sensores salva na EEPROM");
    }
    return;
  }

  if (action == "list_files") {
    sendFilesList(num);
    return;
  }

  if (action == "view_file") {
    uint16_t id = (uint16_t)(doc["id"] | 0);
    sendFileView(num, id);
    return;
  }

  if (action == "delete_file") {
    uint16_t id = (uint16_t)(doc["id"] | 0);
    if (deleteFileById(id)) {
      sendInfoMessage("Arquivo excluído");
      sendFilesList();
    }
    return;
  }

  if (action == "rename_file") {
    uint16_t id = (uint16_t)(doc["id"] | 0);
    String newName = doc["name"] | "";
    if (renameFileById(id, newName)) {
      sendInfoMessage("Arquivo renomeado");
      sendFilesList();
    }
    return;
  }
}

// ============================================================
//                          ESTADOS
// ============================================================
void changeState(States newState, int newMenuSelection) {
  // Requisito: sempre desligar LEDs em TODA transição de estado
  turnOffAllLeds();

  if (newState != STATE_ARQUIVOS) {
    selectedFileIndex = -1;
    selectedFileId = 0;
  } else if (gStorage.fileCount > 0) {
    if (selectedFileIndex < 0 || selectedFileIndex >= gStorage.fileCount) {
      selectedFileIndex = 0;
    }
    selectedFileId = gFiles[selectedFileIndex].id;
  }

  currentState = newState;
  menuSelection = newMenuSelection;
  stateChanged = true;

  Serial.printf("[STATE] -> %d\n", currentState);
}

void processCommand(const String& cmd) {
  if (currentState == STATE_MENU) {
    handleMenuInput(cmd);
  } else if (currentState == STATE_CRONOMETRO) {
    handleChronoInput(cmd);
  } else if (currentState == STATE_CONTADOR) {
    handleContadorInput(cmd);
  } else if (currentState == STATE_CONFIG) {
    handleConfigInput(cmd);
  } else if (currentState == STATE_ARQUIVOS) {
    handleFilesInput(cmd);
  } else if (currentState == STATE_DADOS || currentState == STATE_TESTE) {
    if (cmd == "left") {
      changeState(STATE_MENU, 0);
    }
  }
}

void handleMenuInput(const String& cmd) {
  if (cmd == "up") {
    menuSelection--;
    if (menuSelection < 0) menuSelection = 5;
  } else if (cmd == "down") {
    menuSelection++;
    if (menuSelection > 5) menuSelection = 0;
  } else if (cmd == "ok") {
    switch (menuSelection) {
      case 0:
        captureEventCount = 0;
        draftChronoName = "";
        resetarCronometro();
        changeState(STATE_CRONOMETRO, 0);
        break;
      case 1:
        iniciarContador();
        draftCounterName = "";
        changeState(STATE_CONTADOR, 1);
        break;
      case 2:
        changeState(STATE_ARQUIVOS, 2);
        selectedFileIndex = (gStorage.fileCount > 0) ? 0 : -1;
        selectedFileId = (selectedFileIndex >= 0) ? gFiles[selectedFileIndex].id : 0;
        sendFilesList();
        break;
      case 3:
        configSelectedSensor = 0;
        changeState(STATE_CONFIG, 3);
        break;
      case 4:
        changeState(STATE_TESTE, 4);
        break;
      case 5:
        changeState(STATE_DADOS, 5);
        break;
    }
  }
  stateChanged = true;
}

void handleChronoInput(const String& cmd) {
  if (cmd == "ok") {
    controlarCronometro(!chronoRunning);
  } else if (cmd == "right") {
    if (!chronoRunning) {
      controlarCronometro(true);
    }
  } else if (cmd == "down") {
    finalizarCronometro(draftChronoName);
  } else if (cmd == "left") {
    resetarCronometro();
    changeState(STATE_MENU, 0);
  }
}

void handleContadorInput(const String& cmd) {
  if (cmd == "ok") {
    if (!counterActive) {
      if (captureEventCount == 0) {
        iniciarContador();
      } else {
        counterStartTime = millis() - counterElapsedMs;
        counterActive = true;
        setLEDColor(pixels.Color(0, 40, 120));
      }
      sendInfoMessage("Contador iniciado");
    } else {
      counterElapsedMs = millis() - counterStartTime;
      counterActive = false;
      setLEDColor(pixels.Color(120, 80, 0));
      sendInfoMessage("Contador pausado");
    }
  } else if (cmd == "down") {
    finalizarContador(draftCounterName);
  } else if (cmd == "left") {
    counterActive = false;
    counterElapsedMs = 0;
    counterValues[0] = 0;
    counterValues[1] = 0;
    counterValues[2] = 0;
    captureEventCount = 0;
    changeState(STATE_MENU, 0);
  }
  stateChanged = true;
}

void handleConfigInput(const String& cmd) {
  if (cmd == "up") {
    configSelectedSensor--;
    if (configSelectedSensor < 0) configSelectedSensor = 2;
  } else if (cmd == "down") {
    configSelectedSensor++;
    if (configSelectedSensor > 2) configSelectedSensor = 0;
  } else if (cmd == "right") {
    gStorage.sensorMode[configSelectedSensor] = (gStorage.sensorMode[configSelectedSensor] + 1) % 3;
  } else if (cmd == "ok") {
    if (storageCommit()) {
      sendInfoMessage("Configuração de sensores salva na EEPROM");
    }
  } else if (cmd == "left") {
    changeState(STATE_MENU, 0);
  }
  stateChanged = true;
}

void handleFilesInput(const String& cmd) {
  if (cmd == "left") {
    changeState(STATE_MENU, 0);
    return;
  }

  if (gStorage.fileCount == 0) {
    selectedFileIndex = -1;
    selectedFileId = 0;
    return;
  }

  if (selectedFileIndex < 0 || selectedFileIndex >= gStorage.fileCount) {
    selectedFileIndex = 0;
  }

  if (cmd == "up") {
    selectedFileIndex--;
    if (selectedFileIndex < 0) selectedFileIndex = gStorage.fileCount - 1;
    selectedFileId = gFiles[selectedFileIndex].id;
    stateChanged = true;
  } else if (cmd == "down") {
    selectedFileIndex++;
    if (selectedFileIndex >= gStorage.fileCount) selectedFileIndex = 0;
    selectedFileId = gFiles[selectedFileIndex].id;
    stateChanged = true;
  } else if (cmd == "ok") {
    selectedFileId = gFiles[selectedFileIndex].id;
    sendFileView(255, selectedFileId);
    stateChanged = true;
  }
}

// ============================================================
//                       CRONÔMETRO/CONTADOR
// ============================================================
void controlarCronometro(bool iniciar) {
  if (iniciar && !chronoRunning) {
    chronoStartTime = millis() - chronoPausedTime;
    chronoRunning = true;
    chronoActive = true;

    prevSensor1 = readSensor1();
    prevSensor2 = readSensor2();
    prevSensor3 = readSensor3();

    setLEDColor(pixels.Color(0, 40, 160));
    Serial.println("▶ Cronômetro iniciado");
  } else if (!iniciar && chronoRunning) {
    chronoPausedTime = chronoTime;
    chronoRunning = false;

    setLEDColor(pixels.Color(160, 100, 0));
    Serial.printf("⏸ Cronômetro pausado (%lu ms)\n", chronoPausedTime);
  }
  stateChanged = true;
}

void resetarCronometro() {
  chronoRunning = false;
  chronoActive = false;
  chronoStartTime = 0;
  chronoPausedTime = 0;
  chronoTime = 0;
  captureEventCount = 0;

  prevSensor1 = readSensor1();
  prevSensor2 = readSensor2();
  prevSensor3 = readSensor3();
}

void finalizarCronometro(const String& suggestedName) {
  if (chronoRunning) {
    controlarCronometro(false);
  }

  uint16_t newId = 0;
  bool saved = false;

  // Salvamento automático: apenas se houver eventos capturados.
  if (captureEventCount > 0) {
    saved = saveSessionFile(FILE_TYPE_CHRONO, suggestedName, chronoTime,
                            captureEvents, captureEventCount, newId);
  }

  resetarCronometro();
  changeState(STATE_MENU, 0);
  sendFilesList();

  if (saved) {
    sendInfoMessage("Cronômetro salvo em arquivo #" + String(newId));
  } else {
    sendInfoMessage("Cronômetro encerrado sem eventos: nada foi salvo");
  }
}

void iniciarContador() {
  counterValues[0] = 0;
  counterValues[1] = 0;
  counterValues[2] = 0;
  counterActive = true;
  counterStartTime = millis();
  counterElapsedMs = 0;
  captureEventCount = 0;

  prevSensor1 = readSensor1();
  prevSensor2 = readSensor2();
  prevSensor3 = readSensor3();

  setLEDColor(pixels.Color(0, 40, 120));
}

void finalizarContador(const String& suggestedName) {
  uint32_t duration = counterActive ? (millis() - counterStartTime) : counterElapsedMs;
  uint16_t newId = 0;
  bool saved = false;

  // Salvamento automático: apenas se houver eventos capturados.
  if (captureEventCount > 0) {
    saved = saveSessionFile(FILE_TYPE_COUNTER, suggestedName, duration,
                             captureEvents, captureEventCount, newId);
  }

  counterActive = false;
  counterElapsedMs = 0;
  counterValues[0] = 0;
  counterValues[1] = 0;
  counterValues[2] = 0;
  captureEventCount = 0;

  changeState(STATE_MENU, 0);
  sendFilesList();

  if (saved) {
    sendInfoMessage("Contador salvo em arquivo #" + String(newId));
  } else {
    sendInfoMessage("Contador encerrado sem eventos: nada foi salvo");
  }
}

void updateChrono() {
  if (chronoRunning) {
    chronoTime = millis() - chronoStartTime;
  }
}

bool shouldRegisterByMode(int prevState, int currState, uint8_t mode) {
  if (prevState == currState) return false;

  if (mode == DETECT_BOTH) return true;
  if (mode == DETECT_RISING) return (prevState == LOW && currState == HIGH);
  if (mode == DETECT_FALLING) return (prevState == HIGH && currState == LOW);
  return false;
}

void registerCaptureEvent(uint8_t sensor, uint32_t timestampMs, uint8_t estado, int32_t counterPulses) {
  if (captureEventCount >= MAX_CAPTURE_EVENTS) {
    sendInfoMessage("Limite de eventos da sessão atingido", true);
    return;
  }

  captureEvents[captureEventCount].sensor = sensor;
  captureEvents[captureEventCount].timestampMs = timestampMs;
  captureEvents[captureEventCount].estado = estado;
  captureEvents[captureEventCount].counterPulses = counterPulses;
  captureEventCount++;

  int ledIdx = sensor - 1;
  piscarLED(ledIdx, estado == HIGH ? pixels.Color(180, 0, 0) : pixels.Color(0, 120, 0), 1, 40);
}

void checkSensoresChrono() {
  int s1 = readSensor1();
  int s2 = readSensor2();
  int s3 = readSensor3();

  if (shouldRegisterByMode(prevSensor1, s1, gStorage.sensorMode[0])) {
    registerCaptureEvent(1, chronoTime, s1, counterValues[0]);
  }
  if (shouldRegisterByMode(prevSensor2, s2, gStorage.sensorMode[1])) {
    registerCaptureEvent(2, chronoTime, s2, counterValues[1]);
  }
  if (shouldRegisterByMode(prevSensor3, s3, gStorage.sensorMode[2])) {
    registerCaptureEvent(3, chronoTime, s3, counterValues[2]);
  }

  prevSensor1 = s1;
  prevSensor2 = s2;
  prevSensor3 = s3;

  // cor base durante execução
  setLEDColor(pixels.Color(0, 40, 140));
}

void checkSensoresContador() {
  int s1 = readSensor1();
  int s2 = readSensor2();
  int s3 = readSensor3();
  uint32_t ts = millis() - counterStartTime;

  if (shouldRegisterByMode(prevSensor1, s1, gStorage.sensorMode[0])) {
    counterValues[0]++;
    registerCaptureEvent(1, ts, s1, counterValues[0]);
  }
  if (shouldRegisterByMode(prevSensor2, s2, gStorage.sensorMode[1])) {
    counterValues[1]++;
    registerCaptureEvent(2, ts, s2, counterValues[1]);
  }
  if (shouldRegisterByMode(prevSensor3, s3, gStorage.sensorMode[2])) {
    counterValues[2]++;
    registerCaptureEvent(3, ts, s3, counterValues[2]);
  }

  prevSensor1 = s1;
  prevSensor2 = s2;
  prevSensor3 = s3;
}

// ============================================================
//                 SISTEMA DE ARQUIVOS (EEPROM)
// ============================================================
void storageFactoryReset() {
  memset(&gStorage, 0, sizeof(gStorage));
  memset(gFiles, 0, sizeof(gFiles));
  memset(gEvents, 0, sizeof(gEvents));

  gStorage.magic = STORAGE_MAGIC;
  gStorage.version = STORAGE_VERSION;
  gStorage.sensorMode[0] = DETECT_BOTH;
  gStorage.sensorMode[1] = DETECT_BOTH;
  gStorage.sensorMode[2] = DETECT_BOTH;
  gStorage.fileCount = 0;
  gStorage.usedEvents = 0;
  gStorage.nextChronoNumber = 1;
  gStorage.nextCounterNumber = 1;

  storageCommit();
}

void storageInitOrLoad() {
  if (EEPROM_ADDR_EVENTS + sizeof(EventRecord) * MAX_EVENT_RECORDS > EEPROM_SIZE) {
    Serial.println("✗ Layout de EEPROM excede 4KB");
    while (true) {
      yield();
    }
  }

  EEPROM.get(EEPROM_ADDR_HEADER, gStorage);

  if (gStorage.magic != STORAGE_MAGIC || gStorage.version != STORAGE_VERSION ||
      gStorage.fileCount > MAX_FILES || gStorage.usedEvents > MAX_EVENT_RECORDS) {
    Serial.println("⚠ Estrutura inválida. Recriando sistema de arquivos...");
    storageFactoryReset();
  } else {
    EEPROM.get(EEPROM_ADDR_FILES, gFiles);
    EEPROM.get(EEPROM_ADDR_EVENTS, gEvents);

    bool sensorModesFixed = false;
    for (uint8_t i = 0; i < 3; i++) {
      if (gStorage.sensorMode[i] > DETECT_BOTH) {
        Serial.printf("[CONFIG] Modo inválido em S%u (%u). Ajustando para AMBAS.\n", i + 1, gStorage.sensorMode[i]);
        gStorage.sensorMode[i] = DETECT_BOTH;
        sensorModesFixed = true;
      }
    }
    if (sensorModesFixed) {
      storageCommit();
    }
  }

  Serial.printf("✓ FS EEPROM carregado | arquivos=%u | eventos=%u | livre=%u B\n",
                gStorage.fileCount, gStorage.usedEvents, storageFreeBytes());
  Serial.printf("[CONFIG] EEPROM modos carregados: S1=%u(%s) S2=%u(%s) S3=%u(%s)\n",
                gStorage.sensorMode[0], detectModeLabel(gStorage.sensorMode[0]),
                gStorage.sensorMode[1], detectModeLabel(gStorage.sensorMode[1]),
                gStorage.sensorMode[2], detectModeLabel(gStorage.sensorMode[2]));
}

bool storageCommit() {
  EEPROM.put(EEPROM_ADDR_HEADER, gStorage);
  EEPROM.put(EEPROM_ADDR_FILES, gFiles);
  EEPROM.put(EEPROM_ADDR_EVENTS, gEvents);
  bool ok = EEPROM.commit();
  if (!ok) {
    sendInfoMessage("Falha ao gravar na EEPROM", true);
  }
  return ok;
}

uint16_t storageUsedBytes() {
  uint32_t used = sizeof(StorageHeader);
  used += (uint32_t)gStorage.fileCount * sizeof(FileRecord);
  used += (uint32_t)gStorage.usedEvents * sizeof(EventRecord);
  if (used > EEPROM_SIZE) used = EEPROM_SIZE;
  return (uint16_t)used;
}

uint16_t storageFreeBytes() {
  uint16_t used = storageUsedBytes();
  return (used >= EEPROM_SIZE) ? 0 : (EEPROM_SIZE - used);
}

int findFileIndexById(uint16_t id) {
  for (int i = 0; i < gStorage.fileCount; i++) {
    if (gFiles[i].id == id) return i;
  }
  return -1;
}

String sanitizeFileName(const String& raw, const String& fallback) {
  String out = raw;
  out.trim();
  if (out.length() == 0) out = fallback;
  if (out.length() == 0) out = "Arquivo";

  // Remove chars perigosos para CSV/download e mantém legível
  String cleaned = "";
  for (uint16_t i = 0; i < out.length(); i++) {
    char c = out[i];
    bool ok = (isalnum((unsigned char)c) || c == '_' || c == '-' || c == ' ');
    if (ok) cleaned += c;
  }

  cleaned.trim();
  if (cleaned.length() == 0) cleaned = fallback.length() ? fallback : "Arquivo";
  if (cleaned.length() > FILE_NAME_LEN) cleaned = cleaned.substring(0, FILE_NAME_LEN);
  return cleaned;
}

String defaultFileName(uint8_t fileType) {
  if (fileType == FILE_TYPE_COUNTER) {
    return "Contador_" + String(gStorage.nextCounterNumber);
  }
  return "Cronometro_" + String(gStorage.nextChronoNumber);
}

bool saveSessionFile(uint8_t fileType, const String& requestedName, uint32_t durationMs,
                     EventRecord* events, uint16_t eventCount, uint16_t& outFileId) {

  if (gStorage.fileCount >= MAX_FILES) {
    sendInfoMessage("Memória de arquivos cheia. Exclua arquivos manualmente.", true);
    return false;
  }

  if ((uint32_t)gStorage.usedEvents + eventCount > MAX_EVENT_RECORDS) {
    sendInfoMessage("Memória de eventos baixa. Exclua arquivos manualmente para liberar espaço.", true);
    return false;
  }

  // Indicador preventivo de baixa memória
  uint32_t futureUsed = sizeof(StorageHeader)
                        + (uint32_t)(gStorage.fileCount + 1) * sizeof(FileRecord)
                        + (uint32_t)(gStorage.usedEvents + eventCount) * sizeof(EventRecord);

  if (futureUsed > EEPROM_SIZE) {
    sendInfoMessage("EEPROM insuficiente. Exclua arquivos manualmente.", true);
    return false;
  }

  String fallback = defaultFileName(fileType);
  String finalName = sanitizeFileName(requestedName, fallback);

  FileRecord rec;
  memset(&rec, 0, sizeof(rec));

  rec.id = (uint16_t)millis();
  // Garante id único mesmo com colisão
  while (findFileIndexById(rec.id) >= 0) rec.id++;

  rec.type = fileType;
  finalName.toCharArray(rec.name, sizeof(rec.name));
  rec.timestampSec = (uint32_t)(millis() / 1000UL);
  rec.durationMs = durationMs;
  rec.eventStart = gStorage.usedEvents;
  rec.eventCount = eventCount;

  for (uint16_t i = 0; i < eventCount; i++) {
    gEvents[gStorage.usedEvents + i] = events[i];
  }

  gFiles[gStorage.fileCount] = rec;
  gStorage.fileCount++;
  gStorage.usedEvents += eventCount;

  if (fileType == FILE_TYPE_COUNTER) gStorage.nextCounterNumber++;
  else gStorage.nextChronoNumber++;

  if (!storageCommit()) return false;

  outFileId = rec.id;
  Serial.printf("✓ Arquivo salvo: ID=%u nome=%s tipo=%u eventos=%u\n",
                rec.id, rec.name, rec.type, rec.eventCount);

  if (storageFreeBytes() < 300) {
    sendInfoMessage("Atenção: memória livre baixa. Exclua arquivos manualmente.", true);
  }

  return true;
}

bool deleteFileById(uint16_t id) {
  int idx = findFileIndexById(id);
  if (idx < 0) {
    sendInfoMessage("Arquivo não encontrado", true);
    return false;
  }

  uint16_t start = gFiles[idx].eventStart;
  uint16_t count = gFiles[idx].eventCount;

  // Compacta eventos para liberar memória real
  for (uint16_t i = start; i + count < gStorage.usedEvents; i++) {
    gEvents[i] = gEvents[i + count];
  }

  // Ajusta eventStart dos arquivos posteriores
  for (int i = 0; i < gStorage.fileCount; i++) {
    if (i == idx) continue;
    if (gFiles[i].eventStart > start) {
      gFiles[i].eventStart -= count;
    }
  }

  // Compacta tabela de arquivos
  for (int i = idx; i < gStorage.fileCount - 1; i++) {
    gFiles[i] = gFiles[i + 1];
  }
  memset(&gFiles[gStorage.fileCount - 1], 0, sizeof(FileRecord));

  gStorage.fileCount--;
  gStorage.usedEvents -= count;

  return storageCommit();
}

bool renameFileById(uint16_t id, const String& newName) {
  int idx = findFileIndexById(id);
  if (idx < 0) {
    sendInfoMessage("Arquivo não encontrado", true);
    return false;
  }

  String oldName = String(gFiles[idx].name);
  String safeName = sanitizeFileName(newName, oldName);
  safeName.toCharArray(gFiles[idx].name, sizeof(gFiles[idx].name));

  return storageCommit();
}

String fileTypeLabel(uint8_t type) {
  if (type == FILE_TYPE_COUNTER) return "contador";
  return "cronometro";
}

const char* detectModeLabel(uint8_t mode) {
  if (mode == DETECT_RISING) return "RISING";
  if (mode == DETECT_FALLING) return "FALLING";
  if (mode == DETECT_BOTH) return "BOTH";
  return "INVALID";
}

String csvFromFile(const FileRecord& rec) {
  String csv = "";

  if (rec.type == FILE_TYPE_COUNTER) {
    csv += "seq,sensor,timestamp_ms,estado,contador_pulsos\n";
  } else {
    csv += "seq,sensor,timestamp_ms,estado\n";
  }

  for (uint16_t i = 0; i < rec.eventCount; i++) {
    const EventRecord& ev = gEvents[rec.eventStart + i];
    csv += String(i + 1) + ",";
    csv += "S" + String(ev.sensor) + ",";
    csv += String(ev.timestampMs) + ",";
    csv += (ev.estado == HIGH ? "HIGH" : "LOW");

    if (rec.type == FILE_TYPE_COUNTER) {
      csv += "," + String(ev.counterPulses);
    }

    csv += "\n";
  }

  return csv;
}

void sendFilesList(uint8_t clientNum) {
  if (gStorage.fileCount == 0) {
    selectedFileIndex = -1;
    selectedFileId = 0;
  } else if (selectedFileIndex < 0 || selectedFileIndex >= gStorage.fileCount) {
    selectedFileIndex = 0;
    selectedFileId = gFiles[selectedFileIndex].id;
  }

  DynamicJsonDocument doc(4096);
  doc["type"] = "files_list";
  JsonArray files = doc.createNestedArray("files");

  for (int i = 0; i < gStorage.fileCount; i++) {
    JsonObject f = files.createNestedObject();
    f["id"] = gFiles[i].id;
    f["name"] = gFiles[i].name;
    f["type"] = gFiles[i].type;
    f["typeLabel"] = fileTypeLabel(gFiles[i].type);
    f["timestamp"] = gFiles[i].timestampSec;
    f["durationMs"] = gFiles[i].durationMs;
    f["eventCount"] = gFiles[i].eventCount;
  }

  String out;
  serializeJson(doc, out);

  if (clientNum == 255) webSocket.broadcastTXT(out);
  else webSocket.sendTXT(clientNum, out);
}

void sendFileView(uint8_t clientNum, uint16_t fileId) {
  int idx = findFileIndexById(fileId);
  if (idx < 0) {
    sendInfoMessage("Arquivo não encontrado", true);
    return;
  }

  const FileRecord& rec = gFiles[idx];
  DynamicJsonDocument doc(4096);
  doc["type"] = "file_view";
  doc["id"] = rec.id;
  doc["name"] = rec.name;
  doc["timestamp"] = rec.timestampSec;
  doc["durationMs"] = rec.durationMs;
  doc["eventCount"] = rec.eventCount;
  doc["typeLabel"] = fileTypeLabel(rec.type);
  doc["file_type"] = rec.type;

  JsonArray events = doc.createNestedArray("events");
  for (uint16_t i = 0; i < rec.eventCount; i++) {
    const EventRecord& ev = gEvents[rec.eventStart + i];
    JsonObject o = events.createNestedObject();
    o["sensor"] = ev.sensor;
    o["timestamp_ms"] = ev.timestampMs;
    o["estado"] = (ev.estado == HIGH ? "HIGH" : "LOW");
    o["contador_pulsos"] = ev.counterPulses;
  }

  String out;
  serializeJson(doc, out);
  if (clientNum == 255) {
    webSocket.broadcastTXT(out);
  } else {
    webSocket.sendTXT(clientNum, out);
  }
}

// ============================================================
//                     ENTRADA FÍSICA / BOTÕES
// ============================================================
void checkButtons() {
  for (int i = 0; i < 5; i++) {
    bool reading = (digitalRead(buttons[i].pin) == LOW);

    if (reading && !buttons[i].pressed) {
      buttons[i].pressed = true;
      buttons[i].pressTime = millis();
    }

    if (!reading && buttons[i].pressed) {
      unsigned long dt = millis() - buttons[i].pressTime;
      if (dt >= MIN_PRESS_TIME && dt <= MAX_PRESS_TIME) {
        processCommand(buttons[i].command);
      }
      buttons[i].pressed = false;
    }
  }
}

// ============================================================
//                      LEITURA SENSORES / LEDS
// ============================================================
int readSensor1() { return digitalRead(SENSOR_1_PIN); }
int readSensor2() { return digitalRead(SENSOR_2_PIN); }
int readSensor3() { return digitalRead(SENSOR_3_PIN); }

void updateNeoPixelsFromSensors() {
  int s1 = readSensor1();
  int s2 = readSensor2();
  int s3 = readSensor3();

  pixels.setPixelColor(0, s1 == HIGH ? pixels.Color(220, 0, 0) : pixels.Color(0, 130, 0));
  pixels.setPixelColor(1, s2 == HIGH ? pixels.Color(220, 0, 0) : pixels.Color(0, 130, 0));
  pixels.setPixelColor(2, s3 == HIGH ? pixels.Color(220, 0, 0) : pixels.Color(0, 130, 0));
  pixels.show();
}

void turnOffAllLeds() {
  for (int i = 0; i < NUM_LEDS; i++) pixels.setPixelColor(i, pixels.Color(0, 0, 0));
  pixels.show();
}

void setLEDColor(uint32_t color) {
  for (int i = 0; i < NUM_LEDS; i++) pixels.setPixelColor(i, color);
  pixels.show();
}

void piscarLED(int ledIndex, uint32_t color, int vezes, int delayMs) {
  if (ledIndex < 0 || ledIndex >= NUM_LEDS) return;
  if (vezes <= 0) vezes = 1;
  if (delayMs < 1) delayMs = 1;

  ledFlashState[ledIndex] = 1;  // ON
  ledFlashColor[ledIndex] = color;
  ledFlashCount[ledIndex] = (uint8_t)vezes;
  ledFlashDelayMs[ledIndex] = (uint16_t)delayMs;
  ledFlashStart[ledIndex] = millis();

  pixels.setPixelColor(ledIndex, color);
  pixels.show();
}

void updateLedFlashes() {
  bool changed = false;
  unsigned long now = millis();

  for (int i = 0; i < NUM_LEDS; i++) {
    if (ledFlashState[i] == 0 || ledFlashCount[i] == 0) continue;
    if (now - ledFlashStart[i] < ledFlashDelayMs[i]) continue;

    if (ledFlashState[i] == 1) {
      pixels.setPixelColor(i, pixels.Color(0, 0, 0));
      ledFlashState[i] = 2;
      ledFlashStart[i] = now;
      changed = true;
    } else {
      if (ledFlashCount[i] > 0) {
        ledFlashCount[i]--;
      }

      if (ledFlashCount[i] == 0) {
        ledFlashState[i] = 0;
      } else {
        pixels.setPixelColor(i, ledFlashColor[i]);
        ledFlashState[i] = 1;
        ledFlashStart[i] = now;
        changed = true;
      }
    }
  }

  if (changed) {
    pixels.show();
  }
}

void resetSessionToMenuOnDisconnect() {
  chronoRunning = false;
  chronoActive = false;
  chronoStartTime = 0;
  chronoPausedTime = 0;
  chronoTime = 0;

  counterActive = false;
  counterStartTime = 0;
  counterElapsedMs = 0;
  counterValues[0] = 0;
  counterValues[1] = 0;
  counterValues[2] = 0;

  captureEventCount = 0;
  selectedFileIndex = -1;
  selectedFileId = 0;

  changeState(STATE_MENU, 0);
  sendInfoMessage("Sem clientes WebSocket por 2s: sessão reiniciada e retorno ao menu");
}

void checkAutoResetOnDisconnect() {
  uint8_t currentClients = webSocket.connectedClients();

  if (currentClients != lastClientCount) {
    lastClientCount = currentClients;
    if (currentClients == 0) {
      noClientSinceMs = millis();
    } else {
      noClientSinceMs = 0;
    }
  }

  if (currentState == STATE_MENU) return;
  if (currentClients > 0) return;
  if (noClientSinceMs == 0) {
    noClientSinceMs = millis();
    return;
  }

  if (millis() - noClientSinceMs >= 2000) {
    resetSessionToMenuOnDisconnect();
    noClientSinceMs = millis();
  }
}

// ============================================================
//                     ENVIO DE ESTADO (JSON)
// ============================================================
void sendInfoMessage(const String& msg, bool warning) {
  DynamicJsonDocument doc(256);
  doc["type"] = warning ? "warning" : "info";
  doc["message"] = msg;
  String out;
  serializeJson(doc, out);
  webSocket.broadcastTXT(out);
  if (warning) lastWarning = msg;
}

void sendStateToClients() {
  DynamicJsonDocument doc(3072);
  doc["type"] = "state";
  doc["state"] = currentState;
  doc["menuSelection"] = menuSelection;

  doc["chrono_time"] = chronoTime;
  doc["chrono_running"] = chronoRunning;
  doc["counter_active"] = counterActive;
  doc["counter_value"] = counterValue;
  doc["counter1"] = counterValues[0];
  doc["counter2"] = counterValues[1];
  doc["counter3"] = counterValues[2];

  doc["sensor1"] = readSensor1();
  doc["sensor2"] = readSensor2();
  doc["sensor3"] = readSensor3();

  doc["mode1"] = gStorage.sensorMode[0];
  doc["mode2"] = gStorage.sensorMode[1];
  doc["mode3"] = gStorage.sensorMode[2];

  doc["files_count"] = gStorage.fileCount;
  doc["selected_file_id"] = selectedFileId;
  doc["events_used"] = gStorage.usedEvents;
  doc["mem_used"] = storageUsedBytes();
  doc["mem_free"] = storageFreeBytes();

  doc["default_chrono_name"] = defaultFileName(FILE_TYPE_CHRONO);
  doc["default_counter_name"] = defaultFileName(FILE_TYPE_COUNTER);

  JsonArray evs = doc.createNestedArray("capture_events");
  for (uint16_t i = 0; i < captureEventCount; i++) {
    JsonObject e = evs.createNestedObject();
    e["sensor"] = captureEvents[i].sensor;
    e["ts"] = captureEvents[i].timestampMs;
    e["estado"] = captureEvents[i].estado;
    e["counter"] = captureEvents[i].counterPulses;
  }

  String out;
  serializeJson(doc, out);
  webSocket.broadcastTXT(out);
}