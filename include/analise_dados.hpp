#pragma once

#include <stdint.h>

// Análise de um arquivo CSV já salvo: seleção de repetição e leitura de
// eventos (em blocos, nunca o arquivo inteiro na RAM) — a fonte de dados
// comum a analise_linear e analise_circular.
namespace analise_dados {

constexpr uint8_t MAX_EVENTOS_REPETICAO = 40;

struct EventoLido {
  uint8_t canal;
  char estado;  // 'H' ou 'L'
  int64_t tempoUs;
};

// Localiza o bloco de linhas da repetição "indiceRepeticao" (0-based,
// blocos separados por linha em branco no CSV) e carrega até
// MAX_EVENTOS_REPETICAO eventos dele. Retorna a quantidade carregada (0 se
// o arquivo/repetição não existir ou o cartão estiver indisponível).
uint8_t carregarRepeticao(const char* nomeComExtensao, uint16_t indiceRepeticao);

// Conta quantos blocos de repetição (separados por linha em branco, com
// pelo menos uma linha de dados válida) existem no arquivo. Não usa nem
// altera o estado de carregarRepeticao()/evento() — leitura sequencial
// própria, independente. 0 se o arquivo/cartão estiver indisponível.
uint16_t contarRepeticoes(const char* nomeComExtensao);

uint8_t quantidadeEventosCarregados();
const EventoLido& evento(uint8_t indice);

}  // namespace analise_dados
