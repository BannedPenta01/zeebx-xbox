#include "pch.h"
#include "Input.h"
#include "zeebx_xbox.h"

using namespace winrt;
using namespace winrt::Windows::Gaming::Input;

namespace ZeebxUwp {

namespace {

/// O índice de um botão do Zeebo, pelo nome. Bate o pé se o núcleo não o conhecer: um botão a
/// menos é um jogo que não dá para jogar, e descobrir isso no console é caro.
uint32_t Indice(char const* nome) {
    int32_t achado = zeebx_xbox_botao_por_nome(nome);
    if (achado < 0) {
        throw hresult_error(E_FAIL, L"o núcleo não conhece o botão");
    }
    return static_cast<uint32_t>(achado);
}

/// O manche cru (-1..1) no curso do console (-128..128), com a zona morta que o iOS também usa.
int32_t Curso(double cru) {
    double valor = std::abs(cru) < 0.15 ? 0.0 : cru;
    return static_cast<int32_t>(std::lround(valor * 128.0));
}

}  // namespace

Input::Input()
    : m_b1(Indice("b1")),
      m_b2(Indice("b2")),
      m_b3(Indice("b3")),
      m_b4(Indice("b4")),
      m_zl(Indice("zl")),
      m_zr(Indice("zr")),
      m_l2(Indice("l2")),
      m_r2(Indice("r2")),
      m_cima(Indice("up")),
      m_baixo(Indice("down")),
      m_esquerda(Indice("left")),
      m_direita(Indice("right")),
      m_home(Indice("back")),
      m_start(Indice("start")) {}

void Input::Aplica(struct ZeebxXbox* app) {
    // **A lista de controles é guardada entre voltas.** Ver o comentário do `m_controles` no
    // header: era uma alocação por quadro.
    if (!m_controles || m_controles.Size() == 0) {
        m_controles = Gamepad::Gamepads();
    }
    GamepadReading leitura{};
    bool tem = false;
    if (m_controles.Size() > 0) {
        try {
            auto controle = m_controles.GetAt(0);
            if (controle) {
                leitura = controle.GetCurrentReading();
                tem = true;
            }
        } catch (hresult_error const&) {
            // **O controle que sai da tomada levanta aqui.** Sem esta guarda a exceção subiria de
            // dentro do laço da composição, que é o lugar de onde nada pode subir: o aplicativo
            // fecha no meio da partida. A lista é esquecida e a próxima volta a refaz.
            m_controles = nullptr;
        }
    }

    auto apertado = [&](GamepadButtons botao) {
        return tem && (static_cast<uint32_t>(leitura.Buttons) & static_cast<uint32_t>(botao)) != 0;
    };

    // Sul é o 1, leste o 2, oeste o 3, norte o 4 — o mesmo mapa do Android e do iOS. O Zeebo
    // numerou os botões em volta do losango, e não com letras.
    zeebx_xbox_botao(app, m_b1, apertado(GamepadButtons::A) ? 1 : 0);
    zeebx_xbox_botao(app, m_b2, apertado(GamepadButtons::B) ? 1 : 0);
    zeebx_xbox_botao(app, m_b3, apertado(GamepadButtons::X) ? 1 : 0);
    zeebx_xbox_botao(app, m_b4, apertado(GamepadButtons::Y) ? 1 : 0);
    zeebx_xbox_botao(app, m_zl, apertado(GamepadButtons::LeftShoulder) ? 1 : 0);
    zeebx_xbox_botao(app, m_zr, apertado(GamepadButtons::RightShoulder) ? 1 : 0);
    // Os gatilhos são botões, e não eixos: o controle do Zeebo tem ZL e ZR como botões, e o
    // `l2`/`r2` do mapa existe para quem joga no controle moderno.
    zeebx_xbox_botao(app, m_l2, leitura.LeftTrigger > 0.5 ? 1 : 0);
    zeebx_xbox_botao(app, m_r2, leitura.RightTrigger > 0.5 ? 1 : 0);
    zeebx_xbox_botao(app, m_cima, apertado(GamepadButtons::DPadUp) ? 1 : 0);
    zeebx_xbox_botao(app, m_baixo, apertado(GamepadButtons::DPadDown) ? 1 : 0);
    zeebx_xbox_botao(app, m_esquerda, apertado(GamepadButtons::DPadLeft) ? 1 : 0);
    zeebx_xbox_botao(app, m_direita, apertado(GamepadButtons::DPadRight) ? 1 : 0);
    // O `Menu` é o HOME do console. O `View` **não** vai para o jogo: é o atalho do menu de pausa
    // daqui, e mandá-lo junto faria o jogo receber um botão que não existe no Zeebo.
    zeebx_xbox_botao(app, m_home, apertado(GamepadButtons::Menu) ? 1 : 0);
    zeebx_xbox_botao(app, m_start, apertado(GamepadButtons::View) ? 1 : 0);

    // O Y do Gamepad cresce para cima; o do console, para baixo.
    zeebx_xbox_eixo(app, 0, Curso(leitura.LeftThumbstickX));
    zeebx_xbox_eixo(app, 1, Curso(-leitura.LeftThumbstickY));
    zeebx_xbox_eixo(app, 2, Curso(leitura.RightThumbstickX));
    zeebx_xbox_eixo(app, 3, Curso(-leitura.RightThumbstickY));

    // A borda do `View`: só a transição conta, senão o menu abriria e fecharia a cada quadro.
    bool menu = apertado(GamepadButtons::View);
    m_menuPendente = menu && !m_menuAnterior;
    m_menuAnterior = menu;
}

bool Input::ConsomeMenu() {
    bool pendente = m_menuPendente;
    m_menuPendente = false;
    return pendente;
}

}  // namespace ZeebxUwp
