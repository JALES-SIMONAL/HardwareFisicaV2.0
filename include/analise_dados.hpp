#pragma once

#include <stdint.h>

// Análise de um arquivo CSV já salvo: seleção de repetição, leitura de
// eventos (em blocos, nunca o arquivo inteiro na RAM), diferença de tempo
// entre dois eventos e cálculo de velocidade a partir de uma distância.
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

// delta_t_us = tempo(final) - tempo(inicial). false se os índices forem
// inválidos ou o intervalo não for positivo (dados incompletos/fora de ordem).
bool calcularIntervalo(uint8_t indiceInicial, uint8_t indiceFinal, int64_t& deltaTUsSaida);

// velocidade = distanciaMetros / (deltaTUs / 1e6). false se deltaTUs <= 0.
bool calcularVelocidade(int64_t deltaTUs, float distanciaMetros, float& velocidadeMsSaida);

}  // namespace analise_dados
