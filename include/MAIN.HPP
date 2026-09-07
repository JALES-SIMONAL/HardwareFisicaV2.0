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
// <<< PINAGEM-GERADA-INICIO — nao edite a mao, rode docs/gerar_pinagem.py >>>
// ---------------------------------------------------------------------
// MAPA DE PINOS — ordenado para MINIMIZAR CRUZAMENTO DE FIOS NA PCB
// ---------------------------------------------------------------------
// Tabela completa, com as pontes e os motivos: docs/PINAGEM.md
//
// O criterio nao e o numero do GPIO, e a POSICAO FISICA da via no header
// do DevKitC-1. Dois fios so se cruzam quando a ordem em que saem do
// conector difere da ordem em que chegam ao header — entao cada grupo sobe
// pelo header na sequencia do seu proprio conector. Por isso os GPIO
// parecem fora de ordem: e a ordem das POSICOES que importa aqui.
//
// Posicao: E-04 = fileira esquerda, 4o pino; D-17 = fileira direita, 17o.
// Contagem DE CIMA PARA BAIXO com o conector USB voltado para cima; cada
// fileira tem 22 pinos. Nao e o numero do GPIO nem o do pino do modulo.
//
// As vias sobem junto com os pinos do conector (coluna de GPIO: 5, 6,
// 7, 15, 16, 17, 18, 8, 9), o que corresponde ao modulo montado na
// orientacao normal. Se o display for virado de cabeca para baixo,
// esta ordem passa a cruzar os fios em leque e a coluna precisa ser
// invertida.
//
// Modulo da tela (TJCTM24028-SPI) — coluna esquerda:
//
//   pino  sinal        GPIO   posicao no header
//   ----  -----------  -----  -----------------
//      1  VCC             --  5V
//      2  GND             --  GND
//      3  CS               5  E-05
//      4  RESET            6  E-06
//      5  D/C              7  E-07
//      6  MOSI            15  E-08
//      7  SCK             16  E-09
//      8  LED             17  E-10
//      9  SDO(MISO)       18  E-11
//     10  T_CLK           16  E-09  ponte com o pino 7
//     11  T_CS             8  E-12
//     12  T_DIN           15  E-08  ponte com o pino 6
//     13  T_DO            18  E-11  ponte com o pino 9
//     14  T_IRQ           --  deixar SOLTO
//     15  SD_CS            9  E-15
//     16  SD_MOSI         15  E-08  ponte com o pino 6
//     17  SD_MISO         18  E-11  ponte com o pino 9
//     18  SD_SCK          16  E-09  ponte com o pino 7
//
// Sensores e indicadores — coluna direita:
//
//   Canal 1        GPIO1    D-04
//   Canal 2        GPIO2    D-05
//   Canal 3        GPIO42   D-06
//   Canal 4        GPIO41   D-07
//   Canal 5        GPIO40   D-08
//   Canal 6        GPIO39   D-09
//   NeoPixel DIN   GPIO38   D-10
//   Buzzer +       GPIO47   D-17
//
// T_IRQ (pino 14) fica SOLTO. E uma SAIDA do XPT2046 (open-drain,
// com pull-up no modulo, vai a nivel baixo ao tocar) — amarra-la a
// 3V3 ou GND curto-circuita a saida do controlador. Pino de saida
// nao usado fica desconectado; quem se amarra a trilho e entrada.
//
// Alimentacao (trilhos do header, nao saem de GPIO):
//   3V3  E-01, E-02   sensores dos canais, NeoPixel
//   5V   E-21   VCC do modulo da tela (pino 1)
//   GND  E-22, D-01, D-21, D-22
//        tudo: modulo, sensores, NeoPixel, buzzer
//
// NeoPixel em 3V3 de proposito: o WS2812 pede nivel alto acima de
// 0,7 x VDD no dado. Em 5V isso seria 3,5V e o ESP32 entrega 3,3V —
// ficaria no limite. Em 3V3 o limiar cai para 2,3V.
//
// GPIO reservados nesta placa: 26..32 (flash), 33..37 (PSRAM octal),
// 19/20 (USB nativo), 43/44 (UART0), 0/3/45/46 (strapping), 48 (LED da
// placa). GPIO25 nao existe no S3.
//
// Alimentacao do modulo: com J1 aberto (padrao) VCC = 5V; com J1 fechado,
// 3V3. Os pinos de dados sao 3.3V nos dois casos. Cartao em FAT32.
// <<< PINAGEM-GERADA-FIM >>>
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
constexpr uint8_t PIN_NEO = 38;
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
#define TFT_MISO 18
#endif

#ifndef TFT_MOSI
#define TFT_MOSI 15
#endif

#ifndef TFT_SCLK
#define TFT_SCLK 16
#endif

#ifndef TFT_CS
#define TFT_CS 5
#endif

#ifndef TFT_DC
#define TFT_DC 7
#endif

#ifndef TFT_RST
#define TFT_RST 6
#endif

