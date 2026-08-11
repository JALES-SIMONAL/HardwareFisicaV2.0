#include "maquina_estados.hpp"

#include <Arduino.h>
#include <cstdio>
#include <cstring>
#include <qrcode.h>

#include "MAIN.HPP"
#include "analise_circular.hpp"
#include "analise_dados.hpp"
#include "analise_linear.hpp"
#include "aquisicao.hpp"
#include "armazenamento.hpp"
#include "bluetooth_app.hpp"
#include "canais.hpp"
#include "configuracoes.hpp"
#include "experimentos.hpp"
#include "ihm.hpp"
#include "layout.hpp"
#include "tempo.hpp"

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

// Seletor "valor + Voltar" (índice 0 = editar, índice 1 = voltar),
// reaproveitado por Repetições do experimento e repetição/distância da
// análise — garante que essas telas sempre tenham "Voltar" alcançável
// pelo encoder local, mesmo sem um comando de Back dedicado (definida
// mais abaixo; declarada aqui para poder ser usada antes).
void tratarSeletorValorComVoltar(const Command& cmd, int32_t valorInicialEdicao);
void redesenharValorComVoltar(const char* titulo, int32_t valorExibido, int32_t minimo, int32_t maximo,
                               const char* unidade = nullptr);

// ---------------------------------------------------------------------
// Sequência de boot (não-bloqueante, baseada em millis())
// ---------------------------------------------------------------------
enum class EtapaBoot : uint8_t {
  LogoMonkeyTech,
  LogoUFRN,
  LedVermelho,
  LedAzul,
  LedVerde,
  LedApagado,
  Desenvolvedor,
  Concluido
};

EtapaBoot etapaBootAtual = EtapaBoot::LogoMonkeyTech;
unsigned long inicioEtapaBootMs = 0;
bool etapaBootDesenhada = false;

const char* nomeEtapaBoot(EtapaBoot etapa) {
  switch (etapa) {
    case EtapaBoot::LogoMonkeyTech: return "LOGOTIPO_MONKEY_TECH";
    case EtapaBoot::LogoUFRN: return "LOGOTIPO_UFRN";
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

// Tenta desenhar o BMP correspondente lido da raiz do microSD; se o cartão
// estiver indisponível, o arquivo não existir ou o formato não for
// suportado, cai no texto simples — nunca trava nem deixa a tela em branco.
void desenharLogoMonkeyTech() {
  const int16_t largura = layout::uiWidth(layout::UI_REFERENCE_WIDTH);
  const int16_t altura = layout::uiHeight(layout::UI_REFERENCE_HEIGHT);
  if (!ihm::desenharImagemBMP("Monkey Tech.bmp", 0, 0, largura, altura)) {
    ihm::escreverTextoTela("Monkey Tech", layout::uiMargin(), layout::uiHeight(40),
                            0xFFFF, layout::uiFontSize(1), true);
  }
}

void desenharLogoUFRN() {
  const int16_t largura = layout::uiWidth(layout::UI_REFERENCE_WIDTH);
  const int16_t altura = layout::uiHeight(layout::UI_REFERENCE_HEIGHT);
  if (!ihm::desenharImagemBMP("UFRN.bmp", 0, 0, largura, altura)) {
    ihm::escreverTextoTela("UFRN", layout::uiMargin(), layout::uiHeight(70), 0xFFFF,
                            layout::uiFontSize(1), true);
  }
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
  uint8_t indiceSelecionado = 0;
  uint8_t offsetRolagem = 0;
};

EstadoNavegacao estado;
bool precisaRedesenhar = false;

// true enquanto o app estiver com a tela de teste de canais aberta
// (comando Bluetooth "set_channel_test_active") — independente da tela
// LOCAL exibida no display físico. Sem isto, os NeoPixels só acendiam
// quando o teste era feito localmente (estado.telaAtual==Tela::TesteCanais);
// o app já recebia os níveis (mensagem "teste_canais", publicada sempre que
// conectado) mas os LEDs físicos nunca reagiam a um teste iniciado só pelo
// app. Ver atualizarTelasAoVivo() e aoDesconectarBluetooth().
bool testeCanaisAtivoRemoto = false;

// Pilha de navegação: cada navegarPara() empilha a tela de origem; cada
// voltarUmNivel() desempilha. Substitui um antigo campo único "tela
// anterior" (histórico de só 1 nível) que travava o botão Voltar em
// qualquer fluxo com 3+ níveis de profundidade — ex.: ConfigCanais ->
// ConfigCanaisTodos -> ConfigCanaisTodosConfirmar; ao voltar da
// confirmação para ConfigCanaisTodos, o único campo ficava preso
// apontando pra ConfigCanaisTodos (valor antigo), então um segundo
// "Voltar" ali virava um no-op (telaAtual = telaAnterior = a própria tela
// atual). Com a pilha, cada nível de volta desempilha o nível
// corretamente, não importa a profundidade.
constexpr uint8_t PROFUNDIDADE_MAXIMA_PILHA_TELAS = 16;
Tela pilhaNavegacao[PROFUNDIDADE_MAXIMA_PILHA_TELAS];
uint8_t topoPilhaNavegacao = 0;

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
    "Voltar",
};
constexpr uint8_t QTD_EXPERIMENTOS = 4;

// "Conexao com app" mudou de Experimentos para cá — mesmo lugar (menu de
// nivel do equipamento) onde a analogia mais direta do app (Configuracoes)
// ja tinha esse acesso.
constexpr const char* ITENS_CONFIGURACOES[] = {
    "Conexao com app",  "Modo de operacao", "Brilho da tela",       "Volume",
    "Config. canais/sensores", "Manual",    "Sobre", "Analise de dados", "Voltar",
};
constexpr uint8_t QTD_CONFIGURACOES = 9;
// Índice de "Analise de dados" em ITENS_CONFIGURACOES — usado por
// redesenharConfiguracoes() para acrescentar "ON"/"OFF" ao rótulo (o
// usuário não tinha como saber o estado atual sem entrar no item).
constexpr uint8_t INDICE_ANALISE_DADOS_CONFIGURACOES = 7;

// "Ativado"/"Desativado" pra Tela::AnaliseDadosToggle — mesmo padrao de
// ITENS_MODO_OPERACAO.
constexpr const char* ITENS_ANALISE_DADOS_TOGGLE[] = {"Ativado", "Desativado", "Voltar"};
constexpr uint8_t QTD_ANALISE_DADOS_TOGGLE = 3;

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

// "H para L"=Falling(0), "L para H"=Rising(1), "Ambos"=Both(2),
// "Desabilitado"=Disabled(3): a ordem desta lista casa de propósito com os
// valores do enum EdgeMode ("Voltar" é só um item de UI, sem EdgeMode
// correspondente).
constexpr const char* ITENS_MODO_BORDA[] = {"H para L", "L para H", "Ambos", "Desabilitado", "Voltar"};
constexpr uint8_t QTD_MODO_BORDA = 5;

// Estado temporário compartilhado pelo fluxo de configuração de canais:
// canal em edição (1..NUM_CHANNELS) e modo escolhido, pendente de confirmação.
uint8_t canalSelecionado = 1;
EdgeMode modoPendente = EdgeMode::Both;

constexpr uint8_t ITEM_ARQUIVO_VER_DADOS = 0;
constexpr uint8_t ITEM_ARQUIVO_RENOMEAR = 1;
constexpr uint8_t ITEM_ARQUIVO_EXCLUIR = 2;
constexpr uint8_t ITEM_ARQUIVO_VOLTAR = 3;
constexpr const char* ITENS_ARQUIVO_DETALHE[] = {"Ver dados", "Renomear", "Excluir", "Voltar"};
constexpr uint8_t QTD_ARQUIVO_DETALHE = 4;

// Linhas de dados (canal/estado/tempo_us + repetição) da tela "Ver dados" de
// um arquivo, carregadas inteiras ao entrar na tela (sem paginação — ao
// contrário da tabela rolante do app, que pagina pelo BLE). Cap baixo o
// bastante pra rolar razoavelmente bem num encoder; arquivos maiores só têm
// visualização completa pelo app.
constexpr uint16_t MAX_LINHAS_DADOS_ARQUIVO = 100;
char linhasDadosArquivo[MAX_LINHAS_DADOS_ARQUIVO][28];
uint16_t quantidadeLinhasDadosArquivo = 0;

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
    case Tela::AnaliseDadosToggle: return QTD_ANALISE_DADOS_TOGGLE;
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
    case Tela::AnaliseDadosToggle:
      return (indice < QTD_ANALISE_DADOS_TOGGLE) ? ITENS_ANALISE_DADOS_TOGGLE[indice]
                                                  : "Opcao invalida";
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

// Editor de texto genérico (usado para salvar um experimento novo, renomear
// um arquivo existente e renomear o dispositivo BLE). Alfabeto: [FIM],
// [APAGAR] e [CANCELAR] primeiro (permitem terminar, apagar o último
// caractere ou desistir e voltar sem salvar a qualquer momento — sem
// [CANCELAR], não havia como sair da tela de senha sem acertar a senha; o
// encoder local só gera Next/Previous/Confirm, nunca um "Voltar" de
// verdade, então o cancelamento PRECISA ser um símbolo do alfabeto, não um
// botão físico separado), depois espaço, letras A-Z e dígitos 0-9.
constexpr char ALFABETO_NOME[] = "\x01\x02\x03 ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
constexpr uint8_t MARCADOR_FIM_INDICE = 0;
constexpr uint8_t MARCADOR_APAGAR_INDICE = 1;
constexpr uint8_t MARCADOR_CANCELAR_INDICE = 2;
constexpr uint8_t QTD_ALFABETO_NOME = sizeof(ALFABETO_NOME) - 1;
// 20 cobre tanto nomes de arquivo quanto o nome BLE (bluetooth_app::
// TAMANHO_MAX_NOME_DISPOSITIVO_BT), que reaproveita este mesmo editor.
constexpr uint8_t TAMANHO_MAX_NOME_ARQUIVO = 20;

enum class ModoEdicaoNome : uint8_t {
  SalvarExperimento,
  RenomearArquivo,
  RenomearDispositivoBT,
  ValidarSenha,
};
ModoEdicaoNome modoEdicaoNome = ModoEdicaoNome::SalvarExperimento;

struct EstadoNomeArquivo {
  char buffer[TAMANHO_MAX_NOME_ARQUIVO + 1] = "";
  uint8_t posicaoCursor = 0;
  uint8_t indiceAlfabetoAtual = 0;
};
EstadoNomeArquivo nomeArquivo;
char nomeArquivoPendente[TAMANHO_MAX_NOME_ARQUIVO + 1] = "";

// Duas ações locais também são protegidas por senha (mesma senha das ações
// BLE equivalentes — ver configuracoes::validarSenha): renomear o
// dispositivo BT e ativar/entrar em "Analise de dados" quando ela estiver
// desativada em Configuracoes. Validada uma vez, vale pro resto da sessão
// (desde o boot) — não persiste nem expira sozinha.
enum class AcaoAposSenha : uint8_t { Nenhuma, AbrirAnaliseDados, AbrirRenomearBT, AbrirToggleAnalise };
AcaoAposSenha acaoAposSenha = AcaoAposSenha::Nenhuma;
bool senhaValidadaNestaSessao = false;

// +5 = ".csv" + '\0' — precisa caber o nome inteiro devolvido por
// armazenamento::listarArquivos() (ver comentário em InfoArquivo::nome).
char arquivoSelecionadoNome[TAMANHO_MAX_NOME_ARQUIVO + 5] = "";

constexpr uint16_t MAX_ARQUIVOS_LISTA = 20;
armazenamento::InfoArquivo arquivosListados[MAX_ARQUIVOS_LISTA];
uint16_t quantidadeArquivosListados = 0;

void atualizarListaArquivos() {
  quantidadeArquivosListados = armazenamento::listarArquivos(arquivosListados, MAX_ARQUIVOS_LISTA);
}

// Carrega (até MAX_LINHAS_DADOS_ARQUIVO) linhas de dados do arquivo inteiro
// para a tela "Ver dados" — já formatadas para exibição, ao contrário de
// bluetooth_app::publicarDadosArquivo() (que envia campos separados e pagina
// sob demanda; aqui carrega tudo de uma vez, dentro do limite, porque a
// tela local não tem como pedir mais páginas).
void carregarDadosArquivo(const char* nomeComExtensao) {
  quantidadeLinhasDadosArquivo = 0;
  if (!armazenamento::abrirParaLeitura(nomeComExtensao)) return;

  char linha[32];
  uint16_t repeticaoAtualIdx = 0;
  bool linhaAnteriorEraDados = false;

  while (armazenamento::lerProximaLinha(linha, sizeof(linha))) {
    if (linha[0] == '\0') {
      if (linhaAnteriorEraDados) repeticaoAtualIdx++;
      linhaAnteriorEraDados = false;
      continue;
    }

    unsigned canal = 0;
    char estado = '\0';
    long long tempoUs = 0;
    if (std::sscanf(linha, "%u,%c,%lld", &canal, &estado, &tempoUs) != 3) continue;
    linhaAnteriorEraDados = true;

    if (quantidadeLinhasDadosArquivo >= MAX_LINHAS_DADOS_ARQUIVO) continue;
    snprintf(linhasDadosArquivo[quantidadeLinhasDadosArquivo],
             sizeof(linhasDadosArquivo[quantidadeLinhasDadosArquivo]), "R%u C%u %c %lldus",
             static_cast<unsigned>(repeticaoAtualIdx), canal, estado, tempoUs);
    quantidadeLinhasDadosArquivo++;
  }

  armazenamento::fecharLeitura();
}

char arquivoAnaliseNome[TAMANHO_MAX_NOME_ARQUIVO + 5] = "";

// Distância entre dois pontos/sensores consecutivos do trilho, preservada
// entre visitas à tela (mesmo padrão de analiseCircularRaioMm abaixo) e
// usada para acionar analise_linear::calcular().
int32_t analiseLinearDistanciaCm = 100;
// 0 = gráfico de velocidade, 1 = gráfico de aceleração — ver
// PAGINA_GRAFICO_LINEAR_* perto de tratarAnaliseLinearResultado().
uint8_t analiseLinearPaginaGrafico = 0;
// Mesmo papel de analiseCircularTotalRepeticoes/RepeticoesValidas, mas para
// a análise linear (ver comentário lá).
uint16_t analiseLinearTotalRepeticoes = 0;
uint16_t analiseLinearRepeticoesValidas = 0;

// Parâmetros da análise de movimento circular, preservados entre as telas
// de raio/vãos (cada uma edita e devolve um valor via edicaoValor, que é
// compartilhado com as demais telas "valor + Voltar" do restante do
// firmware) e usados para acionar analise_circular::calcular().
int32_t analiseCircularRaioMm = 10;
int32_t analiseCircularVaosQtd = 20;
// Qual gráfico mostrar em Tela::AnaliseCircularGrafico — ver constantes
// PAGINA_GRAFICO_* (velocidade/aceleração/RPM), definidas perto de
// tratarAnaliseCircularResultado(). Escolhido antes de navegar pra lá, na
// tela de resultado (cada gráfico é uma opção separada da lista).
uint8_t analiseCircularPaginaGrafico = 0;

// Quantidade de repetições do arquivo em análise (analise_dados::
// contarRepeticoes(), calculada uma vez em "Calcular") — usada pra montar a
// lista de Tela::AnaliseCircularEscolherRepeticao ("Rep 1".."Rep N",
// "Media"); inclui blocos sem eventos suficientes (o usuário ainda pode
// escolhê-los, o gráfico só mostra "sem dados"). analiseCircularRepeticoesValidas
// é quantas delas de fato entraram na média dos valores-resumo (ver
// analise_circular::calcularMediaRepeticoes()) — só pra exibição, sempre
// <= analiseCircularTotalRepeticoes.
uint16_t analiseCircularTotalRepeticoes = 0;
uint16_t analiseCircularRepeticoesValidas = 0;

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

  // Muitos fluxos terminam chamando navegarPara() para VOLTAR a uma tela
  // ancestral já visitada (ex.: confirmarConfigTodosSim() chama
  // navegarPara(ConfigCanais) depois de salvar) — isto não é uma navegação
  // nova "para a frente", é um retorno. Empilhar incondicionalmente nesse
  // caso deixava entradas obsoletas na pilha (a própria tela de
  // confirmação que acabou de ser resolvida), e um "Voltar" seguinte
  // reaparecia nela, refazendo a ação (salvar) em loop. Por isso: se
  // "destino" já está entre os ancestrais na pilha atual, trunca até lá em
  // vez de empilhar de novo — só empilha quando é navegação nova de
  // verdade.
  bool destinoJaEraAncestral = false;
  for (uint8_t i = 0; i < topoPilhaNavegacao; i++) {
    if (pilhaNavegacao[i] == destino) {
      topoPilhaNavegacao = i;
      destinoJaEraAncestral = true;
      break;
    }
  }

  if (!destinoJaEraAncestral) {
    if (topoPilhaNavegacao < PROFUNDIDADE_MAXIMA_PILHA_TELAS) {
      pilhaNavegacao[topoPilhaNavegacao] = estado.telaAtual;
      topoPilhaNavegacao++;
    } else {
      // Não deveria acontecer em uso normal (profundidade de menu real é
      // bem menor que 16) — loga em vez de estourar o array; o pior caso é
      // "Voltar" truncar o histórico mais antigo, nunca corromper memória.
      Serial.println("[ESTADO] Aviso: pilha de navegacao cheia, historico mais antigo descartado");
    }
  }
  Serial.printf("[ESTADO] Pilha de navegacao: profundidade=%u (retorno a ancestral: %s)\n",
                static_cast<unsigned>(topoPilhaNavegacao), destinoJaEraAncestral ? "sim" : "nao");

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

  const Tela destino =
      (topoPilhaNavegacao > 0) ? pilhaNavegacao[--topoPilhaNavegacao] : Tela::MenuPrincipal;
  Serial.printf("[ESTADO] Pilha de navegacao: profundidade=%u\n",
                static_cast<unsigned>(topoPilhaNavegacao));

  Serial.printf("[ESTADO] Tela: %s -> %s\n", nomeTela(estado.telaAtual), nomeTela(destino));
  estado.telaAtual = destino;
  estado.indiceSelecionado = 0;
  estado.offsetRolagem = 0;
  edicaoValor.emEdicao = false;
  precisaRedesenhar = true;
}

