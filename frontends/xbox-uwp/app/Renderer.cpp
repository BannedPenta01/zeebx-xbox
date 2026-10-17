#include "pch.h"
#include "Renderer.h"

#include <algorithm>
#include <cmath>

using namespace winrt;
using winrt::Windows::UI::Xaml::Controls::SwapChainPanel;

namespace ZeebxUwp {

namespace {

struct Vertice {
    float x, y, u, v;
};

/// Lê um arquivo do pacote (a pasta de instalação, só de leitura) por Win32.
///
/// **De propósito sem `StorageFile`/`FileIO`.** Aqueles são assíncronos, e esperá-los com
/// `.get()` na linha da interface é pedir um abraço mortal — o `await` volta para a linha que
/// está bloqueada. A pasta de instalação é legível por caminho, e a leitura é síncrona.
///
/// **`CreateFile2`, e não `CreateFileW`.** O `CreateFileW` não existe no recorte de API de um
/// AppContainer: com `WINAPI_FAMILY=WINAPI_FAMILY_APP` ele nem chega a ser declarado, e o erro é
/// `C3861: 'CreateFileW': identifier not found`, que parece um header faltando mas é a família de
/// API. O `CreateFile2` é o mesmo abrir, na forma que a UWP aceita.
std::vector<uint8_t> LeArquivoDoPacote(std::wstring const& nome) {
    std::wstring caminho =
        winrt::Windows::ApplicationModel::Package::Current().InstalledLocation().Path().c_str();
    caminho += L"\\";
    caminho += nome;

    HANDLE arquivo = ::CreateFile2(caminho.c_str(), GENERIC_READ, FILE_SHARE_READ, OPEN_EXISTING,
                                   nullptr);
    if (arquivo == INVALID_HANDLE_VALUE) {
        return {};
    }
    LARGE_INTEGER tamanho{};
    if (!::GetFileSizeEx(arquivo, &tamanho)) {
        ::CloseHandle(arquivo);
        return {};
    }
    std::vector<uint8_t> conteudo(static_cast<size_t>(tamanho.QuadPart));
    DWORD lidos = 0;
    BOOL ok = ::ReadFile(arquivo, conteudo.data(), static_cast<DWORD>(conteudo.size()), &lidos, nullptr);
    ::CloseHandle(arquivo);
    if (!ok) {
        return {};
    }
    conteudo.resize(lidos);
    return conteudo;
}

}  // namespace

void Renderer::Inicializar(SwapChainPanel const& painel) {
    UINT bandeiras = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
#if defined(_DEBUG)
    bandeiras |= D3D11_CREATE_DEVICE_DEBUG;
#endif

    // **Só 11_0.** É o que o Xbox expõe a um aplicativo de UWP (o 12 só com a capacidade de
    // "jogo"), e o 11_0 é o que o `D3D11On12` do console entrega. Pedir uma lista longa de
    // níveis faz o dispositivo nascer com o mais alto disponível e não ajuda em nada aqui.
    D3D_FEATURE_LEVEL niveis[] = {D3D_FEATURE_LEVEL_11_0};
    D3D_FEATURE_LEVEL obtido{};
    winrt::check_hresult(::D3D11CreateDevice(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, bandeiras, niveis, 1, D3D11_SDK_VERSION,
        m_dispositivo.put(), &obtido, m_contexto.put()));

    winrt::com_ptr<IDXGIDevice> dispositivoDxgi;
    winrt::check_hresult(m_dispositivo->QueryInterface(dispositivoDxgi.put()));
    winrt::com_ptr<IDXGIAdapter> adaptador;
    winrt::check_hresult(dispositivoDxgi->GetAdapter(adaptador.put()));
    winrt::com_ptr<IDXGIFactory2> fabrica;
    winrt::check_hresult(adaptador->GetParent(__uuidof(IDXGIFactory2), fabrica.put_void()));

    // **O tamanho pode ter chegado antes de nós.** O `SizeChanged` da raiz dispara na primeira
    // medida, e a primeira medida acontece antes deste `Loaded`. Quando ele já chegou, é ele o
    // número bom, e não o do painel: com a estante à vista o painel do vídeo está colapsado e mede
    // zero, e a cadeia de troca nasceria 1x1.
    if (m_larguraEmDips <= 0.0f || m_alturaEmDips <= 0.0f) {
        m_larguraEmDips = std::max(1.0f, static_cast<float>(painel.ActualWidth()));
        m_alturaEmDips = std::max(1.0f, static_cast<float>(painel.ActualHeight()));
    }

    DXGI_SWAP_CHAIN_DESC1 descricao{};
    // **Em DIPs, e não em pixels.** O `CompositionScale` parecia o número certo para converter os
    // DIPs do painel nos pixels da cadeia, e não é: quem compõe **o `SwapChainPanel` mapeia um pixel
    // da cadeia a um DIP** — a escala do painel vale para a camada do XAML, e não para o que ele
    // recebe de nós. Uma cadeia de 1920x996 px num painel de 1280x664 dip aparece com **1,5x o
    // tamanho** e cortada na borda direita.
    //
    // **Medido, e foi assim que este erro apareceu.** Com a cadeia multiplicada pela escala, o padrão
    // de teste de 320x240 saiu com o centro em 1440 px de uma janela de 1920 — exatamente 960 vezes
    // 1,5 —, o retângulo começando em 444 px em vez de 296, e a tarja direita fora da tela. O
    // tamanho em DIPs devolve os três números ao lugar.
    descricao.Width = static_cast<UINT>(std::lround(std::max(1.0f, m_larguraEmDips)));
    descricao.Height = static_cast<UINT>(std::lround(std::max(1.0f, m_alturaEmDips)));
    descricao.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    descricao.Stereo = false;
    descricao.SampleDesc.Count = 1;
    descricao.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    descricao.BufferCount = 2;
    // **`FLIP_SEQUENTIAL` e `CreateSwapChainForComposition`.** Um `SwapChainPanel` não aceita
    // cadeia de troca de janela: ele compõe a nossa na árvore do XAML, e é isso que permite o
    // menu ser XAML por cima do quadro, em vez de pintado dentro dele.
    descricao.Scaling = DXGI_SCALING_STRETCH;
    descricao.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    descricao.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    winrt::check_hresult(
        fabrica->CreateSwapChainForComposition(m_dispositivo.get(), &descricao, nullptr, m_cadeia.put()));

    // É por esta interface clássica que a cadeia chega ao painel. Guardamos o ponteiro para
    // poder soltá-lo no `Desligar`.
    m_painel = painel.as<ISwapChainPanelNative>();
    winrt::check_hresult(m_painel->SetSwapChain(m_cadeia.get()));

    CriaTubulacao();
    CriaAlvo();
}

void Renderer::CriaTubulacao() {
    auto vs = LeArquivoDoPacote(L"quad.vs.cso");
    auto ps = LeArquivoDoPacote(L"quad.ps.cso");
    if (vs.empty() || ps.empty()) {
        // Sem os bytes do shader não há o que desenhar. Falhar alto é melhor do que apresentar
        // uma tela preta que parece travamento do emulador.
        winrt::throw_hresult(E_FAIL);
    }
    winrt::check_hresult(
        m_dispositivo->CreateVertexShader(vs.data(), vs.size(), nullptr, m_vs.put()));
    winrt::check_hresult(
        m_dispositivo->CreatePixelShader(ps.data(), ps.size(), nullptr, m_ps.put()));

    // **O layout de entrada, sem o qual o `Draw` não desenha nada.** O vertex shader lê `POSITION` e
    // `TEXCOORD0` de um buffer de vértices, e o D3D11 exige o layout declarado: sem ele, a chamada de
    // desenho **não falha e não rasteriza** — o alvo fica com o que o `Clear` deixou, e a tela preta
    // é lida como "não tem vídeo".
    //
    // **Ele era um dos dois bloqueios mudos, e o outro era o alvo de saída.** Nenhum dos dois falha
    // em silêncio no papel: os dois foram achados com um padrão de teste desenhado pela mesma
    // `Apresenta`, e um quadro assim nunca tinha existido antes porque não havia ROM para ele.
    //
    // Os dois campos casam com o `Vertice` do `Renderer.h` e com a `EntradaDeVertice` do
    // `quad.hlsl`, na ordem: `x, y` como posição e `u, v` como textura.
    D3D11_INPUT_ELEMENT_DESC elementos[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, offsetof(Vertice, x),
         D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, offsetof(Vertice, u),
         D3D11_INPUT_PER_VERTEX_DATA, 0},
    };
    winrt::check_hresult(m_dispositivo->CreateInputLayout(elementos, 2, vs.data(), vs.size(),
                                                          m_entrada.put()));

