#include "experimentos.hpp"

#include <Arduino.h>
#include <cstdio>
#include <cstring>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>

#include "MAIN.HPP"
#include "aquisicao.hpp"
#include "armazenamento.hpp"
#include "bluetooth_app.hpp"
#include "ihm.hpp"

namespace experimentos {

namespace {

constexpr const char* NOME_ARQUIVO_TRABALHO = "_tmp_exp";

enum class Fase : uint8_t { Inativo, Executando, AguardandoNome };

Fase fase = Fase::Inativo;
uint16_t repeticaoAtualNum = 1;
uint16_t totalRepeticoesNum = 1;
uint32_t eventosRepeticaoAtual = 0;
int64_t inicioRepeticaoUs = 0;

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
  const int64_t inicio = inicioRepeticaoUs;
  portEXIT_CRITICAL(&mux);

  if (!ativo) return;

  const int64_t tempoRelativoUs = tempoUs - inicio;
  char linha[24];
  snprintf(linha, sizeof(linha), "%u,%c,%lld", static_cast<unsigned>(canal1based),
           novoEstado ? 'H' : 'L', static_cast<long long>(tempoRelativoUs));
  armazenamento::enfileirarLinha(linha);
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
  uint16_t total = totalRepeticoesSolicitadas;
  if (total < 1) total = 1;
  if (total > MAX_REPETICOES) total = MAX_REPETICOES;

  if (!armazenamento::abrirNovoArquivo(NOME_ARQUIVO_TRABALHO, true)) return false;

  portENTER_CRITICAL(&mux);
  fase = Fase::Executando;
  repeticaoAtualNum = 1;
  totalRepeticoesNum = total;
  eventosRepeticaoAtual = 0;
  inicioRepeticaoUs = esp_timer_get_time();
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

  armazenamento::enfileirarLinhaEmBranco();

  if (repAtual >= repTotal) {
    armazenamento::fecharArquivoAtual();
    portENTER_CRITICAL(&mux);
    fase = Fase::AguardandoNome;
    portEXIT_CRITICAL(&mux);
    return;
  }

  portENTER_CRITICAL(&mux);
  repeticaoAtualNum++;
  eventosRepeticaoAtual = 0;
  inicioRepeticaoUs = esp_timer_get_time();
  portEXIT_CRITICAL(&mux);
}

void cancelar() {
  armazenamento::fecharArquivoAtual();
  char nomeComExtensao[24];
  snprintf(nomeComExtensao, sizeof(nomeComExtensao), "%s.csv", NOME_ARQUIVO_TRABALHO);
  armazenamento::excluirArquivo(nomeComExtensao);

  portENTER_CRITICAL(&mux);
  fase = Fase::Inativo;
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
