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
  // 25 = 20 (TAMANHO_MAX_NOME_ARQUIVO, o maior nome digitável no editor de
  // texto da IHM, em maquina_estados.cpp) + 4 (".csv") + 1 ('\0'). Com 16
  // bytes (tamanho antigo), nomes gerados automaticamente com data/hora
  // (ex.: "T10082026_1430.csv", 18 caracteres) ficavam truncados bem no
  // meio da extensão — o nome guardado virava "T10082026_1430." (sem
  // "csv"), e toda operação subsequente com esse arquivo (abrir/excluir)
  // falhava por procurar um nome que não existe no cartão, mesmo o arquivo
  // de verdade existindo e tendo dados válidos.
  char nome[25];
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

// Leitura binária genérica (imagens BMP etc.) — usa um File separado de
// abrirParaLeitura()/lerProximaLinha() (que são linha-a-linha, para CSV
// texto), então os dois podem coexistir sem conflito. Nunca carrega o
// arquivo inteiro na RAM: quem chama lê em blocos/linhas do tamanho que
// precisar.
bool abrirBinarioParaLeitura(const char* nomeComExtensao);
size_t lerBinario(uint8_t* destino, size_t quantidadeBytes);
bool posicionarBinario(uint32_t offset);
void fecharBinario();

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
//
// IMPORTANTE: a reconfiguração física (passo 2) só deve acontecer quando o
// "dono" do barramento realmente MUDA (SD -> Display ou Display -> SD).
// Repetir SPI.begin()/pinMode() a cada acesso — inclusive entre acessos
// consecutivos do MESMO lado, como uma linha de BMP após a outra — chegou
// a reinicializar o periférico de SPI dezenas de vezes por segundo, o que
// na prática corrompeu o estado interno do cartão (falhas repetidas de
// CMD13/SEND_STATUS observadas ao ler um BMP linha a linha). Por isso
// marcarDonoDisplay()/donoAtualEhDisplay() existem: quem reconfigura o
// lado do display (ihm.cpp) consulta e atualiza o dono atual em vez de
// reconfigurar incondicionalmente a cada chamada; o lado do SD
// (armazenamento.cpp, em TravaBarramentoSD) faz o mesmo internamente.
void travarBarramentoSPI();
void destravarBarramentoSPI();

// true se o último lado a reconfigurar fisicamente o barramento foi o
// display. Usada por ihm.cpp para só chamar pinMode() quando o dono está
// de fato mudando de SD para Display.
bool donoAtualEhDisplay();

// Chamada por ihm.cpp IMEDIATAMENTE APÓS reconfigurar os pinos para o TFT
// (pinMode), com o mutex já tomado — registra que o display passou a ser o
// dono do barramento físico.
void marcarDonoDisplay();

// ---------------------------------------------------------------------
// Diagnóstico temporário (falha "File system is not mounted" ao iniciar um
// experimento após navegar bastante pelo menu) — ver comentário grande em
// armazenamento.cpp. Loga core/timestamp/contador de trocas/nível elétrico
// dos pinos compartilhados. Chamada tanto daqui (troca para SD) quanto de
// ihm.cpp (troca para display), para ver os dois lados da troca de dono.
// ---------------------------------------------------------------------
void logDiagnosticoBarramento(const char* contexto);

}  // namespace armazenamento
