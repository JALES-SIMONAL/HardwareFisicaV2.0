#include "tempo.hpp"

#include <Arduino.h>
#include <cstdio>
#include <ctime>

namespace tempo {

namespace {
bool horarioDefinido = false;
uint32_t epochBase = 0;
uint32_t millisBase = 0;
}  // namespace

void definirEpoch(uint32_t epochSegundos) {
  epochBase = epochSegundos;
  millisBase = millis();
  horarioDefinido = true;
}

bool horarioConhecido() { return horarioDefinido; }

void formatarDataHoraAtual(char* saida, size_t tamanhoSaida) {
  if (!horarioDefinido || saida == nullptr || tamanhoSaida == 0) {
    if (saida != nullptr && tamanhoSaida > 0) saida[0] = '\0';
    return;
  }

  // Subtração em uint32_t: correta mesmo se millis() já deu a volta
  // (overflow) desde definirEpoch(), por aritmética modular.
  const uint32_t decorridoS = (millis() - millisBase) / 1000;
  const time_t agora = static_cast<time_t>(epochBase + decorridoS);

  struct tm horario;
  gmtime_r(&agora, &horario);

  snprintf(saida, tamanhoSaida, "%02d-%02d-%04d_%02d-%02d", horario.tm_mday, horario.tm_mon + 1,
            horario.tm_year + 1900, horario.tm_hour, horario.tm_min);
}

}  // namespace tempo
