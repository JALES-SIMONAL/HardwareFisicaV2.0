#include "maquina_estados.hpp"

#include <Arduino.h>
#include <WiFi.h>
#include <cstdio>
#include <cstring>
#include <qrcode.h>

#include "MAIN.HPP"
#include "analise_dados.hpp"
#include "aquisicao.hpp"
#include "armazenamento.hpp"
#include "canais.hpp"
#include "configuracoes.hpp"
#include "experimentos.hpp"
#include "ihm.hpp"
#include "layout.hpp"
#include "mqtt_app.hpp"

namespace maquina_estados {

namespace {

using comandos::Command;
using comandos::CommandType;
using comandos::EdgeMode;
using comandos::Origem;

// Confirmação Sim/Não genérica, reaproveitada por telas de canais,
// experimentos e arquivos que só precisam de uma decisão binária
// (definida mais abaixo; declarada aqui para poder ser usada antes).
void tratarConfirmacaoBinaria(const Command& cmd, void (*aoConfirmarSim)(), void (*aoConfirmarNao)());

// Nome legível da tela, usado tanto para desenhar quanto para os logs de
// diagnóstico de transição de estado (definida mais abaixo).
const char* nomeTela(Tela tela);

// ---------------------------------------------------------------------
// Sequência de boot (não-bloqueante, baseada em millis())
// ---------------------------------------------------------------------
enum class EtapaBoot : uint8_t {
  DiagnosticoDisplay,
  Logos,
  LedVermelho,
  LedAzul,
  LedVerde,
  LedApagado,
  Desenvolvedor,
  Concluido
};

EtapaBoot etapaBootAtual = EtapaBoot::DiagnosticoDisplay;
unsigned long inicioEtapaBootMs = 0;
bool etapaBootDesenhada = false;

const char* nomeEtapaBoot(EtapaBoot etapa) {
  switch (etapa) {
    case EtapaBoot::DiagnosticoDisplay: return "DIAGNOSTICO_DISPLAY";
    case EtapaBoot::Logos: return "LOGOTIPO";
    case EtapaBoot::LedVermelho: return "TESTE_LEDS_VERMELHO";
    case EtapaBoot::LedAzul: return "TESTE_LEDS_AZUL";
    case EtapaBoot::LedVerde: return "TESTE_LEDS_VERDE";
    case EtapaBoot::LedApagado: return "TESTE_LEDS_APAGADO";
    case EtapaBoot::Desenvolvedor: return "AUTOR";
    case EtapaBoot::Concluido: return "FINALIZADO";
    default: return "?";
  }
}

void avancarBoot(EtapaBoot proxima) {
  etapaBootAtual = proxima;
  inicioEtapaBootMs = millis();
  etapaBootDesenhada = false;
  Serial.printf("[STARTUP] Estado: %s\n", nomeEtapaBoot(etapaBootAtual));
}

// Os arquivos docs/Monkey Tech.bmp e docs/UFRN.bmp existem no repositório,
// mas ainda não foram convertidos para um formato embarcável (PROGMEM).
// Esta função isolada usa texto por enquanto; quando os bitmaps estiverem
// prontos, troque só o corpo desta função.
void desenharLogoMonkeyTech() {
  ihm::escreverTextoTela("Monkey Tech", layout::uiMargin(), layout::uiHeight(40),
                          0xFFFF, layout::uiFontSize(1), true);
}

void desenharLogoUFRN() {
  ihm::escreverTextoTela("UFRN", layout::uiMargin(), layout::uiHeight(70), 0xFFFF,
                          layout::uiFontSize(1), false);
}

void desenharTelaDesenvolvedor() {
  ihm::escreverTextoTela("Desenvolvido por", layout::uiMargin(), layout::uiHeight(60),
                          0xFFFF, layout::uiFontSize(1), true);
  ihm::escreverTextoTela("Wilson Simonal", layout::uiMargin(), layout::uiHeight(76),
                          0xFFE0, layout::uiFontSize(1), false);
}

// Usa ihm::controlarTodosLeds() (um único pixels.show() ao final) — nunca
// looping com ihm::controlarLED() por LED, que chamaria show() uma vez por
// LED e acenderia os 6 progressivamente em vez de simultaneamente.
void definirTodosLeds(uint8_t r, uint8_t g, uint8_t b, uint8_t brilho) {
  ihm::controlarTodosLeds(r, g, b, brilho);
}

// ---------------------------------------------------------------------
// Estado de navegação
// ---------------------------------------------------------------------
struct EstadoNavegacao {
  Tela telaAtual = Tela::Boot;
  Tela telaAnterior = Tela::Boot;
  uint8_t indiceSelecionado = 0;
  uint8_t offsetRolagem = 0;
};

EstadoNavegacao estado;
bool precisaRedesenhar = false;

constexpr const char* ITENS_MENU_PRINCIPAL[] = {
    "Configuracoes",
    "Experimentos",
    "Analise de dados",
};
constexpr uint8_t QTD_MENU_PRINCIPAL = 3;

constexpr const char* ITENS_EXPERIMENTOS[] = {
    "Rodar experimento livre",
    "Teste de canal/sensor",
    "Gerenciamento de arquivos",
    "Conexao com app",
    "Voltar",
};
constexpr uint8_t QTD_EXPERIMENTOS = 5;

constexpr const char* ITENS_CONFIGURACOES[] = {
    "Modo de operacao", "Brilho da tela",       "Volume",  "Config. canais/sensores",
    "Manual",           "Sobre",                "Voltar",
};
constexpr uint8_t QTD_CONFIGURACOES = 7;

constexpr const char* ITENS_MODO_OPERACAO[] = {
    "Controle pelo hardware",
    "Controle pelo aplicativo",
    "Voltar",
};
constexpr uint8_t QTD_MODO_OPERACAO = 3;

constexpr const char* ITENS_CONFIG_CANAIS[] = {
    "Configurar todos os canais",
    "Configurar individualmente",
    "Visualizar configuracao",
    "Restaurar config. padrao",
    "Voltar",
};
constexpr uint8_t QTD_CONFIG_CANAIS = 5;

// "H para L"=Falling(0), "L para H"=Rising(1), "Ambos"=Both(2): a ordem
// desta lista casa de propósito com os valores do enum EdgeMode.
constexpr const char* ITENS_MODO_BORDA[] = {"H para L", "L para H", "Ambos", "Voltar"};
constexpr uint8_t QTD_MODO_BORDA = 4;

// Estado temporário compartilhado pelo fluxo de configuração de canais:
// canal em edição (1..NUM_CHANNELS) e modo escolhido, pendente de confirmação.
uint8_t canalSelecionado = 1;
EdgeMode modoPendente = EdgeMode::Both;

constexpr uint8_t ITEM_ARQUIVO_RENOMEAR = 0;
constexpr uint8_t ITEM_ARQUIVO_EXCLUIR = 1;
constexpr uint8_t ITEM_ARQUIVO_VOLTAR = 2;
constexpr const char* ITENS_ARQUIVO_DETALHE[] = {"Renomear", "Excluir", "Voltar"};
constexpr uint8_t QTD_ARQUIVO_DETALHE = 3;

// ---------------------------------------------------------------------
// Títulos de menu para os logs de diagnóstico (Fase de rastreamento da
// IHM): reaproveita EXATAMENTE os mesmos arrays usados para desenhar cada
// tela — nunca uma segunda lista paralela só para a serial. Cobre as
// telas com lista estática; telas com conteúdo dinâmico (canais/arquivos/
// eventos) têm o título montado no próprio local de desenho e são
// reportadas nos logs pela tela + posição.
// ---------------------------------------------------------------------
uint8_t quantidadeOpcoesTela(Tela tela) {
  switch (tela) {
    case Tela::MenuPrincipal: return QTD_MENU_PRINCIPAL;
    case Tela::Experimentos: return QTD_EXPERIMENTOS;
    case Tela::Configuracoes: return QTD_CONFIGURACOES;
    case Tela::ModoOperacao: return QTD_MODO_OPERACAO;
    case Tela::ConfigCanais: return QTD_CONFIG_CANAIS;
    case Tela::ConfigCanaisTodos:
    case Tela::ConfigCanaisIndividualEditar:
      return QTD_MODO_BORDA;
    case Tela::ArquivoDetalhe: return QTD_ARQUIVO_DETALHE;
    case Tela::Brilho:
    case Tela::Volume:
      return 2;
    default: return 0;  // Tela de lista dinâmica ou sem seleção.
  }
}

const char* tituloOpcaoMenu(Tela tela, uint8_t indice) {
  switch (tela) {
    case Tela::MenuPrincipal:
      return (indice < QTD_MENU_PRINCIPAL) ? ITENS_MENU_PRINCIPAL[indice] : "Opcao invalida";
    case Tela::Experimentos:
      return (indice < QTD_EXPERIMENTOS) ? ITENS_EXPERIMENTOS[indice] : "Opcao invalida";
    case Tela::Configuracoes:
      return (indice < QTD_CONFIGURACOES) ? ITENS_CONFIGURACOES[indice] : "Opcao invalida";
    case Tela::ModoOperacao:
      return (indice < QTD_MODO_OPERACAO) ? ITENS_MODO_OPERACAO[indice] : "Opcao invalida";
    case Tela::ConfigCanais:
      return (indice < QTD_CONFIG_CANAIS) ? ITENS_CONFIG_CANAIS[indice] : "Opcao invalida";
    case Tela::ConfigCanaisTodos:
    case Tela::ConfigCanaisIndividualEditar:
      return (indice < QTD_MODO_BORDA) ? ITENS_MODO_BORDA[indice] : "Opcao invalida";
    case Tela::ArquivoDetalhe:
      return (indice < QTD_ARQUIVO_DETALHE) ? ITENS_ARQUIVO_DETALHE[indice] : "Opcao invalida";
    case Tela::Brilho:
    case Tela::Volume:
      return (indice == 0) ? "Valor" : "Voltar";
    default:
      return "Item";  // Tela de lista dinâmica (canais/arquivos/eventos).
  }
}

// Editor de nome de arquivo (usado tanto para salvar um experimento novo
// quanto para renomear um arquivo existente). Alfabeto: [FIM] primeiro
// (permite terminar o nome antes de preencher os 10 caracteres), depois
// espaço, letras A-Z e dígitos 0-9.
constexpr char ALFABETO_NOME[] = "\x01 ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
constexpr uint8_t MARCADOR_FIM_INDICE = 0;
constexpr uint8_t QTD_ALFABETO_NOME = sizeof(ALFABETO_NOME) - 1;
constexpr uint8_t TAMANHO_MAX_NOME_ARQUIVO = 10;

enum class ModoEdicaoNome : uint8_t { SalvarExperimento, RenomearArquivo };
ModoEdicaoNome modoEdicaoNome = ModoEdicaoNome::SalvarExperimento;

struct EstadoNomeArquivo {
  char buffer[TAMANHO_MAX_NOME_ARQUIVO + 1] = "";
  uint8_t posicaoCursor = 0;
  uint8_t indiceAlfabetoAtual = 0;
};
EstadoNomeArquivo nomeArquivo;
char nomeArquivoPendente[TAMANHO_MAX_NOME_ARQUIVO + 1] = "";

char arquivoSelecionadoNome[16] = "";

constexpr uint16_t MAX_ARQUIVOS_LISTA = 20;
armazenamento::InfoArquivo arquivosListados[MAX_ARQUIVOS_LISTA];
uint16_t quantidadeArquivosListados = 0;

void atualizarListaArquivos() {
  quantidadeArquivosListados = armazenamento::listarArquivos(arquivosListados, MAX_ARQUIVOS_LISTA);
}

char arquivoAnaliseNome[16] = "";
int16_t indiceEventoInicialAnalise = -1;
int64_t deltaTAnaliseUs = 0;
float velocidadeAnaliseMs = 0.0f;

// Estado da edição de valor (Brilho/Volume, e outras telas futuras que
// seguem o mesmo padrão "valor + Voltar").
struct EstadoEdicaoValor {
  bool emEdicao = false;
  int32_t valorTemp = 0;
};
EstadoEdicaoValor edicaoValor;

// QR Code da tela Manual, gerado uma única vez (lazy) a partir de
// configuracoes::MANUAL_URL.
constexpr uint8_t QR_VERSAO = 4;
bool qrGerado = false;
QRCode qrManual;
uint8_t qrManualBuffer[200];

bool moduloQRManual(uint8_t x, uint8_t y) { return qrcode_getModule(&qrManual, x, y); }

void prepararQRManual() {
  if (qrGerado) return;
  qrcode_initText(&qrManual, qrManualBuffer, QR_VERSAO, ECC_LOW, configuracoes::MANUAL_URL);
  qrGerado = true;
}

void navegarPara(Tela destino) {
  // Loga a opção que estava selecionada na tela de origem (a que o
  // usuário "abriu" ao confirmar) usando o mesmo array de títulos
  // desenhado na tela — antes de sobrescrever estado.telaAtual.
  Serial.printf("[MENU] Abrindo: %s\n",
                tituloOpcaoMenu(estado.telaAtual, estado.indiceSelecionado));
  Serial.printf("[ESTADO] Tela: %s -> %s\n", nomeTela(estado.telaAtual), nomeTela(destino));
  estado.telaAnterior = estado.telaAtual;
  estado.telaAtual = destino;
  estado.indiceSelecionado = 0;
  estado.offsetRolagem = 0;
  edicaoValor.emEdicao = false;
  precisaRedesenhar = true;
}

void voltarUmNivel() {
  if (estado.telaAtual == Tela::TesteCanais) {
    definirTodosLeds(0, 0, 0, 0);
  }
  Serial.println("[MENU] Abrindo: Voltar");
  Serial.printf("[ESTADO] Tela: %s -> %s\n", nomeTela(estado.telaAtual), nomeTela(estado.telaAnterior));
  estado.telaAtual = estado.telaAnterior;
  estado.indiceSelecionado = 0;
  estado.offsetRolagem = 0;
  edicaoValor.emEdicao = false;
  precisaRedesenhar = true;
}

const char* nomeTela(Tela tela) {
  switch (tela) {
    case Tela::Boot: return "Boot";
    case Tela::MenuPrincipal: return "Menu Principal";
    case Tela::Configuracoes: return "Configuracoes";
    case Tela::ModoOperacao: return "Modo de operacao";
    case Tela::Brilho: return "Brilho";
    case Tela::Volume: return "Volume";
    case Tela::Manual: return "Manual";
    case Tela::Sobre: return "Sobre";
    case Tela::ConfigCanais: return "Config. canais";
    case Tela::ConfigCanaisTodos: return "Config. todos";
    case Tela::ConfigCanaisTodosConfirmar: return "Confirmar";
    case Tela::ConfigCanaisIndividualLista: return "Config. individual";
    case Tela::ConfigCanaisIndividualEditar: return "Editar canal";
    case Tela::ConfigCanaisIndividualConfirmar: return "Confirmar";
    case Tela::ConfigCanaisVisualizar: return "Visualizar config.";
    case Tela::ConfigCanaisRestaurarConfirmar: return "Restaurar padrao";
    case Tela::Experimentos: return "Experimentos";
    case Tela::ExperimentoRepeticoes: return "Repeticoes";
    case Tela::ExperimentoExecucao: return "Experimento";
    case Tela::ExperimentoCancelarConfirmar: return "Cancelar?";
    case Tela::ExperimentoNomeArquivo: return "Nome do arquivo";
    case Tela::ExperimentoSobrescreverConfirmar: return "Sobrescrever?";
    case Tela::TesteCanais: return "Teste de canais";
    case Tela::GerenciamentoArquivos: return "Arquivos";
    case Tela::ArquivoDetalhe: return "Detalhe do arquivo";
    case Tela::ArquivoRenomear: return "Renomear";
    case Tela::ArquivoExcluirConfirmar: return "Excluir?";
    case Tela::ConexaoApp: return "Conexao com app";
    case Tela::AnaliseSelecionarArquivo: return "Selecionar arquivo";
    case Tela::AnaliseSelecionarRepeticao: return "Selecionar repeticao";
    case Tela::AnaliseEventos: return "Eventos";
    case Tela::AnaliseDistancia: return "Distancia";
    case Tela::AnaliseResultado: return "Resultado";
    default: return "Tela";
  }
}

void tratarMenuPrincipal(const Command& cmd) {
  switch (cmd.tipo) {
    case CommandType::Next:
      estado.indiceSelecionado = (estado.indiceSelecionado + 1) % QTD_MENU_PRINCIPAL;
      precisaRedesenhar = true;
      break;
    case CommandType::Previous:
      estado.indiceSelecionado =
          (estado.indiceSelecionado == 0) ? QTD_MENU_PRINCIPAL - 1 : estado.indiceSelecionado - 1;
      precisaRedesenhar = true;
      break;
    case CommandType::Confirm:
      switch (estado.indiceSelecionado) {
        case 0: navegarPara(Tela::Configuracoes); break;
        case 1: navegarPara(Tela::Experimentos); break;
        case 2: navegarPara(Tela::AnaliseSelecionarArquivo); break;
        default: break;
      }
      break;
    default:
      break;
  }
}

void tratarExperimentos(const Command& cmd) {
  switch (cmd.tipo) {
    case CommandType::Next:
      estado.indiceSelecionado = (estado.indiceSelecionado + 1) % QTD_EXPERIMENTOS;
      precisaRedesenhar = true;
      break;
    case CommandType::Previous:
      estado.indiceSelecionado =
          (estado.indiceSelecionado == 0) ? QTD_EXPERIMENTOS - 1 : estado.indiceSelecionado - 1;
      precisaRedesenhar = true;
      break;
    case CommandType::Confirm:
      switch (estado.indiceSelecionado) {
        case 0:
          navegarPara(Tela::ExperimentoRepeticoes);
          edicaoValor.valorTemp = 1;
          break;
        case 1: navegarPara(Tela::TesteCanais); break;
        case 2: navegarPara(Tela::GerenciamentoArquivos); break;
        case 3: navegarPara(Tela::ConexaoApp); break;
        case 4: voltarUmNivel(); break;
        default: break;
      }
      break;
    default:
      break;
  }
}

void tratarExperimentoRepeticoes(const Command& cmd) {
  switch (cmd.tipo) {
    case CommandType::Next:
      if (edicaoValor.valorTemp < MAX_REPETICOES) edicaoValor.valorTemp++;
      precisaRedesenhar = true;
      break;
    case CommandType::Previous:
      if (edicaoValor.valorTemp > 1) edicaoValor.valorTemp--;
      precisaRedesenhar = true;
      break;
    case CommandType::Confirm: {
      const uint16_t total = static_cast<uint16_t>(edicaoValor.valorTemp);
      Serial.printf("[EXPERIMENTO] Iniciando experimento (%u repeticoes)\n",
                    static_cast<unsigned>(total));
      if (experimentos::iniciar(total)) {
        navegarPara(Tela::ExperimentoExecucao);
      } else {
        Serial.println("[EXPERIMENTO] Falha ao iniciar (SD indisponivel?)");
        // Cartão indisponível ou falha ao abrir o arquivo de trabalho: não
        // há como coletar sem armazenamento, então volta ao menu.
        voltarUmNivel();
      }
      break;
    }
    case CommandType::Back:
      voltarUmNivel();
      break;
    default:
      break;
  }
}

void tratarExperimentoExecucao(const Command& cmd) {
  switch (cmd.tipo) {
    case CommandType::Next:
    case CommandType::Previous:
      estado.indiceSelecionado = (estado.indiceSelecionado == 0) ? 1 : 0;
      precisaRedesenhar = true;
      break;
    case CommandType::Confirm:
      if (estado.indiceSelecionado == 0) {
        Serial.printf("[EXPERIMENTO] Finalizando repeticao %u/%u\n",
                      static_cast<unsigned>(experimentos::repeticaoAtual()),
                      static_cast<unsigned>(experimentos::totalRepeticoes()));
        experimentos::finalizarRepeticaoAtual();
        if (experimentos::aguardandoNomeArquivo()) {
          modoEdicaoNome = ModoEdicaoNome::SalvarExperimento;
          nomeArquivo.buffer[0] = '\0';
          nomeArquivo.posicaoCursor = 0;
          nomeArquivo.indiceAlfabetoAtual = 0;
          navegarPara(Tela::ExperimentoNomeArquivo);
        } else {
          precisaRedesenhar = true;
        }
      } else {
        navegarPara(Tela::ExperimentoCancelarConfirmar);
      }
      break;
    default:
      break;
  }
}

void confirmarCancelarExperimentoSim() {
  Serial.println("[EXPERIMENTO] Cancelado pelo usuario");
  experimentos::cancelar();
  navegarPara(Tela::Experimentos);
}
void confirmarCancelarExperimentoNao() { voltarUmNivel(); }

void tratarExperimentoCancelarConfirmar(const Command& cmd) {
  tratarConfirmacaoBinaria(cmd, confirmarCancelarExperimentoSim, confirmarCancelarExperimentoNao);
}

void removerEspacosFinais(char* texto) {
  int comprimento = static_cast<int>(std::strlen(texto));
  while (comprimento > 0 && texto[comprimento - 1] == ' ') {
    texto[comprimento - 1] = '\0';
    comprimento--;
  }
}

void finalizarEdicaoNomeArquivo() {
  char nomeFinal[TAMANHO_MAX_NOME_ARQUIVO + 1];
  std::strncpy(nomeFinal, nomeArquivo.buffer, sizeof(nomeFinal) - 1);
  nomeFinal[sizeof(nomeFinal) - 1] = '\0';
  removerEspacosFinais(nomeFinal);

  if (std::strlen(nomeFinal) == 0) {
    // Nome vazio não é permitido: mantém o usuário na edição.
    ihm::beep(150);
    return;
  }

  if (modoEdicaoNome == ModoEdicaoNome::SalvarExperimento) {
    std::strncpy(nomeArquivoPendente, nomeFinal, sizeof(nomeArquivoPendente) - 1);
    nomeArquivoPendente[sizeof(nomeArquivoPendente) - 1] = '\0';

    char nomeComExtensao[TAMANHO_MAX_NOME_ARQUIVO + 5];
    snprintf(nomeComExtensao, sizeof(nomeComExtensao), "%s.csv", nomeFinal);

    if (armazenamento::arquivoExiste(nomeComExtensao)) {
      navegarPara(Tela::ExperimentoSobrescreverConfirmar);
    } else {
      experimentos::salvarComoArquivoFinal(nomeFinal, false);
      navegarPara(Tela::Experimentos);
    }
    return;
  }

  // ModoEdicaoNome::RenomearArquivo
  char nomeComExtensao[TAMANHO_MAX_NOME_ARQUIVO + 5];
  snprintf(nomeComExtensao, sizeof(nomeComExtensao), "%s.csv", nomeFinal);
  if (armazenamento::renomearArquivo(arquivoSelecionadoNome, nomeComExtensao)) {
    navegarPara(Tela::GerenciamentoArquivos);
  } else {
    // Já existe um arquivo com esse nome: nunca sobrescreve silenciosamente
    // no fluxo de renomear — o usuário tenta outro nome.
    ihm::beep(150);
  }
}

void tratarEdicaoNomeArquivo(const Command& cmd) {
  switch (cmd.tipo) {
    case CommandType::Next:
      nomeArquivo.indiceAlfabetoAtual = (nomeArquivo.indiceAlfabetoAtual + 1) % QTD_ALFABETO_NOME;
      precisaRedesenhar = true;
      break;
    case CommandType::Previous:
      nomeArquivo.indiceAlfabetoAtual = (nomeArquivo.indiceAlfabetoAtual == 0)
                                             ? QTD_ALFABETO_NOME - 1
                                             : nomeArquivo.indiceAlfabetoAtual - 1;
      precisaRedesenhar = true;
      break;
    case CommandType::Confirm: {
      const bool ehFim = (nomeArquivo.indiceAlfabetoAtual == MARCADOR_FIM_INDICE);

      if (!ehFim && nomeArquivo.posicaoCursor < TAMANHO_MAX_NOME_ARQUIVO) {
        nomeArquivo.buffer[nomeArquivo.posicaoCursor] = ALFABETO_NOME[nomeArquivo.indiceAlfabetoAtual];
        nomeArquivo.posicaoCursor++;
        nomeArquivo.buffer[nomeArquivo.posicaoCursor] = '\0';
        nomeArquivo.indiceAlfabetoAtual = 0;
        precisaRedesenhar = true;
      }

      if (ehFim || nomeArquivo.posicaoCursor >= TAMANHO_MAX_NOME_ARQUIVO) {
        finalizarEdicaoNomeArquivo();
      }
      break;
    }
    default:
      break;
  }
}

void confirmarSobrescreverExperimentoSim() {
  experimentos::salvarComoArquivoFinal(nomeArquivoPendente, true);
  navegarPara(Tela::Experimentos);
}
void confirmarSobrescreverExperimentoNao() { voltarUmNivel(); }

void tratarExperimentoSobrescreverConfirmar(const Command& cmd) {
  tratarConfirmacaoBinaria(cmd, confirmarSobrescreverExperimentoSim, confirmarSobrescreverExperimentoNao);
}

void tratarGerenciamentoArquivos(const Command& cmd) {
  if (quantidadeArquivosListados == 0) {
    if (cmd.tipo == CommandType::Confirm || cmd.tipo == CommandType::Back) voltarUmNivel();
    return;
  }

  const uint16_t qtd = quantidadeArquivosListados + 1;  // +1 = "Voltar"
  switch (cmd.tipo) {
    case CommandType::Next:
      estado.indiceSelecionado = (estado.indiceSelecionado + 1) % qtd;
      precisaRedesenhar = true;
      break;
    case CommandType::Previous:
      estado.indiceSelecionado = (estado.indiceSelecionado == 0) ? qtd - 1 : estado.indiceSelecionado - 1;
      precisaRedesenhar = true;
      break;
    case CommandType::Confirm:
      if (estado.indiceSelecionado == quantidadeArquivosListados) {
        voltarUmNivel();
      } else {
        std::strncpy(arquivoSelecionadoNome, arquivosListados[estado.indiceSelecionado].nome,
                     sizeof(arquivoSelecionadoNome) - 1);
        arquivoSelecionadoNome[sizeof(arquivoSelecionadoNome) - 1] = '\0';
        navegarPara(Tela::ArquivoDetalhe);
      }
      break;
    default:
      break;
  }
}

void tratarArquivoDetalhe(const Command& cmd) {
  switch (cmd.tipo) {
    case CommandType::Next:
      estado.indiceSelecionado = (estado.indiceSelecionado + 1) % QTD_ARQUIVO_DETALHE;
      precisaRedesenhar = true;
      break;
    case CommandType::Previous:
      estado.indiceSelecionado =
          (estado.indiceSelecionado == 0) ? QTD_ARQUIVO_DETALHE - 1 : estado.indiceSelecionado - 1;
      precisaRedesenhar = true;
      break;
    case CommandType::Confirm:
      if (estado.indiceSelecionado == ITEM_ARQUIVO_RENOMEAR) {
        modoEdicaoNome = ModoEdicaoNome::RenomearArquivo;
        std::strncpy(nomeArquivo.buffer, arquivoSelecionadoNome, sizeof(nomeArquivo.buffer) - 1);
        nomeArquivo.buffer[sizeof(nomeArquivo.buffer) - 1] = '\0';
        const size_t comprimento = std::strlen(nomeArquivo.buffer);
        if (comprimento > 4 && std::strcmp(nomeArquivo.buffer + comprimento - 4, ".csv") == 0) {
          nomeArquivo.buffer[comprimento - 4] = '\0';
        }
        nomeArquivo.posicaoCursor = static_cast<uint8_t>(std::strlen(nomeArquivo.buffer));
        nomeArquivo.indiceAlfabetoAtual = 0;
        navegarPara(Tela::ArquivoRenomear);
      } else if (estado.indiceSelecionado == ITEM_ARQUIVO_EXCLUIR) {
        navegarPara(Tela::ArquivoExcluirConfirmar);
      } else if (estado.indiceSelecionado == ITEM_ARQUIVO_VOLTAR) {
        voltarUmNivel();
      }
      break;
    default:
      break;
  }
}

void confirmarExcluirArquivoSim() {
  armazenamento::excluirArquivo(arquivoSelecionadoNome);
  navegarPara(Tela::GerenciamentoArquivos);
}
void confirmarExcluirArquivoNao() { voltarUmNivel(); }

void tratarArquivoExcluirConfirmar(const Command& cmd) {
  tratarConfirmacaoBinaria(cmd, confirmarExcluirArquivoSim, confirmarExcluirArquivoNao);
}

void tratarConexaoApp(const Command& cmd) {
  constexpr uint8_t QTD_CONEXAO_APP = 6;
  constexpr uint8_t ITEM_RECONECTAR = 4;
  constexpr uint8_t ITEM_VOLTAR = 5;

  switch (cmd.tipo) {
    case CommandType::Next:
      estado.indiceSelecionado = (estado.indiceSelecionado + 1) % QTD_CONEXAO_APP;
      precisaRedesenhar = true;
      break;
    case CommandType::Previous:
      estado.indiceSelecionado =
          (estado.indiceSelecionado == 0) ? QTD_CONEXAO_APP - 1 : estado.indiceSelecionado - 1;
      precisaRedesenhar = true;
      break;
    case CommandType::Confirm:
      if (estado.indiceSelecionado == ITEM_RECONECTAR) {
        mqtt_app::reconectar();
        precisaRedesenhar = true;
      } else if (estado.indiceSelecionado == ITEM_VOLTAR) {
        voltarUmNivel();
      }
      break;
    default:
      break;
  }
}

void tratarAnaliseSelecionarArquivo(const Command& cmd) {
  if (quantidadeArquivosListados == 0) {
    if (cmd.tipo == CommandType::Confirm || cmd.tipo == CommandType::Back) voltarUmNivel();
    return;
  }

  const uint16_t qtd = quantidadeArquivosListados + 1;
  switch (cmd.tipo) {
    case CommandType::Next:
      estado.indiceSelecionado = (estado.indiceSelecionado + 1) % qtd;
      precisaRedesenhar = true;
      break;
    case CommandType::Previous:
      estado.indiceSelecionado = (estado.indiceSelecionado == 0) ? qtd - 1 : estado.indiceSelecionado - 1;
      precisaRedesenhar = true;
      break;
    case CommandType::Confirm:
      if (estado.indiceSelecionado == quantidadeArquivosListados) {
        voltarUmNivel();
      } else {
        std::strncpy(arquivoAnaliseNome, arquivosListados[estado.indiceSelecionado].nome,
                     sizeof(arquivoAnaliseNome) - 1);
        arquivoAnaliseNome[sizeof(arquivoAnaliseNome) - 1] = '\0';
        navegarPara(Tela::AnaliseSelecionarRepeticao);
        edicaoValor.valorTemp = 0;
      }
      break;
    default:
      break;
  }
}

void tratarAnaliseSelecionarRepeticao(const Command& cmd) {
  constexpr int32_t MAX_REPETICAO_ANALISE = 999;
  switch (cmd.tipo) {
    case CommandType::Next:
      if (edicaoValor.valorTemp < MAX_REPETICAO_ANALISE) edicaoValor.valorTemp++;
      precisaRedesenhar = true;
      break;
    case CommandType::Previous:
      if (edicaoValor.valorTemp > 0) edicaoValor.valorTemp--;
      precisaRedesenhar = true;
      break;
    case CommandType::Confirm: {
      const uint8_t qtd = analise_dados::carregarRepeticao(arquivoAnaliseNome,
                                                             static_cast<uint16_t>(edicaoValor.valorTemp));
      if (qtd > 0) {
        indiceEventoInicialAnalise = -1;
        navegarPara(Tela::AnaliseEventos);
      } else {
        ihm::beep(150);
      }
      break;
    }
    case CommandType::Back:
      voltarUmNivel();
      break;
    default:
      break;
  }
}

void tratarAnaliseEventos(const Command& cmd) {
  const uint8_t qtdEventos = analise_dados::quantidadeEventosCarregados();
  const uint16_t qtd = static_cast<uint16_t>(qtdEventos) + 1;

  switch (cmd.tipo) {
    case CommandType::Next:
      estado.indiceSelecionado = (estado.indiceSelecionado + 1) % qtd;
      precisaRedesenhar = true;
      break;
    case CommandType::Previous:
      estado.indiceSelecionado = (estado.indiceSelecionado == 0) ? qtd - 1 : estado.indiceSelecionado - 1;
      precisaRedesenhar = true;
      break;
    case CommandType::Confirm:
      if (estado.indiceSelecionado == qtdEventos) {
        indiceEventoInicialAnalise = -1;
        voltarUmNivel();
        break;
      }
      if (indiceEventoInicialAnalise < 0) {
        indiceEventoInicialAnalise = static_cast<int16_t>(estado.indiceSelecionado);
        precisaRedesenhar = true;
      } else {
        int64_t delta = 0;
        if (analise_dados::calcularIntervalo(static_cast<uint8_t>(indiceEventoInicialAnalise),
                                              static_cast<uint8_t>(estado.indiceSelecionado), delta)) {
          deltaTAnaliseUs = delta;
          indiceEventoInicialAnalise = -1;
          navegarPara(Tela::AnaliseDistancia);
          edicaoValor.valorTemp = 100;
        } else {
          ihm::beep(150);
          indiceEventoInicialAnalise = -1;
          precisaRedesenhar = true;
        }
      }
      break;
    default:
      break;
  }
}

void tratarAnaliseDistancia(const Command& cmd) {
  constexpr int32_t DISTANCIA_MIN_CM = 1;
  constexpr int32_t DISTANCIA_MAX_CM = 2000;
  switch (cmd.tipo) {
    case CommandType::Next:
      if (edicaoValor.valorTemp < DISTANCIA_MAX_CM) edicaoValor.valorTemp++;
      precisaRedesenhar = true;
      break;
    case CommandType::Previous:
      if (edicaoValor.valorTemp > DISTANCIA_MIN_CM) edicaoValor.valorTemp--;
      precisaRedesenhar = true;
      break;
    case CommandType::Confirm: {
      const float distanciaMetros = static_cast<float>(edicaoValor.valorTemp) / 100.0f;
      if (analise_dados::calcularVelocidade(deltaTAnaliseUs, distanciaMetros, velocidadeAnaliseMs)) {
        mqtt_app::publicarResultadoAnalise(deltaTAnaliseUs, velocidadeAnaliseMs);
      } else {
        velocidadeAnaliseMs = 0.0f;
      }
      navegarPara(Tela::AnaliseResultado);
      break;
    }
    case CommandType::Back:
      voltarUmNivel();
      break;
    default:
      break;
  }
}

// Telas ainda não implementadas nas próximas etapas: mostram um aviso e
// voltam à tela anterior com Confirm ou Back, para permitir navegar/testar
// o esqueleto sem travar em telas mortas. Também reutilizada por telas só
// de leitura (Manual, Sobre), cujo único comando válido é "voltar".
void tratarTelaEmConstrucao(const Command& cmd) {
  if (cmd.tipo == CommandType::Confirm || cmd.tipo == CommandType::Back) {
    voltarUmNivel();
  }
}

void tratarConfiguracoes(const Command& cmd) {
  switch (cmd.tipo) {
    case CommandType::Next:
      estado.indiceSelecionado = (estado.indiceSelecionado + 1) % QTD_CONFIGURACOES;
      precisaRedesenhar = true;
      break;
    case CommandType::Previous:
      estado.indiceSelecionado = (estado.indiceSelecionado == 0) ? QTD_CONFIGURACOES - 1
                                                                   : estado.indiceSelecionado - 1;
      precisaRedesenhar = true;
      break;
    case CommandType::Confirm:
      switch (estado.indiceSelecionado) {
        case 0: navegarPara(Tela::ModoOperacao); break;
        case 1: navegarPara(Tela::Brilho); break;
        case 2: navegarPara(Tela::Volume); break;
        case 3: navegarPara(Tela::ConfigCanais); break;
        case 4: navegarPara(Tela::Manual); break;
        case 5: navegarPara(Tela::Sobre); break;
        case 6: voltarUmNivel(); break;
        default: break;
      }
      break;
    default:
      break;
  }
}

void tratarModoOperacao(const Command& cmd) {
  switch (cmd.tipo) {
    case CommandType::Next:
      estado.indiceSelecionado = (estado.indiceSelecionado + 1) % QTD_MODO_OPERACAO;
      precisaRedesenhar = true;
      break;
    case CommandType::Previous:
      estado.indiceSelecionado = (estado.indiceSelecionado == 0) ? QTD_MODO_OPERACAO - 1
                                                                   : estado.indiceSelecionado - 1;
      precisaRedesenhar = true;
      break;
    case CommandType::Confirm:
      switch (estado.indiceSelecionado) {
        case 0:
          Serial.println("[ESTADO] Modo de operacao: Hardware");
          configuracoes::definirModoOperacao(configuracoes::ModoOperacao::Hardware);
          voltarUmNivel();
          break;
        case 1:
          Serial.println("[ESTADO] Modo de operacao: App");
          configuracoes::definirModoOperacao(configuracoes::ModoOperacao::App);
          voltarUmNivel();
          break;
        case 2:
          voltarUmNivel();
          break;
        default:
          break;
      }
      break;
    default:
      break;
  }
}

// Padrão comum a Brilho/Volume: item 0 = valor (clicar entra em edição),
// item 1 = Voltar. Durante a edição, giro altera um valor temporário
// (aplicado só como preview) e o clique confirma e persiste.
void tratarBrilho(const Command& cmd) {
  constexpr uint8_t ITEM_VALOR = 0;
  constexpr uint8_t ITEM_VOLTAR = 1;

  if (!edicaoValor.emEdicao) {
    switch (cmd.tipo) {
      case CommandType::Next:
      case CommandType::Previous:
        estado.indiceSelecionado = (estado.indiceSelecionado == ITEM_VALOR) ? ITEM_VOLTAR : ITEM_VALOR;
        precisaRedesenhar = true;
        break;
      case CommandType::Confirm:
        if (estado.indiceSelecionado == ITEM_VALOR) {
          Serial.println("[ESTADO] Entrando em edicao: Brilho");
          edicaoValor.emEdicao = true;
          edicaoValor.valorTemp = configuracoes::brilho();
          precisaRedesenhar = true;
        } else {
          voltarUmNivel();
        }
        break;
      default:
        break;
    }
    return;
  }

  switch (cmd.tipo) {
    case CommandType::Next:
      if (edicaoValor.valorTemp < configuracoes::NIVEL_MAXIMO) edicaoValor.valorTemp++;
      ihm::setBrilho(static_cast<uint8_t>(edicaoValor.valorTemp));
      precisaRedesenhar = true;
      break;
    case CommandType::Previous:
      if (edicaoValor.valorTemp > configuracoes::NIVEL_MINIMO) edicaoValor.valorTemp--;
      ihm::setBrilho(static_cast<uint8_t>(edicaoValor.valorTemp));
      precisaRedesenhar = true;
      break;
    case CommandType::Confirm:
      configuracoes::definirBrilho(static_cast<uint8_t>(edicaoValor.valorTemp));
      Serial.printf("[ESTADO] Saindo da edicao: Brilho (valor=%ld)\n",
                    static_cast<long>(edicaoValor.valorTemp));
      edicaoValor.emEdicao = false;
      precisaRedesenhar = true;
      break;
    default:
      break;
  }
}

void tratarVolume(const Command& cmd) {
  constexpr uint8_t ITEM_VALOR = 0;
  constexpr uint8_t ITEM_VOLTAR = 1;

  if (!edicaoValor.emEdicao) {
    switch (cmd.tipo) {
      case CommandType::Next:
      case CommandType::Previous:
        estado.indiceSelecionado = (estado.indiceSelecionado == ITEM_VALOR) ? ITEM_VOLTAR : ITEM_VALOR;
        precisaRedesenhar = true;
        break;
      case CommandType::Confirm:
        if (estado.indiceSelecionado == ITEM_VALOR) {
          Serial.println("[ESTADO] Entrando em edicao: Volume");
          edicaoValor.emEdicao = true;
          edicaoValor.valorTemp = configuracoes::volume();
          precisaRedesenhar = true;
        } else {
          voltarUmNivel();
        }
        break;
      default:
        break;
    }
    return;
  }

  switch (cmd.tipo) {
    case CommandType::Next:
      if (edicaoValor.valorTemp < configuracoes::NIVEL_MAXIMO) edicaoValor.valorTemp++;
      precisaRedesenhar = true;
      break;
    case CommandType::Previous:
      if (edicaoValor.valorTemp > configuracoes::NIVEL_MINIMO) edicaoValor.valorTemp--;
      precisaRedesenhar = true;
      break;
    case CommandType::Confirm:
      configuracoes::definirVolume(static_cast<uint8_t>(edicaoValor.valorTemp));
      Serial.printf("[ESTADO] Saindo da edicao: Volume (valor=%ld)\n",
                    static_cast<long>(edicaoValor.valorTemp));
      ihm::beep(40);
      edicaoValor.emEdicao = false;
      precisaRedesenhar = true;
      break;
    default:
      break;
  }
}

// Confirmação Sim/Não genérica, reaproveitada por todo o fluxo de canais
// (e por outras telas futuras que só precisam de uma decisão binária).
void tratarConfirmacaoBinaria(const Command& cmd, void (*aoConfirmarSim)(), void (*aoConfirmarNao)()) {
  switch (cmd.tipo) {
    case CommandType::Next:
    case CommandType::Previous:
      estado.indiceSelecionado = (estado.indiceSelecionado == 0) ? 1 : 0;
      precisaRedesenhar = true;
      break;
    case CommandType::Confirm:
      if (estado.indiceSelecionado == 0) {
        if (aoConfirmarSim) aoConfirmarSim();
      } else {
        if (aoConfirmarNao) aoConfirmarNao();
      }
      break;
    default:
      break;
  }
}

void tratarConfigCanais(const Command& cmd) {
  switch (cmd.tipo) {
    case CommandType::Next:
      estado.indiceSelecionado = (estado.indiceSelecionado + 1) % QTD_CONFIG_CANAIS;
      precisaRedesenhar = true;
      break;
    case CommandType::Previous:
      estado.indiceSelecionado =
          (estado.indiceSelecionado == 0) ? QTD_CONFIG_CANAIS - 1 : estado.indiceSelecionado - 1;
      precisaRedesenhar = true;
      break;
    case CommandType::Confirm:
      switch (estado.indiceSelecionado) {
        case 0: navegarPara(Tela::ConfigCanaisTodos); break;
        case 1: navegarPara(Tela::ConfigCanaisIndividualLista); break;
        case 2: navegarPara(Tela::ConfigCanaisVisualizar); break;
        case 3: navegarPara(Tela::ConfigCanaisRestaurarConfirmar); break;
        case 4: voltarUmNivel(); break;
        default: break;
      }
      break;
    default:
      break;
  }
}

void tratarConfigCanaisTodos(const Command& cmd) {
  switch (cmd.tipo) {
    case CommandType::Next:
      estado.indiceSelecionado = (estado.indiceSelecionado + 1) % QTD_MODO_BORDA;
      precisaRedesenhar = true;
      break;
    case CommandType::Previous:
      estado.indiceSelecionado =
          (estado.indiceSelecionado == 0) ? QTD_MODO_BORDA - 1 : estado.indiceSelecionado - 1;
      precisaRedesenhar = true;
      break;
    case CommandType::Confirm:
      if (estado.indiceSelecionado == QTD_MODO_BORDA - 1) {
        voltarUmNivel();
      } else {
        modoPendente = static_cast<EdgeMode>(estado.indiceSelecionado);
        navegarPara(Tela::ConfigCanaisTodosConfirmar);
      }
      break;
    default:
      break;
  }
}

void confirmarConfigTodosSim() {
  canais::definirTodos(modoPendente);
  navegarPara(Tela::ConfigCanais);
}
void confirmarConfigTodosNao() { voltarUmNivel(); }

void tratarConfigCanaisTodosConfirmar(const Command& cmd) {
  tratarConfirmacaoBinaria(cmd, confirmarConfigTodosSim, confirmarConfigTodosNao);
}

void tratarConfigCanaisIndividualLista(const Command& cmd) {
  constexpr uint8_t qtd = NUM_CHANNELS + 1;
  switch (cmd.tipo) {
    case CommandType::Next:
      estado.indiceSelecionado = (estado.indiceSelecionado + 1) % qtd;
      precisaRedesenhar = true;
      break;
    case CommandType::Previous:
      estado.indiceSelecionado = (estado.indiceSelecionado == 0) ? qtd - 1 : estado.indiceSelecionado - 1;
      precisaRedesenhar = true;
      break;
    case CommandType::Confirm:
      if (estado.indiceSelecionado == NUM_CHANNELS) {
        voltarUmNivel();
      } else {
        canalSelecionado = estado.indiceSelecionado + 1;
        navegarPara(Tela::ConfigCanaisIndividualEditar);
      }
      break;
    default:
      break;
  }
}

void tratarConfigCanaisIndividualEditar(const Command& cmd) {
  switch (cmd.tipo) {
    case CommandType::Next:
      estado.indiceSelecionado = (estado.indiceSelecionado + 1) % QTD_MODO_BORDA;
      precisaRedesenhar = true;
      break;
    case CommandType::Previous:
      estado.indiceSelecionado =
          (estado.indiceSelecionado == 0) ? QTD_MODO_BORDA - 1 : estado.indiceSelecionado - 1;
      precisaRedesenhar = true;
      break;
    case CommandType::Confirm:
      if (estado.indiceSelecionado == QTD_MODO_BORDA - 1) {
        voltarUmNivel();
      } else {
        modoPendente = static_cast<EdgeMode>(estado.indiceSelecionado);
        navegarPara(Tela::ConfigCanaisIndividualConfirmar);
      }
      break;
    default:
      break;
  }
}

void confirmarConfigIndividualSim() {
  canais::definirModo(canalSelecionado, modoPendente);
  navegarPara(Tela::ConfigCanaisIndividualLista);
}
void confirmarConfigIndividualNao() { voltarUmNivel(); }

void tratarConfigCanaisIndividualConfirmar(const Command& cmd) {
  tratarConfirmacaoBinaria(cmd, confirmarConfigIndividualSim, confirmarConfigIndividualNao);
}

void tratarConfigCanaisVisualizar(const Command& cmd) {
  constexpr uint8_t qtd = NUM_CHANNELS + 1;
  switch (cmd.tipo) {
    case CommandType::Next:
      estado.indiceSelecionado = (estado.indiceSelecionado + 1) % qtd;
      precisaRedesenhar = true;
      break;
    case CommandType::Previous:
      estado.indiceSelecionado = (estado.indiceSelecionado == 0) ? qtd - 1 : estado.indiceSelecionado - 1;
      precisaRedesenhar = true;
      break;
    case CommandType::Confirm:
      if (estado.indiceSelecionado == NUM_CHANNELS) voltarUmNivel();
      break;
    default:
      break;
  }
}

void confirmarRestaurarSim() {
  canais::restaurarPadrao();
  navegarPara(Tela::ConfigCanais);
}
void confirmarRestaurarNao() { voltarUmNivel(); }

void tratarConfigCanaisRestaurarConfirmar(const Command& cmd) {
  tratarConfirmacaoBinaria(cmd, confirmarRestaurarSim, confirmarRestaurarNao);
}

void redesenharValorComVoltar(const char* titulo, int32_t valorExibido) {
  if (edicaoValor.emEdicao) {
    ihm::desenharValorEditavel(titulo, edicaoValor.valorTemp, configuracoes::NIVEL_MINIMO,
                                configuracoes::NIVEL_MAXIMO, nullptr);
    return;
  }

  char linhaValor[24];
  snprintf(linhaValor, sizeof(linhaValor), "%s: %ld", titulo, static_cast<long>(valorExibido));
  const char* itens[2] = {linhaValor, "Voltar"};
  ihm::desenharListaMenu(titulo, itens, 2, estado.indiceSelecionado, 0);
}

void redesenharSobre() {
  char linhaMac[32];
  char linhaModo[32];
  char linhaCanais[32];

  uint8_t macLido[6];
  WiFi.macAddress(macLido);
  snprintf(linhaMac, sizeof(linhaMac), "MAC: %02X:%02X:%02X:%02X:%02X:%02X", macLido[0], macLido[1],
           macLido[2], macLido[3], macLido[4], macLido[5]);

  const bool modoApp = (configuracoes::modoOperacao() == configuracoes::ModoOperacao::App);
  snprintf(linhaModo, sizeof(linhaModo), "Modo: %s", modoApp ? "Aplicativo" : "Hardware");
  snprintf(linhaCanais, sizeof(linhaCanais), "Canais: %u", static_cast<unsigned>(NUM_CHANNELS));

  char linhaNome[32];
  char linhaVersao[32];
  char linhaAutor[40];
  snprintf(linhaNome, sizeof(linhaNome), "%s", configuracoes::NOME_EQUIPAMENTO);
  snprintf(linhaVersao, sizeof(linhaVersao), "Versao: %s", configuracoes::VERSAO_FIRMWARE);
  snprintf(linhaAutor, sizeof(linhaAutor), "Autor: %s", configuracoes::AUTOR);

  char linhaWifi[24];
  char linhaMqtt[24];
  char linhaSd[32];
  snprintf(linhaWifi, sizeof(linhaWifi), "WiFi: %s", mqtt_app::wifiConectado() ? "conectado" : "offline");
  snprintf(linhaMqtt, sizeof(linhaMqtt), "MQTT: %s", mqtt_app::mqttConectado() ? "conectado" : "offline");
  if (armazenamento::cartaoDisponivel()) {
    snprintf(linhaSd, sizeof(linhaSd), "SD: %lu/%lu KB",
             static_cast<unsigned long>(armazenamento::espacoUsadoBytes() / 1024),
             static_cast<unsigned long>(armazenamento::espacoTotalBytes() / 1024));
  } else {
    snprintf(linhaSd, sizeof(linhaSd), "SD: indisponivel");
  }

  const char* linhas[] = {
      linhaNome, linhaVersao, linhaAutor, linhaMac, linhaModo,
      linhaCanais, linhaWifi, linhaMqtt, linhaSd, "Voltar",
  };
  ihm::desenharListaRolavel("Sobre", linhas, 10, estado.offsetRolagem);
}

void redesenharConexaoApp() {
  char linhaWifi[24];
  char linhaIp[24];
  char linhaMqtt[24];
  char linhaId[24];
  snprintf(linhaWifi, sizeof(linhaWifi), "WiFi: %s", mqtt_app::wifiConectado() ? "conectado" : "offline");
  snprintf(linhaIp, sizeof(linhaIp), "IP: %s", mqtt_app::enderecoIP());
  snprintf(linhaMqtt, sizeof(linhaMqtt), "MQTT: %s", mqtt_app::mqttConectado() ? "conectado" : "offline");
  snprintf(linhaId, sizeof(linhaId), "ID: %s", mqtt_app::deviceId());

  const char* itens[] = {linhaWifi, linhaIp, linhaMqtt, linhaId, "Reconectar", "Voltar"};
  ihm::desenharListaMenu("Conexao com app", itens, 6, estado.indiceSelecionado, estado.offsetRolagem);
}

void redesenharConfigCanaisIndividualLista() {
  char buffers[NUM_CHANNELS][20];
  const char* itens[NUM_CHANNELS + 1];
  for (uint8_t i = 0; i < NUM_CHANNELS; i++) {
    snprintf(buffers[i], sizeof(buffers[i]), "C%u - %s", static_cast<unsigned>(i + 1),
             canais::nomeModo(canais::obterModo(i + 1)));
    itens[i] = buffers[i];
  }
  itens[NUM_CHANNELS] = "Voltar";
  ihm::desenharListaMenu("Config. individual", itens, NUM_CHANNELS + 1, estado.indiceSelecionado,
                          estado.offsetRolagem);
}

void redesenharConfigCanaisVisualizar() {
  char buffers[NUM_CHANNELS][20];
  const char* itens[NUM_CHANNELS + 1];
  for (uint8_t i = 0; i < NUM_CHANNELS; i++) {
    snprintf(buffers[i], sizeof(buffers[i]), "C%u: %s", static_cast<unsigned>(i + 1),
             canais::nomeModo(canais::obterModo(i + 1)));
    itens[i] = buffers[i];
  }
  itens[NUM_CHANNELS] = "Voltar";
  ihm::desenharListaMenu("Visualizar config.", itens, NUM_CHANNELS + 1, estado.indiceSelecionado,
                          estado.offsetRolagem);
}

void redesenharTesteCanais() {
  char buffers[NUM_CHANNELS][28];
  const char* itens[NUM_CHANNELS + 1];

  for (uint8_t i = 0; i < NUM_CHANNELS; i++) {
    const uint8_t canal1based = i + 1;
    const bool nivel = aquisicao::nivelAtual(canal1based);
    snprintf(buffers[i], sizeof(buffers[i]), "C%u %s %s (%lu)", static_cast<unsigned>(canal1based),
             nivel ? "HIGH" : "LOW", canais::nomeModo(canais::obterModo(canal1based)),
             static_cast<unsigned long>(aquisicao::quantidadeMudancas(canal1based)));
    itens[i] = buffers[i];
  }
  itens[NUM_CHANNELS] = "Voltar";

  ihm::desenharListaMenu("Teste de canais", itens, NUM_CHANNELS + 1, 0, 0);
}

void redesenharExperimentoExecucao() {
  char titulo[24];
  const int64_t tempoS = experimentos::tempoDecorridoUs() / 1000000;
  snprintf(titulo, sizeof(titulo), "R%u/%u Ev%lu T%llds",
           static_cast<unsigned>(experimentos::repeticaoAtual()),
           static_cast<unsigned>(experimentos::totalRepeticoes()),
           static_cast<unsigned long>(experimentos::eventosNaRepeticaoAtual()),
           static_cast<long long>(tempoS));

  const char* itens[2] = {"Finalizar repeticao", "Cancelar experimento"};
  ihm::desenharListaMenu(titulo, itens, 2, estado.indiceSelecionado, 0);
}

void redesenharEdicaoNomeArquivo() {
  char titulo[24];
  snprintf(titulo, sizeof(titulo), "Nome: %s", nomeArquivo.buffer);

  char caractereAtual[8];
  if (nomeArquivo.indiceAlfabetoAtual == MARCADOR_FIM_INDICE) {
    std::strcpy(caractereAtual, "[FIM]");
  } else if (ALFABETO_NOME[nomeArquivo.indiceAlfabetoAtual] == ' ') {
    std::strcpy(caractereAtual, "[esp]");
  } else {
    caractereAtual[0] = ALFABETO_NOME[nomeArquivo.indiceAlfabetoAtual];
    caractereAtual[1] = '\0';
  }

  ihm::desenharMensagem(titulo, caractereAtual);
}

void redesenharGerenciamentoArquivos() {
  atualizarListaArquivos();

  if (quantidadeArquivosListados == 0) {
    ihm::desenharMensagem("Arquivos",
                          armazenamento::cartaoDisponivel() ? "Nenhum arquivo" : "SD indisponivel");
    return;
  }

  char buffers[MAX_ARQUIVOS_LISTA][24];
  const char* itens[MAX_ARQUIVOS_LISTA + 1];
  for (uint16_t i = 0; i < quantidadeArquivosListados; i++) {
    snprintf(buffers[i], sizeof(buffers[i]), "%s (%lu B)", arquivosListados[i].nome,
             static_cast<unsigned long>(arquivosListados[i].tamanhoBytes));
    itens[i] = buffers[i];
  }
  itens[quantidadeArquivosListados] = "Voltar";

  ihm::desenharListaMenu("Arquivos", itens, quantidadeArquivosListados + 1, estado.indiceSelecionado,
                          estado.offsetRolagem);
}

// Atualiza telas cujo conteúdo muda sozinho, sem entrada do encoder/tecla:
// teste de canais (nível dos sensores) e execução de experimento (tempo,
// eventos). Ambas são redesenhadas em um intervalo fixo, não a cada tick.
void atualizarTelasAoVivo() {
  if (estado.telaAtual == Tela::TesteCanais) {
    for (uint8_t canal1based = 1; canal1based <= NUM_CHANNELS; canal1based++) {
      const uint16_t indiceLed = canal1based - 1;
      if (indiceLed >= NUM_LEDS) break;
      if (aquisicao::nivelAtual(canal1based)) {
        ihm::controlarLED(indiceLed, 200, 0, 0, 30);
      } else {
        ihm::controlarLED(indiceLed, 0, 150, 0, 30);
      }
    }

    static unsigned long ultimoRedesenhoTesteMs = 0;
    const unsigned long agora = millis();
    if (agora - ultimoRedesenhoTesteMs >= 200) {
      ultimoRedesenhoTesteMs = agora;
      precisaRedesenhar = true;
    }
  } else if (estado.telaAtual == Tela::ExperimentoExecucao) {
    static unsigned long ultimoRedesenhoExecucaoMs = 0;
    const unsigned long agora = millis();
    if (agora - ultimoRedesenhoExecucaoMs >= 500) {
      ultimoRedesenhoExecucaoMs = agora;
      precisaRedesenhar = true;
    }
  }
}

void redesenharAnaliseSelecionarArquivo() {
  atualizarListaArquivos();

  if (quantidadeArquivosListados == 0) {
    ihm::desenharMensagem("Analise de dados",
                          armazenamento::cartaoDisponivel() ? "Nenhum arquivo" : "SD indisponivel");
    return;
  }

  char buffers[MAX_ARQUIVOS_LISTA][20];
  const char* itens[MAX_ARQUIVOS_LISTA + 1];
  for (uint16_t i = 0; i < quantidadeArquivosListados; i++) {
    snprintf(buffers[i], sizeof(buffers[i]), "%s", arquivosListados[i].nome);
    itens[i] = buffers[i];
  }
  itens[quantidadeArquivosListados] = "Voltar";

  ihm::desenharListaMenu("Selecionar arquivo", itens, quantidadeArquivosListados + 1,
                          estado.indiceSelecionado, estado.offsetRolagem);
}

void redesenharAnaliseEventos() {
  const uint8_t qtd = analise_dados::quantidadeEventosCarregados();
  char buffers[analise_dados::MAX_EVENTOS_REPETICAO][28];
  const char* itens[analise_dados::MAX_EVENTOS_REPETICAO + 1];

  for (uint8_t i = 0; i < qtd; i++) {
    const analise_dados::EventoLido& ev = analise_dados::evento(i);
    const char marcador = (i == indiceEventoInicialAnalise) ? '*' : ' ';
    snprintf(buffers[i], sizeof(buffers[i]), "%cE%u C%u %c %lldus", marcador, static_cast<unsigned>(i),
             static_cast<unsigned>(ev.canal), ev.estado, static_cast<long long>(ev.tempoUs));
    itens[i] = buffers[i];
  }
  itens[qtd] = "Voltar";

  ihm::desenharListaMenu("Selecionar eventos", itens, static_cast<uint8_t>(qtd + 1),
                          estado.indiceSelecionado, estado.offsetRolagem);
}

void redesenharAnaliseResultado() {
  char mensagem[48];
  snprintf(mensagem, sizeof(mensagem), "dt=%.3fs v=%.3fm/s",
           static_cast<double>(deltaTAnaliseUs) / 1000000.0, static_cast<double>(velocidadeAnaliseMs));
  ihm::desenharMensagem("Resultado", mensagem);
}

// Confirma, sem poluir a serial, que tick() continua rodando (útil para
// descartar travamento após o boot/autotestes). Só imprime a cada 5s.
void imprimirHeartbeat() {
  static unsigned long ultimoHeartbeatMs = 0;
  const unsigned long agora = millis();
  if (agora - ultimoHeartbeatMs < 5000) return;
  ultimoHeartbeatMs = agora;

  Serial.printf("[SISTEMA] Ativo | Tela=%s | Opcao=%s | Display=%s | SD=%s | MQTT=%s | Heap=%u\n",
                nomeTela(estado.telaAtual),
                tituloOpcaoMenu(estado.telaAtual, estado.indiceSelecionado),
                ihm::displayDisponivel() ? "OK" : "FALHA",
                armazenamento::cartaoDisponivel() ? "OK" : "FALHA",
                mqtt_app::mqttConectado() ? "OK" : "DESCONECTADO",
                static_cast<unsigned>(ESP.getFreeHeap()));
}

// Agrupa redesenhos muito próximos no tempo (ex.: giros rápidos e
// sucessivos do encoder) num único redesenho a cada UI_UPDATE_INTERVAL_MS,
// no máximo — reduz flicker sem atrasar a resposta de forma perceptível.
// precisaRedesenhar continua true se recusar, então o próximo tick() tenta
// de novo (nunca perde um redesenho pendente).
bool podeRedesenharAgora() {
  static unsigned long ultimoRedesenhoMs = 0;
  const unsigned long agora = millis();
  if (agora - ultimoRedesenhoMs < UI_UPDATE_INTERVAL_MS) return false;
  ultimoRedesenhoMs = agora;
  return true;
}

void redesenharTelaAtual() {
  // Só loga a mudança de item selecionado quando a tela não mudou (uma
  // mudança de tela já é logada por navegarPara()/voltarUmNivel(), que
  // sempre zeram indiceSelecionado — logar aqui também seria redundante).
  static Tela telaAnteriorLog = Tela::Boot;
  static uint8_t selecaoAnteriorLog = 0;

  Serial.printf("[IHM] Desenhando tela: %s\n", nomeTela(estado.telaAtual));
  // Loga a opção atual sempre que a tela OU a seleção mudou desde o último
  // redesenho (cobre tanto a primeira renderização de uma tela nova quanto
  // a navegação por Next/Previous dentro da mesma tela).
  if (estado.telaAtual != telaAnteriorLog || estado.indiceSelecionado != selecaoAnteriorLog) {
    const uint8_t quantidade = quantidadeOpcoesTela(estado.telaAtual);
    Serial.printf("[MENU] Tela: %s\n", nomeTela(estado.telaAtual));
    Serial.printf("[MENU] Opcao selecionada: %s\n",
                  tituloOpcaoMenu(estado.telaAtual, estado.indiceSelecionado));
    if (quantidade > 0) {
      Serial.printf("[MENU] Posicao: %u de %u\n", static_cast<unsigned>(estado.indiceSelecionado) + 1,
                    static_cast<unsigned>(quantidade));
    }
  }
  telaAnteriorLog = estado.telaAtual;
  selecaoAnteriorLog = estado.indiceSelecionado;

  switch (estado.telaAtual) {
    case Tela::MenuPrincipal:
      ihm::desenharListaMenu("Menu Principal", ITENS_MENU_PRINCIPAL, QTD_MENU_PRINCIPAL,
                              estado.indiceSelecionado, estado.offsetRolagem);
      break;
    case Tela::Configuracoes:
      ihm::desenharListaMenu("Configuracoes", ITENS_CONFIGURACOES, QTD_CONFIGURACOES,
                              estado.indiceSelecionado, estado.offsetRolagem);
      break;
    case Tela::ModoOperacao:
      ihm::desenharListaMenu("Modo de operacao", ITENS_MODO_OPERACAO, QTD_MODO_OPERACAO,
                              estado.indiceSelecionado, estado.offsetRolagem);
      break;
    case Tela::Brilho:
      redesenharValorComVoltar("Brilho", configuracoes::brilho());
      break;
    case Tela::Volume:
      redesenharValorComVoltar("Volume", configuracoes::volume());
      break;
    case Tela::Manual:
      prepararQRManual();
      ihm::desenharGradeModulos("Manual", qrManual.size, moduloQRManual);
      break;
    case Tela::Sobre:
      redesenharSobre();
      break;
    case Tela::ConfigCanais:
      ihm::desenharListaMenu("Config. canais/sensores", ITENS_CONFIG_CANAIS, QTD_CONFIG_CANAIS,
                              estado.indiceSelecionado, estado.offsetRolagem);
      break;
    case Tela::ConfigCanaisTodos:
      ihm::desenharListaMenu("Configurar todos", ITENS_MODO_BORDA, QTD_MODO_BORDA,
                              estado.indiceSelecionado, estado.offsetRolagem);
      break;
    case Tela::ConfigCanaisTodosConfirmar:
      ihm::desenharConfirmacao("Aplicar a todos os canais?", estado.indiceSelecionado);
      break;
    case Tela::ConfigCanaisIndividualLista:
      redesenharConfigCanaisIndividualLista();
      break;
    case Tela::ConfigCanaisIndividualEditar: {
      char titulo[24];
      snprintf(titulo, sizeof(titulo), "Config. canal %u", static_cast<unsigned>(canalSelecionado));
      ihm::desenharListaMenu(titulo, ITENS_MODO_BORDA, QTD_MODO_BORDA, estado.indiceSelecionado,
                              estado.offsetRolagem);
      break;
    }
    case Tela::ConfigCanaisIndividualConfirmar: {
      char pergunta[32];
      snprintf(pergunta, sizeof(pergunta), "Salvar config. do canal %u?",
               static_cast<unsigned>(canalSelecionado));
      ihm::desenharConfirmacao(pergunta, estado.indiceSelecionado);
      break;
    }
    case Tela::ConfigCanaisVisualizar:
      redesenharConfigCanaisVisualizar();
      break;
    case Tela::ConfigCanaisRestaurarConfirmar:
      ihm::desenharConfirmacao("Restaurar todos p/ Ambos?", estado.indiceSelecionado);
      break;
    case Tela::Experimentos:
      ihm::desenharListaMenu("Experimentos", ITENS_EXPERIMENTOS, QTD_EXPERIMENTOS,
                              estado.indiceSelecionado, estado.offsetRolagem);
      break;
    case Tela::TesteCanais:
      redesenharTesteCanais();
      break;
    case Tela::ExperimentoRepeticoes:
      ihm::desenharValorEditavel("Repeticoes", edicaoValor.valorTemp, 1, MAX_REPETICOES, nullptr);
      break;
    case Tela::ExperimentoExecucao:
      redesenharExperimentoExecucao();
      break;
    case Tela::ExperimentoCancelarConfirmar:
      ihm::desenharConfirmacao("Cancelar experimento?", estado.indiceSelecionado);
      break;
    case Tela::ExperimentoNomeArquivo:
    case Tela::ArquivoRenomear:
      redesenharEdicaoNomeArquivo();
      break;
    case Tela::ExperimentoSobrescreverConfirmar: {
      char pergunta[40];
      snprintf(pergunta, sizeof(pergunta), "%s.csv existe. Sobrescrever?", nomeArquivoPendente);
      ihm::desenharConfirmacao(pergunta, estado.indiceSelecionado);
      break;
    }
    case Tela::GerenciamentoArquivos:
      redesenharGerenciamentoArquivos();
      break;
    case Tela::ArquivoDetalhe:
      ihm::desenharListaMenu(arquivoSelecionadoNome, ITENS_ARQUIVO_DETALHE, QTD_ARQUIVO_DETALHE,
                              estado.indiceSelecionado, estado.offsetRolagem);
      break;
    case Tela::ArquivoExcluirConfirmar: {
      char pergunta[32];
      snprintf(pergunta, sizeof(pergunta), "Excluir %s?", arquivoSelecionadoNome);
      ihm::desenharConfirmacao(pergunta, estado.indiceSelecionado);
      break;
    }
    case Tela::ConexaoApp:
      redesenharConexaoApp();
      break;
    case Tela::AnaliseSelecionarArquivo:
      redesenharAnaliseSelecionarArquivo();
      break;
    case Tela::AnaliseSelecionarRepeticao:
      ihm::desenharValorEditavel("Repeticao", edicaoValor.valorTemp, 0, 999, nullptr);
      break;
    case Tela::AnaliseEventos:
      redesenharAnaliseEventos();
      break;
    case Tela::AnaliseDistancia:
      ihm::desenharValorEditavel("Distancia(cm)", edicaoValor.valorTemp, 1, 2000, nullptr);
      break;
    case Tela::AnaliseResultado:
      redesenharAnaliseResultado();
      break;
    default:
      ihm::desenharMensagem(nomeTela(estado.telaAtual), "Em construcao. KEY volta.");
      break;
  }

  Serial.println("[IHM] Renderizacao concluida");
}

void atualizarBoot() {
  const unsigned long decorrido = millis() - inicioEtapaBootMs;

  switch (etapaBootAtual) {
    case EtapaBoot::DiagnosticoDisplay:
      if (!etapaBootDesenhada) {
        // Etapa temporizada (não uma espera separada em ihm::init()):
        // desenha uma única vez e deixa o millis() abaixo decidir quando
        // avançar para o logotipo — a tela nunca fica presa aqui.
        ihm::executarDiagnosticoVisual();
        etapaBootDesenhada = true;
      }
      if (decorrido >= DISPLAY_DIAGNOSTIC_DURATION_MS) avancarBoot(EtapaBoot::Logos);
      break;

    case EtapaBoot::Logos:
      if (!etapaBootDesenhada) {
        desenharLogoMonkeyTech();
        desenharLogoUFRN();
        etapaBootDesenhada = true;
      }
      if (decorrido >= 1500) avancarBoot(EtapaBoot::LedVermelho);
      break;

    case EtapaBoot::LedVermelho:
      if (!etapaBootDesenhada) {
        definirTodosLeds(200, 0, 0, LED_STARTUP_BRIGHTNESS);
        Serial.println("[LEDS] Todos os 6 LEDs: VERMELHO");
        etapaBootDesenhada = true;
      }
      if (decorrido >= 1000) avancarBoot(EtapaBoot::LedAzul);
      break;

    case EtapaBoot::LedAzul:
      if (!etapaBootDesenhada) {
        definirTodosLeds(0, 0, 200, LED_STARTUP_BRIGHTNESS);
        Serial.println("[LEDS] Todos os 6 LEDs: AZUL");
        etapaBootDesenhada = true;
      }
      if (decorrido >= 1000) avancarBoot(EtapaBoot::LedVerde);
      break;

    case EtapaBoot::LedVerde:
      if (!etapaBootDesenhada) {
        definirTodosLeds(0, 200, 0, LED_STARTUP_BRIGHTNESS);
        Serial.println("[LEDS] Todos os 6 LEDs: VERDE");
        etapaBootDesenhada = true;
      }
      if (decorrido >= 1000) avancarBoot(EtapaBoot::LedApagado);
      break;

    case EtapaBoot::LedApagado:
      if (!etapaBootDesenhada) {
        definirTodosLeds(0, 0, 0, 0);
        Serial.println("[LEDS] Todos os 6 LEDs: APAGADOS");
        Serial.println("[LEDS] Teste inicial concluido");
        Serial.println("[LEDS] Controle entregue ao monitoramento dos canais");
        etapaBootDesenhada = true;
      }
      if (decorrido >= 150) avancarBoot(EtapaBoot::Desenvolvedor);
      break;

    case EtapaBoot::Desenvolvedor:
      if (!etapaBootDesenhada) {
        desenharTelaDesenvolvedor();
        etapaBootDesenhada = true;
      }
      if (decorrido >= 2000) avancarBoot(EtapaBoot::Concluido);
      break;

    case EtapaBoot::Concluido:
      Serial.println("[STARTUP] Abrindo menu principal");
      estado.telaAtual = Tela::MenuPrincipal;
      estado.telaAnterior = Tela::MenuPrincipal;
      estado.indiceSelecionado = 0;
      precisaRedesenhar = true;
      break;
  }
}

}  // namespace

void init() {
  estado.telaAtual = Tela::Boot;
  etapaBootAtual = ENABLE_DISPLAY_STARTUP_TEST ? EtapaBoot::DiagnosticoDisplay : EtapaBoot::Logos;
  inicioEtapaBootMs = millis();
  etapaBootDesenhada = false;
  Serial.printf("[STARTUP] Estado: %s\n", nomeEtapaBoot(etapaBootAtual));
}

void tick() {
  // Aquisição/armazenamento rodam em tarefa própria no núcleo 0 (ver
  // main.cpp); esta função (tick) roda no núcleo 1, junto com MQTT.
  static bool primeiroTick = true;
  if (primeiroTick) {
    Serial.println("[TASK][IHM] Tarefa iniciada");
    primeiroTick = false;
  }

  mqtt_app::loop();
  imprimirHeartbeat();

  if (estado.telaAtual == Tela::Boot) {
    atualizarBoot();
    if (precisaRedesenhar && podeRedesenharAgora()) {
      Serial.println("[IHM] Redesenho solicitado");
      redesenharTelaAtual();
      precisaRedesenhar = false;
    }
    return;
  }

  atualizarTelasAoVivo();

  Command cmd;

  const ihm::EventoEncoder evento = ihm::lerEventoEncoder();
  if (evento == ihm::EventoEncoder::Horario) {
    Serial.println("[ENCODER] Sentido: horario");
    cmd.tipo = CommandType::Next;
    processarComando(cmd, Origem::Local);
  } else if (evento == ihm::EventoEncoder::AntiHorario) {
    Serial.println("[ENCODER] Sentido: anti-horario");
    cmd.tipo = CommandType::Previous;
    processarComando(cmd, Origem::Local);
  }

  if (ihm::teclaClicada()) {
    Serial.println("[ENCODER] KEY confirmado");
    cmd.tipo = CommandType::Confirm;
    processarComando(cmd, Origem::Local);
  }

  if (precisaRedesenhar && podeRedesenharAgora()) {
    Serial.println("[IHM] Redesenho solicitado");
    redesenharTelaAtual();
    precisaRedesenhar = false;
  }
}

void processarComando(const Command& cmd, Origem /*origem*/) {
  // Comandos "globais": agem direto sobre os módulos (as MESMAS funções que
  // as telas locais chamam), independente da tela atual. Na prática só o
  // MQTT os emite hoje — o encoder local só gera Next/Previous/Confirm/Back
  // — mas continuam disponíveis para qualquer origem futura.
  switch (cmd.tipo) {
    case CommandType::SetBrightness:
      configuracoes::definirBrilho(static_cast<uint8_t>(cmd.valor));
      if (estado.telaAtual == Tela::Brilho) precisaRedesenhar = true;
      return;
    case CommandType::SetVolume:
      configuracoes::definirVolume(static_cast<uint8_t>(cmd.valor));
      if (estado.telaAtual == Tela::Volume) precisaRedesenhar = true;
      return;
    case CommandType::SetOperationMode:
      configuracoes::definirModoOperacao(cmd.valor == 1 ? configuracoes::ModoOperacao::App
                                                          : configuracoes::ModoOperacao::Hardware);
      return;
    case CommandType::SetChannelMode:
      canais::definirModo(cmd.canal, cmd.modo);
      return;
    case CommandType::SetAllChannelsMode:
      canais::definirTodos(cmd.modo);
      return;
    case CommandType::RestoreChannelDefaults:
      canais::restaurarPadrao();
      return;
    case CommandType::StartExperiment:
      Serial.println("[EXPERIMENTO] Iniciando experimento (comando MQTT)");
      if (experimentos::iniciar(static_cast<uint16_t>(cmd.valor > 0 ? cmd.valor : 1))) {
        navegarPara(Tela::ExperimentoExecucao);
      } else {
        Serial.println("[EXPERIMENTO] Falha ao iniciar (SD indisponivel?)");
      }
      return;
    case CommandType::StopExperiment:
    case CommandType::CancelExperiment:
      Serial.println("[EXPERIMENTO] Cancelado/parado (comando MQTT)");
      experimentos::cancelar();
      navegarPara(Tela::Experimentos);
      return;
    case CommandType::FinishRepetition:
      Serial.println("[EXPERIMENTO] Finalizando repeticao (comando MQTT)");
      experimentos::finalizarRepeticaoAtual();
      precisaRedesenhar = true;
      return;
    default:
      break;
  }

  switch (estado.telaAtual) {
    case Tela::MenuPrincipal:
      tratarMenuPrincipal(cmd);
      break;
    case Tela::Configuracoes:
      tratarConfiguracoes(cmd);
      break;
    case Tela::ModoOperacao:
      tratarModoOperacao(cmd);
      break;
    case Tela::Brilho:
      tratarBrilho(cmd);
      break;
    case Tela::Volume:
      tratarVolume(cmd);
      break;
    case Tela::ConfigCanais:
      tratarConfigCanais(cmd);
      break;
    case Tela::ConfigCanaisTodos:
      tratarConfigCanaisTodos(cmd);
      break;
    case Tela::ConfigCanaisTodosConfirmar:
      tratarConfigCanaisTodosConfirmar(cmd);
      break;
    case Tela::ConfigCanaisIndividualLista:
      tratarConfigCanaisIndividualLista(cmd);
      break;
    case Tela::ConfigCanaisIndividualEditar:
      tratarConfigCanaisIndividualEditar(cmd);
      break;
    case Tela::ConfigCanaisIndividualConfirmar:
      tratarConfigCanaisIndividualConfirmar(cmd);
      break;
    case Tela::ConfigCanaisVisualizar:
      tratarConfigCanaisVisualizar(cmd);
      break;
    case Tela::ConfigCanaisRestaurarConfirmar:
      tratarConfigCanaisRestaurarConfirmar(cmd);
      break;
    case Tela::Experimentos:
      tratarExperimentos(cmd);
      break;
    case Tela::ExperimentoRepeticoes:
      tratarExperimentoRepeticoes(cmd);
      break;
    case Tela::ExperimentoExecucao:
      tratarExperimentoExecucao(cmd);
      break;
    case Tela::ExperimentoCancelarConfirmar:
      tratarExperimentoCancelarConfirmar(cmd);
      break;
    case Tela::ExperimentoNomeArquivo:
    case Tela::ArquivoRenomear:
      tratarEdicaoNomeArquivo(cmd);
      break;
    case Tela::ExperimentoSobrescreverConfirmar:
      tratarExperimentoSobrescreverConfirmar(cmd);
      break;
    case Tela::GerenciamentoArquivos:
      tratarGerenciamentoArquivos(cmd);
      break;
    case Tela::ArquivoDetalhe:
      tratarArquivoDetalhe(cmd);
      break;
    case Tela::ArquivoExcluirConfirmar:
      tratarArquivoExcluirConfirmar(cmd);
      break;
    case Tela::ConexaoApp:
      tratarConexaoApp(cmd);
      break;
    case Tela::AnaliseSelecionarArquivo:
      tratarAnaliseSelecionarArquivo(cmd);
      break;
    case Tela::AnaliseSelecionarRepeticao:
      tratarAnaliseSelecionarRepeticao(cmd);
      break;
    case Tela::AnaliseEventos:
      tratarAnaliseEventos(cmd);
      break;
    case Tela::AnaliseDistancia:
      tratarAnaliseDistancia(cmd);
      break;
    case Tela::Boot:
      break;
    default:
      tratarTelaEmConstrucao(cmd);
      break;
  }
}

Tela telaAtual() { return estado.telaAtual; }

}  // namespace maquina_estados
