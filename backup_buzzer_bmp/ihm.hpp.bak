#pragma once

#include <stdint.h>

namespace ihm {

void init();

// true se display->begin() teve sucesso na inicialização. Quando false,
// todas as funções de desenho abaixo viram no-op (não chamam o ponteiro do
// display) — o restante do firmware (encoder, LEDs, sensores, MQTT, SD)
// continua funcionando normalmente.
bool displayDisponivel();

int readEncoder(int maxPosition);

void controlarLED(uint16_t indice, uint8_t vermelho, uint8_t verde, uint8_t azul,
				  uint8_t brilho = 55);

// Define a MESMA cor para todos os NUM_LEDS e chama pixels.show() uma
// única vez, ao final — ao contrário de chamar controlarLED() em loop (que
// chamaria show() uma vez por LED, atualizando-os progressivamente em vez
// de simultaneamente). Use esta função sempre que todos os LEDs devem
// acender juntos (teste inicial, indicação de erro geral etc.).
void controlarTodosLeds(uint8_t vermelho, uint8_t verde, uint8_t azul, uint8_t brilho = 55);

// Sequência de diagnóstico visual (vermelho/verde/azul/preto + texto),
// usada como uma etapa temporizada da sequência de boot (ver
// maquina_estados::EtapaBoot). Só desenha algo se displayDisponivel().
void executarDiagnosticoVisual();

void escreverTelaApp(const char* titulo, const char* valor,
					 const char* rodape = nullptr,
					 bool forcarRedesenho = false);

void escreverTextoTela(const char* texto, int16_t x = 10, int16_t y = 10,
				   uint16_t cor = 0xFFFF, uint8_t tamanho = 2,
				   bool limparTela = false);

// ---------------------------------------------------------------------
// Entrada não-bloqueante (usada pela máquina de estados)
// ---------------------------------------------------------------------

// Evento discreto de rotação do encoder desde a última chamada.
enum class EventoEncoder : uint8_t { Nenhum, Horario, AntiHorario };

// Independente de readEncoder(); não acumula posição, só reporta o passo
// mais recente (para navegação em listas/edição de valores).
EventoEncoder lerEventoEncoder();

// true por uma única chamada quando a tecla KEY é pressionada e solta
// (debounced, sem repetição enquanto mantida pressionada).
bool teclaClicada();

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
// Primitivas gráficas reutilizáveis (coordenadas via layout.hpp)
// ---------------------------------------------------------------------

// Cabeçalho (título) + rodapé (dica/atalho) padronizados.
void desenharCabecalhoRodape(const char* titulo, const char* rodape = nullptr);

// Lista de opções com rolagem automática e destaque do item selecionado.
// "Voltar" deve ser sempre o último item da lista, por convenção do chamador.
void desenharListaMenu(const char* titulo, const char* const* itens, uint8_t quantidade,
					   uint8_t indiceSelecionado, uint8_t offsetRolagem);

// Caixa de confirmação Sim/Não.
void desenharConfirmacao(const char* pergunta, uint8_t indiceSelecionado);

// Edição de valor numérico com barra proporcional (brilho, volume, repetições...).
void desenharValorEditavel(const char* titulo, int32_t valor, int32_t minimo,
						   int32_t maximo, const char* unidade = nullptr);

// Lista de texto rolável genérica (visualização de configuração, arquivos...).
void desenharListaRolavel(const char* titulo, const char* const* linhas,
						  uint8_t quantidade, uint8_t offsetRolagem);

// Mensagem simples centralizada (avisos, telas de status).
void desenharMensagem(const char* titulo, const char* mensagem);

// Grade genérica de módulos booleanos (usada para desenhar QR Code sem que
// ihm precise conhecer a biblioteca de geração — o chamador fornece um
// callback que responde se o módulo (x,y) está "aceso").
void desenharGradeModulos(const char* titulo, uint8_t dimensao,
                          bool (*modulo)(uint8_t x, uint8_t y));

}