#ifndef TFT_BL
#define TFT_BL 17
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
constexpr uint8_t CHANNEL_PINS[NUM_CHANNELS] = {1, 2, 42, 41, 40, 39};

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
constexpr uint8_t SD_CS_PIN = 9;

// ---------------------------------------------------------------------
// Buzzer
// ---------------------------------------------------------------------
// Movido do GPIO13 da main: naquele projeto o 13 estava livre, aqui ele e
// o MISO do barramento SPI da tela.
constexpr uint8_t BUZZER_PIN = 47;

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
constexpr int16_t UI_ALTURA_MINIMA_ALVO_TOQUE = 42;

// ---------------------------------------------------------------------
// ORDEM DOS CANAIS DE COR DO PAINEL  <-- AJUSTE AQUI
// ---------------------------------------------------------------------
// Painel errado nas cores? Troque a linha UI_ORDEM_CANAIS abaixo e regrave.
// Vale para TUDO: cores da interface e imagens BMP lidas do cartao, porque
// as duas passam pela mesma funcao cor() logo adiante.
//
// Como descobrir a certa: copie docs/UFRN_teste_ordem.bmp para o cartao com
// o nome UFRN.bmp e veja qual das seis celulas sai com as cores corretas —
// o numero da celula e a ordem a escolher aqui. (Aumente antes o
// BOOT_DURACAO_LOGO_UFRN_MS, mais abaixo, senao a tela passa rapido demais.)
//
// Por que isto vive aqui e nao no platformio.ini: a flag -DTFT_RGB_ORDER
// tambem troca vermelho/azul, mas so isso — ela mexe no bit BGR do MADCTL
// do controlador, que nao alcanca as outras quatro combinacoes. E, por ser
// do TFT_eSPI, tem de ser build flag (a biblioteca e compilada separada e
// nao enxerga este header). Deixe aquela flag em 0 e resolva tudo por aqui.
enum class OrdemCanais : uint8_t { RGB, RBG, GRB, GBR, BRG, BGR };

// MEDIDO NESTE PAINEL: com a constante em RGB, as faixas de cor do boot
// saiam AZUL / VERDE / VERMELHO de cima para baixo, quando o firmware
// desenha vermelho / verde / azul. O verde no lugar certo prova que e uma
// troca pura entre os canais das pontas (no RGB565 o verde e o campo do
// meio, e uma inversao R<->B nao o move) — ou seja, o painel e BGR.
constexpr OrdemCanais UI_ORDEM_CANAIS = OrdemCanais::BGR;

// ---------------------------------------------------------------------
// Conversao 0xRRGGBB -> RGB565, aplicando a ordem escolhida
// ---------------------------------------------------------------------
// As cores sao escritas abaixo em hexadecimal de 24 bits, iguais aos do
// tema do app, e convertidas em tempo de COMPILACAO — nao ha custo em
// execucao. A mesma funcao e usada em execucao pelo leitor de BMP
// (ihm.cpp), o que garante que imagem e interface nunca discordem.
//
// Funcao unica com ternarios encadeados de proposito: em C++11 um
// constexpr precisa ser um unico return, e o projeto compila em gnu++11.
constexpr uint8_t canalVermelho(uint32_t rgb) { return (rgb >> 16) & 0xFF; }
constexpr uint8_t canalVerde(uint32_t rgb) { return (rgb >> 8) & 0xFF; }
constexpr uint8_t canalAzul(uint32_t rgb) { return rgb & 0xFF; }

