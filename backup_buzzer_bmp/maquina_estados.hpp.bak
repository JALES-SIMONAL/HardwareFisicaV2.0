#pragma once

#include "comandos.hpp"

// Máquina de estados explícita: dono único da navegação entre telas,
// transições, confirmações e cancelamentos. Comandos locais (encoder/tecla)
// e remotos (MQTT) chegam aqui pela mesma função — não existe lógica de
// funcionamento separada para o aplicativo.
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
  ExperimentoNomeArquivo,
  ExperimentoSobrescreverConfirmar,
  TesteCanais,

  GerenciamentoArquivos,
  ArquivoDetalhe,
  ArquivoRenomear,
  ArquivoExcluirConfirmar,
  ConexaoApp,

  AnaliseSelecionarArquivo,
  AnaliseSelecionarRepeticao,
  AnaliseEventos,
  AnaliseDistancia,
  AnaliseResultado
};

void init();

// Chamada a cada iteração da tarefa de IHM: avança a sequência de boot
// (quando aplicável), lê encoder/tecla local e redesenha só quando algo mudou.
void tick();

// Ponto de entrada único para comandos locais e remotos (MQTT).
void processarComando(const comandos::Command& cmd, comandos::Origem origem);

Tela telaAtual();

}  // namespace maquina_estados