    D3D11_SAMPLER_DESC ponto{};
    ponto.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    ponto.AddressU = ponto.AddressV = ponto.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    ponto.ComparisonFunc = D3D11_COMPARISON_NEVER;
    ponto.MaxLOD = D3D11_FLOAT32_MAX;
    winrt::check_hresult(m_dispositivo->CreateSamplerState(&ponto, m_amostraDePonto.put()));

    D3D11_SAMPLER_DESC linear = ponto;
    linear.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    winrt::check_hresult(m_dispositivo->CreateSamplerState(&linear, m_amostraLinear.put()));

    D3D11_BUFFER_DESC vertices{};
    vertices.ByteWidth = sizeof(Vertice) * 4;
    vertices.Usage = D3D11_USAGE_DYNAMIC;
    vertices.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    vertices.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    winrt::check_hresult(m_dispositivo->CreateBuffer(&vertices, nullptr, m_vertices.put()));
}

void Renderer::CriaAlvo() {
    // O alvo é medido nos mesmos DIPs da cadeia: o painel compõe um por um. Ver o `Inicializar`.
    UINT largura = static_cast<UINT>(std::lround(std::max(1.0f, m_larguraEmDips)));
    UINT altura = static_cast<UINT>(std::lround(std::max(1.0f, m_alturaEmDips)));
    // **Tamanho igual não redimensiona.** O `DefineTamanho` chega a cada mudança de tamanho *e* a
    // cada mudança de escala, e as duas podem cair no mesmo número; um `ResizeBuffers` sem
    // mudança joga fora o alvo e o último quadro à toa.
    if (m_alvo && largura == m_larguraDoAlvo && altura == m_alturaDoAlvo) {
        return;
    }
    m_alvo = nullptr;
    // Redimensionar com um alvo ainda vivo é o defeito que faz o `ResizeBuffers` recusar.
    m_contexto->OMSetRenderTargets(0, nullptr, nullptr);
    winrt::check_hresult(m_cadeia->ResizeBuffers(2, largura, altura, DXGI_FORMAT_B8G8R8A8_UNORM, 0));

    winrt::com_ptr<ID3D11Texture2D> fundo;
    winrt::check_hresult(m_cadeia->GetBuffer(0, __uuidof(ID3D11Texture2D), fundo.put_void()));
    winrt::check_hresult(m_dispositivo->CreateRenderTargetView(fundo.get(), nullptr, m_alvo.put()));
    m_larguraDoAlvo = largura;
    m_alturaDoAlvo = altura;
}

