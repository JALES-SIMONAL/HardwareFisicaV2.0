#pragma once

#include <stddef.h>
#include <stdint.h>

// O firmware não tem RTC próprio: sem receber a hora do app, não há como
// saber a data/hora atual (só o tempo decorrido desde o boot, via millis()).
// Este módulo guarda o instante recebido (epoch UTC) e o millis() do
// momento em que foi recebido, e projeta o instante atual a partir da
// diferença de millis() — válido até o próximo boot (nunca é persistido).
namespace tempo {

// Chamado ao processar o comando BLE "set_datetime" (enviado pelo app no
// início da conexão). O firmware não tem fuso horário: epochSegundos já
// deve vir com os campos do horário LOCAL do celular só "disfarçados" de
// UTC (ver app_controller.setDateTime()), para que formatarDataHoraAtual()
// (que usa gmtime_r) mostre a hora de parede do usuário, não UTC real.
void definirEpoch(uint32_t epochSegundos);

// true a partir da primeira definirEpoch() bem-sucedida neste boot.
bool horarioConhecido();

// Formata o instante atual como "DDMMAAAA_HHMM" (13 caracteres + '\0').
// Só deve ser chamado se horarioConhecido() for true; caso contrário,
// escreve uma string vazia.
void formatarDataHoraAtual(char* saida, size_t tamanhoSaida);

}  // namespace tempo
