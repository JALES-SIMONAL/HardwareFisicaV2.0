#pragma once

#include <stdint.h>

// Fluxo do experimento livre: repetições, tempo relativo por repetição,
// contagem de eventos válidos e entrega das linhas prontas para o módulo de
// armazenamento. Consome os eventos filtrados por "aquisicao" (que já
// aplicou canais::isTransitionEnabled).
namespace experimentos {

// Registra o callback em aquisicao para receber os eventos válidos.
void init();

// Abre o arquivo de trabalho no microSD e começa a 1ª repetição. Retorna
// false se o cartão estiver indisponível.
bool iniciar(uint16_t totalRepeticoesSolicitadas);

// Fecha a repetição atual (linha em branco no CSV). Se era a última,
// fecha o arquivo de trabalho e passa para aguardandoNomeArquivo().
void finalizarRepeticaoAtual();

// Descarta só os eventos da repetição ATUAL (ainda não finalizada) e
// reinicia a contagem/timestamp dela do zero — repetições anteriores já
// finalizadas permanecem intactas no arquivo. Só tem efeito durante uma
// repetição em andamento (emAndamento()==true); sem efeito em outra fase.
void reiniciarRepeticaoAtual();

// Cancela tudo: fecha e descarta o arquivo de trabalho.
void cancelar();

// Desliga (após um curto período) os LEDs acesos por aoReceberEventoValido()
// para indicar um evento válido — ver comentário grande em experimentos.cpp.
// Chamada periodicamente pela mesma tarefa que drena a fila de eventos
// (núcleo 0); nunca bloqueia.
void atualizarLedsPiscando();

bool emAndamento();
bool aguardandoNomeArquivo();

uint16_t repeticaoAtual();
uint16_t totalRepeticoes();
uint32_t eventosNaRepeticaoAtual();
int64_t tempoDecorridoUs();

// Só válido quando aguardandoNomeArquivo(). Renomeia o arquivo de trabalho
// para "<nomeSemExtensao>.csv". Retorna false se já existir e
// sobrescrever==false.
bool salvarComoArquivoFinal(const char* nomeSemExtensao, bool sobrescrever);

}  // namespace experimentos
