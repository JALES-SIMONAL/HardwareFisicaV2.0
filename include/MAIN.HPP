// Pinagem oficial da PCB (ESP32 DOIT DevKit V1, 38 pinos). Os valores abaixo
// seguem a ordem física dos pinos do módulo (coluna esquerda: 34-35-32-33-
// 25-26-27-14-12-GND-13...; coluna direita: ...23-22-TX0-RX0-21-GND-19-18-5-
// 17-16-4-0-2-15...) de propósito, para que o roteamento das trilhas na
// placa fique o mais simples possível — cada sinal fica no pino físico mais
// próximo do seu vizinho lógico no conector.
//
// Duas alterações em relação à lista original do hardware (ver histórico):
// - NeoPixel: GPIO34 é SOMENTE ENTRADA no ESP32 (sem driver de saída, não
//   tem como gerar o sinal WS2812) — mantido no GPIO21 (pino já usado e
//   testado antes).
// - Buzzer: não estava na lista original e GPIO27 já é o canal 6 — colocado
//   no GPIO13, pino livre logo depois de "key" (14) na mesma coluna física
//   dos canais/indicadores (GPIO12, entre eles, é de strapping e fica de
//   propósito sem uso).
#pragma once

#include <Arduino.h>
#include <SPI.h>
#include <SD.h>

//velocidade comunicação serial
constexpr unsigned long SERIAL_BAUD_RATE = 115200;
// pinos do encoder rotativo
constexpr uint8_t ENC_S1_PIN = 2;
constexpr uint8_t ENC_S2_PIN = 15;
constexpr uint8_t ENC_KEY_PIN = 14;

// pino físico dos LEDs endereçáveis (NeoPixel)
constexpr uint8_t PIN_NEO = 21;
constexpr uint16_t NUM_LEDS = 6;

// pinos físicos do display TFT (SPI)
#ifndef TFT_MISO
#define TFT_MISO 19
#endif

#ifndef TFT_MOSI
#define TFT_MOSI 23
#endif

#ifndef TFT_SCLK
#define TFT_SCLK 22
#endif

#ifndef TFT_CS
#define TFT_CS 18
#endif

#ifndef TFT_DC
#define TFT_DC 17
#endif

#ifndef TFT_RST
#define TFT_RST 16
#endif

#ifndef TFT_BL
#define TFT_BL 4
#endif

// ---------------------------------------------------------------------
// Canais de sensores
// ---------------------------------------------------------------------
// Quantidade de canais de sensores suportados pelo equipamento.
constexpr uint8_t NUM_CHANNELS = 6;

// Pinagem oficial da PCB (canal 1..6, nesta ordem).
constexpr uint8_t CHANNEL_PINS[NUM_CHANNELS] = {35, 32, 33, 25, 26, 27};

// Resistor de pull interno do ESP32 usado em TODOS os pinos de canal, para
// estabilizar o nível quando não há sensor conectado (um pino totalmente
// flutuante capta ruído elétrico e gera transições falsas na interrupção
// CHANGE). true = pull-up (nível ocioso HIGH — use quando o sensor aciona
// puxando a linha para GND, o mais comum em chaves/fototransistores).
// false = pull-down (nível ocioso LOW — use quando o sensor aciona
// puxando a linha para VCC). Ajuste conforme a fiação real dos sensores;
// não tem efeito perceptível num sensor que já aciona ativamente os dois
// níveis (push-pull), só protege o caso de canal sem sensor conectado.
constexpr bool CHANNEL_PULLUP = false;

// ---------------------------------------------------------------------
// Cartao microSD (compartilha o barramento SPI do TFT: MOSI/MISO/SCK)
// ---------------------------------------------------------------------
constexpr uint8_t SD_CS_PIN = 5;

// ---------------------------------------------------------------------
// Buzzer
// ---------------------------------------------------------------------
constexpr uint8_t BUZZER_PIN = 13;

// ---------------------------------------------------------------------
// Filas, buffers e limites configuraveis
// ---------------------------------------------------------------------
// Tamanho da fila de linhas CSV prontas (aquisicao -> tarefa de armazenamento).
// Precisa caber uma repeticao inteira de uma vez: finalizarRepeticaoAtual()
// (experimentos.cpp) enfileira todas as linhas acumuladas da repeticao
// (ate MAX_LINHAS_BUFFER_REPETICAO) mais a linha em branco separadora numa
// rajada só, sem ceder a CPU entre elas. Se a fila fosse menor que isso, o
// excesso falhava silenciosamente ao enfileirar (xQueueSend com timeout 0),
// perdendo eventos de repeticoes com muitos eventos.
constexpr uint16_t CSV_LINE_QUEUE_LEN = 320;
// Quantidade de linhas acumuladas no buffer antes de um flush no microSD.
constexpr uint16_t STORAGE_FLUSH_THRESHOLD = 16;
// Quantidade maxima de repeticoes permitida em um experimento livre.
constexpr uint16_t MAX_REPETICOES = 100;

