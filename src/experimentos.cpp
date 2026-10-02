#include "experimentos.hpp"

#include <Arduino.h>
#include <Preferences.h>
#include <cstdio>
#include <cstring>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>

#include "MAIN.HPP"
#include "aquisicao.hpp"
#include "armazenamento.hpp"
#include "bluetooth_app.hpp"
#include "ihm.hpp"
#include "tempo.hpp"

namespace experimentos {

namespace {

constexpr const char* NOME_ARQUIVO_TRABALHO = "_tmp_exp";

// Mesmo tamanho de maquina_estados::TAMANHO_MAX_NOME_ARQUIVO (limite do
// editor de nome, local ou pelo app).
constexpr uint8_t TAMANHO_MAX_NOME_SUGERIDO = 20;
char nomeSugeridoBuffer[TAMANHO_MAX_NOME_SUGERIDO + 1] = "";

// Contador persistido na NVS p/ sugerir "MEDICAOn" quando o horário ainda
// não foi recebido do app. Incrementado a cada medição finalizada, mesmo
// que o usuário troque o nome sugerido.
constexpr const char* NAMESPACE_PREFS_MEDICAO = "hwfisica_med";

uint32_t proximoNumeroMedicao() {
  Preferences prefs;
  prefs.begin(NAMESPACE_PREFS_MEDICAO, false);
  const uint32_t proximo = prefs.getUInt("prox", 1);
  prefs.putUInt("prox", proximo + 1);
  prefs.end();
  return proximo;
}

// Nome sugerido ao entrar em AguardandoNome: data/hora (DD-MM-AAAA_HH-MM)
// se o app já informou o horário atual nesta conexão, ou "MEDICAO" + número
// crescente caso contrário. Gerado UMA ÚNICA VEZ por medição finalizada
// (ver finalizarRepeticaoAtual) — chamar isto a cada publicação de estado
// BLE incrementaria o contador "MEDICAOn" a cada segundo.
void gerarNomeSugerido(char* saida, size_t tamanho) {
  if (tempo::horarioConhecido()) {
    tempo::formatarDataHoraAtual(saida, tamanho);
  } else {
    snprintf(saida, tamanho, "MEDICAO%lu", static_cast<unsigned long>(proximoNumeroMedicao()));
  }
}

enum class Fase : uint8_t { Inativo, Executando, AguardandoNome };

Fase fase = Fase::Inativo;
uint16_t repeticaoAtualNum = 1;
uint16_t totalRepeticoesNum = 1;
uint32_t eventosRepeticaoAtual = 0;
int64_t inicioRepeticaoUs = 0;

// Referência dos timestamps GRAVADOS no CSV (ver aoReceberEventoValido()):
// o primeiro evento válido de cada repetição sempre grava tempo_us=0, e os
// demais eventos da mesma repetição ficam relativos a esse primeiro
// evento — não ao instante em que a repetição começou/foi reiniciada
// (inicioRepeticaoUs), que pode ter um atraso variável e sem sentido físico
// até o primeiro movimento de verdade do usuário. inicioRepeticaoUs
// continua existindo só para o cronômetro AO VIVO da tela de execução
// (tempoDecorridoUs()), que precisa contar desde o início/reinício de
// verdade da repetição, evento nenhum ainda recebido ou não.
int64_t primeiroEventoRepeticaoUs = 0;
bool primeiroEventoRepeticaoDefinido = false;

// Linhas CSV da repetição ATUAL (ainda não finalizada), acumuladas aqui em
// vez de irem direto para armazenamento::enfileirarLinha() — só são
// entregues ao módulo de armazenamento (e, portanto, gravadas no arquivo de
// verdade) quando a repetição é finalizada (finalizarRepeticaoAtual()).
// Isso é o que torna reiniciarRepeticaoAtual() simples e seguro: como nada
// da repetição em andamento chegou a ser escrito no arquivo ainda, "reiniciar"
// só precisa esquecer este buffer — nunca precisa desfazer uma escrita já
// feita, então repetições anteriores (já finalizadas) nunca são tocadas.
constexpr uint16_t MAX_LINHAS_BUFFER_REPETICAO = 300;
char bufferLinhasRepeticao[MAX_LINHAS_BUFFER_REPETICAO][24];
uint16_t quantidadeLinhasBuffer = 0;

// Piscada de LED por evento válido: verde para transição L->H, vermelho
// para H->L (indicação visual imediata de qual canal disparou e em que
// direção, além do bipe e do registro no CSV). ledDesligarEmMs[i]==0
// significa "nada pendente". Só acessado dentro da tarefa do núcleo 0
// (aoReceberEventoValido() e atualizarLedsPiscando() rodam na mesma
// tarefa/loop em main.cpp, nunca concorrentemente) — não precisa do mux
// acima, que só protege o estado compartilhado com o núcleo 1 (IHM).
constexpr uint32_t DURACAO_PISCA_LED_MS = 150;
uint32_t ledDesligarEmMs[NUM_CHANNELS] = {};

// iniciar()/finalizarRepeticaoAtual()/cancelar() e os getters são chamados
// pela IHM (núcleo 1); aoReceberEventoValido() roda na tarefa de aquisição
// (núcleo 0). Este spinlock protege as variáveis acima — nunca envolve
// chamadas de E/S (armazenamento/bluetooth_app), só leitura/escrita das
// variáveis, para a seção crítica ficar curta.
portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;

void aoReceberEventoValido(uint8_t canal1based, bool novoEstado, int64_t tempoUs) {
  portENTER_CRITICAL(&mux);
  const bool ativo = (fase == Fase::Executando);
  if (ativo && !primeiroEventoRepeticaoDefinido) {
    // Este é o primeiro evento válido da repetição atual: vira a
    // referência (tempo_us=0 para ele mesmo, ver cálculo abaixo).
    primeiroEventoRepeticaoUs = tempoUs;
    primeiroEventoRepeticaoDefinido = true;
  }
  const int64_t referencia = primeiroEventoRepeticaoUs;
  portEXIT_CRITICAL(&mux);

  if (!ativo) return;

  const int64_t tempoRelativoUs = tempoUs - referencia;

  // Não escreve direto em armazenamento::enfileirarLinha() — a linha fica só
  // no buffer RAM da repetição atual, e só é entregue de fato ao arquivo
  // quando a repetição é finalizada (ver comentário no topo do arquivo).
  portENTER_CRITICAL(&mux);
  if (quantidadeLinhasBuffer < MAX_LINHAS_BUFFER_REPETICAO) {
    snprintf(bufferLinhasRepeticao[quantidadeLinhasBuffer], 24, "%u,%c,%lld",
             static_cast<unsigned>(canal1based), novoEstado ? 'H' : 'L',
             static_cast<long long>(tempoRelativoUs));
    quantidadeLinhasBuffer++;
  }
  portEXIT_CRITICAL(&mux);

  bluetooth_app::publicarEvento(canal1based, novoEstado ? 'H' : 'L', tempoRelativoUs);
  // Bipe curto de confirmação por evento válido (Fase 10): silencioso se
  // volume==0; curto de propósito para não atrapalhar eventos em sequência
  // rápida. ihm::beep()/tone() não usa o barramento SPI compartilhado nem
  // o canal PWM do backlight — seguro chamar daqui (núcleo 0/tarefa de
  // aquisição), sem tocar em display ou microSD.
  ihm::beep(20);

  // Pisca o LED do canal: verde para L->H, vermelho para H->L. O NeoPixel
  // (PIN_NEO) também não usa o barramento SPI compartilhado, então é
  // seguro acender daqui (núcleo 0) mesmo com a IHM desenhando no núcleo 1
  // — o único risco seria escrever no MESMO LED a partir dos dois núcleos
  // ao mesmo tempo, o que não acontece: a tela de teste de canais (única
  // outra dona dos LEDs) fica numa tela diferente da execução do
  // experimento.
  const uint16_t indiceLed = static_cast<uint16_t>(canal1based - 1);
  if (indiceLed < NUM_LEDS) {
    if (novoEstado) {
      ihm::controlarLED(indiceLed, 0, 255, 0);  // L->H: verde
    } else {
      ihm::controlarLED(indiceLed, 255, 0, 0);  // H->L: vermelho
    }
    ledDesligarEmMs[indiceLed] = millis() + DURACAO_PISCA_LED_MS;
  }

  portENTER_CRITICAL(&mux);
  eventosRepeticaoAtual++;
  portEXIT_CRITICAL(&mux);
}

}  // namespace

void init() { aquisicao::definirCallbackEventoValido(aoReceberEventoValido); }

bool iniciar(uint16_t totalRepeticoesSolicitadas) {
  // Sem isto, iniciar outra medição enquanto a anterior ainda está
  // aguardando nome (Fase::AguardandoNome) sobrescreveria silenciosamente
  // "_tmp_exp.csv" (mesmo nome de trabalho fixo), destruindo os dados da
  // medição anterior antes do usuário salvá-la com nome — cenário real
  // quando uma queda de BLE deixa a medição anterior pendurada sem nome e o
  // app (ou o usuário, localmente) tenta começar de novo sem perceber.
  // Fase::Executando entra na mesma guarda: agora que a tela de execução
  // tem "Voltar" (sai sem cancelar, experimento continua em segundo plano —
  // ver sairExperimentoExecucaoSemCancelar em maquina_estados.cpp), o
  // usuário pode voltar ao menu de Experimentos com uma medição em
  // andamento e escolher "Rodar experimento livre" de novo; sem esta
  // guarda isso reiniciaria a medição em andamento do zero, perdendo o que
  // já tinha sido coletado.
  portENTER_CRITICAL(&mux);
  const bool jaEmAndamento = (fase != Fase::Inativo);
  portEXIT_CRITICAL(&mux);
  if (jaEmAndamento) return false;

  uint16_t total = totalRepeticoesSolicitadas;
  if (total < 1) total = 1;
  if (total > MAX_REPETICOES) total = MAX_REPETICOES;

  if (!armazenamento::abrirNovoArquivo(NOME_ARQUIVO_TRABALHO, true)) return false;

  portENTER_CRITICAL(&mux);
  fase = Fase::Executando;
  repeticaoAtualNum = 1;
  totalRepeticoesNum = total;
  eventosRepeticaoAtual = 0;
  quantidadeLinhasBuffer = 0;
  inicioRepeticaoUs = esp_timer_get_time();
  primeiroEventoRepeticaoDefinido = false;
  portEXIT_CRITICAL(&mux);
  return true;
}

void finalizarRepeticaoAtual() {
  portENTER_CRITICAL(&mux);
  const bool ativo = (fase == Fase::Executando);
  const uint16_t repAtual = repeticaoAtualNum;
  const uint16_t repTotal = totalRepeticoesNum;
  portEXIT_CRITICAL(&mux);

  if (!ativo) return;

  // Só agora as linhas acumuladas da repetição entram de fato na fila de
  // gravação — até aqui elas existiam só no buffer RAM.
  portENTER_CRITICAL(&mux);
  const uint16_t quantidade = quantidadeLinhasBuffer;
  quantidadeLinhasBuffer = 0;
  portEXIT_CRITICAL(&mux);
  for (uint16_t i = 0; i < quantidade; i++) {
    armazenamento::enfileirarLinha(bufferLinhasRepeticao[i]);
  }

  armazenamento::enfileirarLinhaEmBranco();

  if (repAtual >= repTotal) {
    // Assíncrono de propósito (ver comentário grande em
    // armazenamento::solicitarFechamentoArquivo()): esta função roda no
    // núcleo 1 (IHM/Bluetooth) — chamar a versão bloqueante
    // (fecharArquivoAtual()) aqui podia travar a tela/touch/BLE inteiros
    // por tempo indeterminado sempre que o núcleo 0 estivesse no meio de
    // uma escrita lenta no cartão SD, exatamente o bug de "trava ao
    // finalizar repetição" relatado. O fechamento de verdade acontece no
    // núcleo 0 em até ~1ms; salvarComoArquivoFinal() só pode ser chamada
    // depois que o usuário digitar um nome (no mínimo alguns segundos,
    // local ou via BLE), tempo de sobra para o fechamento assíncrono
    // terminar antes do rename.
    armazenamento::solicitarFechamentoArquivo();
    portENTER_CRITICAL(&mux);
    fase = Fase::AguardandoNome;
    portEXIT_CRITICAL(&mux);
    // Fora da secao critica: gerarNomeSugerido() pode fazer I/O na NVS
    // (Preferences), que nao pode rodar com interrupcoes desabilitadas.
    gerarNomeSugerido(nomeSugeridoBuffer, sizeof(nomeSugeridoBuffer));
    return;
  }

  portENTER_CRITICAL(&mux);
  repeticaoAtualNum++;
  eventosRepeticaoAtual = 0;
  inicioRepeticaoUs = esp_timer_get_time();
  primeiroEventoRepeticaoDefinido = false;
  portEXIT_CRITICAL(&mux);
}

void reiniciarRepeticaoAtual() {
  portENTER_CRITICAL(&mux);
  const bool ativo = (fase == Fase::Executando);
  if (ativo) {
    // Nada da repetição atual chegou a ser escrito no arquivo (ver buffer no
    // topo do arquivo) — "reiniciar" é só esquecer o buffer e zerar a
    // contagem/tempo. Repetições anteriores já finalizadas não são tocadas.
    quantidadeLinhasBuffer = 0;
    eventosRepeticaoAtual = 0;
    inicioRepeticaoUs = esp_timer_get_time();
    primeiroEventoRepeticaoDefinido = false;
  }
  portEXIT_CRITICAL(&mux);
}

void cancelar() {
  // Mesmo motivo do fechamento assíncrono em finalizarRepeticaoAtual():
  // também roda no núcleo 1, também não pode ficar bloqueada esperando o
  // SD. armazenamento::solicitarFechamentoEExclusao() só fecha e exclui o
  // arquivo de trabalho quando o núcleo 0 chegar nele (~1ms depois) — o
  // usuário já vê a tela de Experimentos de volta antes disso, mas isso é
  // só uma limpeza de fundo, não algo que o usuário precise esperar ver
  // concluído.
  char nomeComExtensao[24];
  snprintf(nomeComExtensao, sizeof(nomeComExtensao), "%s.csv", NOME_ARQUIVO_TRABALHO);
  armazenamento::solicitarFechamentoEExclusao(nomeComExtensao);

  portENTER_CRITICAL(&mux);
  fase = Fase::Inativo;
  quantidadeLinhasBuffer = 0;
  portEXIT_CRITICAL(&mux);
}

void atualizarLedsPiscando() {
  const uint32_t agora = millis();
  for (uint16_t i = 0; i < NUM_CHANNELS && i < NUM_LEDS; i++) {
    if (ledDesligarEmMs[i] != 0 && agora >= ledDesligarEmMs[i]) {
      ihm::controlarLED(i, 0, 0, 0, 0);
      ledDesligarEmMs[i] = 0;
    }
  }
}

bool emAndamento() {
  portENTER_CRITICAL(&mux);
  const bool r = (fase == Fase::Executando);
  portEXIT_CRITICAL(&mux);
  return r;
}

bool aguardandoNomeArquivo() {
  portENTER_CRITICAL(&mux);
  const bool r = (fase == Fase::AguardandoNome);
  portEXIT_CRITICAL(&mux);
  return r;
}

// nomeSugeridoBuffer só é escrito por gerarNomeSugerido() (chamada uma
// única vez por medição, fora de secao critica, em finalizarRepeticaoAtual)
// e lido aqui — sem necessidade de mux, mas só faz sentido consultar
// enquanto aguardandoNomeArquivo() for true.
const char* nomeSugerido() { return nomeSugeridoBuffer; }

uint16_t repeticaoAtual() {
  portENTER_CRITICAL(&mux);
  const uint16_t r = repeticaoAtualNum;
  portEXIT_CRITICAL(&mux);
  return r;
}

uint16_t totalRepeticoes() {
  portENTER_CRITICAL(&mux);
  const uint16_t r = totalRepeticoesNum;
  portEXIT_CRITICAL(&mux);
  return r;
}

uint32_t eventosNaRepeticaoAtual() {
  portENTER_CRITICAL(&mux);
  const uint32_t r = eventosRepeticaoAtual;
  portEXIT_CRITICAL(&mux);
  return r;
}

int64_t tempoDecorridoUs() {
  portENTER_CRITICAL(&mux);
  const int64_t r = esp_timer_get_time() - inicioRepeticaoUs;
  portEXIT_CRITICAL(&mux);
  return r;
}

bool salvarComoArquivoFinal(const char* nomeSemExtensao, bool sobrescrever) {
  portENTER_CRITICAL(&mux);
  const bool podeSalvar = (fase == Fase::AguardandoNome);
  portEXIT_CRITICAL(&mux);
  if (!podeSalvar) return false;

  char nomeTrabalhoComExtensao[24];
  snprintf(nomeTrabalhoComExtensao, sizeof(nomeTrabalhoComExtensao), "%s.csv", NOME_ARQUIVO_TRABALHO);

  char nomeFinalComExtensao[24];
  snprintf(nomeFinalComExtensao, sizeof(nomeFinalComExtensao), "%s.csv", nomeSemExtensao);

  if (armazenamento::arquivoExiste(nomeFinalComExtensao)) {
    if (!sobrescrever) return false;
    armazenamento::excluirArquivo(nomeFinalComExtensao);
  }

  const bool ok = armazenamento::renomearArquivo(nomeTrabalhoComExtensao, nomeFinalComExtensao);
  if (ok) {
    portENTER_CRITICAL(&mux);
    fase = Fase::Inativo;
    portEXIT_CRITICAL(&mux);
  }
  return ok;
}

}  // namespace experimentos
