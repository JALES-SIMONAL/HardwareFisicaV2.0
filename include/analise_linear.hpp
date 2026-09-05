#pragma once

#include <stdint.h>

// Análise de movimento linear (trilho reto com pontos/sensores igualmente
// espaçados): a partir dos eventos já carregados por
// analise_dados::carregarRepeticao() (um evento por ponto passando), calcula
// a distância percorrida e as séries de velocidade/aceleração ao longo do
// tempo. Mesma ideia de analise_circular, só que sem raio/vãos — a distância
// entre dois pontos consecutivos já é a mesma para todos os intervalos.
// calcularMedia*() são exceção: essas leem o arquivo diretamente (via
// analise_dados), pois precisam percorrer TODAS as repetições, não só a
// carregada no momento.
namespace analise_linear {

// Recalcula tudo a partir dos eventos atualmente carregados em
// analise_dados. distanciaMetros é a distância física entre dois pontos
// consecutivos (igual entre todos os intervalos). Retorna false (e zera os
// resultados) se houver menos de 2 eventos carregados ou distanciaMetros<=0.
bool calcular(float distanciaMetros);

// Distância total percorrida (soma das distâncias de todos os intervalos
// usados no cálculo de velocidade).
float distanciaTotalMetros();

// Velocidade média de cada intervalo entre eventos consecutivos, associada
// ao instante do meio do intervalo (segundos, relativo ao primeiro evento
// carregado). Os dois arrays têm o mesmo tamanho, quantidadeVelocidades().
uint8_t quantidadeVelocidades();
const float* temposVelocidadeS();
const float* velocidadesMs();

// Aceleração média entre pontos de velocidade consecutivos (derivada
// discreta), associada ao instante do meio do intervalo de velocidade.
uint8_t quantidadeAceleracoes();
const float* temposAceleracaoS();
const float* aceleracoesMs2();

// Média aritmética simples de velocidadesMs()/aceleracoesMs2() (mesmos
// valores plotados nos gráficos) — 0 quando não há pontos.
float velocidadeMediaMs();
float aceleracaoMediaMs2();

// ---------------------------------------------------------------------
// Médias entre repetições do mesmo arquivo
// ---------------------------------------------------------------------

// Recarrega e recalcula CADA uma das "totalRepeticoes" repetições de
// "nomeArquivo" (via analise_dados::carregarRepeticao() + calcular()) e faz
// a média simples dos três valores-resumo entre as repetições que tiveram
// dados suficientes (cada repetição pesa igual, independente de quantos
// eventos teve). Sobrescreve o que estava carregado em analise_dados/os
// arrays por-ponto (com os da ÚLTIMA repetição processada) — quem quiser o
// valor por-ponto de uma repetição específica deve carregá-la de novo
// depois. Retorna quantas repetições entraram na média (0 se nenhuma).
uint16_t calcularMediaRepeticoes(const char* nomeArquivo, uint16_t totalRepeticoes, float distanciaMetros);

float distanciaMediaRepeticoesMetros();
float velocidadeMediaRepeticoesMs();
float aceleracaoMediaRepeticoesMs2();

// Calcula a curva média (velocidade/aceleração ponto a ponto) entre as
// "totalRepeticoes" repetições de "nomeArquivo", alinhada por ÍNDICE (não
// por tempo) e truncada no menor número de pontos entre as repetições com
// dados válidos — assume que todas partem do mesmo ponto físico do trilho,
// então o ponto i de cada repetição corresponde ao mesmo intervalo.
// Sobrescreve os mesmos arrays/valores-resumo de calcular() (os getters
// acima passam a refletir a média em vez de uma repetição só). Retorna
// false (e zera os resultados) se nenhuma repetição tiver ao menos 2
// eventos.
bool calcularMediaGrafico(const char* nomeArquivo, uint16_t totalRepeticoes, float distanciaMetros);

}  // namespace analise_linear
