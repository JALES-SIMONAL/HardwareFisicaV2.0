#include "analise_linear.hpp"

#include "analise_dados.hpp"

namespace analise_linear {

namespace {

// Mesmo limite de analise_dados::MAX_EVENTOS_REPETICAO: no pior caso (sem
// nenhum intervalo descartado por timestamp fora de ordem) há um ponto de
// velocidade a menos que eventos carregados, e um ponto de aceleração a
// menos que pontos de velocidade.
constexpr uint8_t CAPACIDADE = analise_dados::MAX_EVENTOS_REPETICAO;

float temposVel[CAPACIDADE];
float velocidades[CAPACIDADE];
uint8_t quantidadeVel = 0;

float temposAcel[CAPACIDADE];
float aceleracoes[CAPACIDADE];
uint8_t quantidadeAcel = 0;

float distanciaTotal = 0.0f;
float velocidadeMedia = 0.0f;
float aceleracaoMedia = 0.0f;

float distanciaMediaReps = 0.0f;
float velocidadeMediaReps = 0.0f;
float aceleracaoMediaReps = 0.0f;

float mediaArray(const float* valores, uint8_t quantidade) {
  if (quantidade == 0) return 0.0f;
  float soma = 0.0f;
  for (uint8_t i = 0; i < quantidade; i++) soma += valores[i];
  return soma / static_cast<float>(quantidade);
}

// Deriva aceleracoes[]/temposAcel[]/quantidadeAcel a partir do conteúdo
// ATUAL de velocidades[]/temposVel[]/quantidadeVel (derivada discreta entre
// pontos de velocidade consecutivos) — reutilizada tanto por calcular()
// quanto por calcularMediaGrafico() (que só muda o conteúdo de
// velocidades[]/temposVel[] antes de chamar isto).
void derivarAceleracao() {
  quantidadeAcel = 0;
  for (uint8_t i = 0; i + 1 < quantidadeVel && quantidadeAcel < CAPACIDADE; i++) {
    const float deltaT = temposVel[i + 1] - temposVel[i];
    if (deltaT <= 0.0f) continue;

    aceleracoes[quantidadeAcel] = (velocidades[i + 1] - velocidades[i]) / deltaT;
    temposAcel[quantidadeAcel] = (temposVel[i] + temposVel[i + 1]) / 2.0f;
    quantidadeAcel++;
  }
}

}  // namespace

bool calcular(float distanciaMetros) {
  quantidadeVel = 0;
  quantidadeAcel = 0;
  distanciaTotal = 0.0f;
  velocidadeMedia = 0.0f;
  aceleracaoMedia = 0.0f;

  const uint8_t qtdEventos = analise_dados::quantidadeEventosCarregados();
  if (qtdEventos < 2 || distanciaMetros <= 0.0f) return false;

  const float tempoReferenciaUs = static_cast<float>(analise_dados::evento(0).tempoUs);

  for (uint8_t i = 0; i + 1 < qtdEventos && quantidadeVel < CAPACIDADE; i++) {
    const int64_t tInicialUs = analise_dados::evento(i).tempoUs;
    const int64_t tFinalUs = analise_dados::evento(i + 1).tempoUs;
    const int64_t deltaTUs = tFinalUs - tInicialUs;
    if (deltaTUs <= 0) continue;  // ignora amostras fora de ordem/duplicadas

    const float deltaTS = static_cast<float>(deltaTUs) / 1000000.0f;
    velocidades[quantidadeVel] = distanciaMetros / deltaTS;

    const float tempoMedioUs = (static_cast<float>(tInicialUs) + static_cast<float>(tFinalUs)) / 2.0f;
    temposVel[quantidadeVel] = (tempoMedioUs - tempoReferenciaUs) / 1000000.0f;

    quantidadeVel++;
    distanciaTotal += distanciaMetros;
  }

  derivarAceleracao();

  velocidadeMedia = mediaArray(velocidades, quantidadeVel);
  aceleracaoMedia = mediaArray(aceleracoes, quantidadeAcel);

  return true;
}

float distanciaTotalMetros() { return distanciaTotal; }

uint8_t quantidadeVelocidades() { return quantidadeVel; }
const float* temposVelocidadeS() { return temposVel; }
const float* velocidadesMs() { return velocidades; }

uint8_t quantidadeAceleracoes() { return quantidadeAcel; }
const float* temposAceleracaoS() { return temposAcel; }
const float* aceleracoesMs2() { return aceleracoes; }

float velocidadeMediaMs() { return velocidadeMedia; }
float aceleracaoMediaMs2() { return aceleracaoMedia; }

uint16_t calcularMediaRepeticoes(const char* nomeArquivo, uint16_t totalRepeticoes, float distanciaMetros) {
  float somaDist = 0.0f;
  float somaVel = 0.0f;
  float somaAcel = 0.0f;
  uint16_t validas = 0;

  for (uint16_t rep = 0; rep < totalRepeticoes; rep++) {
    if (analise_dados::carregarRepeticao(nomeArquivo, rep) < 2) continue;
    if (!calcular(distanciaMetros)) continue;

    somaDist += distanciaTotal;
    somaVel += velocidadeMedia;
    somaAcel += aceleracaoMedia;
    validas++;
  }

  const float divisor = (validas > 0) ? static_cast<float>(validas) : 1.0f;
  distanciaMediaReps = somaDist / divisor;
  velocidadeMediaReps = somaVel / divisor;
  aceleracaoMediaReps = somaAcel / divisor;
  if (validas == 0) {
    distanciaMediaReps = 0.0f;
    velocidadeMediaReps = 0.0f;
    aceleracaoMediaReps = 0.0f;
  }

  return validas;
}

float distanciaMediaRepeticoesMetros() { return distanciaMediaReps; }
float velocidadeMediaRepeticoesMs() { return velocidadeMediaReps; }
float aceleracaoMediaRepeticoesMs2() { return aceleracaoMediaReps; }

bool calcularMediaGrafico(const char* nomeArquivo, uint16_t totalRepeticoes, float distanciaMetros) {
  // Passo 1: acha o menor número de pontos de velocidade entre as
  // repetições com dados válidos — o índice i só faz sentido comparando o
  // mesmo intervalo entre repetições, então a média não pode ir além do
  // que a repetição mais curta tem.
  uint8_t minPontos = CAPACIDADE;
  uint16_t validas = 0;
  for (uint16_t rep = 0; rep < totalRepeticoes; rep++) {
    if (analise_dados::carregarRepeticao(nomeArquivo, rep) < 2) continue;
    if (!calcular(distanciaMetros)) continue;
    if (quantidadeVel < minPontos) minPontos = quantidadeVel;
    validas++;
  }

  if (validas == 0 || minPontos == 0) {
    quantidadeVel = 0;
    quantidadeAcel = 0;
    distanciaTotal = 0.0f;
    velocidadeMedia = 0.0f;
    aceleracaoMedia = 0.0f;
    return false;
  }

  // Passo 2: acumula, por índice, os primeiros minPontos de cada repetição.
  float somaTempo[CAPACIDADE] = {};
  float somaVel[CAPACIDADE] = {};

  for (uint16_t rep = 0; rep < totalRepeticoes; rep++) {
    if (analise_dados::carregarRepeticao(nomeArquivo, rep) < 2) continue;
    if (!calcular(distanciaMetros)) continue;

    for (uint8_t i = 0; i < minPontos; i++) {
      somaTempo[i] += temposVel[i];
      somaVel[i] += velocidades[i];
    }
  }

  for (uint8_t i = 0; i < minPontos; i++) {
    temposVel[i] = somaTempo[i] / static_cast<float>(validas);
    velocidades[i] = somaVel[i] / static_cast<float>(validas);
  }
  quantidadeVel = minPontos;

  derivarAceleracao();

  // Distância de UM ponto não depende da repetição, só da distância entre
  // pontos configurada — multiplica pela quantidade de pontos médios em
  // vez de somar por repetição, igual calcular() faria para uma única
  // repetição com quantidadeVel==minPontos.
  distanciaTotal = distanciaMetros * static_cast<float>(quantidadeVel);

  velocidadeMedia = mediaArray(velocidades, quantidadeVel);
  aceleracaoMedia = mediaArray(aceleracoes, quantidadeAcel);

  return true;
}

}  // namespace analise_linear
