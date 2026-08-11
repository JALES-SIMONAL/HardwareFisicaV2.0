#pragma once

#include <stdint.h>

// Configurações de nível de dispositivo (brilho, volume, modo de operação)
// persistidas em Preferences/NVS. Usadas tanto pela IHM local quanto pelo
// Bluetooth — sempre pelas mesmas funções.
namespace configuracoes {

enum class ModoOperacao : uint8_t { Hardware = 0, App = 1 };

constexpr uint8_t NIVEL_MINIMO = 0;
constexpr uint8_t NIVEL_MAXIMO = 30;

constexpr const char* NOME_EQUIPAMENTO = "HardwareFisica";
constexpr const char* VERSAO_FIRMWARE = "1.0.0";
constexpr const char* AUTOR = "Wilson Douglas Jales Simonal";

// Nenhum endereço real de manual foi fornecido para o QR Code da tela
// Manual — ajuste esta constante quando o manual estiver publicado.
constexpr const char* MANUAL_URL = "http://PLACEHOLDER-CONFIRMAR/manual";

// Carrega brilho/volume/modo salvos (ou usa os padrões) e já aplica
// brilho/volume atuais na IHM.
void init();

uint8_t brilho();
uint8_t volume();
ModoOperacao modoOperacao();

void definirBrilho(uint8_t nivel);
void definirVolume(uint8_t nivel);
void definirModoOperacao(ModoOperacao modo);

// Protege duas ações sensíveis (trocar o nome anunciado no BLE e
// ativar/desativar o menu "Analise de dados"), local ou pelo app — evita
// que qualquer pessoa que pegue o equipamento/conecte via BLE mude essas
// configurações sem querer. Não é criptografia forte, só um PIN simples;
// guardado em texto puro na NVS, mesmo nível de proteção do resto das
// configurações deste módulo.
constexpr uint8_t SENHA_TAMANHO_MINIMO = 3;
constexpr uint8_t SENHA_TAMANHO_MAXIMO = 10;
constexpr const char* SENHA_PADRAO = "fisica123";

bool analiseDadosHabilitada();
void definirAnaliseDadosHabilitada(bool habilitada);

// Compara com a senha atual (case-sensitive).
bool validarSenha(const char* tentativa);

// Só chamar depois de validarSenha(senhaAtual) ter retornado true. Retorna
// false (sem trocar nada) se o tamanho estiver fora de
// [SENHA_TAMANHO_MINIMO, SENHA_TAMANHO_MAXIMO].
bool definirSenha(const char* novaSenha);

}  // namespace configuracoes
