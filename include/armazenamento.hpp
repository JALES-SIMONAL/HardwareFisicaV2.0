#pragma once

#include <stddef.h>
#include <stdint.h>

// Gerencia o cartão microSD: criação/escrita bufferizada de arquivos CSV de
// experimento e listagem/renomeação/exclusão de arquivos. As escritas reais
// no cartão só acontecem em processarFila() — nunca dentro de uma ISR.
namespace armazenamento {

// Chama SD.begin(SD_CS_PIN); não trava o equipamento se o cartão falhar —
// só marca cartaoDisponivel() como falso.
void init();

bool cartaoDisponivel();

bool arquivoExiste(const char* nomeComExtensao);

// Abre "<nomeSemExtensao>.csv" para escrita e já grava o cabeçalho CSV.
// Retorna false se o arquivo já existir e sobrescrever==false, ou se o
// cartão não estiver disponível.
bool abrirNovoArquivo(const char* nomeSemExtensao, bool sobrescrever);

// Enfileira uma linha (sem '\n', adicionado internamente). Não bloqueia: se
// a fila estiver cheia, descarta e incrementa o contador de erros.
void enfileirarLinha(const char* linhaCsv);
void enfileirarLinhaEmBranco();

// Drena a fila interna para o arquivo aberto, em blocos. Chamada
// periodicamente pela tarefa de armazenamento (core 0).
void processarFila();

// Descarrega o que houver no buffer e fecha o arquivo atual.
void fecharArquivoAtual();

struct InfoArquivo {
  char nome[16];
  uint32_t tamanhoBytes;
};

uint16_t listarArquivos(InfoArquivo* destino, uint16_t capacidadeDestino);
bool renomearArquivo(const char* nomeAtual, const char* novoNome);
bool excluirArquivo(const char* nome);

// Leitura sequencial para análise de dados: nunca carrega o arquivo inteiro
// na RAM, só uma linha por vez.
bool abrirParaLeitura(const char* nomeComExtensao);
bool lerProximaLinha(char* destino, size_t tamanhoDestino);
void fecharLeitura();

uint64_t espacoTotalBytes();
uint64_t espacoUsadoBytes();
uint64_t espacoLivreBytes();

// Eventos perdidos por fila cheia + falhas de escrita/abertura, acumulado.
uint32_t contadorErros();

// ---------------------------------------------------------------------
// Barramento SPI físico compartilhado com o TFT
// ---------------------------------------------------------------------
// O microSD (SD_CS_PIN) e o TFT (TFT_CS) compartilham fisicamente o mesmo
// barramento SPI — MOSI/MISO/SCK são os MESMOS pinos (só o CS muda, ver
// comentário em MAIN.HPP). O SD usa o periférico de SPI de HARDWARE do
// ESP32 (via SD.begin()/SPIClass), que precisa rotear esses pinos pela
// matriz de GPIO; o TFT (Arduino_GFX + Arduino_SWSPI, em ihm.cpp) desenha
// via bit-bang, chamando digitalWrite() diretamente nos mesmos pinos.
//
// Um periférico de hardware "prende" o roteamento do pino até algo
// religá-lo de volta a GPIO simples — por isso os dois nunca podem operar
// ao mesmo tempo nem "ao acaso": cada lado precisa (1) tomar este mutex,
// (2) reconfigurar os pinos para o seu próprio uso (SD chama
// SPI.begin(...) de novo; o TFT chama pinMode() de novo), (3) fazer sua
// operação, (4) liberar o mutex. Sem isso, o primeiro acesso ao SD depois
// do TFT (ou vice-versa) deixa o outro periférico sem resposta física no
// barramento, mesmo que o código pareça correto.
void travarBarramentoSPI();
void destravarBarramentoSPI();

}  // namespace armazenamento
