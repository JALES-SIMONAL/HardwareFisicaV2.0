#pragma once

// Autotestes internos, determinísticos, das funções puras/isoláveis do
// firmware (Fase 32 da validação geral). Não alteram o funcionamento normal
// quando ENABLE_FIRMWARE_SELF_TESTS==0 (padrão): nesse caso executar() é um
// no-op vazio, sem custo de flash/RAM relevante.
//
// Para habilitar: defina ENABLE_FIRMWARE_SELF_TESTS=1 (build_flags do
// platformio.ini ou no topo deste arquivo) e observe o resultado via
// Serial (115200 bps) logo após o boot.
#ifndef ENABLE_FIRMWARE_SELF_TESTS
#define ENABLE_FIRMWARE_SELF_TESTS 0
#endif

namespace autoteste {

// Roda todos os casos e imprime PASS/FAIL de cada um + um resumo final no
// Serial. Deve ser chamada em setup(), depois de todos os módulos terem
// sido inicializados (precisa de canais::init()/configuracoes::init() já
// aplicados). Só produz efeito quando ENABLE_FIRMWARE_SELF_TESTS==1.
void executar();

}  // namespace autoteste
