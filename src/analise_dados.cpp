#include "analise_dados.hpp"

#include <cstdio>

#include "armazenamento.hpp"

namespace analise_dados {

namespace {

EventoLido eventosCarregados[MAX_EVENTOS_REPETICAO];
uint8_t quantidadeCarregada = 0;

}  // namespace

uint8_t carregarRepeticao(const char* nomeComExtensao, uint16_t indiceRepeticao) {
  quantidadeCarregada = 0;

  if (!armazenamento::abrirParaLeitura(nomeComExtensao)) return 0;

  char linha[32];
  uint16_t repeticaoAtualIdx = 0;
  bool linhaAnteriorEraDados = false;

  while (armazenamento::lerProximaLinha(linha, sizeof(linha))) {
    if (linha[0] == '\0') {
      // Linha em branco: separa repetições. Só avança o índice se a
      // repetição anterior teve alguma linha de dados válida (evita contar
      // o cabeçalho isolado ou linhas em branco consecutivas).
      if (linhaAnteriorEraDados) repeticaoAtualIdx++;
      linhaAnteriorEraDados = false;
      if (repeticaoAtualIdx > indiceRepeticao) break;
      continue;
    }

    unsigned canal = 0;
    char estado = '\0';
    long long tempoUs = 0;
    if (std::sscanf(linha, "%u,%c,%lld", &canal, &estado, &tempoUs) == 3) {
      linhaAnteriorEraDados = true;
      if (repeticaoAtualIdx == indiceRepeticao && quantidadeCarregada < MAX_EVENTOS_REPETICAO) {
        eventosCarregados[quantidadeCarregada].canal = static_cast<uint8_t>(canal);
        eventosCarregados[quantidadeCarregada].estado = estado;
        eventosCarregados[quantidadeCarregada].tempoUs = static_cast<int64_t>(tempoUs);
        quantidadeCarregada++;
      }
    }
    // Linhas que não casam com "canal,estado,tempo_us" (ex.: cabeçalho) são ignoradas.
  }

  armazenamento::fecharLeitura();
  return quantidadeCarregada;
}

uint16_t contarRepeticoes(const char* nomeComExtensao) {
  if (!armazenamento::abrirParaLeitura(nomeComExtensao)) return 0;

  char linha[32];
  uint16_t quantidade = 0;
  bool blocoAtualTemDados = false;

  while (armazenamento::lerProximaLinha(linha, sizeof(linha))) {
    if (linha[0] == '\0') {
      if (blocoAtualTemDados) quantidade++;
      blocoAtualTemDados = false;
      continue;
    }

    unsigned canal = 0;
    char estado = '\0';
    long long tempoUs = 0;
    if (std::sscanf(linha, "%u,%c,%lld", &canal, &estado, &tempoUs) == 3) {
      blocoAtualTemDados = true;
    }
  }
  // Último bloco pode não ter linha em branco final (fim de arquivo).
  if (blocoAtualTemDados) quantidade++;

  armazenamento::fecharLeitura();
  return quantidade;
}

uint8_t quantidadeEventosCarregados() { return quantidadeCarregada; }

const EventoLido& evento(uint8_t indice) {
  static const EventoLido vazio{0, '?', 0};
  if (indice >= quantidadeCarregada) return vazio;
  return eventosCarregados[indice];
}

}  // namespace analise_dados