// RGB565: 5 bits de vermelho, 6 de verde, 5 de azul. Os bits de baixo se
// perdem, entao a cor na tela e uma aproximacao do hex de 24 bits.
constexpr uint16_t empacotar565(uint8_t r, uint8_t g, uint8_t b) {
  return static_cast<uint16_t>(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

constexpr uint16_t cor(uint32_t rgb) {
  return UI_ORDEM_CANAIS == OrdemCanais::RGB
             ? empacotar565(canalVermelho(rgb), canalVerde(rgb), canalAzul(rgb))
         : UI_ORDEM_CANAIS == OrdemCanais::RBG
             ? empacotar565(canalVermelho(rgb), canalAzul(rgb), canalVerde(rgb))
         : UI_ORDEM_CANAIS == OrdemCanais::GRB
             ? empacotar565(canalVerde(rgb), canalVermelho(rgb), canalAzul(rgb))
         : UI_ORDEM_CANAIS == OrdemCanais::GBR
             ? empacotar565(canalVerde(rgb), canalAzul(rgb), canalVermelho(rgb))
         : UI_ORDEM_CANAIS == OrdemCanais::BRG
             ? empacotar565(canalAzul(rgb), canalVermelho(rgb), canalVerde(rgb))
             : empacotar565(canalAzul(rgb), canalVerde(rgb), canalVermelho(rgb));
}

// ---------------------------------------------------------------------
// PALETA — espelha o tema do aplicativo (repo meu_primeiro_app)
// ---------------------------------------------------------------------
// O app e Flutter/Material 3 e monta o tema inteiro a partir de tres cores
// semente tiradas do logo (lib/core/theme/app_colors.dart):
//
//   primary   #04BBD3  ciano/teal do corpo do macaco
//   secondary #1B3A6B  azul-marinho dos contornos
//   tertiary  #F2A61C  amarelo-banana de destaque
//   error     #B3261E  |  nivel logico alto  #2E7D32
//
// Como o MD3 gera uma escala tonal a partir da semente, o valor de
// "primary" no ESQUEMA ESCURO nao e o proprio #04BBD3 e sim um tom mais
// claro dele (#4FD8EE) — e o esquema escuro e o que faz sentido aqui, num
// painel pequeno e retroiluminado. Por isso os valores abaixo sao os tons
// escuros do MD3, nao as sementes cruas.
constexpr uint16_t UI_COR_SUPERFICIE = cor(0x0E1415);        // fundo
constexpr uint16_t UI_COR_CARTAO = cor(0x2B3133);            // fundo do item
constexpr uint16_t UI_COR_CONTORNO = cor(0x3F484A);          // borda do item
constexpr uint16_t UI_COR_TEXTO_PRINCIPAL = cor(0xDEE3E4);
constexpr uint16_t UI_COR_TEXTO_SECUNDARIO = cor(0xBFC8CA);
constexpr uint16_t UI_COR_PRIMARIA = cor(0x4FD8EE);          // ciano (tom escuro MD3)
constexpr uint16_t UI_COR_SOBRE_PRIMARIA = cor(0x003640);    // texto sobre o ciano
constexpr uint16_t UI_COR_TERCIARIA = cor(0xF2A61C);         // amarelo-banana
constexpr uint16_t UI_COR_ERRO = cor(0xB3261E);
constexpr uint16_t UI_COR_SUCESSO = cor(0x2E7D32);

// ---------------------------------------------------------------------
// Papeis da IHM, mapeados na paleta acima
// ---------------------------------------------------------------------
// O cabecalho deixou de ser uma faixa laranja e passou a ser plano, como a
// AppBar do app (backgroundColor: colorScheme.surface, elevation: 0) — a
// separacao vem de uma regua fina, nao de um bloco de cor.
constexpr uint16_t UI_COR_CABECALHO = UI_COR_SUPERFICIE;
constexpr uint16_t UI_COR_TEXTO_CABECALHO = UI_COR_TEXTO_PRINCIPAL;

// Item selecionado: preenchido com a primaria, texto escuro por cima —
// equivalente ao FilledButton/indicador do app. Trocou o laranja sobre
// preto da versao anterior.
constexpr uint16_t UI_COR_SELECIONADO = UI_COR_PRIMARIA;
constexpr uint16_t UI_COR_TEXTO_SELECIONADO = UI_COR_SOBRE_PRIMARIA;

// Botoes do rodape no estilo OutlinedButton do app: fundo do cartao,
// contorno visivel, texto claro.
constexpr uint16_t UI_COR_BOTAO = UI_COR_CARTAO;
constexpr uint16_t UI_COR_TEXTO_BOTAO = UI_COR_TEXTO_PRINCIPAL;
constexpr uint16_t UI_COR_BOTAO_PRESSIONADO = UI_COR_PRIMARIA;

// Contorno que delimita toda area tocavel — o mesmo papel do outlineVariant
// que o Card do app usa em BorderedListTile.
constexpr uint16_t UI_COR_BORDA_TOQUE = UI_COR_CONTORNO;

// Recuo (px) do cartao dentro da area tocavel. A area que RESPONDE ao toque
// e de proposito um pouco maior que o cartao desenhado: erra-se mais para
// fora da borda do que para dentro, entao a folga trabalha a favor de quem
// mira. Tambem e o "margin: bottom 8" entre cards do app.
constexpr int16_t UI_RECUO_BORDA_TOQUE = 3;

// Raio dos cantos arredondados. O app usa 20 em Card e 14 em botao, sobre
// uma tela de celular; reduzido proporcionalmente para os 320px daqui.
constexpr int16_t UI_RAIO_CARTAO = 7;
constexpr int16_t UI_RAIO_BOTAO = 5;

// Marcador ">" a direita de cada item navegavel — o trailing padrao do
// BorderedListTile do app (Icons.chevron_right), que sinaliza "isto abre
// outra tela".
constexpr bool UI_MOSTRAR_SETA_ITEM = true;

// Quando true, todo texto da IHM local é desenhado duas vezes — a segunda
// com 1px de deslocamento horizontal — pra simular negrito ("double
// strike"): a fonte embutida da biblioteca de display não tem uma
// variante bold de verdade. Custo: cada texto leva ~2x o tempo pra
// desenhar (imperceptível no uso normal, a tela já é redesenhada só sob
// demanda).
constexpr bool UI_FONTE_NEGRITO = true;