void Renderer::CriaTextura(uint32_t largura, uint32_t altura) {
    m_textura = nullptr;
    m_srv = nullptr;

    D3D11_TEXTURE2D_DESC descricao{};
    descricao.Width = largura;
    descricao.Height = altura;
    descricao.MipLevels = 1;
    descricao.ArraySize = 1;
    // **`B8G8R8A8_UNORM`**, que é o que a conversão do núcleo já produz: sem troca de canal
    // aqui, e sem uma passagem extra.
    descricao.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    descricao.SampleDesc.Count = 1;
    descricao.Usage = D3D11_USAGE_DEFAULT;
    descricao.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    winrt::check_hresult(m_dispositivo->CreateTexture2D(&descricao, nullptr, m_textura.put()));
    winrt::check_hresult(
        m_dispositivo->CreateShaderResourceView(m_textura.get(), nullptr, m_srv.put()));

    m_larguraDaTextura = largura;
    m_alturaDaTextura = altura;
}

void Renderer::DefineTamanho(float larguraEmDips, float alturaEmDips) {
    if (larguraEmDips <= 0.0f || alturaEmDips <= 0.0f) {
        return;
    }
    m_larguraEmDips = larguraEmDips;
    m_alturaEmDips = alturaEmDips;
    // **Este caminho roda antes de existir dispositivo.** O `SizeChanged` da raiz dispara na
    // primeira medida — XAML mede e arranja a árvore antes de carregá-la —, e o dispositivo, a
    // cadeia de troca e o `Inicializar` só vêm no `Loaded`, que é depois. Guardar o tamanho e voltar
    // é o conserto; o `Inicializar` já encontra este número e nasce com ele.
    //
    // **Medido, e não deduzido.** Sem esta guarda o aplicativo morria no primeiro segundo de vida,
    // com o WER dizendo só `0xc0000005, Fault offset 0x2c6dcb` num UWP onde não há depurador para
    // pendurar. O `.map` do ligador diz que `0x2c6dcb` é `CriaAlvo` + 123 bytes: a primeira linha
    // que toca o `m_contexto`, que ali era nulo.
    if (!m_contexto || !m_cadeia) {
        return;
    }
    CriaAlvo();
}

