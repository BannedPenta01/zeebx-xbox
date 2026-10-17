#pragma once

#include "pch.h"

struct ZeebxXbox;

// `ZeebxUwp` pela razão explicada em `Renderer.h`: `ZeebxXbox` já é o `struct` opaco da ABI.
namespace ZeebxUwp {

/// Traduz o controle do Xbox para o controle do Zeebo.
///
/// **Os índices vêm do núcleo, por nome.** `zeebx_xbox_botao_por_nome("b1")` é quem diz qual
/// posição é o botão 1; copiar a tabela à mão aqui daria um mapa que envelhece calado quando o
/// núcleo ganha um botão. É o mesmo cuidado que o iOS e o desktop tomam.
class Input {
public:
    Input();

    /// Lê o controle da vez e o entrega ao núcleo. Sem controle conectado, **zera tudo**: um
    /// botão que fica preso porque o controle caiu no meio do quadro é o defeito que faz o
    /// personagem andar sozinho.
    void Aplica(struct ZeebxXbox* app);

    /// Se o botão de menu (o `View`, no controle do Xbox) foi apertado nesta volta e ainda não
    /// foi consumido. Não é entregue ao jogo: é o atalho do menu de pausa do aplicativo.
    bool ConsomeMenu();

private:
    /// Um índice por botão do Zeebo, resolvido no construtor.
    uint32_t m_b1, m_b2, m_b3, m_b4, m_zl, m_zr, m_l2, m_r2;
    uint32_t m_cima, m_baixo, m_esquerda, m_direita, m_home, m_start;
    bool m_menuAnterior = false;
    bool m_menuPendente = false;
    /// A lista de controles conectados, guardada entre voltas.
    ///
    /// **`Gamepads()` monta uma `IVectorView` nova a cada chamada**, e esta chamada é uma por
    /// quadro: era uma alocação por quadro, sessenta por segundo, para achar um controle que não
    /// mudou. Ela é refeita quando não há controle nenhum — o caso de ligar o controle depois do
    /// aplicativo — e quando a leitura de um controle falha, que é o controle que saiu da tomada.
    winrt::Windows::Foundation::Collections::IVectorView<winrt::Windows::Gaming::Input::Gamepad>
        m_controles{nullptr};
};

}  // namespace ZeebxUwp