// ---------------------------------------------------------------------
// Diagnóstico de boot/display (temporário — ajuda a confirmar que o
// display físico está de fato recebendo dados e que o backlight está
// aceso antes de investigar problemas de hardware).
// ---------------------------------------------------------------------
// Quando true, força o duty do PWM do backlight para o máximo em tempo de
// execução (só no valor aplicado ao ledcWrite), mesmo que o brilho salvo
// na NVS seja baixo/zero — só para diagnóstico. Não altera nem grava o
// valor persistido em Preferences/NVS.
constexpr bool FORCE_MAX_BRIGHTNESS_FOR_DIAGNOSTIC = true;

// Quando true, ihm::setBrilho() NEM CHAMA ledcWrite(): o backlight fica só
// no digitalWrite(TFT_BL, HIGH) já feito em ihm::init(), sem passar pelo
// canal LEDC/PWM. Mais forte que FORCE_MAX_BRIGHTNESS_FOR_DIAGNOSTIC — usa
// isto para descartar totalmente o PWM como causa de um backlight apagado
// (elimina qualquer suspeita de canal/frequência/resolução do LEDC).
constexpr bool FORCE_DISPLAY_BACKLIGHT_DIAGNOSTIC = true;

// Intervalo mínimo (ms) entre redesenhos da tela atual — evita que giros
// rápidos do encoder disparem múltiplos redesenhos completos no mesmo
// instante (reduz flicker); não é usado para paginar a leitura do encoder
// em si, só para agrupar redesenhos muito próximos no tempo.
constexpr uint32_t UI_UPDATE_INTERVAL_MS = 16;

// Brilho (0-255) dos NeoPixel durante os testes de diagnóstico (teste
// visual do display e teste inicial dos 6 LEDs), independente do brilho
// normal usado depois pela indicação dos canais.
constexpr uint8_t LED_STARTUP_BRIGHTNESS = 80;

// ---------------------------------------------------------------------
// Duração (ms) de cada tela da sequência de boot (maquina_estados.cpp,
// atualizarBoot()) — ajuste aqui sem precisar mexer no código.
// ---------------------------------------------------------------------
constexpr uint32_t BOOT_DURACAO_LOGO_MONKEY_TECH_MS = 1000;
constexpr uint32_t BOOT_DURACAO_LOGO_UFRN_MS = 1000;
constexpr uint32_t BOOT_DURACAO_LED_VERMELHO_MS = 500;
constexpr uint32_t BOOT_DURACAO_LED_AZUL_MS = 500;
constexpr uint32_t BOOT_DURACAO_LED_VERDE_MS = 500;
constexpr uint32_t BOOT_DURACAO_LED_APAGADO_MS = 150;
constexpr uint32_t BOOT_DURACAO_DESENVOLVEDOR_MS = 1000;

// ---------------------------------------------------------------------
// Aparência da IHM local (display) — ajuste aqui sem precisar mexer em
// layout.cpp/ihm.cpp.
// ---------------------------------------------------------------------
// Multiplicador extra de tamanho de fonte de toda a interface local (1.0 =
// tamanho proporcional normal calculado por layout.cpp; 2.0 = dobro, etc.).
// Aceita fração (ex.: 1.5) — é float de propósito, não uint8_t, senão um
// valor como 1.5 seria truncado silenciosamente para 1 (sem erro de
// compilação, só sem efeito nenhum). Aumentar isto também aumenta
// automaticamente o espaçamento entre linhas de lista e a altura do
// cabeçalho/rodapé (layout.cpp), para o texto maior nunca se sobrepor —
// trade-off: menos itens cabem por tela, mais rolagem.
constexpr float UI_FONT_SIZE_MULTIPLICADOR = 1.0f;

// Cor (RGB565, formato 5-6-5 do Arduino_GFX) do cabeçalho de cada tela — a
// faixa que mostra o título do menu/tela atual — e da barra de
// preenchimento de valores editáveis (brilho, volume, repetições...).
constexpr uint16_t UI_COR_CABECALHO = 0xFD20;  // laranja (#FFA500)

// Cor (RGB565) do texto do título, desenhado por cima de UI_COR_CABECALHO —
// precisa contrastar com ela (preto contrasta melhor que branco com o
// laranja acima).
constexpr uint16_t UI_COR_TEXTO_CABECALHO = 0x0000;  // preto

// Cor (RGB565) do fundo do item atualmente selecionado numa lista/menu/
// teclado de texto. Deliberadamente diferente de UI_COR_CABECALHO acima,
// para dar pra distinguir de relance "em que tela estou" (cabeçalho) de
// "qual item está selecionado" (destaque da lista).
constexpr uint16_t UI_COR_SELECIONADO = 0xFD20;  // laranja (#FFA500)

// Cor (RGB565) do texto do item selecionado, desenhado por cima de
// UI_COR_SELECIONADO — precisa contrastar com ela (branco contrasta melhor
// que preto com o preto acima).
constexpr uint16_t UI_COR_TEXTO_SELECIONADO = 0x0000;  // branco

// Quando true, todo texto da IHM local é desenhado duas vezes — a segunda
// com 1px de deslocamento horizontal — pra simular negrito ("double
// strike"): a fonte embutida da biblioteca de display não tem uma
// variante bold de verdade. Custo: cada texto leva ~2x o tempo pra
// desenhar (imperceptível no uso normal, a tela já é redesenhada só sob
// demanda).
constexpr bool UI_FONTE_NEGRITO = true;