void Renderer::Apresenta(QuadroDeVideo const& quadro, float proporcao, bool escalaInteira) {
    if (!m_alvo || quadro.pixels == nullptr || quadro.largura == 0 || quadro.altura == 0) {
        return;
    }
    if (quadro.largura != m_larguraDaTextura || quadro.altura != m_alturaDaTextura) {
        CriaTextura(quadro.largura, quadro.altura);
    }
    m_contexto->UpdateSubresource(m_textura.get(), 0, nullptr, quadro.pixels,
                                  quadro.largura * 4, 0);

    float painelL = std::max(1.0f, m_larguraEmDips);
    float painelA = std::max(1.0f, m_alturaEmDips);

    // O retângulo: cabe a proporção pedida dentro do painel, com tarja preta no que sobrar.
    float alvoL = painelL;
    float alvoA = painelL / proporcao;
    if (alvoA > painelA) {
        alvoA = painelA;
        alvoL = painelA * proporcao;
    }
    if (escalaInteira) {
        // **O maior múltiplo inteiro que cabe.** Uma escala fracionária de 3,4x transforma o
        // pixel do console numa faixa de 3 e outra de 4: a imagem fica lavada de um jeito que
        // não aparece em captura nenhuma, só na TV.
        float fatorX = painelL / static_cast<float>(quadro.largura);
        float fatorY = painelA / static_cast<float>(quadro.altura);
        float fator = std::floor(std::min(fatorX, fatorY));
        if (fator >= 1.0f) {
            alvoL = static_cast<float>(quadro.largura) * fator;
            alvoA = static_cast<float>(quadro.altura) * fator;
        }
    }

    float deslocamentoX = (painelL - alvoL) * 0.5f;
    float deslocamentoY = (painelA - alvoA) * 0.5f;
    float x0 = (deslocamentoX / painelL) * 2.0f - 1.0f;
    float x1 = ((deslocamentoX + alvoL) / painelL) * 2.0f - 1.0f;
    // O Y do NDC cresce para cima e o da textura para baixo: os dois sinais trocados aqui.
    float y0 = 1.0f - (deslocamentoY / painelA) * 2.0f;
    float y1 = 1.0f - ((deslocamentoY + alvoA) / painelA) * 2.0f;

    D3D11_MAPPED_SUBRESOURCE mapa{};
    if (SUCCEEDED(m_contexto->Map(m_vertices.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapa))) {
        auto* v = static_cast<Vertice*>(mapa.pData);
        v[0] = {x0, y0, 0.0f, 0.0f};
        v[1] = {x1, y0, 1.0f, 0.0f};
        v[2] = {x0, y1, 0.0f, 1.0f};
        v[3] = {x1, y1, 1.0f, 1.0f};
        m_contexto->Unmap(m_vertices.get(), 0);
    }

    const float preto[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    m_contexto->ClearRenderTargetView(m_alvo.get(), preto);
    // **`get()`, e não `put()`.** Esta linha era a única do arquivo que usava `put()` num parâmetro
    // de *entrada*, e o `put()` de um `com_ptr` **solta o ponteiro antes de devolver o endereço
    // dele**: o D3D recebia um alvo nulo. Sem alvo de saída nada do desenho é escrito — o `Clear`
    // não passa por aqui e continuava aparecendo —, e o `Apresenta` do quadro seguinte saía cedo
    // pela guarda do `m_alvo` nulo. Foi isso que produziu a "tela preta sem vídeo" no console: a
    // cadeia de troca compunha, o `Present` apresentava, e o quadro desenhado nunca existiu.
    //
    // **Medido.** Com o `Clear` vermelho, o PC mostrou a tela inteira vermelha e **nenhum** pixel do
    // padrão de teste; o layout de entrada já estava posto e não mudou nada. Trocado o `put()` por
    // `get()`, o padrão apareceu no mesmo build, no lugar certo.
    //
    // As outras ligações do arquivo sempre usaram `get()` — é esta a linha que fugia do padrão.
    ID3D11RenderTargetView* alvos[] = {m_alvo.get()};
    m_contexto->OMSetRenderTargets(1, alvos, nullptr);

    D3D11_VIEWPORT vista{};
    vista.Width = painelL;
    vista.Height = painelA;
    vista.MaxDepth = 1.0f;
    m_contexto->RSSetViewports(1, &vista);

    UINT passo = sizeof(Vertice);
    UINT deslocamento = 0;
    ID3D11Buffer* buffers[] = {m_vertices.get()};
    m_contexto->IASetInputLayout(m_entrada.get());
    m_contexto->IASetVertexBuffers(0, 1, buffers, &passo, &deslocamento);
    m_contexto->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    m_contexto->VSSetShader(m_vs.get(), nullptr, 0);
    m_contexto->PSSetShader(m_ps.get(), nullptr, 0);
    ID3D11ShaderResourceView* vistas[] = {m_srv.get()};
    m_contexto->PSSetShaderResources(0, 1, vistas);
    ID3D11SamplerState* amostras[] = {escalaInteira ? m_amostraDePonto.get() : m_amostraLinear.get()};
    m_contexto->PSSetSamplers(0, 1, amostras);
    m_contexto->Draw(4, 0);

    // Soltar a vista antes de apresentar: a cadeia de troca não pode receber a textura de volta
    // se ela ainda estiver amarrada à saída.
    ID3D11ShaderResourceView* solta[] = {nullptr};
    m_contexto->PSSetShaderResources(0, 1, solta);
    m_cadeia->Present(1, 0);
}

void Renderer::Desligar() {
    if (m_painel && m_cadeia) {
        // Soltar a cadeia do painel ANTES de largar o dispositivo: o painel fica apontando para
        // ela por conta própria, e um `Present` do XAML depois disto é um acesso a objeto morto.
        m_painel->SetSwapChain(nullptr);
    }
    m_painel = nullptr;
    m_alvo = nullptr;
    m_srv = nullptr;
    m_textura = nullptr;
    m_vertices = nullptr;
    m_cadeia = nullptr;
    m_contexto = nullptr;
    m_dispositivo = nullptr;
}

}  // namespace ZeebxUwp
