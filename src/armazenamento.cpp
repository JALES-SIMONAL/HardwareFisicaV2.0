#include "armazenamento.hpp"

#include <Arduino.h>
#include <SD.h>
#include <SPI.h>
#include <cstdio>
#include <cstring>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>

#include "MAIN.HPP"

namespace armazenamento {

// Declarada aqui (fora do namespace anônimo) para poder ser chamada de
// ihm.cpp; definida mais abaixo, depois do namespace anônimo, mas ainda
// acessa contadorTrocasBarramento/donoEhDisplay normalmente — variáveis de
// namespace anônimo têm ligação interna, mas são visíveis em toda a
// unidade de tradução, não só dentro do bloco anônimo.
void logDiagnosticoBarramento(const char* contexto);

namespace {

struct LinhaCSV {
  char texto[24];
};

QueueHandle_t filaLinhas = nullptr;
File arquivoAtual;
bool arquivoAberto = false;
bool cartaoOk = false;
uint32_t erros = 0;

char bufferFlush[STORAGE_FLUSH_THRESHOLD][24];
uint16_t linhasNoBuffer = 0;

// Pedido de fechamento assíncrono (ver solicitarFechamentoArquivo() /
// solicitarFechamentoEExclusao()) — só escritos/lidos como bool simples de
// 1 byte, sem mutex dedicado: o pior caso de corrida entre núcleos aqui é
// processarFila() ver o pedido um tick (~1ms) depois do esperado, nunca
// corrompe estado. O nome a excluir é escrito ANTES da flag correspondente
// (ordem importa: processarFila() só olha o nome depois de ver a flag).
volatile bool fechamentoPendente = false;
volatile bool exclusaoAposFechamentoPendente = false;
char nomeExclusaoAposFechamento[32] = "";

File arquivoLeitura;
bool leituraAberta = false;

// File separado de arquivoLeitura: leitura binária genérica (imagens
// BMP etc.), independente da leitura linha-a-linha usada para CSV.
File arquivoBinario;
bool binarioAberto = false;

// abrirNovoArquivo()/fecharArquivoAtual() são chamadas pela IHM (núcleo 1,
// em resposta ao usuário); processarFila() roda na tarefa de armazenamento
// (núcleo 0). Este mutex protege arquivoAtual/arquivoAberto/buffer contra
// acesso concorrente entre os dois núcleos — nunca é tomado durante a
// leitura de análise de dados, que usa um File separado (arquivoLeitura).
SemaphoreHandle_t mutexArquivo = nullptr;

// Protege o barramento SPI físico compartilhado com o TFT (ver comentário
// grande em armazenamento.hpp). nullptr até init() rodar — travarBarramentoSPI()
// vira no-op nesse intervalo (ihm::init() desenha antes de armazenamento::init()
// existir; não há ainda nenhum acesso ao SD para disputar o barramento).
SemaphoreHandle_t mutexBarramentoSPI = nullptr;

// true = último lado a reconfigurar fisicamente o barramento foi o
// display; false = foi o SD. Só existe para evitar reconfigurar
// (SPI.begin()/pinMode()) quando o dono não mudou — ver comentário grande
// em armazenamento.hpp. Começa true: ihm::init() (display->begin(), via
// Arduino_SWSPI bit-bang) sempre roda antes de armazenamento::init() nesta
// aplicação, então os pinos já estão fisicamente em modo GPIO/bit-bang
// quando o primeiro TravaBarramentoSD desta sessão é construído — sem
// isto, o SD.begin() inicial rodaria achando (por causa do valor padrão)
// que não precisa chamar SPI.begin(), e ficaria sem resposta física.
bool donoEhDisplay = true;

// ---------------------------------------------------------------------
// Diagnóstico temporário da falha "File system is not mounted" /
// "sdSelectCard(): Select Failed" observada ao iniciar um experimento
// depois de navegar bastante pelo menu (muitas trocas de dono do
// barramento). Conta quantas vezes o barramento troca de dono desde o
// boot e imprime o nível elétrico atual de cada pino compartilhado nos
// pontos-chave (troca de dono, remontagem, abertura de arquivo) — ajuda a
// ver se algum pino fica "preso" num nível errado quando a falha ocorre.
// ---------------------------------------------------------------------
uint32_t contadorTrocasBarramento = 0;

// RAII: toma o mutex do barramento e, só se o dono estiver de fato
// mudando (do display para o SD), rotea os pinos de volta para o
// periférico de SPI de hardware. Chamar SPI.begin() incondicionalmente a
// cada acesso — inclusive entre leituras consecutivas do próprio SD, como
// uma linha de BMP após a outra — reinicializa o periférico de SPI dezenas
// de vezes por segundo e corrompe o estado interno do cartão na prática.
class TravaBarramentoSD {
 public:
  TravaBarramentoSD() {
    if (mutexBarramentoSPI != nullptr) xSemaphoreTake(mutexBarramentoSPI, portMAX_DELAY);
    if (donoEhDisplay) {
      logDiagnosticoBarramento("ANTES troca Display->SD");
      // Desseleciona o TFT (CS em HIGH) ANTES de usar o SPI de hardware
      // para o SD — simétrico à mesma proteção do lado do display (ver
      // TravaBarramentoDisplay em ihm.cpp). Sem isto, se TFT_CS ficasse em
      // LOW durante o tráfego SPI do SD, o controlador do display
      // interpretaria esse tráfego como comandos endereçados a ele.
      pinMode(TFT_CS, OUTPUT);
      digitalWrite(TFT_CS, HIGH);
      // SPI.begin() só reanexa SCK/MISO/MOSI ao periférico de hardware na
      // PRIMEIRA vez que é chamado nesta instância — depois disso, se _spi
      // já não é nulo, ele retorna sem reanexar nada (ver
      // SPIClass::begin() em SPI.cpp: "if (_spi) { return; }"). Como
      // TravaBarramentoDisplay desanexa esses pinos de propósito
      // (pinMode() para bit-bang), sem o end() aqui o SPI.begin() abaixo
      // virava no-op a partir da segunda troca de dono: os pinos ficavam
      // presos em modo GPIO simples, nunca voltavam a ser roteados para o
      // periférico SPI, e o cartão ficava sem resposta física dali em
      // diante ("sdSelectCard(): Select Failed" mesmo após remontar).
      SPI.end();
      SPI.begin(TFT_SCLK, TFT_MISO, TFT_MOSI, SD_CS_PIN);
      donoEhDisplay = false;
      contadorTrocasBarramento++;
      logDiagnosticoBarramento("DEPOIS troca Display->SD");
    }
  }
  ~TravaBarramentoSD() {
    if (mutexBarramentoSPI != nullptr) xSemaphoreGive(mutexBarramentoSPI);
  }
};

// Reinicialização completa do cartão (CMD0/CMD8/ACMD41 de novo via
// SD.begin()) — diferente de só religar o roteamento dos pinos
// (TravaBarramentoSD). Observado na prática: mesmo com o CS do outro lado
// sempre desselecionado, a primeira operação de SD logo após um desenho no
// display às vezes falha (sdSelectCard()/CMD13 repetidamente) porque a
// sessão interna do cartão morre — só um SD.end()+SD.begin() novo
// recupera; tentar SD.open() de novo sem remontar bate na mesma falha,
// porque o driver já esgotou as tentativas dele antes de devolver erro.
// Só chamar já dentro de um TravaBarramentoSD (bus já roteado para o SD).
bool remontarCartaoSD() {
  Serial.println("[SD] Operacao falhou, tentando remontar o cartao");
  logDiagnosticoBarramento("ANTES remontarCartaoSD (SD.end)");
  SD.end();
  // Pequeno atraso de acomodacao antes de tentar de novo: se a falha for de
  // origem eletrica/tempo de estabilizacao do barramento apos a troca de
  // dono (display->SD), remontar imediatamente pode bater na mesma falha.
  delay(50);
  logDiagnosticoBarramento("ANTES remontarCartaoSD (SD.begin)");
  const bool ok = SD.begin(SD_CS_PIN);
  Serial.printf("[SD] Remontagem: %s\n", ok ? "sucesso" : "falha");
  logDiagnosticoBarramento("DEPOIS remontarCartaoSD");
  return ok;
}

// Só chamar já com mutexArquivo tomado.
void descarregarBufferInterno() {
  if (linhasNoBuffer == 0 || !arquivoAberto) return;

  for (uint16_t i = 0; i < linhasNoBuffer; i++) {
    arquivoAtual.print(bufferFlush[i]);
    arquivoAtual.print("\n");
  }
  arquivoAtual.flush();
  linhasNoBuffer = 0;
}

// Só chamar já com mutexArquivo tomado. Retira um item de filaLinhas (se
// houver) e o coloca no buffer, descarregando-o se encher. Retorna false
// quando a fila estava vazia (nada para fazer).
bool processarUmItemFilaInterno() {
  if (filaLinhas == nullptr) return false;

  LinhaCSV item;
  if (xQueueReceive(filaLinhas, &item, 0) != pdTRUE) return false;

  if (arquivoAberto) {
    std::strncpy(bufferFlush[linhasNoBuffer], item.texto, sizeof(bufferFlush[linhasNoBuffer]) - 1);
    bufferFlush[linhasNoBuffer][sizeof(bufferFlush[linhasNoBuffer]) - 1] = '\0';
    linhasNoBuffer++;

    if (linhasNoBuffer >= STORAGE_FLUSH_THRESHOLD) descarregarBufferInterno();
  }
  return true;
}

// Só chamar já com mutexArquivo tomado.
void fecharArquivoAtualInterno() {
  // Esvazia filaLinhas ANTES de fechar. Sem isto havia uma corrida entre
  // esta função e processarFila() (tarefa separada, núcleo 0): quando
  // finalizarRepeticaoAtual() enfileira as linhas da ÚLTIMA repetição e, em
  // seguida, chama fecharArquivoAtual() (ver experimentos.cpp), o fecho
  // podia acontecer antes de processarFila() rodar de novo e drenar essas
  // linhas — o arquivo fechava (arquivoAberto=false) e, quando
  // processarFila() finalmente processava os itens já enfileirados, o
  // "if (arquivoAberto)" os descartava silenciosamente. Isso perdia sempre
  // os dados da última repetição de cada experimento, nunca os das
  // repetições anteriores (essas nunca corriam contra um fechamento
  // imediato de arquivo).
  while (processarUmItemFilaInterno()) {
  }
  descarregarBufferInterno();
  if (arquivoAberto) arquivoAtual.close();
  arquivoAberto = false;
}

}  // namespace

void logDiagnosticoBarramento(const char* contexto) {
  Serial.printf(
      "[DIAG][BARRAMENTO] %s | core=%d | ms=%lu | trocas=%lu | donoEhDisplay=%d | "
      "TFT_CS=%d TFT_MOSI=%d TFT_SCLK=%d TFT_MISO=%d SD_CS=%d | heap=%u\n",
      contexto, static_cast<int>(xPortGetCoreID()), static_cast<unsigned long>(millis()),
      static_cast<unsigned long>(contadorTrocasBarramento), static_cast<int>(donoEhDisplay),
      digitalRead(TFT_CS), digitalRead(TFT_MOSI), digitalRead(TFT_SCLK), digitalRead(TFT_MISO),
      digitalRead(SD_CS_PIN), static_cast<unsigned>(ESP.getFreeHeap()));
}

void init() {
  filaLinhas = xQueueCreate(CSV_LINE_QUEUE_LEN, sizeof(LinhaCSV));
  mutexArquivo = xSemaphoreCreateMutex();
  mutexBarramentoSPI = xSemaphoreCreateMutex();

  Serial.println("[SD] Iniciando microSD");
  TravaBarramentoSD travaBus;
  cartaoOk = SD.begin(SD_CS_PIN);
  if (!cartaoOk) {
    Serial.println("[SD] Falha ao montar o cartao (verifique SD_CS_PIN em MAIN.HPP)");
    Serial.println("[SD] Aplicacao continuara em modo sem armazenamento");
  } else {
    Serial.println("[SD] microSD montado com sucesso");
  }
}

bool cartaoDisponivel() { return cartaoOk; }

bool arquivoExiste(const char* nomeComExtensao) {
  if (!cartaoOk) return false;
  TravaBarramentoSD travaBus;
  char caminho[32];
  snprintf(caminho, sizeof(caminho), "/%s", nomeComExtensao);
  return SD.exists(caminho);
}

bool abrirNovoArquivo(const char* nomeSemExtensao, bool sobrescrever) {
  Serial.printf("[DIAG][SD] abrirNovoArquivo(\"%s\") chamado | cartaoOk=%d\n", nomeSemExtensao,
                static_cast<int>(cartaoOk));
  if (!cartaoOk) return false;

  TravaBarramentoSD travaBus;
  logDiagnosticoBarramento("abrirNovoArquivo apos TravaBarramentoSD");

  char caminho[32];
  snprintf(caminho, sizeof(caminho), "/%s.csv", nomeSemExtensao);

  const bool existiaAntes = SD.exists(caminho);
  Serial.printf("[DIAG][SD] SD.exists(\"%s\") = %d\n", caminho, static_cast<int>(existiaAntes));
  if (existiaAntes && !sobrescrever) return false;

  xSemaphoreTake(mutexArquivo, portMAX_DELAY);

  if (arquivoAberto) fecharArquivoAtualInterno();

  arquivoAtual = SD.open(caminho, FILE_WRITE);
  Serial.printf("[DIAG][SD] 1a tentativa SD.open(\"%s\", FILE_WRITE) = %d\n", caminho,
                static_cast<int>(static_cast<bool>(arquivoAtual)));
  if (!arquivoAtual) {
    // Mesmo sintoma documentado em remontarCartaoSD(): a primeira operação
    // de SD após uma sequência de desenhos no display (troca de barramento
    // display<->SD) pode achar a sessão do cartão morta ("File system is
    // not mounted"). Sem isto, abrir um experimento novo falhava sempre
    // que o usuário tinha acabado de navegar pelo menu antes de confirmar.
    if (remontarCartaoSD()) {
      arquivoAtual = SD.open(caminho, FILE_WRITE);
      Serial.printf("[DIAG][SD] 2a tentativa (pos remontagem) SD.open(\"%s\", FILE_WRITE) = %d\n",
                    caminho, static_cast<int>(static_cast<bool>(arquivoAtual)));
    }
  }
  const bool ok = static_cast<bool>(arquivoAtual);
  if (ok) {
    arquivoAtual.print("canal,estado,tempo_us\n");
    arquivoAberto = true;
    linhasNoBuffer = 0;
  } else {
    erros++;
  }
  Serial.printf("[DIAG][SD] abrirNovoArquivo(\"%s\") resultado final = %d\n", nomeSemExtensao,
                static_cast<int>(ok));

  xSemaphoreGive(mutexArquivo);
  return ok;
}

void enfileirarLinha(const char* linhaCsv) {
  if (filaLinhas == nullptr) return;

  LinhaCSV item;
  std::strncpy(item.texto, linhaCsv, sizeof(item.texto) - 1);
  item.texto[sizeof(item.texto) - 1] = '\0';

  if (xQueueSend(filaLinhas, &item, 0) != pdTRUE) {
    erros++;
  }
}

void enfileirarLinhaEmBranco() { enfileirarLinha(""); }

void processarFila() {
  if (filaLinhas == nullptr) return;

  while (true) {
    // Toma o barramento antes do mutexArquivo (mesma ordem em toda função
    // desta unidade) e o reivindica para o SD — o buffer só acumula em
    // RAM na maioria das iterações, mas o flush real (a cada
    // STORAGE_FLUSH_THRESHOLD linhas) precisa do barramento já roteado.
    // Um item por vez (em vez de drenar a fila inteira sob um único lock)
    // para não segurar o barramento SPI por muito tempo e atrasar o
    // display, que também disputa esse barramento.
    TravaBarramentoSD travaBus;
    xSemaphoreTake(mutexArquivo, portMAX_DELAY);
    const bool processouAlgo = processarUmItemFilaInterno();
    xSemaphoreGive(mutexArquivo);

    if (!processouAlgo) break;
  }

  // Atende um pedido de fechamento assíncrono (núcleo 1, ver
  // solicitarFechamentoArquivo()) DEPOIS de esvaziar a fila acima — mesma
  // ordem que fecharArquivoAtualInterno() já impõe internamente, só que
  // aqui quem bloqueia é esta própria tarefa (núcleo 0), nunca a de IHM/
  // Bluetooth. É seguro fazer isto a cada volta do loop() de
  // tarefaAquisicaoArmazenamento (chamada a cada ~1ms): sem pedido
  // pendente, os dois "if" abaixo são só leituras de bool.
  if (fechamentoPendente) {
    fechamentoPendente = false;
    {
      TravaBarramentoSD travaBus;
      xSemaphoreTake(mutexArquivo, portMAX_DELAY);
      fecharArquivoAtualInterno();
      xSemaphoreGive(mutexArquivo);
    }
    if (exclusaoAposFechamentoPendente) {
      exclusaoAposFechamentoPendente = false;
      excluirArquivo(nomeExclusaoAposFechamento);
    }
  }
}

void fecharArquivoAtual() {
  TravaBarramentoSD travaBus;
  xSemaphoreTake(mutexArquivo, portMAX_DELAY);
  fecharArquivoAtualInterno();
  xSemaphoreGive(mutexArquivo);
}

void solicitarFechamentoArquivo() { fechamentoPendente = true; }

void solicitarFechamentoEExclusao(const char* nomeComExtensao) {
  std::strncpy(nomeExclusaoAposFechamento, nomeComExtensao,
               sizeof(nomeExclusaoAposFechamento) - 1);
  nomeExclusaoAposFechamento[sizeof(nomeExclusaoAposFechamento) - 1] = '\0';
  exclusaoAposFechamentoPendente = true;
  fechamentoPendente = true;
}

namespace {

bool nomesIguaisSemCase(const char* a, const char* b) {
  while (*a != '\0' && *b != '\0') {
    const char ca = (*a >= 'a' && *a <= 'z') ? static_cast<char>(*a - 'a' + 'A') : *a;
    const char cb = (*b >= 'a' && *b <= 'z') ? static_cast<char>(*b - 'a' + 'A') : *b;
    if (ca != cb) return false;
    a++;
    b++;
  }
  return *a == '\0' && *b == '\0';
}

// Logotipos de boot (docs/*.bmp copiados para a raiz do SD): não são
// arquivos de dados de experimento, então nunca devem aparecer nas telas de
// Gerenciamento de arquivos/Análise de dados (local ou pelo app).
bool ehArquivoOcultoDoUsuario(const char* nome) {
  return nomesIguaisSemCase(nome, "Monkey Tech.bmp") || nomesIguaisSemCase(nome, "UFRN.bmp");
}

}  // namespace

uint16_t listarArquivos(InfoArquivo* destino, uint16_t capacidadeDestino) {
  if (!cartaoOk || destino == nullptr) return 0;

  TravaBarramentoSD travaBus;
  File raiz = SD.open("/");
  if (!raiz) return 0;

  uint16_t quantidade = 0;
  File entrada = raiz.openNextFile();
  while (entrada && quantidade < capacidadeDestino) {
    if (!entrada.isDirectory() && !ehArquivoOcultoDoUsuario(entrada.name())) {
      std::strncpy(destino[quantidade].nome, entrada.name(), sizeof(destino[quantidade].nome) - 1);
      destino[quantidade].nome[sizeof(destino[quantidade].nome) - 1] = '\0';
      destino[quantidade].tamanhoBytes = static_cast<uint32_t>(entrada.size());
      quantidade++;
    }
    entrada.close();
    entrada = raiz.openNextFile();
  }
  raiz.close();

  return quantidade;
}

uint16_t excluirTodosArquivosCsv() {
  if (!cartaoOk) return 0;

  // Um unico TravaBarramentoSD para a passada inteira — NAO chamar
  // excluirArquivo() daqui dentro (ela toma o mesmo mutex de novo, e por
  // nao ser recursivo isso trava o firmware).
  TravaBarramentoSD travaBus;
  File raiz = SD.open("/");
  if (!raiz) return 0;

  uint16_t excluidos = 0;
  File entrada = raiz.openNextFile();
  while (entrada) {
    char nome[32];
    std::strncpy(nome, entrada.name(), sizeof(nome) - 1);
    nome[sizeof(nome) - 1] = '\0';
    const bool ehDiretorio = entrada.isDirectory();
    entrada.close();

    const size_t comprimento = std::strlen(nome);
    const bool ehCsv = comprimento > 4 && std::strcmp(nome + comprimento - 4, ".csv") == 0;
    if (!ehDiretorio && ehCsv) {
      char caminho[34];
      snprintf(caminho, sizeof(caminho), "/%s", nome);
      if (SD.remove(caminho)) excluidos++;
    }

    entrada = raiz.openNextFile();
  }
  raiz.close();

  return excluidos;
}

bool renomearArquivo(const char* nomeAtual, const char* novoNome) {
  if (!cartaoOk) return false;

  // Nome novo igual ao atual (ex.: usuário abriu "Renomear" e confirmou
  // sem editar nada): nada a fazer. Sem isto, o SD.exists(para) logo
  // abaixo sempre achava o PRÓPRIO arquivo e retornava false (\"já
  // existe\"), fazendo o chamador (finalizarEdicaoNomeArquivo(), em
  // maquina_estados.cpp) só apitar e deixar o usuário preso na tela de
  // edição, sem navegar para lugar nenhum nem explicar o motivo.
  if (nomesIguaisSemCase(nomeAtual, novoNome)) return true;

  TravaBarramentoSD travaBus;
  char de[32];
  char para[32];
  snprintf(de, sizeof(de), "/%s", nomeAtual);
  snprintf(para, sizeof(para), "/%s", novoNome);

  if (SD.exists(para)) return false;
  return SD.rename(de, para);
}

bool excluirArquivo(const char* nome) {
  if (!cartaoOk) return false;
  TravaBarramentoSD travaBus;
  char caminho[32];
  snprintf(caminho, sizeof(caminho), "/%s", nome);
  return SD.remove(caminho);
}

bool abrirParaLeitura(const char* nomeComExtensao) {
  if (!cartaoOk) return false;

  TravaBarramentoSD travaBus;
  char caminho[32];
  snprintf(caminho, sizeof(caminho), "/%s", nomeComExtensao);

  arquivoLeitura = SD.open(caminho, FILE_READ);
  if (!arquivoLeitura) return false;

  leituraAberta = true;
  return true;
}

bool lerProximaLinha(char* destino, size_t tamanhoDestino) {
  if (!leituraAberta || tamanhoDestino == 0) {
    if (tamanhoDestino > 0) destino[0] = '\0';
    return false;
  }

  TravaBarramentoSD travaBus;
  if (!arquivoLeitura.available()) {
    destino[0] = '\0';
    return false;
  }

  size_t i = 0;
  while (arquivoLeitura.available()) {
    const int c = arquivoLeitura.read();
    if (c < 0 || c == '\n') break;
    if (c == '\r') continue;
    if (i < tamanhoDestino - 1) destino[i++] = static_cast<char>(c);
  }
  destino[i] = '\0';
  return true;
}

void fecharLeitura() {
  TravaBarramentoSD travaBus;
  if (leituraAberta) arquivoLeitura.close();
  leituraAberta = false;
}

bool abrirBinarioParaLeitura(const char* nomeComExtensao) {
  if (!cartaoOk) return false;

  TravaBarramentoSD travaBus;
  char caminho[32];
  snprintf(caminho, sizeof(caminho), "/%s", nomeComExtensao);

  arquivoBinario = SD.open(caminho, FILE_READ);
  if (!arquivoBinario) {
    // Falha na primeira tentativa: pode ser a sessão do cartão tendo
    // morrido (ver comentário de remontarCartaoSD()) — remonta e tenta
    // uma única vez mais antes de desistir.
    if (remontarCartaoSD()) {
      arquivoBinario = SD.open(caminho, FILE_READ);
    }
  }
  if (!arquivoBinario) return false;

  binarioAberto = true;
  return true;
}

size_t lerBinario(uint8_t* destino, size_t quantidadeBytes) {
  if (!binarioAberto || destino == nullptr || quantidadeBytes == 0) return 0;
  TravaBarramentoSD travaBus;
  return arquivoBinario.read(destino, quantidadeBytes);
}

bool posicionarBinario(uint32_t offset) {
  if (!binarioAberto) return false;
  TravaBarramentoSD travaBus;
  return arquivoBinario.seek(offset);
}

void fecharBinario() {
  TravaBarramentoSD travaBus;
  if (binarioAberto) arquivoBinario.close();
  binarioAberto = false;
}

uint64_t espacoTotalBytes() {
  if (!cartaoOk) return 0;
  TravaBarramentoSD travaBus;
  return SD.totalBytes();
}
uint64_t espacoUsadoBytes() {
  if (!cartaoOk) return 0;
  TravaBarramentoSD travaBus;
  return SD.usedBytes();
}
uint64_t espacoLivreBytes() {
  if (!cartaoOk) return 0;
  TravaBarramentoSD travaBus;
  return SD.totalBytes() - SD.usedBytes();
}

uint32_t contadorErros() { return erros; }

void travarBarramentoSPI() {
  if (mutexBarramentoSPI != nullptr) xSemaphoreTake(mutexBarramentoSPI, portMAX_DELAY);
}

void destravarBarramentoSPI() {
  if (mutexBarramentoSPI != nullptr) xSemaphoreGive(mutexBarramentoSPI);
}

bool donoAtualEhDisplay() { return donoEhDisplay; }

void marcarDonoDisplay() {
  donoEhDisplay = true;
  contadorTrocasBarramento++;
}

}  // namespace armazenamento
