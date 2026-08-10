#pragma once

#include <stdint.h>

// Análise de movimento circular: a partir dos eventos já carregados por
// analise_dados::carregarRepeticao() (um evento por "vão" do encoder
// passando pelo sensor), calcula a distância linear percorrida e as séries
// de velocidade/aceleração ao longo do tempo. Não lê o arquivo diretamente —
// reaproveita os eventos que analise_dados já tem em RAM.
namespace analise_circular {

// Recalcula tudo a partir dos eventos atualmente carregados em
// analise_dados. raioMetros é o raio do encoder; vaos é a quantidade de
// vãos (fendas) por volta — cada evento corresponde à passagem de um vão
// (arco percorrido = 2*pi*raioMetros/vaos). Retorna false (e zera os
// resultados) se houver menos de 2 eventos carregados ou vaos==0.
bool calcular(float raioMetros, uint16_t vaos);

// Distância linear total percorrida (soma dos arcos de todos os intervalos
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

}  // namespace analise_circular
