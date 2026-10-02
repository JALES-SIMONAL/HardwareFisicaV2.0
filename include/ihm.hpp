#pragma once

#include <stdint.h>

namespace ihm {

void init();

// true se o display inicializou. Quando false, todas as funções de desenho
// abaixo viram no-op (não tocam no driver do display) — o restante do
// firmware (touch, LEDs, sensores, Bluetooth, SD) continua funcionando.
bool displayDisponivel();

// true se a calibração do touch foi carregada/realizada com sucesso. Quando
// false, o firmware continua rodando: a IHM local fica só de leitura (nada
// responde ao toque) e o controle pelo aplicativo Bluetooth continua
// inteiro — a máquina de estados aceita os mesmos comandos das duas origens.
bool toqueDisponivel();

void controlarLED(uint16_t indice, uint8_t vermelho, uint8_t verde, uint8_t azul,
				  uint8_t brilho = 55);

// Define a MESMA cor para todos os NUM_LEDS e chama pixels.show() uma
// única vez, ao final — ao contrário de chamar controlarLED() em loop (que
// chamaria show() uma vez por LED, atualizando-os progressivamente em vez
// de simultaneamente). Use esta função sempre que todos os LEDs devem
// acender juntos (teste inicial, indicação de erro geral etc.).
void controlarTodosLeds(uint8_t vermelho, uint8_t verde, uint8_t azul, uint8_t brilho = 55);

void escreverTelaApp(const char* titulo, const char* valor,
					 const char* rodape = nullptr,
					 bool forcarRedesenho = false);

void escreverTextoTela(const char* texto, int16_t x = 10, int16_t y = 10,
				   uint16_t cor = 0xFFFF, uint8_t tamanho = 2,
				   bool limparTela = false);

// ---------------------------------------------------------------------
// Entrada não-bloqueante (usada pela máquina de estados)
// ---------------------------------------------------------------------
//
// COMO O TOUCH SUBSTITUIU O ENCODER
//
// Na versão anterior (branch main) a entrada local era um encoder rotativo
// com botão: girar gerava Horario/AntiHorario e o botão KEY gerava o
// clique. A máquina de estados traduzia isso em três comandos —
// Next / Previous / Confirm — e é SÓ isso que ela consome; a navegação
// inteira do firmware está construída sobre esse vocabulário.
//
// Este porte manteve exatamente esse vocabulário e trocou apenas quem o
// produz. O toque direto num item de lista não vira "um comando novo": a
// IHM sabe qual item está selecionado (recebe indiceSelecionado ao
// desenhar) e qual foi tocado, e ENFILEIRA a diferença como uma rajada de
// Proximo/Anterior. Do ponto de vista da máquina de estados, é como se o
// usuário tivesse girado o encoder até o item — nenhuma tela precisou ser
// reescrita, e o controle pelo aplicativo Bluetooth continua entrando pelo
// mesmo caminho.
//
// PRESSIONAR MOVE, SOLTAR ACIONA
//
//   - encostar o dedo num item  -> o cursor pula para ele na hora;
//   - soltar sobre o mesmo item -> confirma;
//   - arrastar para fora antes de soltar -> nao aciona nada, e o cursor
//     fica onde parou.
//
// Substituiu a selecao em dois toques. Ela protegia contra erro de mira,
// mas cobrava um toque a mais em CADA escolha; este modelo da a mesma
// protecao de graca, porque o destaque aparece enquanto o dedo ainda esta
// na tela e da tempo de arrastar para fora se o alvo estiver errado. E e o
// comportamento que qualquer interface de toque tem, entao nao precisa ser
// aprendido.
//
// O cursor SALTA para o item tocado, sem passar pelos itens do meio: a
// maquina de estados drena todos os passos no mesmo tick e so entao
// redesenha (ver o laco em maquina_estados::tick()).
//
// ---------------------------------------------------------------------
// GESTOS
// ---------------------------------------------------------------------
//   - arrastar na vertical .......... rola a lista
//   - deslizar para a direita ....... voltar (mesmo efeito do botão <)
//   - arrastar na horizontal
//     na tela de gráfico ............ desloca a janela visível
//   - botões - / + no gráfico ....... afasta / aproxima o zoom
//
// Um arrasto NUNCA aciona a zona de onde partiu: assim que o dedo passa do
// limiar de gesto, o toque é descartado como seleção. Sem isso, rolar uma
// lista também selecionaria o item onde o dedo encostou.
//
// NÃO HÁ PINÇA PARA AMPLIAR, e não é uma omissão: o XPT2046 é resistivo de
// ponto único — com dois dedos na tela ele devolve um ponto no meio dos
// dois, então o gesto é fisicamente indetectável neste hardware. Por isso o
// zoom do gráfico está nos botões - / +, e o arrasto de um dedo faz o
// deslocamento lateral, que é a parte que dá para fazer com um ponto só.
//
// Os eventos saem da fila UM POR TICK e na ordem em que entraram
// (lerEventoNavegacao() só retira Proximo/Anterior; confirmacaoSolicitada()
// só retira Confirmar), então a ordem "move, move, ..., confirma" é
// preservada. Como o redesenho é limitado por UI_UPDATE_INTERVAL_MS, a
// rajada inteira é absorvida em um único redesenho — a tela não pisca item
// por item.

enum class EventoNavegacao : uint8_t { Nenhum, Proximo, Anterior };

// Lê o touch, atualiza o estado interno e alimenta a fila de eventos.
// Deve ser chamada uma vez por tick(), ANTES de lerEventoNavegacao() /
// confirmacaoSolicitada() / voltarSolicitado().
void atualizarToque();

// Próximo evento de navegação da fila (equivalente ao passo do encoder).
EventoNavegacao lerEventoNavegacao();

// true por uma única chamada quando há um Confirmar pendente na fila
// (equivalente ao clique da tecla KEY do encoder).
bool confirmacaoSolicitada();

// true por uma única chamada quando o botão "Voltar" do rodapé foi tocado.
// Não tinha equivalente no encoder — lá só se voltava selecionando o item
// "Voltar" da lista, que continua existindo. A máquina de estados já tinha
// o comando Back (só o Bluetooth o emitia); agora o toque também o emite.
bool voltarSolicitado();

// ---------------------------------------------------------------------
// Brilho (PWM em TFT_BL) e som (buzzer)
// ---------------------------------------------------------------------

// nivel: 0..30 (0 = tela apagada).
void setBrilho(uint8_t nivel);

// nivel: 0..30 (0 = mudo). Não emite som, só define o volume usado por beep().
void setVolume(uint8_t nivel);

// Bipe curto, não-bloqueante; silencioso se o volume estiver em 0.
void beep(uint16_t duracaoMs = 60);

// ---------------------------------------------------------------------
// Indicação de conexão/desconexão BLE (NeoPixels + buzzer)
// ---------------------------------------------------------------------

// Inicia a animação de conexão BLE bem-sucedida: todos os NeoPixels piscam
// em azul duas vezes + dois bipes curtos, tudo dentro de ~1.5s. Não-
// bloqueante — só arma o estado; atualizarIndicacoes() precisa continuar
// sendo chamada a cada tick() (já é, dentro de maquina_estados::tick())
// para a animação progredir sem travar o resto do firmware (toque, BLE,
// display) durante o 1.5s.
void iniciarIndicacaoConexao();

// Inicia a indicação de queda de conexão BLE: todos os NeoPixels piscam em
// amarelo uma única vez. Mesmo padrão não-bloqueante acima.
void iniciarIndicacaoDesconexao();

// Avança a animação em andamento (conexão ou desconexão), se houver —
// barato/no-op quando nenhuma está pendente. Chamar a cada tick().
void atualizarIndicacoes();

// ---------------------------------------------------------------------
// Primitivas gráficas reutilizáveis (coordenadas via layout.hpp)
// ---------------------------------------------------------------------
//
// Cada uma destas funções, além de desenhar, REGISTRA as áreas tocáveis da
// tela que acabou de montar (itens de lista, opções, teclas, botões do
// rodapé). O registro é refeito do zero a cada desenho, então uma tela
// nunca herda as zonas de toque da anterior — o que causaria o clássico
// "toquei aqui e ele ativou outra coisa" depois de trocar de tela.

// Cabeçalho (título) + rodapé (dica/atalho) padronizados.
void desenharCabecalhoRodape(const char* titulo, const char* rodape = nullptr);

// Lista de opções com rolagem automática e destaque do item selecionado.
// "Voltar" deve ser sempre o último item da lista, por convenção do chamador.
// offsetRolagem é ajustado internamente (por referência) para manter o item
// selecionado sempre visível, e o valor ajustado fica disponível para o
// chamador reutilizar no próximo redesenho.
void desenharListaMenu(const char* titulo, const char* const* itens, uint8_t quantidade,
					   uint8_t indiceSelecionado, uint8_t& offsetRolagem);

// Caixa de confirmação Sim/Não.
void desenharConfirmacao(const char* pergunta, uint8_t indiceSelecionado);

// Edição de valor numérico com barra proporcional (brilho, volume, repetições...).
void desenharValorEditavel(const char* titulo, int32_t valor, int32_t minimo,
						   int32_t maximo, const char* unidade = nullptr);

// Lista de texto rolável genérica (visualização de configuração, arquivos...).
void desenharListaRolavel(const char* titulo, const char* const* linhas,
						  uint8_t quantidade, uint8_t offsetRolagem);

// Quantas linhas desenharListaRolavel() mostra de uma vez — para quem rola
// saber o maior offsetRolagem útil.
uint8_t linhasVisiveisListaRolavel();

// Mensagem simples centralizada (avisos, telas de status).
void desenharMensagem(const char* titulo, const char* mensagem);

// Gráfico de linha simples: eixo X = tempo (segundos, "temposS"), eixo Y =
// "valoresY" — ambos escalados automaticamente para caber inteiros na área
// útil do display (entre cabeçalho e rodapé), sem paginação/zoom. "titulo"
// já deve trazer a grandeza/unidade (ex.: "Velocidade (m/s)"); os valores
// mínimo/máximo do eixo Y são escritos nos cantos da área do gráfico.
void desenharGrafico(const char* titulo, const float* temposS, const float* valoresY, uint8_t quantidade);

// Grade genérica de módulos booleanos (usada para desenhar QR Code sem que
// ihm precise conhecer a biblioteca de geração — o chamador fornece um
// callback que responde se o módulo (x,y) está "aceso").
void desenharGradeModulos(const char* titulo, uint8_t dimensao,
                          bool (*modulo)(uint8_t x, uint8_t y));

// Editor de texto em grade tipo teclado: mostra TODOS os símbolos do
// alfabeto de uma vez (em vez de um por vez), com o símbolo atualmente
// selecionado destacado — "rotulos" tem um texto curto por símbolo (ex.:
// "A", "_" para espaço, "OK" para o marcador de fim). O cabeçalho mostra
// "<rotuloCampo>: <valorAtual>" — ex. "Nome: ABC" ou "Senha: ***" —, pra
// deixar claro o que está sendo digitado (nome de arquivo, nome do
// dispositivo BT, senha etc.), já que o mesmo editor é reaproveitado pra
// todos esses casos. Com touch, cada célula é tocável diretamente.
void desenharTecladoTexto(const char* rotuloCampo, const char* valorAtual,
                          const char* const* rotulos, uint8_t quantidade,
                          uint8_t indiceSelecionado);

// ---------------------------------------------------------------------
// Imagem BMP lida do microSD (logotipos de boot)
// ---------------------------------------------------------------------

// Lê "/<nomeComExtensao>" do microSD (via armazenamento::abrirBinarioParaLeitura)
// e desenha, redimensionado por vizinho-mais-próximo (preserva proporção,
// nunca amplia) para caber em até larguraMaxima x alturaMaxima, centralizado
// em (x,y). Suporta BMP não comprimido de 24 ou 32 bits (BI_RGB); qualquer
// outro formato, arquivo ausente ou cartão indisponível retorna false sem
// desenhar nada — quem chama deve ter um retrocesso (ex.: texto) nesse caso.
bool desenharImagemBMP(const char* nomeComExtensao, int16_t x, int16_t y, int16_t larguraMaxima,
                        int16_t alturaMaxima);

// ---------------------------------------------------------------------
// Calibração do touch
// ---------------------------------------------------------------------

// Refaz a calibração dos 4 cantos e regrava na NVS. Bloqueante (espera o
// usuário tocar em cada canto) — só é chamada a partir do menu de
// configurações, nunca durante um experimento em andamento.
void calibrarToque();

}
