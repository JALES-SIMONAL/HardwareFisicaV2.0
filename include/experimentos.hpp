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

// Cancela tudo: fecha e descarta o arquivo de trabalho.
void cancelar();

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
