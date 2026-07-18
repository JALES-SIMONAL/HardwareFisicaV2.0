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

File arquivoLeitura;
bool leituraAberta = false;

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

// RAII: toma o mutex do barramento e o rotea de volta para o periférico de
// SPI de hardware (a IHM pode tê-lo devolvido para GPIO simples desde o
// último acesso ao cartão) — só então é seguro fazer qualquer SD.*/File.*.
class TravaBarramentoSD {
 public:
  TravaBarramentoSD() {
    if (mutexBarramentoSPI != nullptr) xSemaphoreTake(mutexBarramentoSPI, portMAX_DELAY);
    SPI.begin(TFT_SCLK, TFT_MISO, TFT_MOSI, SD_CS_PIN);
  }
  ~TravaBarramentoSD() {
    if (mutexBarramentoSPI != nullptr) xSemaphoreGive(mutexBarramentoSPI);
  }
};

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

// Só chamar já com mutexArquivo tomado.
void fecharArquivoAtualInterno() {
  descarregarBufferInterno();
  if (arquivoAberto) arquivoAtual.close();
  arquivoAberto = false;
}

}  // namespace

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
  if (!cartaoOk) return false;

  TravaBarramentoSD travaBus;

  char caminho[32];
  snprintf(caminho, sizeof(caminho), "/%s.csv", nomeSemExtensao);

  if (SD.exists(caminho) && !sobrescrever) return false;

  xSemaphoreTake(mutexArquivo, portMAX_DELAY);

  if (arquivoAberto) fecharArquivoAtualInterno();

  arquivoAtual = SD.open(caminho, FILE_WRITE);
  const bool ok = static_cast<bool>(arquivoAtual);
  if (ok) {
    arquivoAtual.print("canal,estado,tempo_us\n");
    arquivoAberto = true;
    linhasNoBuffer = 0;
  } else {
    erros++;
  }

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

  LinhaCSV item;
  while (xQueueReceive(filaLinhas, &item, 0) == pdTRUE) {
    // Toma o barramento antes do mutexArquivo (mesma ordem em toda função
    // desta unidade) e o reivindica para o SD — o buffer só acumula em
    // RAM na maioria das iterações, mas o flush real (a cada
    // STORAGE_FLUSH_THRESHOLD linhas) precisa do barramento já roteado.
    TravaBarramentoSD travaBus;
    xSemaphoreTake(mutexArquivo, portMAX_DELAY);

    if (arquivoAberto) {
      std::strncpy(bufferFlush[linhasNoBuffer], item.texto, sizeof(bufferFlush[linhasNoBuffer]) - 1);
      bufferFlush[linhasNoBuffer][sizeof(bufferFlush[linhasNoBuffer]) - 1] = '\0';
      linhasNoBuffer++;

      if (linhasNoBuffer >= STORAGE_FLUSH_THRESHOLD) descarregarBufferInterno();
    }

    xSemaphoreGive(mutexArquivo);
  }
}

void fecharArquivoAtual() {
  TravaBarramentoSD travaBus;
  xSemaphoreTake(mutexArquivo, portMAX_DELAY);
  fecharArquivoAtualInterno();
  xSemaphoreGive(mutexArquivo);
}

uint16_t listarArquivos(InfoArquivo* destino, uint16_t capacidadeDestino) {
  if (!cartaoOk || destino == nullptr) return 0;

  TravaBarramentoSD travaBus;
  File raiz = SD.open("/");
  if (!raiz) return 0;

  uint16_t quantidade = 0;
  File entrada = raiz.openNextFile();
  while (entrada && quantidade < capacidadeDestino) {
    if (!entrada.isDirectory()) {
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

bool renomearArquivo(const char* nomeAtual, const char* novoNome) {
  if (!cartaoOk) return false;

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

}  // namespace armazenamento