void executarAcaoAposSenha() {
  const AcaoAposSenha acao = acaoAposSenha;
  acaoAposSenha = AcaoAposSenha::Nenhuma;
  switch (acao) {
    case AcaoAposSenha::AbrirAnaliseDados:
      navegarPara(Tela::AnaliseSelecionarArquivo);
      break;
    case AcaoAposSenha::AbrirRenomearBT:
      modoEdicaoNome = ModoEdicaoNome::RenomearDispositivoBT;
      std::strncpy(nomeArquivo.buffer, bluetooth_app::nomeDispositivo(), sizeof(nomeArquivo.buffer) - 1);
      nomeArquivo.buffer[sizeof(nomeArquivo.buffer) - 1] = '\0';
      nomeArquivo.posicaoCursor = static_cast<uint8_t>(std::strlen(nomeArquivo.buffer));
      nomeArquivo.indiceAlfabetoAtual = 0;
      navegarPara(Tela::ConexaoAppRenomear);
      break;
    case AcaoAposSenha::AbrirToggleAnalise:
      navegarPara(Tela::AnaliseDadosToggle);
      break;
    case AcaoAposSenha::Nenhuma:
    default:
      voltarUmNivel();
      break;
  }
}

// Ponto único de entrada pras duas ações locais protegidas (ver
// AcaoAposSenha) — pula direto pra ação se a senha já foi validada nesta
// sessão (desde o boot), senão manda pro editor de texto reaproveitado
// (mesmo usado para nomear arquivo/dispositivo BT) em modo ValidarSenha.
void solicitarSenhaOuExecutar(AcaoAposSenha acao) {
  acaoAposSenha = acao;
  if (senhaValidadaNestaSessao) {
    executarAcaoAposSenha();
    return;
  }

  modoEdicaoNome = ModoEdicaoNome::ValidarSenha;
  nomeArquivo.buffer[0] = '\0';
  nomeArquivo.posicaoCursor = 0;
  nomeArquivo.indiceAlfabetoAtual = 0;
  navegarPara(Tela::SenhaValidar);
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
    case Tela::SenhaValidar: return "Senha";
    case Tela::AnaliseDadosToggle: return "Analise de dados";
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
    case Tela::ExperimentoReiniciarConfirmar: return "Reiniciar?";
    case Tela::ExperimentoNomeArquivo: return "Nome do arquivo";
    case Tela::ExperimentoSobrescreverConfirmar: return "Sobrescrever?";
    case Tela::TesteCanais: return "Teste de canais";
    case Tela::GerenciamentoArquivos: return "Arquivos";
    case Tela::ArquivoDetalhe: return "Detalhe do arquivo";
    case Tela::ArquivoRenomear: return "Renomear";
    case Tela::ArquivoExcluirConfirmar: return "Excluir?";
    case Tela::ArquivosExcluirTodosConfirmar: return "Excluir todos?";
    case Tela::ArquivoDados: return "Ver dados";
    case Tela::ConexaoApp: return "Conexao com app";
    case Tela::ConexaoAppRenomear: return "Renomear dispositivo BT";
    case Tela::AnaliseSelecionarArquivo: return "Selecionar arquivo";
    case Tela::AnaliseTipo: return "Tipo de analise";
    case Tela::AnaliseLinearDistancia: return "Distancia entre pontos";
    case Tela::AnaliseLinearResultado: return "Resultado linear";
    case Tela::AnaliseLinearEscolherRepeticao: return "Qual repeticao?";
    case Tela::AnaliseLinearGrafico: return "Grafico";
    case Tela::AnaliseCircularRaioVaos: return "Raio e vaos";
    case Tela::AnaliseCircularResultado: return "Resultado circular";
    case Tela::AnaliseCircularEscolherRepeticao: return "Qual repeticao?";
    case Tela::AnaliseCircularGrafico: return "Grafico";
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
        case 2:
          if (configuracoes::analiseDadosHabilitada()) {
            navegarPara(Tela::AnaliseSelecionarArquivo);
          } else {
            solicitarSenhaOuExecutar(AcaoAposSenha::AbrirAnaliseDados);
          }
          break;
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
          if (experimentos::emAndamento()) {
            // Já existe uma medição rodando em segundo plano (usuário saiu
            // da tela de execução por "Voltar" sem cancelar) — reabre a
            // tela ao vivo em vez de ir para o seletor de repetições, que
            // configuraria uma medição NOVA (e experimentos::iniciar()
            // recusaria, já que a guarda cobre Fase::Executando).
            navegarPara(Tela::ExperimentoExecucao);
          } else {
            navegarPara(Tela::ExperimentoRepeticoes);
            edicaoValor.valorTemp = 1;
          }
          break;
        case 1: navegarPara(Tela::TesteCanais); break;
        case 2: navegarPara(Tela::GerenciamentoArquivos); break;
        case 3: voltarUmNivel(); break;
        default: break;
      }
      break;
    default:
      break;
  }
}

void tratarExperimentoRepeticoes(const Command& cmd) {
  // Seletor "Repeticoes: N / Voltar" — garante que dá para sair desta
  // tela pelo encoder local sem precisar iniciar um experimento (antes,
  // só existia edição direta, sem "Voltar" alcançável localmente).
  if (!edicaoValor.emEdicao) {
    tratarSeletorValorComVoltar(cmd, edicaoValor.valorTemp);
    return;
  }

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
      // Sai da edição, volta ao seletor (não à tela anterior) — igual ao
      // padrão de Brilho/Volume.
      edicaoValor.emEdicao = false;
      precisaRedesenhar = true;
      break;
    default:
      break;
  }
}

// Itens da lista da tela de execução do experimento (ver
// redesenharExperimentoExecucao()): 0=Finalizar repetição, 1=Reiniciar
// repetição, 2=Cancelar experimento, 3=Voltar.
constexpr uint8_t NUM_ITENS_EXPERIMENTO_EXECUCAO = 4;

