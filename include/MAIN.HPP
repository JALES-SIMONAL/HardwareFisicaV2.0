// Pinagem oficial desta versao (ESP32-S3-WROOM-1 N16R8) — porte da
// HardwareFisicaV2.0 do ESP32 DOIT DevKit V1 para o ESP32-S3 com display
// SPI + touch resistivo XPT2046 no lugar do ST7735 + encoder rotativo.
//
// ---------------------------------------------------------------------
// REGRA QUE GEROU ESTA PINAGEM
// ---------------------------------------------------------------------
// A pinagem da main foi mantida onde era possivel; onde havia conflito com
// a pinagem da tela touch, a tela venceu (era o criterio definido). Alem
// disso, boa parte da pinagem antiga simplesmente NAO EXISTE no ESP32-S3:
//
//   - GPIO26..32 no S3 sao a flash SPI interna e nao podem ser usados.
//   - GPIO33..37 sao consumidos pela PSRAM octal do modulo N16R8
//     (board_build.arduino.memory_type = qio_opi).
//   - GPIO25 nao existe no S3.
//   - GPIO19/20 sao o USB nativo (D-/D+), usado aqui para o Serial CDC.
//   - GPIO43/44 sao a UART0.
//
// Ou seja, os seis canais de sensores da main ({35,32,33,25,26,27}) eram
// todos inviaveis nesta placa e precisaram ser remapeados. O que deu para
// preservar da main foi preservado: SD_CS (5), NeoPixel (21), quantidade de
// canais/LEDs, baud rate e todas as constantes de comportamento e de
// aparencia da IHM.
//
// ---------------------------------------------------------------------
// MAPA FINAL (nenhum pino repetido)
// ---------------------------------------------------------------------
//   Barramento SPI comum (tela + touch + cartao SD):
//     SCK  12 | MOSI 11 | MISO 13
//   Tela:   CS 10 | DC 9 | RST 14 | BL 15
//   Touch:  T_CS 8 | T_IRQ 7 (nao usado)
//   SD:     CS 5                          <- mantido da main
//   NeoPixel: 21                          <- mantido da main
//   Buzzer:   16                          <- movido (13 virou o MISO)
//   Canais:   1, 2, 4, 6, 17, 18          <- remapeados (ver acima)
#pragma once

#include <Arduino.h>
#include <SPI.h>
#include <SD.h>

// velocidade comunicação serial
constexpr unsigned long SERIAL_BAUD_RATE = 115200;

// ---------------------------------------------------------------------
// Entrada: tela touch (substitui o encoder rotativo da main)
// ---------------------------------------------------------------------
// O encoder (ENC_S1_PIN/ENC_S2_PIN/ENC_KEY_PIN) deixou de existir. A
// navegacao inteira passou para o toque, mas o VOCABULARIO de entrada da
// maquina de estados continua o mesmo (Next/Previous/Confirm/Back) — ver
// ihm.hpp. Os pinos 2, 14 e 15 que o encoder ocupava agora sao,
// respectivamente, canal de sensor, RST da tela e backlight.

// pino físico dos LEDs endereçáveis (NeoPixel)
constexpr uint8_t PIN_NEO = 21;
constexpr uint16_t NUM_LEDS = 6;

// ---------------------------------------------------------------------
// Display TFT + touch (SPI)
// ---------------------------------------------------------------------
// Os valores reais vem do platformio.ini (-DTFT_MOSI=11 etc.), porque o
// TFT_eSPI so aceita a pinagem por build flag — a biblioteca e compilada
// separada do nosso codigo e nao enxerga este header. Os #ifndef abaixo
// existem so como documentacao/retrocesso: se a flag estiver la (esta), o
// valor daqui e ignorado. NAO altere aqui achando que muda a pinagem da
// tela; altere no platformio.ini.
#ifndef TFT_MISO
#define TFT_MISO 13
#endif

#ifndef TFT_MOSI
#define TFT_MOSI 11
#endif

#ifndef TFT_SCLK
#define TFT_SCLK 12
#endif

#ifndef TFT_CS
#define TFT_CS 10
#endif

#ifndef TFT_DC
#define TFT_DC 9
#endif

#ifndef TFT_RST
#define TFT_RST 14
#endif

#ifndef TFT_BL
#define TFT_BL 15
#endif

// Chip select do controlador de touch XPT2046 (mesmo barramento SPI).
#ifndef TOUCH_CS
#define TOUCH_CS 8
#endif

// ---------------------------------------------------------------------
// Canais de sensores
// ---------------------------------------------------------------------
// Quantidade de canais de sensores suportados pelo equipamento.
constexpr uint8_t NUM_CHANNELS = 6;

// Pinagem dos canais 1..6, nesta ordem. Remapeada em relacao a main
// ({35,32,33,25,26,27}) porque nenhum daqueles pinos e utilizavel no
// ESP32-S3 (flash/PSRAM/inexistentes — ver cabecalho). Todos os escolhidos
// aqui sao GPIO livres, com entrada digital e interrupcao normais.
constexpr uint8_t CHANNEL_PINS[NUM_CHANNELS] = {1, 2, 4, 6, 17, 18};

