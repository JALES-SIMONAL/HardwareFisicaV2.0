#pragma once

#include <stdint.h>

// Vocabulário de comandos compartilhado entre a entrada local (encoder/tecla
// KEY) e a entrada remota (MQTT). Ambas as origens convertem sua entrada
// para o mesmo Command e chamam a mesma função da máquina de estados —
// não existe lógica de funcionamento separada para o aplicativo.
namespace comandos {

// Modo de borda considerado válido para registro de um canal de sensor.
enum class EdgeMode : uint8_t {
  Falling = 0,  // H para L
  Rising = 1,   // L para H
  Both = 2      // ambas
};

enum class CommandType : uint8_t {
  None = 0,
  Next,               // giro horário: próxima opção / incrementar valor
  Previous,           // giro anti-horário: opção anterior / decrementar valor
  Confirm,            // clique da tecla KEY / confirmar no app
  Back,               // voltar / cancelar edição
  StartExperiment,
  StopExperiment,
  CancelExperiment,
  FinishRepetition,
  SetRepetitionCount,
  SetChannelMode,
  SetAllChannelsMode,
  RestoreChannelDefaults,
  SetBrightness,
  SetVolume,
  SetOperationMode,
  SelectFile,
  RenameFile,
  DeleteFile,
  Reconnect
};

enum class Origem : uint8_t { Local, MQTT };

// Estrutura mínima e genérica: cada CommandType usa só os campos relevantes.
struct Command {
  CommandType tipo = CommandType::None;
  int32_t valor = 0;        // deltas de encoder, brilho/volume, repetições...
  uint8_t canal = 0;        // índice do canal (1..NUM_CHANNELS) quando aplicável
  EdgeMode modo = EdgeMode::Both;
  char texto[24] = "";      // nome de arquivo/valor textual quando aplicável
};

}  // namespace comandos