// Sai da tela de execução SEM cancelar o experimento (a coleta continua em
// segundo plano, na tarefa do núcleo 0 — ver tarefaAquisicaoArmazenamento em
// main.cpp — independente da tela exibida). Sem isto, a única forma de sair
// desta tela era pelo fluxo "Cancelar experimento", e responder "Não" na
// confirmação só devolvia o usuário para esta mesma tela sem nenhuma outra
// saída — um loop percebido pelo usuário como "preso" tentando sair.
// navegarPara() (não voltarUmNivel()) porque Experimentos já é sempre um
// ancestral na pilha neste ponto (foi de lá que o experimento começou),
// então a pilha é truncada de volta pra lá em vez de crescer.
void sairExperimentoExecucaoSemCancelar() {
  Serial.println("[EXPERIMENTO] Saindo da tela de execucao (experimento continua em segundo plano)");
  navegarPara(Tela::Experimentos);
}

void tratarExperimentoExecucao(const Command& cmd) {
  switch (cmd.tipo) {
    case CommandType::Next:
      estado.indiceSelecionado =
          (estado.indiceSelecionado + 1) % NUM_ITENS_EXPERIMENTO_EXECUCAO;
      precisaRedesenhar = true;
      break;
    case CommandType::Previous:
      estado.indiceSelecionado = (estado.indiceSelecionado == 0)
                                      ? (NUM_ITENS_EXPERIMENTO_EXECUCAO - 1)
                                      : (estado.indiceSelecionado - 1);
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
          std::strncpy(nomeArquivo.buffer, experimentos::nomeSugerido(), sizeof(nomeArquivo.buffer) - 1);
          nomeArquivo.buffer[sizeof(nomeArquivo.buffer) - 1] = '\0';
          nomeArquivo.posicaoCursor = static_cast<uint8_t>(std::strlen(nomeArquivo.buffer));
          nomeArquivo.indiceAlfabetoAtual = 0;
          navegarPara(Tela::ExperimentoNomeArquivo);
        } else {
          precisaRedesenhar = true;
        }
      } else if (estado.indiceSelecionado == 1) {
        navegarPara(Tela::ExperimentoReiniciarConfirmar);
      } else if (estado.indiceSelecionado == 2) {
        navegarPara(Tela::ExperimentoCancelarConfirmar);
      } else {
        sairExperimentoExecucaoSemCancelar();
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

void confirmarReiniciarRepeticaoSim() {
  Serial.printf("[EXPERIMENTO] Reiniciando repeticao %u/%u\n",
                static_cast<unsigned>(experimentos::repeticaoAtual()),
                static_cast<unsigned>(experimentos::totalRepeticoes()));
  experimentos::reiniciarRepeticaoAtual();
  voltarUmNivel();
}
void confirmarReiniciarRepeticaoNao() { voltarUmNivel(); }

void tratarExperimentoReiniciarConfirmar(const Command& cmd) {
  tratarConfirmacaoBinaria(cmd, confirmarReiniciarRepeticaoSim, confirmarReiniciarRepeticaoNao);
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

  if (modoEdicaoNome == ModoEdicaoNome::ValidarSenha) {
    if (!configuracoes::validarSenha(nomeFinal)) {
      // Senha errada: some com o que foi digitado e deixa tentar de novo,
      // igual ao "nome vazio" acima — não sai da tela sozinho.
      ihm::beep(150);
      nomeArquivo.buffer[0] = '\0';
      nomeArquivo.posicaoCursor = 0;
      precisaRedesenhar = true;
      return;
    }
    senhaValidadaNestaSessao = true;
    executarAcaoAposSenha();
    return;
  }

  if (modoEdicaoNome == ModoEdicaoNome::RenomearDispositivoBT) {
    bluetooth_app::definirNomeDispositivo(nomeFinal);
    navegarPara(Tela::ConexaoApp);
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
      bluetooth_app::publicarListaArquivos();
      navegarPara(Tela::Experimentos);
    }
    return;
  }

  // ModoEdicaoNome::RenomearArquivo
  char nomeComExtensao[TAMANHO_MAX_NOME_ARQUIVO + 5];
  snprintf(nomeComExtensao, sizeof(nomeComExtensao), "%s.csv", nomeFinal);
  if (armazenamento::renomearArquivo(arquivoSelecionadoNome, nomeComExtensao)) {
    bluetooth_app::publicarListaArquivos();
    navegarPara(Tela::GerenciamentoArquivos);
  } else {
    // Já existe um arquivo com esse nome: nunca sobrescreve silenciosamente
    // no fluxo de renomear — o usuário tenta outro nome.
    ihm::beep(150);
  }
}

// Símbolo [ESC] (MARCADOR_CANCELAR_INDICE) ou "back" via BLE: desiste da
// edição sem salvar nada, para todos os modos — não descarta dados já
// existentes (ex.: em ValidarSenha, a medição continua aguardando nome no
// equipamento; só a ação pendente é esquecida).
void cancelarEdicaoNomeArquivo() {
  if (modoEdicaoNome == ModoEdicaoNome::ValidarSenha) {
    acaoAposSenha = AcaoAposSenha::Nenhuma;
  }
  voltarUmNivel();
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
      if (nomeArquivo.indiceAlfabetoAtual == MARCADOR_CANCELAR_INDICE) {
        cancelarEdicaoNomeArquivo();
        break;
      }

      if (nomeArquivo.indiceAlfabetoAtual == MARCADOR_APAGAR_INDICE) {
        // Apaga o último caractere (se houver) e permanece no próprio
        // símbolo [APAGAR] — permite apagar vários seguidos sem precisar
        // navegar de novo até ele a cada vez.
        if (nomeArquivo.posicaoCursor > 0) {
          nomeArquivo.posicaoCursor--;
          nomeArquivo.buffer[nomeArquivo.posicaoCursor] = '\0';
          precisaRedesenhar = true;
        }
        break;
      }

      const bool ehFim = (nomeArquivo.indiceAlfabetoAtual == MARCADOR_FIM_INDICE);

      if (!ehFim && nomeArquivo.posicaoCursor < TAMANHO_MAX_NOME_ARQUIVO) {
        nomeArquivo.buffer[nomeArquivo.posicaoCursor] = ALFABETO_NOME[nomeArquivo.indiceAlfabetoAtual];
        nomeArquivo.posicaoCursor++;
        nomeArquivo.buffer[nomeArquivo.posicaoCursor] = '\0';
        // Mantém o cursor no mesmo símbolo em vez de voltar pro [OK] —
        // útil para digitar o mesmo caractere (ou um vizinho na grade)
        // várias vezes seguidas sem precisar navegar de novo.
        precisaRedesenhar = true;
      }

      if (ehFim || nomeArquivo.posicaoCursor >= TAMANHO_MAX_NOME_ARQUIVO) {
        finalizarEdicaoNomeArquivo();
      }
      break;
    }
    case CommandType::Back:
      // O encoder local nunca gera Back (só Next/Previous/Confirm — ver
      // símbolo [ESC]/MARCADOR_CANCELAR_INDICE acima, que é o cancelamento
      // de verdade alcançável fisicamente); isto só importa se algum dia o
      // app mandar um "back" bruto pelo BLE enquanto o equipamento estiver
      // nesta tela.
      cancelarEdicaoNomeArquivo();
      break;
    default:
      break;
  }
}

void confirmarSobrescreverExperimentoSim() {
  experimentos::salvarComoArquivoFinal(nomeArquivoPendente, true);
  bluetooth_app::publicarListaArquivos();
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

  // +1 = "Excluir todos", +1 = "Voltar".
  const uint16_t itemExcluirTodos = quantidadeArquivosListados;
  const uint16_t itemVoltar = quantidadeArquivosListados + 1;
  const uint16_t qtd = quantidadeArquivosListados + 2;
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
      if (estado.indiceSelecionado == itemExcluirTodos) {
        navegarPara(Tela::ArquivosExcluirTodosConfirmar);
      } else if (estado.indiceSelecionado == itemVoltar) {
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
      if (estado.indiceSelecionado == ITEM_ARQUIVO_VER_DADOS) {
        carregarDadosArquivo(arquivoSelecionadoNome);
        navegarPara(Tela::ArquivoDados);
      } else if (estado.indiceSelecionado == ITEM_ARQUIVO_RENOMEAR) {
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
  // Igual ao comando Bluetooth equivalente: avisa o app que a lista mudou,
  // mesmo quando a exclusão foi feita pelo encoder local.
  bluetooth_app::publicarListaArquivos();
  navegarPara(Tela::GerenciamentoArquivos);
}
void confirmarExcluirArquivoNao() { voltarUmNivel(); }

void tratarArquivoExcluirConfirmar(const Command& cmd) {
  tratarConfirmacaoBinaria(cmd, confirmarExcluirArquivoSim, confirmarExcluirArquivoNao);
}

void confirmarExcluirTodosArquivosSim() {
  const uint16_t excluidos = armazenamento::excluirTodosArquivosCsv();
  Serial.printf("[ARQUIVOS] Excluir todos: %u csv(s) removido(s)\n", static_cast<unsigned>(excluidos));
  bluetooth_app::publicarListaArquivos();
  navegarPara(Tela::GerenciamentoArquivos);
}
void confirmarExcluirTodosArquivosNao() { voltarUmNivel(); }

void tratarArquivosExcluirTodosConfirmar(const Command& cmd) {
  tratarConfirmacaoBinaria(cmd, confirmarExcluirTodosArquivosSim, confirmarExcluirTodosArquivosNao);
}

// Só leitura/rolagem — "Voltar" é o único item que faz algo ao confirmar,
// igual ao padrão já usado em tratarConfigCanaisVisualizar().
void tratarArquivoDados(const Command& cmd) {
  const uint16_t qtd = quantidadeLinhasDadosArquivo + 1;
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
    case CommandType::Back:
      if (estado.indiceSelecionado == quantidadeLinhasDadosArquivo || cmd.tipo == CommandType::Back) {
        voltarUmNivel();
      }
      break;
    default:
      break;
  }
}

void tratarConexaoApp(const Command& cmd) {
  constexpr uint8_t QTD_CONEXAO_APP = 7;
  constexpr uint8_t ITEM_RENOMEAR = 4;
  constexpr uint8_t ITEM_RECONECTAR = 5;
  constexpr uint8_t ITEM_VOLTAR = 6;

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
      if (estado.indiceSelecionado == ITEM_RENOMEAR) {
        solicitarSenhaOuExecutar(AcaoAposSenha::AbrirRenomearBT);
      } else if (estado.indiceSelecionado == ITEM_RECONECTAR) {
        bluetooth_app::reconectar();
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
        // Sempre carrega a primeira (e, na prática, única) repetição do
        // arquivo — a antiga tela "Selecionar repeticao" (pedir um índice
        // 0..999 sem o usuário saber quantas repetições o arquivo tem) foi
        // removida por não ter função real nesse fluxo local.
        if (analise_dados::carregarRepeticao(arquivoAnaliseNome, 0) > 0) {
          navegarPara(Tela::AnaliseTipo);
        } else {
          ihm::beep(150);
        }
      }
      break;
    default:
      break;
  }
}

// Escolha do tipo de análise para a repetição já carregada: 0 = análise
// linear (trilho reto: distância fixa entre pontos -> distância percorrida
// + gráficos de velocidade/aceleração), 1 = movimento circular (raio +
// vãos -> distância + gráficos de velocidade/aceleração/RPM), 2 = voltar.
// Sem o item "Voltar" (e sem tratar CommandType::Back) esta tela era um
// beco sem saída no encoder local: ele só gera Next/Previous/Confirm (ver
// tick()), nunca Back — Back só existe vindo do app Bluetooth.
constexpr uint8_t QTD_ANALISE_TIPO = 3;
constexpr uint8_t ITEM_ANALISE_TIPO_VOLTAR = 2;

void tratarAnaliseTipo(const Command& cmd) {
  switch (cmd.tipo) {
    case CommandType::Next:
      estado.indiceSelecionado = (estado.indiceSelecionado + 1) % QTD_ANALISE_TIPO;
      precisaRedesenhar = true;
      break;
    case CommandType::Previous:
      estado.indiceSelecionado =
          (estado.indiceSelecionado == 0) ? (QTD_ANALISE_TIPO - 1) : (estado.indiceSelecionado - 1);
      precisaRedesenhar = true;
      break;
    case CommandType::Confirm:
      if (estado.indiceSelecionado == 0) {
        navegarPara(Tela::AnaliseLinearDistancia);
      } else if (estado.indiceSelecionado == 1) {
        navegarPara(Tela::AnaliseCircularRaioVaos);
      } else if (estado.indiceSelecionado == ITEM_ANALISE_TIPO_VOLTAR) {
        voltarUmNivel();
      }
      break;
    case CommandType::Back:
      voltarUmNivel();
      break;
    default:
      break;
  }
}