// Resistor de pull interno usado em TODOS os pinos de canal, para
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
// Movido do GPIO13 da main: naquele projeto o 13 estava livre, aqui ele e
// o MISO do barramento SPI da tela.
constexpr uint8_t BUZZER_PIN = 16;

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
// Diagnóstico de boot/display
// ---------------------------------------------------------------------
// Quando true, força o duty do PWM do backlight para o máximo em tempo de
// execução (só no valor aplicado ao ledcWrite), mesmo que o brilho salvo
// na NVS seja baixo/zero — só para diagnóstico. Não altera nem grava o
// valor persistido em Preferences/NVS.
constexpr bool FORCE_MAX_BRIGHTNESS_FOR_DIAGNOSTIC = false;

// Quando true, ihm::setBrilho() NEM CHAMA ledcWrite(): o backlight fica só
// no digitalWrite(TFT_BL, HIGH) já feito em ihm::init(), sem passar pelo
// canal LEDC/PWM. Use isto para descartar totalmente o PWM como causa de um
// backlight apagado (elimina qualquer suspeita de canal/frequência/
// resolução do LEDC).
constexpr bool FORCE_DISPLAY_BACKLIGHT_DIAGNOSTIC = false;

// Intervalo mínimo (ms) entre redesenhos da tela atual — agrupa redesenhos
// muito próximos no tempo (reduz flicker). No porte para touch isto ganhou
// uma segunda utilidade: um toque que salta varios itens de lista de uma
// vez gera uma rajada de eventos Next/Previous, e este intervalo faz a tela
// ser redesenhada UMA vez no fim da rajada, em vez de uma vez por item.
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

// Altura mínima (px) de qualquer elemento que possa ser TOCADO — linha de
// lista, opcao de confirmacao, tecla do teclado de texto, botao do rodape.
// Nao existia na main: com encoder, uma linha de 12px era perfeitamente
// utilizavel, porque a selecao vinha do giro e nao da mira do dedo. Com
// touch resistivo e um dedo adulto, alvos abaixo de ~30px produzem erro de
// selecao constante (o usuario acerta a linha vizinha). layout.cpp usa este
// valor como PISO do espacamento de linha, sem mexer no tamanho da fonte —
// o texto continua do tamanho proporcional calculado, so ganha mais espaco
// em volta. Custo: menos itens por tela, mais rolagem.
constexpr int16_t UI_ALTURA_MINIMA_ALVO_TOQUE = 32;

// Cor (RGB565, formato 5-6-5) do cabeçalho de cada tela — a faixa que
// mostra o título do menu/tela atual — e da barra de preenchimento de
// valores editáveis (brilho, volume, repetições...).
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
// UI_COR_SELECIONADO — precisa contrastar com ela.
constexpr uint16_t UI_COR_TEXTO_SELECIONADO = 0x0000;  // preto

// Cor (RGB565) de fundo dos botoes de toque desenhados no rodape (Voltar,
// -, +, OK). Nao existia na main — la o rodape era so um texto de dica
// ("Gire para ajustar, KEY confirma"), porque nao havia onde tocar.
constexpr uint16_t UI_COR_BOTAO = 0x39E7;         // cinza medio
constexpr uint16_t UI_COR_TEXTO_BOTAO = 0xFFFF;   // branco

// Cor (RGB565) do contorno que delimita TODA area tocavel — item de lista,
// opcao de confirmacao, tecla do teclado e botao do rodape.
//
// Nao existia na versao com encoder e nao faria sentido la: com o giro, a
// area de um item era irrelevante, o que importava era qual estava
// destacado. Com toque, o usuario precisa saber ONDE encostar antes de
// encostar — sem o contorno, um item nao selecionado e so um texto solto no
// preto, e nao ha como adivinhar que a faixa clicavel vai de ponta a ponta
// da tela e e mais alta que a letra.
constexpr uint16_t UI_COR_BORDA_TOQUE = 0x4A69;  // cinza escuro

// Recuo (px) do contorno para dentro da area tocavel. A area que RESPONDE
// ao toque e de proposito um pouco maior que o retangulo desenhado: erra-se
// mais para fora da borda do que para dentro, entao a folga trabalha a
// favor de quem mira.
constexpr int16_t UI_RECUO_BORDA_TOQUE = 2;
// Cor do botao enquanto o dedo esta em cima dele (retorno visual imediato
// do toque — com encoder isso nao fazia sentido, com touch e essencial:
// sem ele o usuario nao sabe se o toque foi registrado).
constexpr uint16_t UI_COR_BOTAO_PRESSIONADO = 0xFD20;

// Quando true, todo texto da IHM local é desenhado duas vezes — a segunda
// com 1px de deslocamento horizontal — pra simular negrito ("double
// strike"): a fonte embutida da biblioteca de display não tem uma
// variante bold de verdade. Custo: cada texto leva ~2x o tempo pra
// desenhar (imperceptível no uso normal, a tela já é redesenhada só sob
// demanda).
constexpr bool UI_FONTE_NEGRITO = true;
