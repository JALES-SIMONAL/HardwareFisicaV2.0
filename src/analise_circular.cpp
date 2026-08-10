#include "analise_circular.hpp"

#include "analise_dados.hpp"

namespace analise_circular {

namespace {

constexpr float PI_F = 3.14159265358979323846f;

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

}  // namespace

bool calcular(float raioMetros, uint16_t vaos) {
  quantidadeVel = 0;
  quantidadeAcel = 0;
  distanciaTotal = 0.0f;

  const uint8_t qtdEventos = analise_dados::quantidadeEventosCarregados();
  if (qtdEventos < 2 || vaos == 0) return false;

  const float passoLinear = (2.0f * PI_F * raioMetros) / static_cast<float>(vaos);
  const float tempoReferenciaUs = static_cast<float>(analise_dados::evento(0).tempoUs);

  for (uint8_t i = 0; i + 1 < qtdEventos && quantidadeVel < CAPACIDADE; i++) {
    const int64_t tInicialUs = analise_dados::evento(i).tempoUs;
    const int64_t tFinalUs = analise_dados::evento(i + 1).tempoUs;
    const int64_t deltaTUs = tFinalUs - tInicialUs;
    if (deltaTUs <= 0) continue;  // ignora amostras fora de ordem/duplicadas

    const float deltaTS = static_cast<float>(deltaTUs) / 1000000.0f;
    velocidades[quantidadeVel] = passoLinear / deltaTS;

    const float tempoMedioUs = (static_cast<float>(tInicialUs) + static_cast<float>(tFinalUs)) / 2.0f;
    temposVel[quantidadeVel] = (tempoMedioUs - tempoReferenciaUs) / 1000000.0f;

    quantidadeVel++;
    distanciaTotal += passoLinear;
  }

  for (uint8_t i = 0; i + 1 < quantidadeVel && quantidadeAcel < CAPACIDADE; i++) {
    const float deltaT = temposVel[i + 1] - temposVel[i];
    if (deltaT <= 0.0f) continue;

    aceleracoes[quantidadeAcel] = (velocidades[i + 1] - velocidades[i]) / deltaT;
    temposAcel[quantidadeAcel] = (temposVel[i] + temposVel[i + 1]) / 2.0f;
    quantidadeAcel++;
  }

  return true;
}

float distanciaTotalMetros() { return distanciaTotal; }

uint8_t quantidadeVelocidades() { return quantidadeVel; }
const float* temposVelocidadeS() { return temposVel; }
const float* velocidadesMs() { return velocidades; }

uint8_t quantidadeAceleracoes() { return quantidadeAcel; }
const float* temposAceleracaoS() { return temposAcel; }
const float* aceleracoesMs2() { return aceleracoes; }

}  // namespace analise_circular