constexpr int32_t ANALISE_LINEAR_DISTANCIA_MIN_CM = 1;
constexpr int32_t ANALISE_LINEAR_DISTANCIA_MAX_CM = 2000;

// Seletor "Distancia: N cm / Voltar" — mesmo padrão de
// tratarSeletorValorComVoltar/redesenharValorComVoltar usado por
// Brilho/Volume/Repetições do experimento: só uma grandeza aqui (distância
// entre dois pontos/sensores consecutivos do trilho, igual entre todos os
// intervalos), então não precisa da lista customizada que Raio/Vaos usa
// (essa tem duas grandezas).
void tratarAnaliseLinearDistancia(const Command& cmd) {
  if (!edicaoValor.emEdicao) {
    tratarSeletorValorComVoltar(cmd, analiseLinearDistanciaCm);
    return;
  }

  switch (cmd.tipo) {
    case CommandType::Next:
      if (edicaoValor.valorTemp < ANALISE_LINEAR_DISTANCIA_MAX_CM) edicaoValor.valorTemp++;
      precisaRedesenhar = true;
      break;
    case CommandType::Previous:
      if (edicaoValor.valorTemp > ANALISE_LINEAR_DISTANCIA_MIN_CM) edicaoValor.valorTemp--;
      precisaRedesenhar = true;
      break;
    case CommandType::Confirm: {
      analiseLinearDistanciaCm = edicaoValor.valorTemp;
      const float distanciaMetros = static_cast<float>(analiseLinearDistanciaCm) / 100.0f;
      // Os valores-resumo mostrados no resultado são a média entre TODAS
      // as repetições do arquivo (mesmo raciocínio de
      // tratarAnaliseCircularRaioVaos()) — cada repetição pesa igual,
      // independente de quantos eventos teve.
      analiseLinearTotalRepeticoes = analise_dados::contarRepeticoes(arquivoAnaliseNome);
      if (analiseLinearTotalRepeticoes > MAX_REPETICOES) {
        analiseLinearTotalRepeticoes = MAX_REPETICOES;
      }
      analiseLinearRepeticoesValidas = analise_linear::calcularMediaRepeticoes(
          arquivoAnaliseNome, analiseLinearTotalRepeticoes, distanciaMetros);
      navegarPara(Tela::AnaliseLinearResultado);
      break;
    }
    case CommandType::Back:
      edicaoValor.emEdicao = false;
      precisaRedesenhar = true;
      break;
    default:
      break;
  }
}

// Itens da lista da tela de resultado (ver redesenharAnaliseLinearResultado()):
// os quatro primeiros (distancia, repeticoes, vel. media, acel. media —
// média entre TODAS as repetições do arquivo) são só informativos —
// Confirm neles não faz nada, mesmo padrão de
// tratarArquivoDados()/tratarConfigCanaisVisualizar(); os dois gráficos são
// opções separadas; cada uma abre Tela::AnaliseLinearEscolherRepeticao
// antes de plotar, pra perguntar de qual repetição (ou da média entre
// elas) vem a curva. "Voltar" fecha a análise.
constexpr uint8_t QTD_ANALISE_LINEAR_RESULTADO = 7;
constexpr uint8_t ITEM_LINEAR_RESULTADO_GRAFICO_VELOCIDADE = 4;
constexpr uint8_t ITEM_LINEAR_RESULTADO_GRAFICO_ACELERACAO = 5;
constexpr uint8_t ITEM_LINEAR_RESULTADO_VOLTAR = 6;

// Páginas de ihm::desenharGrafico() na tela Tela::AnaliseLinearGrafico.
constexpr uint8_t PAGINA_GRAFICO_LINEAR_VELOCIDADE = 0;
constexpr uint8_t PAGINA_GRAFICO_LINEAR_ACELERACAO = 1;

void tratarAnaliseLinearResultado(const Command& cmd) {
  switch (cmd.tipo) {
    case CommandType::Next:
      estado.indiceSelecionado = (estado.indiceSelecionado + 1) % QTD_ANALISE_LINEAR_RESULTADO;
      precisaRedesenhar = true;
      break;
    case CommandType::Previous:
      estado.indiceSelecionado = (estado.indiceSelecionado == 0)
                                      ? (QTD_ANALISE_LINEAR_RESULTADO - 1)
                                      : (estado.indiceSelecionado - 1);
      precisaRedesenhar = true;
      break;
    case CommandType::Confirm:
      if (estado.indiceSelecionado == ITEM_LINEAR_RESULTADO_GRAFICO_VELOCIDADE) {
        analiseLinearPaginaGrafico = PAGINA_GRAFICO_LINEAR_VELOCIDADE;
        navegarPara(Tela::AnaliseLinearEscolherRepeticao);
      } else if (estado.indiceSelecionado == ITEM_LINEAR_RESULTADO_GRAFICO_ACELERACAO) {
        analiseLinearPaginaGrafico = PAGINA_GRAFICO_LINEAR_ACELERACAO;
        navegarPara(Tela::AnaliseLinearEscolherRepeticao);
      } else if (estado.indiceSelecionado == ITEM_LINEAR_RESULTADO_VOLTAR) {
        voltarUmNivel();
      }
      break;
    case CommandType::Back:
      voltarUmNivel();
      break;
    default:
      break;
  }
}

