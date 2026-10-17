#pragma once

#include "pch.h"

// `ZeebxUwp`, e não `ZeebxXbox`: `ZeebxXbox` sem qualificação já é duas outras coisas — o
// `struct` opaco da ABI (`include/zeebx_xbox.h`) e o namespace do XAML (`winrt::ZeebxXbox`). Um
// tipo e um namespace não podem dividir o nome (C2757), e um `ZeebxXbox::Renderer` daqui de
// dentro resolveria para o namespace do WinRT. Ver `docs/implementacao/25-frontend-xbox-uwp.md`.
namespace ZeebxUwp {

/// O quadro que o núcleo publicou, em BGRA8, com a origem no canto superior esquerdo.
struct QuadroDeVideo {
    uint8_t const* pixels = nullptr;
    uint32_t largura = 0;
    uint32_t altura = 0;
};

/// Sobe o quadro do Zeebo para o `SwapChainPanel`, com tarja preta.
///
/// **A GUI não passa por aqui.** O XAML desenha por cima do painel, e é de propósito: um menu
/// desenhado à mão em D3D custaria fonte, layout e foco, tudo o que o XAML já faz e que num
/// controle de console é justamente o que se quer certo.
class Renderer {
public:
    /// Cria o dispositivo, a cadeia de troca e o `ISwapChainPanelNative` do painel.
    void Inicializar(winrt::Windows::UI::Xaml::Controls::SwapChainPanel const& painel);

    /// O tamanho do painel em DIPs — e só ele.
    ///
    /// **A escala da composição não entra nesta conta, e essa foi a medição que custou uma rodada.**
    /// O `CompositionScaleX/Y` do painel vale para a camada do XAML; o `SwapChainPanel` compõe o que
    /// recebe de nós **um pixel por DIP**. Uma cadeia de 1920x996 px num painel de 1280x664 dip sai
    /// com 1,5x o tamanho e cortada à direita — medido com o padrão de teste, cujo centro apareceu em
    /// 1440 px de uma janela de 1920.
    ///
    /// A escala continua indo para o relatório, porque é ela que diz em que contexto de DPI o console
    /// abriu o aplicativo; o que ela não faz é dimensionar coisa nenhuma.
    void DefineTamanho(float larguraEmDips, float alturaEmDips);

    /// O tamanho em DIPs do alvo que a cadeia de troca tem agora.
    ///
    /// **Existe para o relatório.** "Não tem vídeo" e "tem vídeo no tamanho errado" são a mesma
    /// tela e o mesmo silêncio num console, e a linha do relatório é o que separa os dois.
    uint32_t LarguraDoAlvo() const { return m_larguraDoAlvo; }
    uint32_t AlturaDoAlvo() const { return m_alturaDoAlvo; }

    /// Apresenta um quadro. `proporcao` é a largura sobre a altura desejada (4/3 no console,
    /// 16/9 nas melhorias); `escalaInteira` evita a amostragem fracionária que embaralha pixels.
    void Apresenta(QuadroDeVideo const& quadro, float proporcao, bool escalaInteira);

    /// Larga o que segura a cadeia de troca, para o painel não ficar com um ponteiro morto.
    void Desligar();

private:
    void CriaAlvo();
    void CriaTextura(uint32_t largura, uint32_t altura);
    void CriaTubulacao();

    winrt::com_ptr<ID3D11Device> m_dispositivo;
    winrt::com_ptr<ID3D11DeviceContext> m_contexto;
    winrt::com_ptr<IDXGISwapChain1> m_cadeia;
    winrt::com_ptr<ID3D11RenderTargetView> m_alvo;
    winrt::com_ptr<ID3D11VertexShader> m_vs;
    winrt::com_ptr<ID3D11PixelShader> m_ps;
    /// Como os quatro vértices se ligam à `EntradaDeVertice` do `quad.hlsl`.
    ///
    /// **Sem ele o `Draw` não desenha, e não reclama.** O vertex shader lê `POSITION` e `TEXCOORD0`,
    /// e o D3D11 não rasteriza nada quando o layout de entrada não está posto: o alvo fica com o que
    /// o `Clear` deixou. Foi assim que "não tem vídeo" sobreviveu a duas rodadas — não havia ROM, e
    /// nenhum quadro tinha sido desenhado para o erro aparecer.
    winrt::com_ptr<ID3D11InputLayout> m_entrada;
    winrt::com_ptr<ID3D11SamplerState> m_amostraDePonto;
    winrt::com_ptr<ID3D11SamplerState> m_amostraLinear;
    winrt::com_ptr<ID3D11Texture2D> m_textura;
    winrt::com_ptr<ID3D11ShaderResourceView> m_srv;
    winrt::com_ptr<ID3D11Buffer> m_vertices;
    /// O painel guarda o ponteiro da cadeia de troca; sem soltá-lo no fim, o XAML fica com um
    /// ponteiro para um objeto morto e o fechamento do aplicativo quebra.
    winrt::com_ptr<ISwapChainPanelNative> m_painel;
    uint32_t m_larguraDaTextura = 0;
    uint32_t m_alturaDaTextura = 0;
    float m_larguraEmDips = 0.0f;
    float m_alturaEmDips = 0.0f;
    /// O tamanho que a cadeia de troca tem de fato. Guardado para não redimensionar à toa, e para
    /// o relatório poder dizer o número.
    uint32_t m_larguraDoAlvo = 0;
    uint32_t m_alturaDoAlvo = 0;
};

}  // namespace ZeebxUwp
