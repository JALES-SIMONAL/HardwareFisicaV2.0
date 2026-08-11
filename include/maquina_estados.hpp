#pragma once

#include "comandos.hpp"

// Máquina de estados explícita: dono único da navegação entre telas,
// transições, confirmações e cancelamentos. Comandos locais (encoder/tecla)
// e remotos (Bluetooth) chegam aqui pela mesma função — não existe lógica
// de funcionamento separada para o aplicativo.
namespace maquina_estados {

enum class Tela : uint8_t {
  Boot,
  MenuPrincipal,

  Configuracoes,
  ModoOperacao,
  Brilho,
  Volume,
  Manual,
  Sobre,
  SenhaValidar,
  AnaliseDadosToggle,

  ConfigCanais,
  ConfigCanaisTodos,
  ConfigCanaisTodosConfirmar,
  ConfigCanaisIndividualLista,
  ConfigCanaisIndividualEditar,
  ConfigCanaisIndividualConfirmar,
  ConfigCanaisVisualizar,
  ConfigCanaisRestaurarConfirmar,

  Experimentos,
  ExperimentoRepeticoes,
  ExperimentoExecucao,
  ExperimentoCancelarConfirmar,
  ExperimentoReiniciarConfirmar,
  ExperimentoNomeArquivo,
  ExperimentoSobrescreverConfirmar,
  TesteCanais,

  GerenciamentoArquivos,
  ArquivoDetalhe,
  ArquivoRenomear,
  ArquivoExcluirConfirmar,
  ArquivosExcluirTodosConfirmar,
  ArquivoDados,
  ConexaoApp,
  ConexaoAppRenomear,

  AnaliseSelecionarArquivo,
  AnaliseTipo,

  AnaliseLinearDistancia,
  AnaliseLinearResultado,
  AnaliseLinearEscolherRepeticao,
  AnaliseLinearGrafico,

  AnaliseCircularRaioVaos,
  AnaliseCircularResultado,
  AnaliseCircularEscolherRepeticao,
  AnaliseCircularGrafico
};

void init();

// Chamada a cada iteração da tarefa de IHM: avança a sequência de boot
// (quando aplicável), lê encoder/tecla local e redesenha só quando algo mudou.
void tick();

// Ponto de entrada único para comandos locais e remotos (Bluetooth).
void processarComando(const comandos::Command& cmd, comandos::Origem origem);

Tela telaAtual();

// Chamada por bluetooth_app::loop() ao detectar que um cliente conectou —
// dispara a indicação de conexão bem-sucedida (NeoPixels piscando em azul
// duas vezes + dois bipes, ver ihm::iniciarIndicacaoConexao()).
void aoConectarBluetooth();

// Chamada por bluetooth_app::loop() ao detectar que o cliente desconectou —
// limpa estado que só faz sentido com o app conectado (hoje: teste de
// canais remoto, ver testeCanaisAtivoRemoto em maquina_estados.cpp), pra
// nunca deixar os NeoPixels acesos indefinidamente se a conexão cair sem
// um "set_channel_test_active":false explícito.
void aoDesconectarBluetooth();

}  // namespace maquina_estados