// Escolha de qual repetição vira o gráfico (analiseLinearPaginaGrafico já
// foi escolhido em tratarAnaliseLinearResultado()): itens 0..N-1 = "Rep
// 1".."Rep N", item N = "Media" (analise_linear::calcularMediaGrafico()),
// item N+1 = "Voltar". Mesmo design de tratarAnaliseCircularEscolherRepeticao().
void tratarAnaliseLinearEscolherRepeticao(const Command& cmd) {
  const uint16_t qtd = analiseLinearTotalRepeticoes + 2;
  const uint16_t itemMedia = analiseLinearTotalRepeticoes;
  const uint16_t itemVoltar = analiseLinearTotalRepeticoes + 1;

  switch (cmd.tipo) {
    case CommandType::Next:
      estado.indiceSelecionado = (estado.indiceSelecionado + 1) % qtd;
      precisaRedesenhar = true;
      break;
    case CommandType::Previous:
      estado.indiceSelecionado = (estado.indiceSelecionado == 0) ? (qtd - 1) : (estado.indiceSelecionado - 1);
      precisaRedesenhar = true;
      break;
    case CommandType::Confirm: {
      const float distanciaMetros = static_cast<float>(analiseLinearDistanciaCm) / 100.0f;
      if (estado.indiceSelecionado == itemMedia) {
        analise_linear::calcularMediaGrafico(arquivoAnaliseNome, analiseLinearTotalRepeticoes,
                                              distanciaMetros);
        navegarPara(Tela::AnaliseLinearGrafico);
      } else if (estado.indiceSelecionado == itemVoltar) {
        voltarUmNivel();
      } else {
        analise_dados::carregarRepeticao(arquivoAnaliseNome, estado.indiceSelecionado);
        analise_linear::calcular(distanciaMetros);
        navegarPara(Tela::AnaliseLinearGrafico);
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

void tratarAnaliseLinearGrafico(const Command& cmd) {
  switch (cmd.tipo) {
    case CommandType::Confirm:
    case CommandType::Back:
      voltarUmNivel();
      break;
    default:
      break;
  }
}

constexpr int32_t ANALISE_CIRCULAR_RAIO_MIN_MM = 1;
constexpr int32_t ANALISE_CIRCULAR_RAIO_MAX_MM = 500;
constexpr int32_t ANALISE_CIRCULAR_VAOS_MIN = 1;
constexpr int32_t ANALISE_CIRCULAR_VAOS_MAX = 200;

// Raio e vãos numa página só (ver redesenharAnaliseCircularRaioVaos()): uma
// lista de 4 itens ("Raio: N mm", "Vaos: N", "Calcular", "Voltar"); Confirm
// nos dois primeiros entra em edição do respectivo valor (mesma mecânica de
// tratarSeletorValorComVoltar, só que com dois valores em vez de um — por
// isso não reaproveita aquele helper). "Calcular" roda a análise com os
// valores atuais e vai para o resultado; "Voltar" sai para AnaliseTipo.
constexpr uint8_t QTD_CIRCULAR_RAIO_VAOS = 4;
constexpr uint8_t ITEM_CIRCULAR_RAIO = 0;
constexpr uint8_t ITEM_CIRCULAR_VAOS = 1;
constexpr uint8_t ITEM_CIRCULAR_CALCULAR = 2;
constexpr uint8_t ITEM_CIRCULAR_VOLTAR = 3;

void tratarAnaliseCircularRaioVaos(const Command& cmd) {
  if (!edicaoValor.emEdicao) {
    switch (cmd.tipo) {
      case CommandType::Next:
        estado.indiceSelecionado = (estado.indiceSelecionado + 1) % QTD_CIRCULAR_RAIO_VAOS;
        precisaRedesenhar = true;
        break;
      case CommandType::Previous:
        estado.indiceSelecionado = (estado.indiceSelecionado == 0)
                                        ? (QTD_CIRCULAR_RAIO_VAOS - 1)
                                        : (estado.indiceSelecionado - 1);
        precisaRedesenhar = true;
        break;
      case CommandType::Confirm:
        if (estado.indiceSelecionado == ITEM_CIRCULAR_RAIO) {
          edicaoValor.emEdicao = true;
          edicaoValor.valorTemp = analiseCircularRaioMm;
          precisaRedesenhar = true;
        } else if (estado.indiceSelecionado == ITEM_CIRCULAR_VAOS) {
          edicaoValor.emEdicao = true;
          edicaoValor.valorTemp = analiseCircularVaosQtd;
          precisaRedesenhar = true;
        } else if (estado.indiceSelecionado == ITEM_CIRCULAR_CALCULAR) {
          const float raioMetros = static_cast<float>(analiseCircularRaioMm) / 1000.0f;
          const uint16_t vaos = static_cast<uint16_t>(analiseCircularVaosQtd);
          // Os valores-resumo mostrados no resultado são a média entre
          // TODAS as repetições do arquivo, não só a que estava carregada
          // — cada repetição pesa igual, independente de quantos eventos
          // teve (ver analise_circular::calcularMediaRepeticoes()). Limita
          // a MAX_REPETICOES (mesmo teto de experimentos::iniciar()): é o
          // tamanho do buffer da lista "qual repeticao" (ver
          // redesenharAnaliseCircularEscolherRepeticao()).
          analiseCircularTotalRepeticoes = analise_dados::contarRepeticoes(arquivoAnaliseNome);
          if (analiseCircularTotalRepeticoes > MAX_REPETICOES) {
            analiseCircularTotalRepeticoes = MAX_REPETICOES;
          }
          analiseCircularRepeticoesValidas = analise_circular::calcularMediaRepeticoes(
              arquivoAnaliseNome, analiseCircularTotalRepeticoes, raioMetros, vaos);
          navegarPara(Tela::AnaliseCircularResultado);
        } else if (estado.indiceSelecionado == ITEM_CIRCULAR_VOLTAR) {
          voltarUmNivel();
        }
        break;
      case CommandType::Back:
        voltarUmNivel();
        break;
      default:
        break;
    }
    return;
  }

  // Editando o campo selecionado (Raio ou Vaos) — Confirm salva no campo
  // certo e volta para a lista (sem avançar de tela: dá pra editar o outro
  // campo em seguida); Back cancela a edição sem salvar.
  const bool editandoRaio = (estado.indiceSelecionado == ITEM_CIRCULAR_RAIO);
  const int32_t minimo = editandoRaio ? ANALISE_CIRCULAR_RAIO_MIN_MM : ANALISE_CIRCULAR_VAOS_MIN;
  const int32_t maximo = editandoRaio ? ANALISE_CIRCULAR_RAIO_MAX_MM : ANALISE_CIRCULAR_VAOS_MAX;

  switch (cmd.tipo) {
    case CommandType::Next:
      if (edicaoValor.valorTemp < maximo) edicaoValor.valorTemp++;
      precisaRedesenhar = true;
      break;
    case CommandType::Previous:
      if (edicaoValor.valorTemp > minimo) edicaoValor.valorTemp--;
      precisaRedesenhar = true;
      break;
    case CommandType::Confirm:
      if (editandoRaio) {
        analiseCircularRaioMm = edicaoValor.valorTemp;
      } else {
        analiseCircularVaosQtd = edicaoValor.valorTemp;
      }
      edicaoValor.emEdicao = false;
      precisaRedesenhar = true;
      break;
    case CommandType::Back:
      edicaoValor.emEdicao = false;
      precisaRedesenhar = true;
      break;
    default:
      break;
  }
}

// Itens da lista da tela de resultado (ver redesenharAnaliseCircularResultado()):
// os cinco primeiros (distancia, repeticoes, vel. media, acel. media, rpm
// medio — média entre TODAS as repetições do arquivo) são só informativos
// — Confirm neles não faz nada, mesmo padrão de
// tratarArquivoDados()/tratarConfigCanaisVisualizar(); os três gráficos são
// opções separadas (não uma única tela alternada por rotação); cada uma
// abre Tela::AnaliseCircularEscolherRepeticao antes de plotar, pra
// perguntar de qual repetição (ou da média entre elas) vem a curva.
// "Voltar" fecha a análise.
constexpr uint8_t QTD_ANALISE_CIRCULAR_RESULTADO = 9;
constexpr uint8_t ITEM_CIRCULAR_RESULTADO_GRAFICO_VELOCIDADE = 5;
constexpr uint8_t ITEM_CIRCULAR_RESULTADO_GRAFICO_ACELERACAO = 6;
constexpr uint8_t ITEM_CIRCULAR_RESULTADO_GRAFICO_RPM = 7;
constexpr uint8_t ITEM_CIRCULAR_RESULTADO_VOLTAR = 8;

// Páginas de ihm::desenharGrafico() na tela Tela::AnaliseCircularGrafico.
constexpr uint8_t PAGINA_GRAFICO_VELOCIDADE = 0;
constexpr uint8_t PAGINA_GRAFICO_ACELERACAO = 1;
constexpr uint8_t PAGINA_GRAFICO_RPM = 2;

void tratarAnaliseCircularResultado(const Command& cmd) {
  switch (cmd.tipo) {
    case CommandType::Next:
      estado.indiceSelecionado = (estado.indiceSelecionado + 1) % QTD_ANALISE_CIRCULAR_RESULTADO;
      precisaRedesenhar = true;
      break;
    case CommandType::Previous:
      estado.indiceSelecionado = (estado.indiceSelecionado == 0)
                                      ? (QTD_ANALISE_CIRCULAR_RESULTADO - 1)
                                      : (estado.indiceSelecionado - 1);
      precisaRedesenhar = true;
      break;
    case CommandType::Confirm:
      if (estado.indiceSelecionado == ITEM_CIRCULAR_RESULTADO_GRAFICO_VELOCIDADE) {
        analiseCircularPaginaGrafico = PAGINA_GRAFICO_VELOCIDADE;
        navegarPara(Tela::AnaliseCircularEscolherRepeticao);
      } else if (estado.indiceSelecionado == ITEM_CIRCULAR_RESULTADO_GRAFICO_ACELERACAO) {
        analiseCircularPaginaGrafico = PAGINA_GRAFICO_ACELERACAO;
        navegarPara(Tela::AnaliseCircularEscolherRepeticao);
      } else if (estado.indiceSelecionado == ITEM_CIRCULAR_RESULTADO_GRAFICO_RPM) {
        analiseCircularPaginaGrafico = PAGINA_GRAFICO_RPM;
        navegarPara(Tela::AnaliseCircularEscolherRepeticao);
      } else if (estado.indiceSelecionado == ITEM_CIRCULAR_RESULTADO_VOLTAR) {
        voltarUmNivel();
      }
      break;
    case CommandType::Back:
      voltarUmNivel();
      break;
    default:
      break;
  }
}

// Escolha de qual repetição vira o gráfico (analiseCircularPaginaGrafico já
// foi escolhido em tratarAnaliseCircularResultado()): itens 0..N-1 = "Rep
// 1".."Rep N", item N = "Media" (analise_circular::calcularMediaGrafico()),
// item N+1 = "Voltar".
void tratarAnaliseCircularEscolherRepeticao(const Command& cmd) {
  const uint16_t qtd = analiseCircularTotalRepeticoes + 2;
  const uint16_t itemMedia = analiseCircularTotalRepeticoes;
  const uint16_t itemVoltar = analiseCircularTotalRepeticoes + 1;

  switch (cmd.tipo) {
    case CommandType::Next:
      estado.indiceSelecionado = (estado.indiceSelecionado + 1) % qtd;
      precisaRedesenhar = true;
      break;
    case CommandType::Previous:
      estado.indiceSelecionado = (estado.indiceSelecionado == 0) ? (qtd - 1) : (estado.indiceSelecionado - 1);
      precisaRedesenhar = true;
      break;
    case CommandType::Confirm: {
      const float raioMetros = static_cast<float>(analiseCircularRaioMm) / 1000.0f;
      const uint16_t vaos = static_cast<uint16_t>(analiseCircularVaosQtd);
      if (estado.indiceSelecionado == itemMedia) {
        analise_circular::calcularMediaGrafico(arquivoAnaliseNome, analiseCircularTotalRepeticoes,
                                                raioMetros, vaos);
        navegarPara(Tela::AnaliseCircularGrafico);
      } else if (estado.indiceSelecionado == itemVoltar) {
        voltarUmNivel();
      } else {
        analise_dados::carregarRepeticao(arquivoAnaliseNome, estado.indiceSelecionado);
        analise_circular::calcular(raioMetros, vaos);
        navegarPara(Tela::AnaliseCircularGrafico);
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

void tratarAnaliseCircularGrafico(const Command& cmd) {
  switch (cmd.tipo) {
    case CommandType::Confirm:
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
        case 0: navegarPara(Tela::ConexaoApp); break;
        case 1: navegarPara(Tela::ModoOperacao); break;
        case 2: navegarPara(Tela::Brilho); break;
        case 3: navegarPara(Tela::Volume); break;
        case 4: navegarPara(Tela::ConfigCanais); break;
        case 5: navegarPara(Tela::Manual); break;
        case 6: navegarPara(Tela::Sobre); break;
        case 7: solicitarSenhaOuExecutar(AcaoAposSenha::AbrirToggleAnalise); break;
        case 8: voltarUmNivel(); break;
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

// Só alcançada depois de solicitarSenhaOuExecutar(AbrirToggleAnalise) —
// senha já validada nesta sessão a essa altura.
void tratarAnaliseDadosToggle(const Command& cmd) {
  switch (cmd.tipo) {
    case CommandType::Next:
      estado.indiceSelecionado = (estado.indiceSelecionado + 1) % QTD_ANALISE_DADOS_TOGGLE;
      precisaRedesenhar = true;
      break;
    case CommandType::Previous:
      estado.indiceSelecionado = (estado.indiceSelecionado == 0) ? QTD_ANALISE_DADOS_TOGGLE - 1
                                                                    : estado.indiceSelecionado - 1;
      precisaRedesenhar = true;
      break;
    case CommandType::Confirm:
      switch (estado.indiceSelecionado) {
        case 0:
          configuracoes::definirAnaliseDadosHabilitada(true);
          bluetooth_app::publicarEstado();
          voltarUmNivel();
          break;
        case 1:
          configuracoes::definirAnaliseDadosHabilitada(false);
          bluetooth_app::publicarEstado();
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
  Serial.printf("[DIAG][MENU] confirmarConfigTodosSim: chamando canais::definirTodos(%u)\n",
                static_cast<unsigned>(modoPendente));
  canais::definirTodos(modoPendente);
  // Igual ao comando Bluetooth equivalente: sem isto, uma mudança feita pelo
  // encoder local não chegava ao app enquanto ele não desconectasse/reconectasse.
  bluetooth_app::publicarConfiguracaoCanais();
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
  Serial.printf("[DIAG][MENU] confirmarConfigIndividualSim: canais::definirModo(canal=%u, modo=%u)\n",
                static_cast<unsigned>(canalSelecionado), static_cast<unsigned>(modoPendente));
  canais::definirModo(canalSelecionado, modoPendente);
  bluetooth_app::publicarConfiguracaoCanais();
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
  bluetooth_app::publicarConfiguracaoCanais();
  navegarPara(Tela::ConfigCanais);
}
void confirmarRestaurarNao() { voltarUmNivel(); }

void tratarConfigCanaisRestaurarConfirmar(const Command& cmd) {
  tratarConfirmacaoBinaria(cmd, confirmarRestaurarSim, confirmarRestaurarNao);
}

// Padrão reutilizado por toda tela "valor + Voltar" (Brilho, Volume,
// Repetições do experimento, repetição/distância da análise): item 0 =
// valor atual (clicar entra em edição), item 1 = Voltar — sempre
// alcançável pelo encoder local antes de entrar em edição, mesmo em
// telas que não têm um comando local de "Back" dedicado.
void redesenharValorComVoltar(const char* titulo, int32_t valorExibido, int32_t minimo, int32_t maximo,
                               const char* unidade) {
  if (edicaoValor.emEdicao) {
    ihm::desenharValorEditavel(titulo, edicaoValor.valorTemp, minimo, maximo, unidade);
    return;
  }

  char linhaValor[24];
  if (unidade != nullptr) {
    snprintf(linhaValor, sizeof(linhaValor), "%s: %ld%s", titulo, static_cast<long>(valorExibido), unidade);
  } else {
    snprintf(linhaValor, sizeof(linhaValor), "%s: %ld", titulo, static_cast<long>(valorExibido));
  }
  const char* itens[2] = {linhaValor, "Voltar"};
  uint8_t offsetFixo = 0;
  ihm::desenharListaMenu(titulo, itens, 2, estado.indiceSelecionado, offsetFixo);
}

// Trata Next/Previous/Confirm do SELETOR "valor + Voltar" (índice 0 =
// entrar em edição, índice 1 = voltar) — a parte comum às 5 telas que
// usam este padrão. Só chamar quando !edicaoValor.emEdicao; cada tela
// trata sua própria lógica de edição (valores/limites/ação ao confirmar
// são diferentes em cada uma) separadamente, só ativada quando
// edicaoValor.emEdicao já estiver true.
void tratarSeletorValorComVoltar(const Command& cmd, int32_t valorInicialEdicao) {
  constexpr uint8_t ITEM_VALOR = 0;
  constexpr uint8_t ITEM_VOLTAR = 1;

  switch (cmd.tipo) {
    case CommandType::Next:
    case CommandType::Previous:
      estado.indiceSelecionado = (estado.indiceSelecionado == ITEM_VALOR) ? ITEM_VOLTAR : ITEM_VALOR;
      precisaRedesenhar = true;
      break;
    case CommandType::Confirm:
      if (estado.indiceSelecionado == ITEM_VALOR) {
        edicaoValor.emEdicao = true;
        edicaoValor.valorTemp = valorInicialEdicao;
        precisaRedesenhar = true;
      } else {
        voltarUmNivel();
      }
      break;
    case CommandType::Back:
      voltarUmNivel();
      break;
    default:
      break;
  }
}

void redesenharSobre() {
  char linhaMac[32];
  char linhaModo[32];
  char linhaCanais[32];

  snprintf(linhaMac, sizeof(linhaMac), "MAC: %s", bluetooth_app::enderecoMac());

  const bool modoApp = (configuracoes::modoOperacao() == configuracoes::ModoOperacao::App);
  snprintf(linhaModo, sizeof(linhaModo), "Modo: %s", modoApp ? "Aplicativo" : "Hardware");
  snprintf(linhaCanais, sizeof(linhaCanais), "Canais: %u", static_cast<unsigned>(NUM_CHANNELS));

  char linhaNome[32];
  char linhaVersao[32];
  char linhaAutor[40];
  snprintf(linhaNome, sizeof(linhaNome), "%s", configuracoes::NOME_EQUIPAMENTO);
  snprintf(linhaVersao, sizeof(linhaVersao), "Versao: %s", configuracoes::VERSAO_FIRMWARE);
  snprintf(linhaAutor, sizeof(linhaAutor), "Autor: %s", configuracoes::AUTOR);

  char linhaBt[24];
  char linhaSd[32];
  snprintf(linhaBt, sizeof(linhaBt), "BT: %s", bluetooth_app::conectado() ? "conectado" : "desconectado");
  if (armazenamento::cartaoDisponivel()) {
    snprintf(linhaSd, sizeof(linhaSd), "SD: %lu/%lu KB",
             static_cast<unsigned long>(armazenamento::espacoUsadoBytes() / 1024),
             static_cast<unsigned long>(armazenamento::espacoTotalBytes() / 1024));
  } else {
    snprintf(linhaSd, sizeof(linhaSd), "SD: indisponivel");
  }

  const char* linhas[] = {
      linhaNome, linhaVersao, linhaAutor, linhaMac, linhaModo,
      linhaCanais, linhaBt, linhaSd, "Voltar",
  };
  ihm::desenharListaRolavel("Sobre", linhas, 9, estado.offsetRolagem);
}

void redesenharConexaoApp() {
  char linhaBt[24];
  char linhaMac[24];
  char linhaId[24];
  char linhaNome[32];
  snprintf(linhaBt, sizeof(linhaBt), "BT: %s", bluetooth_app::conectado() ? "conectado" : "desconectado");
  snprintf(linhaMac, sizeof(linhaMac), "MAC: %s", bluetooth_app::enderecoMac());
  snprintf(linhaId, sizeof(linhaId), "ID: %s", bluetooth_app::deviceId());
  snprintf(linhaNome, sizeof(linhaNome), "Nome: %s", bluetooth_app::nomeDispositivo());

  const char* itens[] = {linhaBt, linhaMac, linhaId, linhaNome, "Renomear", "Reconectar", "Voltar"};
  ihm::desenharListaMenu("Conexao com app", itens, 7, estado.indiceSelecionado, estado.offsetRolagem);
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

  uint8_t offsetFixo = 0;
  ihm::desenharListaMenu("Teste de canais", itens, NUM_CHANNELS + 1, 0, offsetFixo);
}

void redesenharExperimentoExecucao() {
  char titulo[24];
  const int64_t tempoS = experimentos::tempoDecorridoUs() / 1000000;
  snprintf(titulo, sizeof(titulo), "R%u/%u Ev%lu T%llds",
           static_cast<unsigned>(experimentos::repeticaoAtual()),
           static_cast<unsigned>(experimentos::totalRepeticoes()),
           static_cast<unsigned long>(experimentos::eventosNaRepeticaoAtual()),
           static_cast<long long>(tempoS));

  const char* itens[NUM_ITENS_EXPERIMENTO_EXECUCAO] = {"Finalizar repeticao", "Reiniciar repeticao",
                                                        "Cancelar experimento", "Voltar"};
  uint8_t offsetFixo = 0;
  ihm::desenharListaMenu(titulo, itens, NUM_ITENS_EXPERIMENTO_EXECUCAO, estado.indiceSelecionado, offsetFixo);
}

// Rótulo curto (até 3 caracteres — "ESC" precisa dos 3) por símbolo do
// alfabeto, para o teclado em grade — construído uma única vez (os
// símbolos nunca mudam) e reaproveitado a cada redesenho.
const char* const* rotulosAlfabeto() {
  static char buffers[QTD_ALFABETO_NOME][4];
  static const char* rotulos[QTD_ALFABETO_NOME];
  static bool preparado = false;

  if (!preparado) {
    for (uint8_t i = 0; i < QTD_ALFABETO_NOME; i++) {
      if (i == MARCADOR_FIM_INDICE) {
        std::strcpy(buffers[i], "OK");
      } else if (i == MARCADOR_APAGAR_INDICE) {
        std::strcpy(buffers[i], "<-");
      } else if (i == MARCADOR_CANCELAR_INDICE) {
        std::strcpy(buffers[i], "ESC");
      } else if (ALFABETO_NOME[i] == ' ') {
        std::strcpy(buffers[i], "_");
      } else {
        buffers[i][0] = ALFABETO_NOME[i];
        buffers[i][1] = '\0';
      }
      rotulos[i] = buffers[i];
    }
    preparado = true;
  }

  return rotulos;
}

void redesenharEdicaoNomeArquivo() {
  // Mesmo editor reaproveitado pra nome de arquivo, nome do dispositivo BT
  // e senha — o rótulo do campo (e, pra senha, mascarar o que foi digitado)
  // é o que diferencia visualmente cada caso, senão fica tudo "Nome: ...".
  if (modoEdicaoNome == ModoEdicaoNome::ValidarSenha) {
    char mascara[TAMANHO_MAX_NOME_ARQUIVO + 1];
    const size_t comprimento = std::strlen(nomeArquivo.buffer);
    for (size_t i = 0; i < comprimento; i++) mascara[i] = '*';
    mascara[comprimento] = '\0';
    ihm::desenharTecladoTexto("Senha", mascara, rotulosAlfabeto(), QTD_ALFABETO_NOME,
                              nomeArquivo.indiceAlfabetoAtual);
    return;
  }

  const char* rotuloCampo =
      (modoEdicaoNome == ModoEdicaoNome::RenomearDispositivoBT) ? "Nome BT" : "Nome";
  ihm::desenharTecladoTexto(rotuloCampo, nomeArquivo.buffer, rotulosAlfabeto(), QTD_ALFABETO_NOME,
                            nomeArquivo.indiceAlfabetoAtual);
}

// Mesmo array de ITENS_CONFIGURACOES, só que com "Analise de dados"
// mostrando "ON"/"OFF" — sem isso o usuário só descobria o estado atual
// entrando no item.
void redesenharConfiguracoes() {
  const char* itens[QTD_CONFIGURACOES];
  char rotuloAnalise[24];
  snprintf(rotuloAnalise, sizeof(rotuloAnalise), "Analise de dados: %s",
           configuracoes::analiseDadosHabilitada() ? "ON" : "OFF");

  for (uint8_t i = 0; i < QTD_CONFIGURACOES; i++) {
    itens[i] = (i == INDICE_ANALISE_DADOS_CONFIGURACOES) ? rotuloAnalise : ITENS_CONFIGURACOES[i];
  }

  ihm::desenharListaMenu("Configuracoes", itens, QTD_CONFIGURACOES, estado.indiceSelecionado,
                          estado.offsetRolagem);
}

void redesenharGerenciamentoArquivos() {
  atualizarListaArquivos();

  if (quantidadeArquivosListados == 0) {
    ihm::desenharMensagem("Arquivos",
                          armazenamento::cartaoDisponivel() ? "Nenhum arquivo" : "SD indisponivel");
    return;
  }

  char buffers[MAX_ARQUIVOS_LISTA][24];
  const char* itens[MAX_ARQUIVOS_LISTA + 2];
  for (uint16_t i = 0; i < quantidadeArquivosListados; i++) {
    snprintf(buffers[i], sizeof(buffers[i]), "%s (%lu B)", arquivosListados[i].nome,
             static_cast<unsigned long>(arquivosListados[i].tamanhoBytes));
    itens[i] = buffers[i];
  }
  itens[quantidadeArquivosListados] = "Excluir todos";
  itens[quantidadeArquivosListados + 1] = "Voltar";

  ihm::desenharListaMenu("Arquivos", itens, static_cast<uint8_t>(quantidadeArquivosListados + 2),
                          estado.indiceSelecionado, estado.offsetRolagem);
}

void redesenharArquivoDados() {
  if (quantidadeLinhasDadosArquivo == 0) {
    ihm::desenharMensagem("Ver dados", "Sem dados (ou SD indisponivel)");
    return;
  }

  const char* itens[MAX_LINHAS_DADOS_ARQUIVO + 1];
  for (uint16_t i = 0; i < quantidadeLinhasDadosArquivo; i++) {
    itens[i] = linhasDadosArquivo[i];
  }
  itens[quantidadeLinhasDadosArquivo] = "Voltar";

  ihm::desenharListaMenu("Ver dados", itens, quantidadeLinhasDadosArquivo + 1, estado.indiceSelecionado,
                          estado.offsetRolagem);
}

// Atualiza telas cujo conteúdo muda sozinho, sem entrada do encoder/tecla:
// teste de canais (nível dos sensores) e execução de experimento (tempo,
// eventos). Ambas são redesenhadas em um intervalo fixo, não a cada tick.
void atualizarTelasAoVivo() {
  // LEDs: acompanham o teste de canais tanto local (tela física em
  // Tela::TesteCanais) quanto remoto (app com a tela aberta, ver
  // testeCanaisAtivoRemoto) — as duas fontes usam os mesmos LEDs, nunca ao
  // mesmo tempo na prática (um teste remoto não impede a navegação local
  // para outra tela, mas se o usuário local também entrar em TesteCanais
  // enquanto o app testa, os dois só concordam, já que refletem o mesmo
  // nível físico).
  if (estado.telaAtual == Tela::TesteCanais || testeCanaisAtivoRemoto) {
    for (uint8_t canal1based = 1; canal1based <= NUM_CHANNELS; canal1based++) {
      const uint16_t indiceLed = canal1based - 1;
      if (indiceLed >= NUM_LEDS) break;
      if (aquisicao::nivelAtual(canal1based)) {
        ihm::controlarLED(indiceLed, 200, 0, 0, 30);
      } else {
        ihm::controlarLED(indiceLed, 0, 150, 0, 30);
      }
    }
  }

  if (estado.telaAtual == Tela::TesteCanais) {
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

void redesenharAnaliseLinearResultado() {
  char itemDistancia[32];
  char itemRepeticoes[32];
  char itemVelocidade[32];
  char itemAceleracao[32];
  // Valores-resumo são a média entre todas as repetições do arquivo (ver
  // analise_linear::calcularMediaRepeticoes(), chamada em
  // tratarAnaliseLinearDistancia()).
  snprintf(itemDistancia, sizeof(itemDistancia), "Distancia: %.3fm",
           static_cast<double>(analise_linear::distanciaMediaRepeticoesMetros()));
  snprintf(itemRepeticoes, sizeof(itemRepeticoes), "Repeticoes: %u/%u",
           static_cast<unsigned>(analiseLinearRepeticoesValidas),
           static_cast<unsigned>(analiseLinearTotalRepeticoes));
  snprintf(itemVelocidade, sizeof(itemVelocidade), "Vel. media: %.2fm/s",
           static_cast<double>(analise_linear::velocidadeMediaRepeticoesMs()));
  snprintf(itemAceleracao, sizeof(itemAceleracao), "Acel. media: %.2fm/s2",
           static_cast<double>(analise_linear::aceleracaoMediaRepeticoesMs2()));

  const char* itens[QTD_ANALISE_LINEAR_RESULTADO] = {
      itemDistancia, itemRepeticoes, itemVelocidade, itemAceleracao, "Ver grafico veloc.",
      "Ver grafico acel.", "Voltar"};

  ihm::desenharListaMenu("Resultado", itens, QTD_ANALISE_LINEAR_RESULTADO, estado.indiceSelecionado,
                          estado.offsetRolagem);
}

void redesenharAnaliseLinearEscolherRepeticao() {
  char buffers[MAX_REPETICOES][10];
  const char* itens[MAX_REPETICOES + 2];
  for (uint16_t i = 0; i < analiseLinearTotalRepeticoes; i++) {
    snprintf(buffers[i], sizeof(buffers[i]), "Rep %u", static_cast<unsigned>(i + 1));
    itens[i] = buffers[i];
  }
  itens[analiseLinearTotalRepeticoes] = "Media";
  itens[analiseLinearTotalRepeticoes + 1] = "Voltar";

  ihm::desenharListaMenu("Qual repeticao?", itens, static_cast<uint8_t>(analiseLinearTotalRepeticoes + 2),
                          estado.indiceSelecionado, estado.offsetRolagem);
}

void redesenharAnaliseLinearGrafico() {
  if (analiseLinearPaginaGrafico == PAGINA_GRAFICO_LINEAR_VELOCIDADE) {
    ihm::desenharGrafico("Velocidade (m/s)", analise_linear::temposVelocidadeS(),
                          analise_linear::velocidadesMs(), analise_linear::quantidadeVelocidades());
  } else {
    ihm::desenharGrafico("Aceleracao (m/s2)", analise_linear::temposAceleracaoS(),
                          analise_linear::aceleracoesMs2(), analise_linear::quantidadeAceleracoes());
  }
}

void redesenharAnaliseTipo() {
  static const char* const itens[QTD_ANALISE_TIPO] = {"Analise linear", "Mov. circular", "Voltar"};
  ihm::desenharListaMenu("Tipo de analise", itens, QTD_ANALISE_TIPO, estado.indiceSelecionado,
                          estado.offsetRolagem);
}

void redesenharAnaliseCircularRaioVaos() {
  if (edicaoValor.emEdicao) {
    if (estado.indiceSelecionado == ITEM_CIRCULAR_RAIO) {
      ihm::desenharValorEditavel("Raio", edicaoValor.valorTemp, ANALISE_CIRCULAR_RAIO_MIN_MM,
                                  ANALISE_CIRCULAR_RAIO_MAX_MM, "mm");
    } else {
      ihm::desenharValorEditavel("Vaos", edicaoValor.valorTemp, ANALISE_CIRCULAR_VAOS_MIN,
                                  ANALISE_CIRCULAR_VAOS_MAX);
    }
    return;
  }

  char itemRaio[24];
  char itemVaos[24];
  snprintf(itemRaio, sizeof(itemRaio), "Raio: %ldmm", static_cast<long>(analiseCircularRaioMm));
  snprintf(itemVaos, sizeof(itemVaos), "Vaos: %ld", static_cast<long>(analiseCircularVaosQtd));
  const char* itens[QTD_CIRCULAR_RAIO_VAOS] = {itemRaio, itemVaos, "Calcular", "Voltar"};

  ihm::desenharListaMenu("Raio e vaos", itens, QTD_CIRCULAR_RAIO_VAOS, estado.indiceSelecionado,
                          estado.offsetRolagem);
}

void redesenharAnaliseCircularResultado() {
  char itemDistancia[32];
  char itemRepeticoes[32];
  char itemVelocidade[32];
  char itemAceleracao[32];
  char itemRpm[32];
  // Valores-resumo são a média entre todas as repetições do arquivo (ver
  // analise_circular::calcularMediaRepeticoes(), chamada em "Calcular").
  snprintf(itemDistancia, sizeof(itemDistancia), "Distancia: %.3fm",
           static_cast<double>(analise_circular::distanciaMediaRepeticoesMetros()));
  snprintf(itemRepeticoes, sizeof(itemRepeticoes), "Repeticoes: %u/%u",
           static_cast<unsigned>(analiseCircularRepeticoesValidas),
           static_cast<unsigned>(analiseCircularTotalRepeticoes));
  snprintf(itemVelocidade, sizeof(itemVelocidade), "Vel. media: %.2fm/s",
           static_cast<double>(analise_circular::velocidadeMediaRepeticoesMs()));
  snprintf(itemAceleracao, sizeof(itemAceleracao), "Acel. media: %.2fm/s2",
           static_cast<double>(analise_circular::aceleracaoMediaRepeticoesMs2()));
  snprintf(itemRpm, sizeof(itemRpm), "RPM medio: %.1f",
           static_cast<double>(analise_circular::rpmMediaRepeticoes()));

  const char* itens[QTD_ANALISE_CIRCULAR_RESULTADO] = {
      itemDistancia, itemRepeticoes, itemVelocidade, itemAceleracao, itemRpm,
      "Ver grafico veloc.", "Ver grafico acel.", "Ver grafico rpm", "Voltar"};

  ihm::desenharListaMenu("Resultado", itens, QTD_ANALISE_CIRCULAR_RESULTADO, estado.indiceSelecionado,
                          estado.offsetRolagem);
}

void redesenharAnaliseCircularEscolherRepeticao() {
  char buffers[MAX_REPETICOES][10];
  const char* itens[MAX_REPETICOES + 2];
  for (uint16_t i = 0; i < analiseCircularTotalRepeticoes; i++) {
    snprintf(buffers[i], sizeof(buffers[i]), "Rep %u", static_cast<unsigned>(i + 1));
    itens[i] = buffers[i];
  }
  itens[analiseCircularTotalRepeticoes] = "Media";
  itens[analiseCircularTotalRepeticoes + 1] = "Voltar";

  ihm::desenharListaMenu("Qual repeticao?", itens,
                          static_cast<uint8_t>(analiseCircularTotalRepeticoes + 2), estado.indiceSelecionado,
                          estado.offsetRolagem);
}

void redesenharAnaliseCircularGrafico() {
  if (analiseCircularPaginaGrafico == PAGINA_GRAFICO_VELOCIDADE) {
    ihm::desenharGrafico("Velocidade (m/s)", analise_circular::temposVelocidadeS(),
                          analise_circular::velocidadesMs(), analise_circular::quantidadeVelocidades());
  } else if (analiseCircularPaginaGrafico == PAGINA_GRAFICO_ACELERACAO) {
    ihm::desenharGrafico("Aceleracao (m/s2)", analise_circular::temposAceleracaoS(),
                          analise_circular::aceleracoesMs2(), analise_circular::quantidadeAceleracoes());
  } else {
    ihm::desenharGrafico("RPM", analise_circular::temposRpmS(), analise_circular::rpmValores(),
                          analise_circular::quantidadeRpm());
  }
}

// Confirma, sem poluir a serial, que tick() continua rodando (útil para
// descartar travamento após o boot/autotestes). Só imprime a cada 5s.
void imprimirHeartbeat() {
  static unsigned long ultimoHeartbeatMs = 0;
  const unsigned long agora = millis();
  if (agora - ultimoHeartbeatMs < 5000) return;
  ultimoHeartbeatMs = agora;

  Serial.printf("[SISTEMA] Ativo | Tela=%s | Opcao=%s | Display=%s | SD=%s | BT=%s | Heap=%u\n",
                nomeTela(estado.telaAtual),
                tituloOpcaoMenu(estado.telaAtual, estado.indiceSelecionado),
                ihm::displayDisponivel() ? "OK" : "FALHA",
                armazenamento::cartaoDisponivel() ? "OK" : "FALHA",
                bluetooth_app::conectado() ? "CONECTADO" : "DESCONECTADO",
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
      redesenharConfiguracoes();
      break;
    case Tela::ModoOperacao:
      ihm::desenharListaMenu("Modo de operacao", ITENS_MODO_OPERACAO, QTD_MODO_OPERACAO,
                              estado.indiceSelecionado, estado.offsetRolagem);
      break;
    case Tela::AnaliseDadosToggle:
      ihm::desenharListaMenu("Analise de dados", ITENS_ANALISE_DADOS_TOGGLE,
                              QTD_ANALISE_DADOS_TOGGLE, estado.indiceSelecionado,
                              estado.offsetRolagem);
      break;
    case Tela::Brilho:
      redesenharValorComVoltar("Brilho", configuracoes::brilho(), configuracoes::NIVEL_MINIMO,
                                configuracoes::NIVEL_MAXIMO);
      break;
    case Tela::Volume:
      redesenharValorComVoltar("Volume", configuracoes::volume(), configuracoes::NIVEL_MINIMO,
                                configuracoes::NIVEL_MAXIMO);
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
      redesenharValorComVoltar("Repeticoes", edicaoValor.valorTemp, 1, MAX_REPETICOES);
      break;
    case Tela::ExperimentoExecucao:
      redesenharExperimentoExecucao();
      break;
    case Tela::ExperimentoCancelarConfirmar:
      ihm::desenharConfirmacao("Cancelar experimento?", estado.indiceSelecionado);
      break;
    case Tela::ExperimentoReiniciarConfirmar:
      ihm::desenharConfirmacao("Reiniciar repeticao?", estado.indiceSelecionado);
      break;
    case Tela::ExperimentoNomeArquivo:
    case Tela::ArquivoRenomear:
    case Tela::ConexaoAppRenomear:
    case Tela::SenhaValidar:
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
    case Tela::ArquivosExcluirTodosConfirmar:
      ihm::desenharConfirmacao("Excluir todos os CSVs?", estado.indiceSelecionado);
      break;
    case Tela::ArquivoDados:
      redesenharArquivoDados();
      break;
    case Tela::ConexaoApp:
      redesenharConexaoApp();
      break;
    case Tela::AnaliseSelecionarArquivo:
      redesenharAnaliseSelecionarArquivo();
      break;
    case Tela::AnaliseTipo:
      redesenharAnaliseTipo();
      break;
    case Tela::AnaliseLinearDistancia:
      redesenharValorComVoltar("Distancia", analiseLinearDistanciaCm, ANALISE_LINEAR_DISTANCIA_MIN_CM,
                                ANALISE_LINEAR_DISTANCIA_MAX_CM, "cm");
      break;
    case Tela::AnaliseLinearResultado:
      redesenharAnaliseLinearResultado();
      break;
    case Tela::AnaliseLinearEscolherRepeticao:
      redesenharAnaliseLinearEscolherRepeticao();
      break;
    case Tela::AnaliseLinearGrafico:
      redesenharAnaliseLinearGrafico();
      break;
    case Tela::AnaliseCircularRaioVaos:
      redesenharAnaliseCircularRaioVaos();
      break;
    case Tela::AnaliseCircularResultado:
      redesenharAnaliseCircularResultado();
      break;
    case Tela::AnaliseCircularEscolherRepeticao:
      redesenharAnaliseCircularEscolherRepeticao();
      break;
    case Tela::AnaliseCircularGrafico:
      redesenharAnaliseCircularGrafico();
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
    case EtapaBoot::LogoMonkeyTech:
      if (!etapaBootDesenhada) {
        desenharLogoMonkeyTech();
        etapaBootDesenhada = true;
      }
      // Contado a partir do desenho (BMP lido do SD é mais lento que
      // texto; a tela nunca fica presa aqui).
      if (decorrido >= BOOT_DURACAO_LOGO_MONKEY_TECH_MS) avancarBoot(EtapaBoot::LogoUFRN);
      break;

    case EtapaBoot::LogoUFRN:
      if (!etapaBootDesenhada) {
        desenharLogoUFRN();
        etapaBootDesenhada = true;
      }
      if (decorrido >= BOOT_DURACAO_LOGO_UFRN_MS) avancarBoot(EtapaBoot::LedVermelho);
      break;

    case EtapaBoot::LedVermelho:
      if (!etapaBootDesenhada) {
        definirTodosLeds(200, 0, 0, LED_STARTUP_BRIGHTNESS);
        Serial.println("[LEDS] Todos os 6 LEDs: VERMELHO");
        etapaBootDesenhada = true;
      }
      if (decorrido >= BOOT_DURACAO_LED_VERMELHO_MS) avancarBoot(EtapaBoot::LedAzul);
      break;

    case EtapaBoot::LedAzul:
      if (!etapaBootDesenhada) {
        definirTodosLeds(0, 0, 200, LED_STARTUP_BRIGHTNESS);
        Serial.println("[LEDS] Todos os 6 LEDs: AZUL");
        etapaBootDesenhada = true;
      }
      if (decorrido >= BOOT_DURACAO_LED_AZUL_MS) avancarBoot(EtapaBoot::LedVerde);
      break;

    case EtapaBoot::LedVerde:
      if (!etapaBootDesenhada) {
        definirTodosLeds(0, 200, 0, LED_STARTUP_BRIGHTNESS);
        Serial.println("[LEDS] Todos os 6 LEDs: VERDE");
        etapaBootDesenhada = true;
      }
      if (decorrido >= BOOT_DURACAO_LED_VERDE_MS) avancarBoot(EtapaBoot::LedApagado);
      break;

    case EtapaBoot::LedApagado:
      if (!etapaBootDesenhada) {
        definirTodosLeds(0, 0, 0, 0);
        Serial.println("[LEDS] Todos os 6 LEDs: APAGADOS");
        Serial.println("[LEDS] Teste inicial concluido");
        Serial.println("[LEDS] Controle entregue ao monitoramento dos canais");
        etapaBootDesenhada = true;
      }
      if (decorrido >= BOOT_DURACAO_LED_APAGADO_MS) avancarBoot(EtapaBoot::Desenvolvedor);
      break;

    case EtapaBoot::Desenvolvedor:
      if (!etapaBootDesenhada) {
        desenharTelaDesenvolvedor();
        etapaBootDesenhada = true;
      }
      if (decorrido >= BOOT_DURACAO_DESENVOLVEDOR_MS) avancarBoot(EtapaBoot::Concluido);
      break;

    case EtapaBoot::Concluido:
      Serial.println("[STARTUP] Abrindo menu principal");
      // Confirmação sonora de que a inicialização terminou e o
      // equipamento está pronto (silenciosa se volume==0).
      ihm::beep(200);
      estado.telaAtual = Tela::MenuPrincipal;
      topoPilhaNavegacao = 0;  // pilha vazia: MenuPrincipal é a raiz da navegação
      estado.indiceSelecionado = 0;
      precisaRedesenhar = true;
      break;
  }
}

}  // namespace

void init() {
  estado.telaAtual = Tela::Boot;
  etapaBootAtual = EtapaBoot::LogoMonkeyTech;
  inicioEtapaBootMs = millis();
  etapaBootDesenhada = false;
  Serial.printf("[STARTUP] Estado: %s\n", nomeEtapaBoot(etapaBootAtual));
}

void tick() {
  // Aquisição/armazenamento rodam em tarefa própria no núcleo 0 (ver
  // main.cpp); esta função (tick) roda no núcleo 1, junto com Bluetooth.
  static bool primeiroTick = true;
  if (primeiroTick) {
    Serial.println("[TASK][IHM] Tarefa iniciada");
    primeiroTick = false;
  }

  bluetooth_app::loop();
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
  // Depois de atualizarTelasAoVivo() de propósito: se uma indicação de
  // conexão/desconexão estiver em andamento, ela tem prioridade visual
  // sobre os LEDs de nível do teste de canais, caso as duas coincidam.
  ihm::atualizarIndicacoes();

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
  // Bluetooth os emite hoje — o encoder local só gera
  // Next/Previous/Confirm/Back — mas continuam disponíveis para qualquer
  // origem futura.
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
      // Redesenha sempre (não só quando a tela local é ModoOperacao/Sobre):
      // é barato (redesenharTelaAtual() sempre busca os dados atuais de
      // novo) e garante que a tela física fique sincronizada com qualquer
      // mudança feita pelo app, não só quando o usuário local por acaso
      // está na tela exata que mostra aquele dado.
      precisaRedesenhar = true;
      return;
    case CommandType::SetChannelMode:
      canais::definirModo(cmd.canal, cmd.modo);
      // Sem isto, o app aplicava a mudança de verdade no firmware (NVS/RAM)
      // mas continuava mostrando a config antiga: "channels" só era
      // publicado uma vez, no momento da conexão — a tela de config.
      // individual/todos do app lê exclusivamente desse tópico.
      bluetooth_app::publicarConfiguracaoCanais();
      precisaRedesenhar = true;
      return;
    case CommandType::SetAllChannelsMode:
      canais::definirTodos(cmd.modo);
      bluetooth_app::publicarConfiguracaoCanais();
      precisaRedesenhar = true;
      return;
    case CommandType::RestoreChannelDefaults:
      canais::restaurarPadrao();
      bluetooth_app::publicarConfiguracaoCanais();
      precisaRedesenhar = true;
      return;
    case CommandType::StartExperiment:
      Serial.println("[EXPERIMENTO] Iniciando experimento (comando Bluetooth)");
      if (experimentos::iniciar(static_cast<uint16_t>(cmd.valor > 0 ? cmd.valor : 1))) {
        navegarPara(Tela::ExperimentoExecucao);
      } else {
        Serial.println("[EXPERIMENTO] Falha ao iniciar (SD indisponivel?)");
      }
      return;
    case CommandType::StopExperiment:
    case CommandType::CancelExperiment:
      Serial.println("[EXPERIMENTO] Cancelado/parado (comando Bluetooth)");
      experimentos::cancelar();
      navegarPara(Tela::Experimentos);
      return;
    case CommandType::FinishRepetition:
      Serial.println("[EXPERIMENTO] Finalizando repeticao (comando Bluetooth)");
      experimentos::finalizarRepeticaoAtual();
      precisaRedesenhar = true;
      return;
    case CommandType::RestartRepetition:
      Serial.println("[EXPERIMENTO] Reiniciando repeticao (comando Bluetooth)");
      experimentos::reiniciarRepeticaoAtual();
      precisaRedesenhar = true;
      return;
    case CommandType::ListFiles:
      bluetooth_app::publicarListaArquivos();
      return;
    case CommandType::RenameFile: {
      // Mesmo limite da edição local (TAMANHO_MAX_NOME_ARQUIVO): sem isto,
      // um nome maior vindo do app seria truncado em silêncio pelo
      // snprintf abaixo em vez de ser recusado.
      if (std::strlen(cmd.texto2) > TAMANHO_MAX_NOME_ARQUIVO) return;
      char nomeComExtensao[TAMANHO_MAX_NOME_ARQUIVO + 5];
      snprintf(nomeComExtensao, sizeof(nomeComExtensao), "%s.csv", cmd.texto2);
      armazenamento::renomearArquivo(cmd.texto, nomeComExtensao);
      bluetooth_app::publicarListaArquivos();
      precisaRedesenhar = true;
      return;
    }
    case CommandType::DeleteFile:
      armazenamento::excluirArquivo(cmd.texto);
      bluetooth_app::publicarListaArquivos();
      precisaRedesenhar = true;
      return;
    case CommandType::DeleteAllFiles: {
      const uint16_t excluidos = armazenamento::excluirTodosArquivosCsv();
      Serial.printf("[ARQUIVOS] Excluir todos (comando Bluetooth): %u csv(s) removido(s)\n",
                    static_cast<unsigned>(excluidos));
      bluetooth_app::publicarListaArquivos();
      precisaRedesenhar = true;
      return;
    }
    case CommandType::SaveMeasurementName: {
      // Mesmo caminho de finalizarEdicaoNomeArquivo()/ModoEdicaoNome::
      // SalvarExperimento (menu local), só que sem passar pela tela de
      // edição de texto — o app já manda o nome pronto.
      const size_t comprimento = std::strlen(cmd.texto);
      if (comprimento == 0 || comprimento > TAMANHO_MAX_NOME_ARQUIVO) {
        bluetooth_app::publicarResultadoNomeMedicao(false, false);
        return;
      }

      char nomeComExtensao[TAMANHO_MAX_NOME_ARQUIVO + 5];
      snprintf(nomeComExtensao, sizeof(nomeComExtensao), "%s.csv", cmd.texto);
      const bool sobrescrever = cmd.valor != 0;
      const bool nomeJaExiste = armazenamento::arquivoExiste(nomeComExtensao);
      if (nomeJaExiste && !sobrescrever) {
        // Espelha Tela::ExperimentoSobrescreverConfirmar do menu local: o
        // app decide se pergunta ao usuário e reenvia com sobrescrever=true.
        bluetooth_app::publicarResultadoNomeMedicao(false, true);
        return;
      }

      const bool ok = experimentos::salvarComoArquivoFinal(cmd.texto, sobrescrever);
      bluetooth_app::publicarResultadoNomeMedicao(ok, false);
      if (ok) {
        bluetooth_app::publicarListaArquivos();
        if (estado.telaAtual == Tela::ExperimentoNomeArquivo ||
            estado.telaAtual == Tela::ExperimentoExecucao) {
          navegarPara(Tela::Experimentos);
        } else {
          precisaRedesenhar = true;
        }
      }
      return;
    }
    case CommandType::LoadRepetition:
      analise_dados::carregarRepeticao(cmd.texto, static_cast<uint16_t>(cmd.valor));
      bluetooth_app::publicarEventosAnalise();
      precisaRedesenhar = true;
      return;
    case CommandType::GetChannels:
      // Sob demanda: o app pede isto ao abrir uma tela que exibe a config.
      // de canais, para nunca mostrar um valor obsoleto de antes da conexão
      // (ou de uma mudança local perdida enquanto o app estava fora).
      bluetooth_app::publicarConfiguracaoCanais();
      return;
    case CommandType::ReadFileData:
      // Paginado: cmd.valor é o offset (em linhas de dados) da página
      // pedida pela tabela rolante do app; sem equivalente na tela física.
      bluetooth_app::publicarDadosArquivo(cmd.texto, static_cast<uint16_t>(cmd.valor));
      return;
    case CommandType::SetDeviceName:
      if (!configuracoes::validarSenha(cmd.texto2)) {
        bluetooth_app::publicarResultadoAcaoProtegida("set_device_name", false);
        return;
      }
      bluetooth_app::definirNomeDispositivo(cmd.texto);
      bluetooth_app::publicarResultadoAcaoProtegida("set_device_name", true);
      if (estado.telaAtual == Tela::ConexaoApp) precisaRedesenhar = true;
      return;
    case CommandType::SetDateTime:
      tempo::definirEpoch(static_cast<uint32_t>(cmd.valor));
      return;
    case CommandType::SetDataAnalysisEnabled:
      if (!configuracoes::validarSenha(cmd.texto)) {
        bluetooth_app::publicarResultadoAcaoProtegida("set_data_analysis_enabled", false);
        return;
      }
      configuracoes::definirAnaliseDadosHabilitada(cmd.valor != 0);
      bluetooth_app::publicarResultadoAcaoProtegida("set_data_analysis_enabled", true);
      bluetooth_app::publicarEstado();
      return;
    case CommandType::SetPassword: {
      if (!configuracoes::validarSenha(cmd.texto)) {
        bluetooth_app::publicarResultadoAcaoProtegida("set_password", false);
        return;
      }
      const bool ok = configuracoes::definirSenha(cmd.texto2);
      bluetooth_app::publicarResultadoAcaoProtegida("set_password", ok);
      return;
    }
    case CommandType::SetChannelTestActive:
      testeCanaisAtivoRemoto = (cmd.valor != 0);
      Serial.printf("[EXPERIMENTO] Teste de canais remoto (app): %s\n",
                    testeCanaisAtivoRemoto ? "ativado" : "desativado");
      // Sem tela local também aberta em TesteCanais, nada mais vai zerar os
      // LEDs no próximo tick — sem isto eles ficariam acesos indefinidamente
      // após o app sair da tela.
      if (!testeCanaisAtivoRemoto && estado.telaAtual != Tela::TesteCanais) {
        definirTodosLeds(0, 0, 0, 0);
      }
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
    case Tela::AnaliseDadosToggle:
      tratarAnaliseDadosToggle(cmd);
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
    case Tela::ExperimentoReiniciarConfirmar:
      tratarExperimentoReiniciarConfirmar(cmd);
      break;
    case Tela::ExperimentoNomeArquivo:
    case Tela::ArquivoRenomear:
    case Tela::ConexaoAppRenomear:
    case Tela::SenhaValidar:
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
    case Tela::ArquivosExcluirTodosConfirmar:
      tratarArquivosExcluirTodosConfirmar(cmd);
      break;
    case Tela::ArquivoDados:
      tratarArquivoDados(cmd);
      break;
    case Tela::ConexaoApp:
      tratarConexaoApp(cmd);
      break;
    case Tela::AnaliseSelecionarArquivo:
      tratarAnaliseSelecionarArquivo(cmd);
      break;
    case Tela::AnaliseTipo:
      tratarAnaliseTipo(cmd);
      break;
    case Tela::AnaliseLinearDistancia:
      tratarAnaliseLinearDistancia(cmd);
      break;
    case Tela::AnaliseLinearResultado:
      tratarAnaliseLinearResultado(cmd);
      break;
    case Tela::AnaliseLinearEscolherRepeticao:
      tratarAnaliseLinearEscolherRepeticao(cmd);
      break;
    case Tela::AnaliseLinearGrafico:
      tratarAnaliseLinearGrafico(cmd);
      break;
    case Tela::AnaliseCircularRaioVaos:
      tratarAnaliseCircularRaioVaos(cmd);
      break;
    case Tela::AnaliseCircularResultado:
      tratarAnaliseCircularResultado(cmd);
      break;
    case Tela::AnaliseCircularEscolherRepeticao:
      tratarAnaliseCircularEscolherRepeticao(cmd);
      break;
    case Tela::AnaliseCircularGrafico:
      tratarAnaliseCircularGrafico(cmd);
      break;
    case Tela::Boot:
      break;
    default:
      tratarTelaEmConstrucao(cmd);
      break;
  }
}

Tela telaAtual() { return estado.telaAtual; }

void aoConectarBluetooth() { ihm::iniciarIndicacaoConexao(); }

void aoDesconectarBluetooth() {
  // Sem isto, um app que fecha/derruba a conexão enquanto o teste de canais
  // está aberto (sem mandar "set_channel_test_active":false) deixava os
  // NeoPixels acesos pra sempre — nada mais ia zerá-los. A própria
  // animação de desconexão (pisca amarelo e apaga) já cobre isso; se a
  // tela local estiver em TesteCanais, o próximo tick já reacende os LEDs
  // com o nível de verdade dos canais (ver atualizarTelasAoVivo()).
  testeCanaisAtivoRemoto = false;
  ihm::iniciarIndicacaoDesconexao();
}

}  // namespace maquina_estados
