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

// Descarrega o que houver no buffer e fecha o arquivo atual — BLOQUEIA
// (portMAX_DELAY) até conseguir os locks do arquivo/barramento SPI. Só
// deve ser chamada de dentro da própria tarefa de armazenamento (núcleo 0,
// via processarFila()) ou em pontos onde bloquear é aceitável (ex.: boot).
// Chamar isto do núcleo 1 (IHM/Bluetooth) pode travar a tela/encoder/BLE
// inteiros se o núcleo 0 estiver no meio de uma escrita lenta no SD —
// use solicitarFechamentoArquivo()/solicitarFechamentoEExclusao() nesse
// caso.
void fecharArquivoAtual();

// Pede o fechamento do arquivo atual em segundo plano — NÃO bloqueia quem
// chama. O fechamento de fato acontece dentro de processarFila() (núcleo
// 0), na próxima vez que ela rodar (dentro de ~1ms). Use isto em vez de
// fecharArquivoAtual() sempre que quem chama roda no núcleo 1 (IHM/
// Bluetooth) e não pode ficar bloqueado esperando o SD.
void solicitarFechamentoArquivo();

// Mesmo que solicitarFechamentoArquivo(), mas também exclui
// "nomeComExtensao" logo depois que o fechamento terminar (usado ao
// cancelar um experimento: precisa fechar o arquivo de trabalho antes de
// poder excluí-lo).
void solicitarFechamentoEExclusao(const char* nomeComExtensao);

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

// Exclui todos os arquivos ".csv" (coletas) do cartão — usado tanto pelo
// item "Excluir todos" do menu local (ArquivosExcluirTodosConfirmar) quanto
// pelo comando Bluetooth equivalente ("delete_all_files"). Imagens de boot
// e qualquer outro arquivo não-".csv" ficam intocados. Retorna a
// quantidade de arquivos efetivamente removidos.
uint16_t excluirTodosArquivosCsv();

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
// O microSD (SD_CS_PIN), o TFT (TFT_CS) e o touch (TOUCH_CS) compartilham
// fisicamente o mesmo barramento SPI — MOSI/MISO/SCK são os MESMOS pinos,
// só o CS muda (ver o mapa em MAIN.HPP).
//
// MUDANÇA IMPORTANTE EM RELAÇÃO À VERSÃO ANTERIOR (branch main): lá o TFT
// era desenhado por bit-bang (Arduino_SWSPI, digitalWrite direto nos
// pinos) enquanto o cartão usava o periférico de SPI de hardware nos
// mesmos pinos. Como um periférico de hardware "prende" o roteamento do
// pino, cada troca de lado exigia reconfigurar fisicamente o barramento —
// SPI.begin()/SPI.end() de um lado, pinMode() do outro — e era daí que
// vinham as falhas de cartão daquela versão (sdSelectCard()/CMD13
// repetidos ao ler um BMP linha a linha).
//
// Nesta versão os três usam o MESMO periférico de SPI de hardware, pela
// MESMA instância SPIClass (a do TFT_eSPI, obtida com
// TFT_eSPI::getSPIinstance() — ver armazenamento.cpp e ihm.cpp), e se
// distinguem apenas pelo CS. Não existe mais "dono" do barramento para
// trocar, nenhum pino é reconfigurado em tempo de execução, e aquela
// classe de falha deixou de ser possível.
//
// O que continua sendo necessário é a EXCLUSÃO MÚTUA: o desenho roda no
// núcleo 1 e a gravação no cartão no núcleo 0, e uma operação de desenho
// do TFT_eSPI mantém o CS ativo por várias transferências seguidas —
// deixar o SD intercalar no meio disso misturaria as duas conversas no
// mesmo fio. Por isso travarBarramentoSPI()/destravarBarramentoSPI()
// permanecem, e todo acesso a um dos periféricos deve estar dentro deles.
//
// marcarDonoDisplay()/donoAtualEhDisplay() continuam existindo só para não
// quebrar a API pública; hoje não influenciam mais nada.
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
