#include "pch.h"
#include "MainPage.h"
// Gerado pelo `cppwinrt` a partir do `MainPage.idl`, e o par do `MainPage.g.h`: traz a fábrica da
// página (o `winrt_make_ZeebxXbox_MainPage`, que o `module.g.cpp` chama). O modelo do VS inclui
// os dois no `.cpp` da página.
#include "MainPage.g.cpp"

#include "zeebx_xbox.h"

// O relatório do arranque: quem abre e escreve o arquivo é o `App`, e esta página acrescenta linhas
// a ele — o que a caixa do sistema respondeu, o que o núcleo devolveu ao abrir um jogo. Ver o
// `App::Anota`.
#include "App.h"

// O seletor do sistema e o cofre de permissões dele. São duas famílias que o `pch.h` não traz, e
// que só esta tela usa — uma dependência de cabeçalho a menos para todo mundo que compila depois.
#include <winrt/Windows.Storage.AccessCache.h>
#include <winrt/Windows.Storage.Pickers.h>

// A arte dos cartões: pixels crus viram um `SoftwareBitmap` (`Windows.Graphics.Imaging`) e um
// `IBuffer` (`Windows.Storage.Streams`), que é o que o `SoftwareBitmapSource` aceita; o
// `SoftwareBitmapSource` em si é de `Windows.UI.Xaml.Media.Imaging`.
#include <winrt/Windows.Graphics.Imaging.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Windows.UI.Xaml.Media.Imaging.h>

#include <chrono>
#include <string>

// O arquivo dos tokens das pastas escolhidas. Um arquivo comum, e não o `LocalSettings` — a
// explicação está no `CaminhoDosTokens`, e ela nasceu de uma medição.
#include <fstream>

using namespace winrt;
using namespace winrt::Windows::ApplicationModel;
using namespace winrt::Windows::Foundation;
using namespace winrt::Windows::Storage;
using namespace winrt::Windows::Storage::AccessCache;
using namespace winrt::Windows::Storage::Pickers;
using namespace winrt::Windows::Storage::Streams;
using namespace winrt::Windows::Graphics::Imaging;
using namespace winrt::Windows::UI::Xaml::Media::Imaging;
using namespace winrt::Windows::UI::Xaml;
using namespace winrt::Windows::UI::Xaml::Controls;
using namespace winrt::Windows::UI::Xaml::Media;

namespace winrt::ZeebxXbox::implementation {

// **`IInspectable` sem qualificação é ambíguo neste arquivo.** O `using namespace winrt` traz o
// `winrt::IInspectable` (o tipo base da ABI) e o `using namespace winrt::Windows::Foundation` traz
// o `winrt::Windows::Foundation::IInspectable` (a projeção); o compilador trata os dois como nomes
// distintos e acusa `C2872: 'IInspectable': ambiguous symbol` em cada uma das dezesseis definições
// de tratador abaixo. O apelido diz qual dos dois — o mesmo que o `MainPage.h` declara nos
// parâmetros e o mesmo que o conector gerado passa. Repetir o apelido é legal: o próprio
// `MainPage.xaml.g.h` o declara neste mesmo namespace.
using IInspectable = winrt::Windows::Foundation::IInspectable;

namespace {

/// Quantos slots a GUI oferece. O mesmo número do `SLOTS` do lado Rust.
constexpr uint32_t SLOTS = 10;

/// Um `char*` em UTF-8 para o `hstring` do WinRT.
///
/// **Converte à mão, e não com `to_hstring`.** O núcleo devolve os títulos em UTF-8, e
/// `to_hstring(std::string_view)` interpreta os bytes como a página de código do sistema: os
/// acentos dos títulos brasileiros sairiam trocados ("Turma da Mônica" é o caso que aparece logo).
std::wstring DeUtf8(char const* texto) {
    if (!texto || !*texto) {
        return {};
    }
    int tamanho = ::MultiByteToWideChar(CP_UTF8, 0, texto, -1, nullptr, 0);
    if (tamanho <= 1) {
        return {};
    }
    std::wstring saida(static_cast<size_t>(tamanho - 1), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, texto, -1, saida.data(), tamanho);
    return saida;
}

/// O contrário: o caminho do WinRT para o `const char*` que a ABI pede.
///
/// **Toma `wstring_view`, e não `std::wstring`.** Quem chama tem um `hstring` — o caminho que o
/// `ApplicationData::LocalFolder()` devolve —, e o `hstring` se converte sozinho para
/// `wstring_view` e não para `std::wstring`: com o parâmetro em `std::wstring`, as duas chamadas
/// do `OnCarregado` morriam em `C2664: cannot convert argument 1 from 'winrt::hstring' to
/// 'const std::wstring &'`. O tamanho vai explícito, e não por `-1`, porque o `wstring_view` não
/// promete terminador.
std::string ParaUtf8(std::wstring_view texto) {
    if (texto.empty()) {
        return {};
    }
    int tamanho = ::WideCharToMultiByte(CP_UTF8, 0, texto.data(), static_cast<int>(texto.size()),
                                        nullptr, 0, nullptr, nullptr);
    if (tamanho <= 0) {
        return {};
    }
    std::string saida(static_cast<size_t>(tamanho), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, texto.data(), static_cast<int>(texto.size()), saida.data(),
                          tamanho, nullptr, nullptr);
    return saida;
}

/// Troca `{marcador}` por `valor` num texto que veio do catálogo.
///
/// **A troca é feita aqui, e não no núcleo, porque quem chama é o C++.** O `Catalog` do Rust tem
/// um `format` que substitui os marcadores, mas a ABI devolve o texto cru: quem pede o
/// `library.count` recebe `{count} game(s)`, e o número é de quem chamou.
std::wstring ComPosto(std::wstring texto, std::wstring_view marcador, std::wstring_view valor) {
    for (size_t at = texto.find(marcador); at != std::wstring::npos;
         at = texto.find(marcador, at + valor.size())) {
        texto.replace(at, marcador.size(), valor);
    }
    return texto;
}

/// Um número de vírgula em texto curto, para as linhas do relatório.
///
/// **`swprintf_s` e não `std::to_wstring`.** O `to_wstring` de um `float` escreve seis casas
/// ("1.500000"), e o relatório lido no Portal do Dispositivo fica ilegível.
std::wstring Curto(float valor) {
    wchar_t texto[32]{};
    ::swprintf_s(texto, L"%.2f", static_cast<double>(valor));
    return texto;
}

/// O primeiro controle que aceita foco dentro de `raiz`, sem contar a própria `raiz`.
///
/// **Não é o `FocusManager::FindFirstFocusableElement`.** Aquele devolve a própria raiz quando ela
/// é focável, e é exatamente o caso do `GridView` da estante: pedir o primeiro focável dele
/// devolveria ele mesmo — que é quem recusa o foco programático, o defeito que esta função existe
/// para contornar. Aqui a busca começa nos filhos e desce até achar um controle que aceita foco; no
/// cartão, é o `Button` de dentro, que é quem recebe o A.
///
/// A descida é pela árvore **visual**, e não pela lógica: o `Button` do cartão nasce do
/// `DataTemplate`, e a árvore lógica do `GridView` não o alcança sem passar pelos itens — a visual é
/// a que tem o que está na tela.
Control PrimeiroFocavel(DependencyObject const& raiz) {
    try {
        int32_t filhos = VisualTreeHelper::GetChildrenCount(raiz);
        for (int32_t i = 0; i < filhos; ++i) {
            auto filho = VisualTreeHelper::GetChild(raiz, i);
            if (auto controle = filho.try_as<Control>()) {
                if (controle.IsTabStop() && controle.IsEnabled() &&
                    controle.Visibility() == Visibility::Visible) {
                    return controle;
                }
            }
            if (auto neto = PrimeiroFocavel(filho)) {
                return neto;
            }
        }
    } catch (hresult_error const&) {
        // Um contêiner reciclado no meio da descida. Sem resposta o `FocaEstante` cai no plano B;
        // um índice ou um elemento a menos seria pior que isso.
    }
    return nullptr;
}

/// O arquivo onde moram os tokens do `FutureAccessList`: um por linha, em UTF-8.
///
/// **Um arquivo comum, e não o `LocalSettings`.** Medido no PC: o `Insert` no `LocalSettings` não
/// chegou ao `settings.dat` — a gravação ficou no log de transação do registro, e um fechamento
/// brusco a levou embora —, e o arranque seguinte não achou token nenhum: a pasta escolhida sumia
/// sem deixar recado. O `LocalSettings` é um saco sem tipo, cuja gravação não tem hora marcada nem
/// como ser conferida. Um arquivo ao lado do `settings.json` do núcleo tem as duas coisas: dá para
/// lê-lo, e o jogador o vê pelo Portal do Dispositivo no dia em que a pasta não reaparecer.
std::wstring CaminhoDosTokens() {
    return std::wstring{ApplicationData::Current().LocalFolder().Path()} + L"\\fontes.txt";
}

/// Os tokens guardados, na ordem em que foram gravados. Arquivo ausente é lista vazia.
std::vector<std::string> LeTokens() {
    std::vector<std::string> tokens;
    std::ifstream arquivo(CaminhoDosTokens().c_str());
    if (!arquivo) {
        return tokens;
    }
    std::string linha;
    while (std::getline(arquivo, linha)) {
        // O arquivo é escrito com `\n`, mas um editor que passe por aqui pode deixar `\r\n`: um `\r`
        // no fim viraria parte do token, e o `GetFolderAsync` recusaria um token que era válido.
        if (!linha.empty() && linha.back() == '\r') {
            linha.pop_back();
        }
        if (!linha.empty()) {
            tokens.push_back(linha);
        }
    }
    return tokens;
}

/// Reescreve o arquivo dos tokens do zero.
void GravaTokens(std::vector<std::string> const& tokens) {
    std::ofstream arquivo(CaminhoDosTokens().c_str(), std::ios::binary | std::ios::trunc);
    for (auto const& token : tokens) {
        arquivo << token << '\n';
    }
}

}  // namespace

MainPage::MainPage() {
    InitializeComponent();
}

MainPage::~MainPage() {
    // **O "voltar" do sistema sai primeiro.** Um `BackRequested` que chegasse depois do fim da
    // página chamaria um objeto morto; desassinar com o token que a inscrição devolveu é o que
    // fecha essa porta.
    if (m_navegacao) {
        m_navegacao.BackRequested(m_assinaturaDoVoltar);
        m_navegacao = nullptr;
    }
    DesmontaLaco();
    m_audio.Desligar();
    m_renderer.Desligar();
    if (m_app) {
        zeebx_xbox_destroi(m_app);
        m_app = nullptr;
    }
}

std::wstring MainPage::Texto(std::string_view chave) {
    if (!m_app) {
        return {};
    }
    // **A cópia para `std::wstring` é obrigatória, e não uma otimização.** A ABI devolve um
    // ponteiro que vale até a chamada seguinte: guardar dois ponteiros e ler os dois depois
    // devolveria o segundo texto no primeiro ponteiro. O `DeUtf8` copia aqui, na hora.
    return DeUtf8(zeebx_xbox_texto(m_app, std::string(chave).c_str()));
}

hstring MainPage::Rotulo(std::string_view chave) {
    return hstring(Texto(chave));
}

void MainPage::OnCarregado(IInspectable const&, RoutedEventArgs const&) {
    // O caminho onde o aplicativo pode escrever. É a `LocalState` do pacote: o único lugar que
    // um UWP pode gravar, e o que o Portal do Dispositivo expõe para o jogador pôr as ROMs.
    auto dados = ApplicationData::Current().LocalFolder().Path();
    auto roms = dados + L"\\roms";
    auto dadosUtf8 = ParaUtf8(dados);
    auto romsUtf8 = ParaUtf8(roms);

    m_app = zeebx_xbox_cria(romsUtf8.c_str(), dadosUtf8.c_str());
    if (!m_app) {
        // **Este aviso sai em português, e é o único.** Nem o núcleo existe para responder uma
        // chave: a `LocalState` não abriu, e não há catálogo de onde tirar frase nenhuma.
        MostraAviso(L"Não deu para iniciar", L"A pasta de dados do aplicativo não pôde ser aberta.");
        return;
    }

    // Daqui para baixo a língua é a que o núcleo diz, e toda frase desta tela vem do catálogo
    // dele — o `AplicaTextos` é o único lugar deste arquivo que escreve texto na tela.
    AplicaTextos();

    try {
        m_renderer.Inicializar(Video());
    } catch (hresult_error const& erro) {
        MostraAviso(Texto("xbox.failed.video"),
                    ComPosto(Texto("xbox.failed.video_reason"), L"{reason}", erro.message().c_str()));
        return;
    }
    // **O tamanho, agora que o dispositivo existe.** O `SizeChanged` da raiz disparou antes deste
    // `Loaded` e só pôde guardar os números: sem cadeia de troca não havia o que redimensionar. A
    // chamada aqui é o que faz a cadeia nascer com o tamanho realmente medido — e é dela que sai a
    // linha do relatório com os três números.
    AplicaTamanhoDoVideo();
    // **A margem segura, e só depois de haver tamanho.** Ela sai do tamanho medido da raiz, que
    // aqui já existe; aplicada antes, sairia de zero.
    AplicaMargemSegura();
    // A placa de som é opcional: sem ela o jogo roda mudo, e é isso que o `Inicializar` devolve
    // `false` para dizer. Não é motivo para não abrir a estante.
    m_audio.Inicializar(zeebx_xbox_taxa_de_audio());

    // **A varredura não é chamada aqui.** Ela é pedida no fim do `ReataFontes`: as pastas do
    // seletor só voltam a valer depois que o `FutureAccessList` as reabre, e varrer antes disso
    // seria varrer sem enxergar a unidade de fora — o defeito de "os jogos do HD não aparecem até
    // apertar varrer de novo" pela primeira vez.
    AtualizaEstadoDosSlots();

    auto pacote = Package::Current();
    auto versao = pacote.Id().Version();
    auto numero = std::to_wstring(versao.Major) + L"." + std::to_wstring(versao.Minor) + L"." +
                  std::to_wstring(versao.Build);
    VersaoAplicativo().Text(ComPosto(Texto("about.version"), L"{version}", numero));

    // As pastas do seletor do sistema voltam a valer: o `FutureAccessList` é quem as reabre, e a
    // lista que ele devolve fica viva na `m_fontesVivas`. Ver o comentário do `ReataFontes`.
    ReataFontes();

    // O "voltar" do sistema — no console, o B. **É aqui que ele é ouvido, e não no XAML**: o Xbox
    // levanta este evento por fora da árvore, e um aplicativo que não o consome é fechado por ele.
    if (!m_navegacao) {
        m_navegacao = winrt::Windows::UI::Core::SystemNavigationManager::GetForCurrentView();
        m_assinaturaDoVoltar = m_navegacao.BackRequested({this, &MainPage::OnVoltarDoSistema});
    }

    FocaEstante();
}

void MainPage::OnTamanhoMudou(IInspectable const&, SizeChangedEventArgs const&) {
    AplicaTamanhoDoVideo();
    AplicaMargemSegura();
}

void MainPage::OnEscalaDaComposicaoMudou(SwapChainPanel const&, IInspectable const&) {
    // O painel mudou de escala de composição: o mesmo tamanho lógico vale outro número de pixels.
    AplicaTamanhoDoVideo();
}

void MainPage::AplicaTamanhoDoVideo() {
    // **O tamanho vem da raiz.** O painel do vídeo está colapsado enquanto a estante aparece e
    // mediria zero; como ele preenche a raiz, os dois coincidem quando ele aparece.
    //
    // **A escala da composição é lida só para o relatório.** Ela não dimensiona nada: o
    // `SwapChainPanel` compõe um pixel da cadeia por DIP, e quem multiplicava os DIPs por ela saía
    // com o quadro 1,5x maior e cortado à direita neste PC. Ver o `Renderer::DefineTamanho`, que
    // carrega a medição.
    float escalaX = static_cast<float>(Video().CompositionScaleX());
    float escalaY = static_cast<float>(Video().CompositionScaleY());
    if (escalaX <= 0.0f || escalaY <= 0.0f) {
        // Painel ainda não composto: vale o que o XAML diz, para a linha do relatório não sair com
        // zero. A conta do tamanho não depende disto.
        if (auto raizXaml = Raiz().XamlRoot()) {
            escalaX = escalaY = static_cast<float>(raizXaml.RasterizationScale());
        }
    }
    float larguraEmDips = static_cast<float>(Raiz().ActualWidth());
    float alturaEmDips = static_cast<float>(Raiz().ActualHeight());
    m_renderer.DefineTamanho(larguraEmDips, alturaEmDips);

    // **A linha que diz de que tamanho o quadro nasceu.** "Não tem vídeo" e "tem vídeo no tamanho
    // errado" são a mesma tela e o mesmo silêncio num console, e é esta linha que separa os dois:
    // o tamanho lógico da raiz, a escala que o painel pede, e o que a cadeia de troca ficou. Ela
    // sai a cada mudança de tamanho — e não só na primeira —, porque no console o tamanho certo
    // chega **depois** do primeiro: é o `CompositionScaleChanged` que o traz.
    uint32_t alvoL = m_renderer.LarguraDoAlvo();
    uint32_t alvoA = m_renderer.AlturaDoAlvo();
    if (alvoL > 0 && (alvoL != m_larguraAnotada || alvoA != m_alturaAnotada)) {
        m_larguraAnotada = alvoL;
        m_alturaAnotada = alvoA;
        App::Anota(L"video: raiz " + Curto(larguraEmDips) + L"x" + Curto(alturaEmDips) +
                   L" dip, escala do painel " + Curto(escalaX) + L"x" + Curto(escalaY) +
                   L", cadeia de troca " + std::to_wstring(alvoL) + L"x" + std::to_wstring(alvoA) +
                   L" dip");
    }
}

void MainPage::Mostra(UIElement const& alvo) {
    // **O explorador de fontes entra na lista das camadas.** Sem ele aqui, abrir as fontes
    // deixaria a estante visível por baixo, e o foco do controle andaria nas duas.
    UIElement camadas[] = {Video(), Biblioteca(), Fontes(), Configuracoes(), Sobre()};
    for (auto const& camada : camadas) {
        camada.Visibility(camada == alvo ? Visibility::Visible : Visibility::Collapsed);
    }
    // A pausa é uma sobreposição: ela não troca de camada, ela cobre a do jogo.
    if (alvo != Video()) {
        Pausa().Visibility(Visibility::Collapsed);
    }
}

void MainPage::AplicaMargemSegura() {
    // **Uma fração do lado menor, e não um número de DIPs.** É o que uma TV corta de qualquer
    // imagem, o overscan, e a referência da própria Microsoft para TV é da ordem de 5%. O que
    // estava a 20 e a 28 DIPs da borda — a linha das dicas, os contadores, o pé das folhas —
    // ficava exatamente na faixa que a TV come: era daí que vinha "o menu cortado".
    //
    // **O quadro do jogo fica de fora.** Ele é a tela inteira do console, e recuá-lo seria uma
    // moldura preta em volta do que o jogo desenhou.
    float largura = static_cast<float>(Raiz().ActualWidth());
    float altura = static_cast<float>(Raiz().ActualHeight());
    if (largura <= 0.0f || altura <= 0.0f) {
        return;
    }
    float margem = std::clamp(std::min(largura, altura) * 0.05f, 12.0f, 48.0f);
    Thickness recuo{margem, margem, margem, margem};
    // **A lista é de `FrameworkElement`, e não um `auto` de chaves.** Os tipos são diferentes — três
    // `Grid` e um `Border` —, e lista de chaves com tipos diferentes não deduz tipo nenhum. O
    // `Margin` mora no `FrameworkElement`, e não no `UIElement`.
    FrameworkElement camadas[] = {Biblioteca(), Fontes(), Configuracoes(), Sobre()};
    for (auto const& camada : camadas) {
        camada.Margin(recuo);
    }
    // A caixa da pausa recua, mas a cortina dela não: a cortina escurece o quadro inteiro, e com
    // margem sobraria uma moldura clara em volta da tela escurecida.
    CaixaDaPausa().Margin(recuo);
    // Os contadores ficam encostados no canto: o recuo deles é o de sempre mais o seguro.
    Contadores().Margin(Thickness{0.0, 0.0, margem + 12.0f, margem + 8.0f});
}

void MainPage::MostraOcupado(std::string_view chave) {
    OcupadoTexto().Text(Rotulo(chave));
    Rolo().IsActive(true);
    Ocupado().Visibility(Visibility::Visible);
    // **Qual camada estava à vista, antes de escondê-las.** É para ela que o `EscondeOcupado`
    // devolve o jogador: quem estava em Fontes quando a pasta entrou volta para Fontes, e não para
    // a estante.
    if (!m_camadaDoOcupado) {
        for (auto const& camada : {Biblioteca(), Fontes(), Configuracoes(), Sobre()}) {
            if (camada.Visibility() == Visibility::Visible) {
                m_camadaDoOcupado = camada;
                break;
            }
        }
    }
    // **As camadas de tela cheia saem de cena enquanto o aviso está de pé.** Não é por estética: o
    // direcional anda por todo elemento visível da árvore, e um `Grid` não tem como ser desligado —
    // `IsEnabled` é do `Control`, e painel não é `Control`. Com as camadas colapsadas, o que o
    // controle alcança é o próprio aviso, e o A não cai num botão que ninguém está vendo.
    Mostra(Ocupado());
}

void MainPage::EscondeOcupado() {
    Ocupado().Visibility(Visibility::Collapsed);
    Rolo().IsActive(false);
    if (m_camadaDoOcupado) {
        Mostra(m_camadaDoOcupado);
        m_camadaDoOcupado = nullptr;
    }
}

void MainPage::ComecaVarredura() {
    if (!m_app) {
        return;
    }
    // **A lista antiga fica na tela até a nova entrar.** Esvaziar a estante no começo da varredura
    // faria a tela piscar vazia a cada "Varrer de novo"; o trabalho é o mesmo, e quem olha já tem
    // o que olhar.
    if (zeebx_xbox_varre_inicia(m_app) != 0) {
        // Já havia uma andando — quem pediu de novo espera a que está lá. Dois fios lendo a mesma
        // unidade ao mesmo tempo é o que esta saída evita.
        return;
    }
    m_varrendo = true;
    m_inicioDaVarredura = ::GetTickCount64();
    MostraOcupado("library.scanning");
    if (!m_relogioDaVarredura) {
        m_relogioDaVarredura = DispatcherTimer();
        // 120 ms: a varredura de uma pasta pequena termina em menos que isto, e perguntar mais
        // rápido que isso gastaria mais do que a própria varredura. O que se quer não é precisão
        // de relógio, é a tela voltar assim que houver resposta.
        m_relogioDaVarredura.Interval(TimeSpan{std::chrono::milliseconds(120)});
        m_relogioDaVarredura.Tick({this, &MainPage::OnRelogioDaVarredura});
    }
    m_relogioDaVarredura.Start();
    App::Anota(L"varredura de fora comecou");
}

void MainPage::OnRelogioDaVarredura(IInspectable const&, IInspectable const&) {
    if (!m_app || !m_varrendo) {
        return;
    }
    int32_t colhido = zeebx_xbox_varre_colhe(m_app);
    if (colhido == 0) {
        return;
    }
    m_varrendo = false;
    if (m_relogioDaVarredura) {
        m_relogioDaVarredura.Stop();
    }
    EscondeOcupado();
    if (colhido == 1) {
        MontaEstante();
        // A linha do relatório: num console ela é a resposta a "os jogos não aparecem", e o
        // número que ela traz separa "a varredura não achou nada" de "a varredura não rodou". O
        // tempo é a outra metade: ele diz se a espera é a varredura ou o resto.
        uint64_t gasto = ::GetTickCount64() - m_inicioDaVarredura;
        App::Anota(L"varredura de fora: " + std::to_wstring(m_quantidade) + L" jogo(s) em " +
                   std::to_wstring(gasto) + L" ms");
        // Apontar para a pasta errada é o erro mais provável da tela de fontes, e o resultado dele
        // é uma varredura que passa e não acha nada. O recado diz para onde olhar em vez de deixar a
        // dúvida, e sai de cena quando há jogo: quem acrescentou uma pasta e viu os jogos aparecerem
        // não precisa de aviso nenhum. **É aqui, e não no `AcrescentaCaminho`**, porque é aqui que o
        // número existe.
        if (Fontes().Visibility() == Visibility::Visible) {
            FontesRecado().Text(m_quantidade == 0 ? Rotulo("xbox.no_games.hint") : hstring{});
        }
        if (Biblioteca().Visibility() == Visibility::Visible) {
            FocaEstante();
        }
    }
}

void MainPage::MontaEstante() {
    m_quantidade = m_app ? zeebx_xbox_jogos(m_app) : 0;
    auto itens = single_threaded_observable_vector<hstring>();
    for (int32_t i = 0; i < m_quantidade; ++i) {
        itens.Append(hstring(DeUtf8(zeebx_xbox_titulo(m_app, i))));
    }
    Jogos().ItemsSource(itens);
    Vazio().Visibility(m_quantidade == 0 ? Visibility::Visible : Visibility::Collapsed);
    AtualizaSubtitulo();
}

void MainPage::AtualizaSubtitulo() {
    // Sem jogo a frase é a da casa; com jogo, é a contagem. **A contagem é a chave
    // `library.count`, a mesma do desktop** ("{count} game(s)"): a mesma biblioteca, contada do
    // mesmo jeito nas duas telas, e uma tradução a menos para manter. Quando não há jogo, a chave
    // é a da frase da casa, que não tem marcador nenhum — o `ComPosto` não acha o que trocar e
    // devolve o texto como veio.
    Subtitulo().Text(hstring(ComPosto(
        m_quantidade == 0 ? Texto("xbox.tagline") : Texto("library.count"), L"{count}",
        std::to_wstring(m_quantidade))));
}

fire_and_forget MainPage::OnAtualizar(IInspectable const&, RoutedEventArgs const&) {
    // **O pedido de varredura sai depois de uma volta ao despachante.** A varredura em si já não
    // trava esta linha — ela roda num fio do núcleo e quem espera é o relógio —, mas a camada de
    // "procurando" precisa de uma volta para ser pintada antes de o botão parecer apertado à toa.
    co_await winrt::resume_foreground(winrt::Windows::System::DispatcherQueue::GetForCurrentThread());
    ComecaVarredura();
    AtualizaEstadoDosSlots();
}

void MainPage::OnAbrirFontes(IInspectable const&, RoutedEventArgs const&) {
    Mostra(Fontes());
    // A lista das pastas em uso e o explorador são montados na hora: a unidade pode ter sido
    // desplugada desde a última varredura, e a lista tem de mostrar isso.
    AtualizaFontes();
    AtualizaExplorador();
    // **O foco, e é isto que põe a tela de pé.** O botão que trouxe o jogador até aqui acabou de
    // ser colapsado junto com a estante, e foco em controle colapsado é foco nenhum: sem esta
    // linha o direcional não anda em lugar nenhum e a tela parece quebrada. O alvo é o primeiro
    // controle útil — o que abre o seletor.
    BotaoEscolherFonte().Focus(FocusState::Programmatic);
}

void MainPage::OnAbrirConfiguracoes(IInspectable const&, RoutedEventArgs const&) {
    Mostra(Configuracoes());
    // O idioma é o primeiro item das configurações, e a razão mais comum de alguém abrir esta
    // tela. Mesma razão do foco em `OnAbrirFontes`.
    Idioma().Focus(FocusState::Programmatic);
}

void MainPage::OnAbrirSobre(IInspectable const&, RoutedEventArgs const&) {
    Mostra(Sobre());
    // Esta tela não tem controle nenhum além do "Voltar", e o foco vai para ele: é para onde o
    // jogador quer ir, e o B também sai daqui.
    BotaoVoltarDoSobre().Focus(FocusState::Programmatic);
}

void MainPage::OnVoltar(IInspectable const&, RoutedEventArgs const&) {
    Mostra(Biblioteca());
    FocaEstante();
}

void MainPage::FocaEstante() {
    // **O foco é do jogador, e não do cartão.** Com jogo na estante ele vai para a estante, que é
    // onde há o que fazer; sem jogo, para o botão que resolve o problema — o "Fontes de jogos" —,
    // porque uma estante vazia não tem item em que parar o foco, e o direcional ficaria sem lugar
    // nenhum para ir na primeira execução, que é justamente quando alguém precisa dele.
    if (m_quantidade > 0) {
        // **O foco vai para o cartão, e não para a lista.** Medido no PC: com jogo na estante,
        // `Jogos().Focus(...)` não para ali — o foco escorrega para o próximo da ordem de tabulação,
        // que é o botão "Varrer de novo". Quem recebe o A é o cartão, então é nele que o foco tem de
        // parar; o `PrimeiroFocavel` acha esse cartão descendo a árvore a partir do primeiro
        // contêiner, e o direcional começa a andar de onde o jogador espera.
        Control cartao = PrimeiroFocavel(Jogos());
        if (!cartao) {
            // **A varredura acabou de trocar o `ItemsSource`, e o contêiner ainda não nasceu.** O
            // layout do XAML é assíncrono: os cartões da estante são criados numa passagem que ainda
            // não rodou quando o `Loaded` da página chega, e é por isso que o foco caía no primeiro
            // botão da página — quem mediu viu o foco em "Varrer de novo". O `UpdateLayout` roda a
            // passagem agora, de forma síncrona, e depois dela o cartão existe para receber o foco.
            Raiz().UpdateLayout();
            cartao = PrimeiroFocavel(Jogos());
        }
        if (cartao) {
            cartao.Focus(FocusState::Programmatic);
            return;
        }
        Jogos().Focus(FocusState::Programmatic);
    } else {
        BotaoFontes().Focus(FocusState::Programmatic);
    }
}

int32_t MainPage::IndiceDoCartao(ItemsControl const& lista, DependencyObject const& elemento) {
    // **Sobe a árvore até o contêiner, e é isto que dá o índice certo.** O caminho curto seria
    // procurar o título do cartão na lista, e dois jogos de mesmo nome abririam sempre o primeiro
    // — um defeito que só aparece com a biblioteca cheia. O contêiner (`GridViewItem`,
    // `ListViewItem`) é quem sabe o próprio índice; o `IndexFromContainer` devolve -1 para
    // qualquer elemento que não seja contêiner desta lista, e é por isso que o laço pode testar
    // cada ancestral sem perguntar que tipo ele é.
    try {
        for (auto atual = elemento; atual; atual = VisualTreeHelper::GetParent(atual)) {
            int32_t indice = lista.IndexFromContainer(atual);
            if (indice >= 0) {
                return indice;
            }
        }
    } catch (hresult_error const&) {
        // Uma lista que não é desta página, ou um contêiner já reciclado pelo deslize. Sem índice
        // não se abre o jogo errado: não se abre jogo nenhum.
    }
    return -1;
}

int32_t MainPage::IndiceDoItem(ItemsControl const& lista, IInspectable const& item) {
    try {
        if (auto conteiner = lista.ContainerFromItem(item)) {
            return lista.IndexFromContainer(conteiner);
        }
    } catch (hresult_error const&) {
        // Item que já não pertence a esta lista — a varredura seguinte troca o `ItemsSource` —, ou
        // um contêiner que ainda não nasceu. Sem índice, não se abre jogo nenhum.
    }
    return -1;
}

void MainPage::OnJogoEscolhido(IInspectable const& remetente, RoutedEventArgs const&) {
    if (!m_app) {
        return;
    }
    // **O índice sai do cartão que foi acionado, e não de um argumento de evento.** No controle,
    // o A aciona o `Button` que tem o foco — o `ItemClick` do `GridView` só chega pelo ponteiro,
    // e era ele que estava ligado aqui. Era este o defeito: o direcional andava pela estante, o
    // cartão se destacava, e o A não abria nada.
    int32_t indice = IndiceDoCartao(Jogos(), remetente.as<DependencyObject>());
    if (indice >= 0) {
        AbreJogo(indice);
    }
}

void MainPage::OnJogoClicado(IInspectable const&, ItemClickEventArgs const& args) {
    // **A outra rota, e as duas levam ao mesmo lugar.** Quem o A aciona é quem tem o foco: o
    // `Button` de dentro do cartão (`OnJogoEscolhido`) ou o contêiner da lista, que é quem levanta
    // este evento. Qual dos dois o console escolhe não se mede num PC — por isso os dois caminhos
    // existem, e por isso os dois resolvem o índice pelo caminho exato. Uma tecla aciona um só:
    // quem trata a tecla não a repassa.
    int32_t indice = IndiceDoItem(Jogos(), args.ClickedItem());
    if (indice >= 0) {
        AbreJogo(indice);
    }
}

/// A primeira `Image` dentro de uma árvore visual.
///
/// **O `Image` do cartão mora dentro do `DataTemplate`**, e nome dentro de modelo não é alcançável
/// daqui — o modelo tem o namescope dele. O que é alcançável é o contêiner, que é o que o evento da
/// lista entrega; a busca pela árvore acha a imagem sem depender da posição dela no modelo.
static Image EncontraImagem(DependencyObject const& raiz) {
    if (auto imagem = raiz.try_as<Image>()) {
        return imagem;
    }
    try {
        int32_t filhos = VisualTreeHelper::GetChildrenCount(raiz);
        for (int32_t i = 0; i < filhos; ++i) {
            if (auto achada = EncontraImagem(VisualTreeHelper::GetChild(raiz, i))) {
                return achada;
            }
        }
    } catch (hresult_error const&) {
        // Contêiner reciclado no meio da varredura da árvore: sem imagem, sem arte, e o cartão
        // seguinte levanta este evento de novo.
    }
    return nullptr;
}

fire_and_forget MainPage::OnConteinerMudou(ItemsControl const&,
                                           ContainerContentChangingEventArgs const& args) {
    // **O contêiner que sai de cena não pede arte.** Quando a lista recicla um cartão, o índice do
    // argumento já é o do item que ficou, e pedir pixels aqui seria pedir para o cartão errado.
    if (args.InRecycleQueue() || !m_app) {
        co_return;
    }
    int32_t indice = args.ItemIndex();
    if (indice < 0) {
        co_return;
    }
    auto conteiner = args.ItemContainer();
    auto imagem = EncontraImagem(conteiner);
    if (!imagem) {
        co_return;
    }
    uint32_t largura = 0;
    uint32_t altura = 0;
    auto pixels = zeebx_xbox_imagem(m_app, indice, &largura, &altura);
    if (!pixels || largura == 0 || altura == 0) {
        // Sem arte, o cartão é a faixa e o título: é o que um `.mod` sem `.mif` — e um jogo cujo
        // `.mif` não traz ícone nenhum — tem para mostrar, e é um cartão inteiro, não um buraco.
        // Nada vai para o relatório: numa estante de teste a maioria não tem arte, e a linha não
        // diria nada que o olho já não estivesse vendo.
        co_return;
    }
    // **`SoftwareBitmapSource` é o único caminho do XAML para pixels crus**, e ele quer BGRA8
    // premultiplicado — que é exatamente o que o núcleo entrega. O `DataWriter` é só o jeito de
    // fazer um `IBuffer` a partir de um ponteiro: `CopyFromBuffer` não aceita memória crua.
    SoftwareBitmap quadro{BitmapPixelFormat::Bgra8, static_cast<int32_t>(largura),
                          static_cast<int32_t>(altura), BitmapAlphaMode::Premultiplied};
    DataWriter escritor;
    escritor.WriteBytes(array_view<uint8_t const>(
        pixels, pixels + static_cast<size_t>(largura) * static_cast<size_t>(altura) * 4u));
    quadro.CopyFromBuffer(escritor.DetachBuffer());
    SoftwareBitmapSource fonte;
    co_await fonte.SetBitmapAsync(quadro);
    // **O contêiner pode ter sido reciclado durante o `co_await`.** Sem esta conferência, a arte de
    // um jogo apareceria no cartão de outro — o defeito clássico de lista virtualizada. A pergunta
    // é a mesma do `OnJogoEscolhido`: qual índice este contêiner serve agora.
    if (IndiceDoCartao(Jogos(), conteiner) != indice) {
        co_return;
    }
    imagem.Source(fonte);
}

fire_and_forget MainPage::AbreJogo(int32_t indice) {
    // **Um jogo por vez.** As duas rotas de A chegam aqui, e um jogo já aberto não aceita outro: sem
    // isto, uma tecla que acionasse as duas abriria dois avisos com o mesmo motivo. A varredura
    // também barra: abrir um jogo no meio de uma lista que ainda vai ser trocada é abrir o jogo de
    // um índice que pode não valer mais.
    if (!m_app || m_jogando || m_varrendo) {
        co_return;
    }
    // **O recado antes do trabalho, e uma volta ao despachante entre os dois.** O
    // `zeebx_xbox_comeca` descompacta e carrega o jogo nesta mesma linha — são segundos num jogo
    // grande —, e sem o `co_await` o compositor não chegaria a pintar a camada antes de a linha
    // travar: o jogador veria a estante parada e apertaria o A de novo.
    MostraOcupado("xbox.loading");
    co_await winrt::resume_foreground(winrt::Windows::System::DispatcherQueue::GetForCurrentThread());

    auto comeco = std::chrono::steady_clock::now();
    int32_t resultado = zeebx_xbox_comeca(m_app, indice);
    auto gasto = std::chrono::duration_cast<std::chrono::milliseconds>(
                     std::chrono::steady_clock::now() - comeco)
                     .count();
    EscondeOcupado();
    if (resultado != 0) {
        // A razão vem do núcleo, e sai no idioma dele — hoje só português. Traduzir as mensagens
        // do `session` e do `loader` é serviço para outro dia, e está anotado no LEIAME: elas não
        // moram no catálogo, e sim no meio do caminho que carrega o jogo.
        //
        // **A recusa vai também para o relatório.** É o console que diz se o jogo abriu, e a linha
        // do erro é o que sobra dele depois que o jogador fecha a caixa sem ler: com ela, "o jogo
        // não abre" chega aqui com o motivo do núcleo escrito.
        App::Anota(L"jogo nao abriu (indice " + std::to_wstring(indice) + L", " +
                   std::to_wstring(gasto) + L" ms): " + DeUtf8(zeebx_xbox_erro(m_app)));
        MostraAviso(Texto("xbox.failed.open"), DeUtf8(zeebx_xbox_erro(m_app)));
        co_return;
    }
    // **Quanto tempo o jogo levou para abrir, e qual núcleo entrou.** É a medição que separa "o
    // emulador está lento" de "o carregamento é lento": as duas telas se parecem, e o número é o
    // que decide onde mexer.
    //
    // O núcleo vem na mesma linha porque no Xbox ele é escolha de execução: o mesmo pacote roda
    // recompilado ou interpretado conforme o aparelho, e sem esta palavra o tempo acima não diz o
    // que o produziu. A decisão foi tomada no arranque (`App::Inicializar`); esta é a confirmação
    // de que ela chegou ao emulador.
    App::Anota(L"jogo abriu (indice " + std::to_wstring(indice) + L") em " +
               std::to_wstring(gasto) + L" ms, nucleo " + DeUtf8(zeebx_xbox_nucleo(m_app)));
    m_jogando = true;
    m_pausado = false;
    // As duas linhas do relatório que são por partida, e não por execução.
    m_quadroAnotado = false;
    m_imagemAnotada = false;
    m_quadrosVistos = 0;
    m_voltarAnotado = false;
    Mostra(Video());
    // **O painel do vídeo acabou de aparecer, e é agora que ele tem uma escala de composição.**
    // Colapsado ele mede zero e não diz escala nenhuma; o `UpdateLayout` força a passagem de layout
    // que o mede, e a remedida sai com o número certo. Sem isto o primeiro quadro nasce com os
    // pixels do tamanho lógico — a imagem deslocada, e o resto da tela preto.
    Raiz().UpdateLayout();
    AplicaTamanhoDoVideo();
    // O foco sai dos botões: com a estante escondida, um botão focado continua recebendo o
    // direcional, e o jogo ficaria sem entrada.
    //
    // **`Focus` no `this`, e não no `Raiz()`.** O `Raiz` é um `Grid`, e um `Grid` é `Panel`, não
    // `Control`: não tem `Focus` — `C2039: 'Focus': is not a member of Grid` —, porque painel
    // nenhum recebe foco no XAML. A página recebe: `Page` desce de `UserControl`, que desce de
    // `Control`.
    Focus(FocusState::Programmatic);
    MontaLaco();
}

void MainPage::SaiDoJogo() {
    DesmontaLaco();
    m_audio.Silencia();
    if (m_app) {
        zeebx_xbox_para(m_app);
    }
    m_jogando = false;
    m_pausado = false;
    Mostra(Biblioteca());
    AtualizaEstadoDosSlots();
    FocaEstante();
}

void MainPage::OnSairDoJogo(IInspectable const&, RoutedEventArgs const&) {
    SaiDoJogo();
}

void MainPage::MontaLaco() {
    if (m_lacoLigado) {
        return;
    }
    m_assinaturaDoLaco = CompositionTarget::Rendering(
        [this](IInspectable const& remetente, IInspectable const& argumento) {
            OnQuadro(remetente.try_as<CompositionTarget>(), argumento);
        });
    m_lacoLigado = true;
}

void MainPage::DesmontaLaco() {
    if (!m_lacoLigado) {
        return;
    }
    CompositionTarget::Rendering(m_assinaturaDoLaco);
    m_lacoLigado = false;
}

void MainPage::OnQuadro(CompositionTarget const&, IInspectable const&) {
    if (!m_jogando || !m_app) {
        return;
    }

    m_input.Aplica(m_app);
    if (m_input.ConsomeMenu()) {
        AlternaPausa();
    }
    if (m_pausado) {
        // Pausado, nada avança e nada é apresentado: a cadeia de troca fica com o último quadro,
        // que é o que se quer ver atrás do menu.
        return;
    }

    int32_t codigo = zeebx_xbox_passo(m_app);
    if (codigo == ZEEBX_PAROU) {
        std::wstring motivo = DeUtf8(zeebx_xbox_erro(m_app));
        SaiDoJogo();
        if (!motivo.empty()) {
            MostraAviso(Texto("xbox.failed.stopped"), motivo);
        }
        return;
    }

    // **A volta sem desenho novo não apresenta.** O Zeebo desenha a 30 quadros por segundo dentro
    // de uma tela que compõe a 60: na volta em que o jogo não desenhou, o núcleo devolve o mesmo
    // quadro, e apresentá-lo de novo custava uma conversão de 1,2 MB e um `Present` por volta
    // vazia — metade do trabalho de vídeo era refazer um quadro que já estava na tela. O som, esse,
    // não pode esperar, e por isso o `zeebx_xbox_audio` continua rodando em toda volta.
    if (codigo != ZEEBX_SEM_QUADRO_NOVO) {
        uint32_t largura = 0;
        uint32_t altura = 0;
        if (auto pixels = zeebx_xbox_quadro(m_app, &largura, &altura)) {
            // **O tamanho que o núcleo publica vai para o relatório, uma vez.** É a outra metade da
            // linha do tamanho: o alvo diz em que pixels desenhamos, e esta diz quantos pixels o jogo
            // mandou. Um jogo que publica 320x240 num alvo de 1920x1080 é o esperado; um quadro de 0x0
            // é outra coisa, e sem esta linha as duas se parecem.
            ++m_quadrosVistos;
            if (!m_quadroAnotado) {
                m_quadroAnotado = true;
                App::Anota(L"primeiro quadro: " + std::to_wstring(largura) + L"x" +
                           std::to_wstring(altura) + L" px");
            }
            // **"Preto" e "sem vídeo" são a mesma tela num console**, e é aqui que os dois se separam:
            // se o jogo mandar um quadro com pixels, o defeito é da apresentação; se nunca mandar nada
            // além de preto, o defeito está antes, no núcleo, e não há ajuste de escala que o resolva.
            // O jogo costuma abrir com a tela preta, então a linha só sai quando **alguma coisa** foi
            // desenhada — e o número de quadros diz quanto tempo isso levou.
            if (!m_imagemAnotada) {
                size_t total = static_cast<size_t>(largura) * altura * 4u;
                for (size_t i = 0; i < total; ++i) {
                    if (pixels[i] != 0) {
                        m_imagemAnotada = true;
                        App::Anota(L"primeira imagem: " + std::to_wstring(largura) + L"x" +
                                   std::to_wstring(altura) + L" px, no quadro " +
                                   std::to_wstring(m_quadrosVistos));
                        break;
                    }
                }
            }
            m_renderer.Apresenta({pixels, largura, altura}, m_proporcao, m_escalaInteira);
        }
    }

    uint32_t quadros = 0;
    if (auto amostras = zeebx_xbox_audio(m_app, &quadros)) {
        m_audio.Enfileira(amostras, quadros);
    }

    if (m_contadores) {
        // Os números são os do núcleo, e não uma contagem nossa de voltas do laço: uma volta
        // pode não apresentar quadro, e aí a contagem daqui mentiria. A frase é a do catálogo,
        // com os três números entrando nos marcadores — é o que faz "quadros" virar "frames".
        wchar_t milhoes[32]{};
        ::swprintf_s(milhoes, L"%.1f", static_cast<double>(zeebx_xbox_instrucoes(m_app)) / 1e6);
        auto texto = ComPosto(Texto("xbox.counters"), L"{seconds}",
                              std::to_wstring(zeebx_xbox_relogio_ms(m_app) / 1000));
        texto = ComPosto(std::move(texto), L"{frames}", std::to_wstring(zeebx_xbox_quadros(m_app)));
        TextoDosContadores().Text(hstring(ComPosto(std::move(texto), L"{instructions}", milhoes)));
    }
}

void MainPage::AlternaPausa() {
    m_pausado = !m_pausado;
    if (m_pausado) {
        m_audio.Silencia();
        AtualizaEstadoDosSlots();
        Pausa().Visibility(Visibility::Visible);
        // O foco vai para o primeiro botão do menu — o "Continuar" —: sem isto o direcional não
        // anda na pausa. **O alvo é o botão, e não o `Pausa()`**: a sobreposição é um `Grid`, e
        // painel não recebe foco (`C2039`).
        BotaoContinuar().Focus(FocusState::Programmatic);
    } else {
        Pausa().Visibility(Visibility::Collapsed);
        // De volta ao jogo, o foco sai do menu pela mesma razão do `AbreJogo`: ver a nota de lá.
        Focus(FocusState::Programmatic);
    }
}

void MainPage::OnContinuar(IInspectable const&, RoutedEventArgs const&) {
    // Só faz sentido na pausa: o botão mora na sobreposição, que só existe durante o jogo.
    if (m_pausado) {
        AlternaPausa();
    }
}

int32_t MainPage::SlotEscolhido() {
    // Na pausa vale o comboio da pausa; na estante, o das configurações.
    auto comboio = m_pausado ? SlotDaPausa() : Slot();
    int32_t indice = comboio.SelectedIndex();
    return indice < 0 ? 0 : indice;
}

void MainPage::AtualizaEstadoDosSlots() {
    uint32_t slot = static_cast<uint32_t>(SlotEscolhido());
    bool tem = m_app && zeebx_xbox_tem_estado(m_app, slot) != 0;
    // A mesma frase nos dois lugares: é o mesmo slot e o mesmo save, e a pausa mostra o que a
    // estante mostra.
    auto texto = hstring(ComPosto(Texto(tem ? "xbox.slot_used" : "xbox.slot_empty"), L"{slot}",
                                  std::to_wstring(slot)));
    EstadoDoSlot().Text(texto);
    EstadoDoSlotDaPausa().Text(texto);
}

void MainPage::OnSalvar(IInspectable const&, RoutedEventArgs const&) {
    if (!m_app) {
        return;
    }
    uint32_t slot = static_cast<uint32_t>(SlotEscolhido());
    if (zeebx_xbox_salva_estado(m_app, slot) == 0) {
        AtualizaEstadoDosSlots();
        return;
    }
    MostraAviso(Texto("xbox.failed.save"), DeUtf8(zeebx_xbox_erro(m_app)));
}

void MainPage::OnCarregar(IInspectable const&, RoutedEventArgs const&) {
    if (!m_app) {
        return;
    }
    uint32_t slot = static_cast<uint32_t>(SlotEscolhido());
    int32_t resultado = zeebx_xbox_carrega_estado(m_app, slot);
    if (resultado == 0) {
        AtualizaEstadoDosSlots();
        if (m_pausado) {
            AlternaPausa();
        }
        return;
    }
    if (resultado == 1) {
        // Slot vazio é ausência, não falha: avisar com caixa de diálogo seria ruído.
        AtualizaEstadoDosSlots();
        return;
    }
    MostraAviso(Texto("xbox.failed.load"), DeUtf8(zeebx_xbox_erro(m_app)));
}

void MainPage::OnVolumeMudou(IInspectable const&,
                             Primitives::RangeBaseValueChangedEventArgs const&) {
    if (m_app) {
        zeebx_xbox_volume(m_app, static_cast<uint32_t>(Volume().Value()));
    }
}

void MainPage::OnProporcaoMudou(IInspectable const&, SelectionChangedEventArgs const&) {
    // A remontagem da lista passa por aqui com o índice em -1 e depois com o de volta; a trava
    // impede que o valor verdadeiro seja trocado pelo padrão no meio do caminho.
    if (m_remontandoListas) {
        return;
    }
    m_proporcao = Proporcao().SelectedIndex() == 1 ? (16.0f / 9.0f) : (4.0f / 3.0f);
}

void MainPage::OnEscalaMudou(IInspectable const&, RoutedEventArgs const&) {
    m_escalaInteira = EscalaInteira().IsOn();
}

void MainPage::OnContadoresMudou(IInspectable const&, RoutedEventArgs const&) {
    m_contadores = ContadoresLigados().IsOn();
    Contadores().Visibility(m_contadores ? Visibility::Visible : Visibility::Collapsed);
}

// ================================= O idioma =================================

void MainPage::AplicaTextos() {
    if (!m_app) {
        return;
    }
    // **Uma tela, uma chave por texto: quem mexe na GUI não inventa frase aqui.** Toda frase desta
    // função vem do catálogo do núcleo, pela chave — é o que faz a mesma tela falar inglês,
    // espanhol e português sem uma linha de tradução deste lado. Chave que não existe no catálogo
    // volta como ela mesma, e é assim que o erro aparece na tela: "xbox.alguma.coisa" no lugar do
    // rótulo é um defeito, e um visível.
    DicaJogar().Text(Rotulo("xbox.card_play"));
    DicaVoltar().Text(Rotulo("xbox.hint.back"));
    DicaMenu().Text(Rotulo("xbox.hint.menu"));
    BotaoAtualizar().Content(box_value(Rotulo("library.rescan")));
    BotaoFontes().Content(box_value(Rotulo("xbox.sources")));
    BotaoConfiguracoes().Content(box_value(Rotulo("nav.settings")));
    BotaoSobre().Content(box_value(Rotulo("settings.tab.about")));
    VazioTitulo().Text(Rotulo("xbox.no_games"));
    VazioDica().Text(Rotulo("xbox.no_games.hint"));

    FontesTitulo().Text(Rotulo("xbox.sources"));
    FontesEscolherTitulo().Text(Rotulo("xbox.sources.add"));
    // **Os dois botões ficam à vista, cada um com o seu rótulo.** O primeiro abre a caixa de pasta;
    // o segundo, a de arquivo, e a pasta é a que contém o jogo escolhido. Esconder o segundo atrás
    // de uma tentativa que falhou seria adivinhar o aparelho — e no console, onde o primeiro não
    // existe, deixaria o jogador com um botão que não faz nada e não diz por quê.
    BotaoEscolherFonte().Content(box_value(Rotulo("xbox.sources.choose")));
    BotaoEscolherJogo().Content(box_value(Rotulo("xbox.sources.choose_file")));
    FontesEscolherDica().Text(Rotulo("xbox.sources.choose_hint"));
    FontesEmUsoTitulo().Text(Rotulo("xbox.sources.in_use"));
    FontesVazio().Text(Rotulo("xbox.sources.none_here"));
    BotaoTirarFonte().Content(box_value(Rotulo("xbox.sources.remove")));
    FontesUnidadesTitulo().Text(Rotulo("xbox.sources.drives_title"));
    FontesDica().Text(Rotulo("xbox.sources.hint"));
    BotaoSubir().Content(box_value(Rotulo("xbox.sources.up")));
    BotaoUsarPasta().Content(box_value(Rotulo("xbox.sources.pick")));
    BotaoVoltarDasFontes().Content(box_value(Rotulo("xbox.back")));

    ConfiguracoesTitulo().Text(Rotulo("settings.title"));
    IdiomaTitulo().Text(Rotulo("settings.language"));
    IdiomaDica().Text(Rotulo("settings.language.hint"));
    VolumeTitulo().Text(Rotulo("audio.volume"));
    ProporcaoTitulo().Text(Rotulo("xbox.aspect"));
    EscalaInteira().Header(box_value(Rotulo("graphics.scaling.integer")));
    ContadoresLigados().Header(box_value(Rotulo("debug.overlay")));
    // O mesmo par ligado/desligado nos dois interruptores: "Ligado" serve para os dois, e um par
    // por interruptor seria duas traduções para a mesma palavra.
    for (auto const& interruptor : {EscalaInteira(), ContadoresLigados()}) {
        interruptor.OnContent(box_value(Rotulo("xbox.on")));
        interruptor.OffContent(box_value(Rotulo("xbox.off")));
    }
    EstadoTitulo().Text(Rotulo("play.state.title"));
    BotaoSalvar().Content(box_value(Rotulo("play.state.save")));
    BotaoSalvarDaPausa().Content(box_value(Rotulo("play.state.save")));
    BotaoCarregar().Content(box_value(Rotulo("play.state.load")));
    BotaoCarregarDaPausa().Content(box_value(Rotulo("play.state.load")));
    CpuTitulo().Text(Rotulo("xbox.cpu"));
    CpuDica().Text(Rotulo("xbox.cpu.hint"));
    BotaoVoltarDasConfiguracoes().Content(box_value(Rotulo("xbox.back")));

    SobreTitulo().Text(Rotulo("settings.tab.about"));
    SobreBlurb().Text(Rotulo("xbox.about.blurb"));
    SobreLicencaTitulo().Text(Rotulo("xbox.about.license_title"));
    SobreLicenca().Text(Rotulo("xbox.about.license"));
    SobreJogosTitulo().Text(Rotulo("xbox.about.games_title"));
    SobreJogos().Text(Rotulo("xbox.about.games"));
    BotaoVoltarDoSobre().Content(box_value(Rotulo("xbox.back")));

    PausaTitulo().Text(Rotulo("play.paused"));
    BotaoContinuar().Content(box_value(Rotulo("play.resume")));
    BotaoSair().Content(box_value(Rotulo("play.stop")));

    // **Nome acessível nos controles que não têm `Header`.** O Narrador — e qualquer coisa que fale
    // UIA, inclusive o teste que clica por nome — lê o `Name` do elemento. Sem isto, os três
    // comboios e o volume chegam como "caixa de seleção" e "controle deslizante", sem dizer de quê
    // (foi o que a árvore de acessibilidade mostrou). Os dois interruptores têm `Header` e já o
    // ganham de graça.
    Automation::AutomationProperties::SetName(Idioma(), Rotulo("settings.language"));
    Automation::AutomationProperties::SetName(Proporcao(), Rotulo("xbox.aspect"));
    Automation::AutomationProperties::SetName(Slot(), Rotulo("play.state.title"));
    Automation::AutomationProperties::SetName(SlotDaPausa(), Rotulo("play.state.title"));
    Automation::AutomationProperties::SetName(Volume(), Rotulo("audio.volume"));
    Automation::AutomationProperties::SetName(Pastas(), Rotulo("xbox.sources.drives"));

    // Estas fecham o `AplicaTextos` porque dependem **do idioma em uso**, e não só de uma chave: o
    // nome de cada idioma na lista, o rótulo "Slot N", os dois da proporção, a contagem do subtítulo
    // e a marca da pasta que sumiu. Nenhuma delas varre a biblioteca de novo.
    PreencheIdiomas();
    PreencheProporcao();
    PreencheSlots();
    AtualizaSubtitulo();
    AtualizaFontes();
    AtualizaEstadoDosSlots();
}

void MainPage::PreencheProporcao() {
    // **Os dois itens nascem aqui, e não no XAML.** A parte fechada de um `ComboBox` guarda uma
    // cópia do texto do item escolhido: trocar o `Content` de um item que já foi desenhado não
    // chega até ela, e a caixa continua mostrando a frase do idioma anterior — foi o que a captura
    // em português pegou, "Native (4:3)" embaixo de "Proporção do quadro". Remontar a lista é o que
    // obriga a caixa a se refazer, e é o mesmo caminho do `PreencheSlots`, pelo mesmo motivo.
    //
    // A ordem é a que o `OnProporcaoMudou` lê: zero é a nativa (4:3), um é 16:9.
    int32_t escolhido = Proporcao().SelectedIndex();
    m_remontandoListas = true;
    Proporcao().Items().Clear();
    Proporcao().Items().Append(box_value(Rotulo("graphics.aspect.native")));
    Proporcao().Items().Append(box_value(Rotulo("graphics.aspect.16x9")));
    // A escolha sobrevive à troca de idioma: quem estava em 16:9 continua em 16:9.
    Proporcao().SelectedIndex(escolhido < 0 ? 0 : escolhido);
    m_remontandoListas = false;
}

void MainPage::PreencheIdiomas() {
    // **O que identifica o idioma é o código, e não o índice do comboio.** É o código que vai para
    // o `settings.json` e é ele que o núcleo guarda: a escolha sobrevive mesmo que a lista mude de
    // ordem — e é por isso que ela é lida por índice e gravada por código.
    std::wstring atual = DeUtf8(zeebx_xbox_idioma(m_app));
    auto itens = single_threaded_observable_vector<hstring>();
    int32_t quantos = zeebx_xbox_idiomas(m_app);
    int32_t escolhido = 0;
    for (int32_t i = 0; i < quantos; ++i) {
        std::string codigo = zeebx_xbox_idioma_codigo(m_app, i);
        if (DeUtf8(codigo.c_str()) == atual) {
            escolhido = i;
        }
        // O nome sai no próprio idioma — "English", "Português (Brasil)" —, que é o que se procura
        // na lista: quem quer português procura "Português", e não "Portuguese".
        itens.Append(hstring(DeUtf8(zeebx_xbox_idioma_nome(m_app, i))));
    }
    // **A trava é o que impede o laço infinito.** Remarcar o item dispara o `SelectionChanged`, que
    // pede troca de idioma; esta é a hora em que a marcação é nossa, e não do jogador.
    m_remontandoListas = true;
    Idioma().ItemsSource(itens);
    Idioma().SelectedIndex(escolhido);
    m_remontandoListas = false;
}

void MainPage::PreencheSlots() {
    // Os dois comboios de slot são os mesmos dez; um na estante e outro na pausa, porque a pausa
    // não deve obrigar a sair do jogo para trocar de slot. O rótulo de cada linha é "Slot N", que
    // muda com o idioma: por isso isto é uma função, e não um `for` no `OnCarregado`.
    for (auto const& comboio : {Slot(), SlotDaPausa()}) {
        // O slot escolhido sobrevive à troca de idioma: quem estava no 7 continua no 7.
        int32_t escolhido = comboio.SelectedIndex();
        comboio.Items().Clear();
        for (uint32_t i = 0; i < SLOTS; ++i) {
            comboio.Items().Append(
                box_value(hstring(ComPosto(Texto("play.state.slot"), L"{slot}", std::to_wstring(i)))));
        }
        comboio.SelectedIndex(escolhido < 0 ? 0 : escolhido);
    }
}

void MainPage::OnIdiomaMudou(IInspectable const&, SelectionChangedEventArgs const&) {
    if (!m_app || m_remontandoListas) {
        return;
    }
    int32_t escolhido = Idioma().SelectedIndex();
    if (escolhido < 0) {
        return;
    }
    // **O código é copiado antes de trocar, e a cópia não é zelo.** O ponteiro que a ABI devolve
    // aponta para dentro do núcleo, e a troca de idioma reescreve essas listas: passar o ponteiro
    // direto seria entregar à chamada um ponteiro que ela mesma libera.
    std::string codigo = zeebx_xbox_idioma_codigo(m_app, escolhido);
    if (zeebx_xbox_define_idioma(m_app, codigo.c_str()) == 0) {
        AplicaTextos();
    }
}

// ========================== As fontes de jogos ==========================

void MainPage::AtualizaFontes() {
    if (!m_app) {
        return;
    }
    // As pastas que a biblioteca lê, com a marca de quando a pasta sumiu: a unidade pode ter sido
    // desplugada desde a última varredura, e uma linha que continua ali sem avisar mente.
    auto itens = single_threaded_observable_vector<hstring>();
    int32_t quantas = zeebx_xbox_fontes(m_app);
    for (int32_t i = 0; i < quantas; ++i) {
        std::wstring caminho = DeUtf8(zeebx_xbox_fonte(m_app, i));
        if (zeebx_xbox_fonte_existe(m_app, i) == 0) {
            caminho += L"  —  " + Texto("common.folder_missing");
        }
        itens.Append(hstring(caminho));
    }
    FontesEmUso().ItemsSource(itens);
    FontesVazio().Visibility(quantas == 0 ? Visibility::Visible : Visibility::Collapsed);
    BotaoTirarFonte().IsEnabled(quantas > 0);
}

fire_and_forget MainPage::AtualizaExplorador() {
    if (!m_app) {
        co_return;
    }
    auto recado = FontesRecado();
    recado.Text(L"");
    auto itens = single_threaded_observable_vector<hstring>();
    m_filhos.clear();
    bool falhou = false;

    // **A permissão é pedida em tempo de execução, e é coisa de console.** No Xbox o acesso às
    // unidades removíveis abre uma caixa do sistema; no PC não há caixa, e o pedido pode nem
    // existir. Por isso a falha do pedido **não** é tratada como recusa: no PC ela é o normal, e a
    // listagem logo abaixo é quem diz se o acesso veio ou não.
    if (m_caminho.empty()) {
        bool permitido = true;
        try {
            auto situacao = co_await KnownFolders::RequestAccessAsync(KnownFolderId::RemovableDevices);
            permitido = situacao == KnownFoldersAccessStatus::Allowed ||
                        situacao == KnownFoldersAccessStatus::AllowedPerAppFolder;
        } catch (hresult_error const&) {
        }
        if (!permitido) {
            recado.Text(Rotulo("xbox.sources.refused"));
            FontesCaminho().Text(Rotulo("xbox.sources.drives"));
            BotaoSubir().IsEnabled(false);
            BotaoUsarPasta().IsEnabled(false);
            Pastas().ItemsSource(itens);
            // O recado é aqui dentro, então a seção tem de aparecer — senão ele sairia numa
            // caixa colapsada e o jogador veria a recusa sem ler o motivo.
            BlocoUnidades().Visibility(Visibility::Visible);
            co_return;
        }
    }

    // A raiz é uma pasta virtual — as unidades —, e depois dela é a pasta em que se está. O
    // caminho vem da pilha, e **não** do `GetParentAsync`: numa unidade removível a pasta raiz não
    // tem pai que o WinRT devolva, e quem sabe por onde se desceu é quem desceu.
    StorageFolder pasta = m_caminho.empty() ? KnownFolders::RemovableDevices() : m_caminho.back();
    try {
        auto pastas = co_await pasta.GetFoldersAsync();
        for (auto const& filho : pastas) {
            m_filhos.push_back(filho);
            // **O nome na linha, e o caminho na linha de cima.** A coluna mostra "Zeebo"; repetir
            // "E:\Zeebo" em cada linha de uma lista de vinte é ruído.
            itens.Append(filho.Name());
        }
    } catch (hresult_error const&) {
        // Uma unidade que sumiu no meio da navegação, ou uma pasta sem permissão de leitura. A
        // exceção é do WinRT, e vale como recusa.
        falhou = true;
        recado.Text(Rotulo("xbox.sources.denied"));
        m_filhos.clear();
        itens.Clear();
    }

    FontesCaminho().Text(m_caminho.empty() ? Rotulo("xbox.sources.drives") : m_caminho.back().Path());
    // Subir só vale de dentro; usar a pasta, idem: a raiz é virtual, e as unidades não se
    // acrescentam à biblioteca como se fossem uma pasta.
    BotaoSubir().IsEnabled(!m_caminho.empty());
    BotaoUsarPasta().IsEnabled(!m_caminho.empty());
    // Sem unidade e sem subpasta são recados diferentes: um pede o pendrive, o outro não pede nada
    // — pasta sem subpasta é o normal, e quem tem os jogos ali a vê listada.
    if (m_filhos.empty() && !falhou && m_caminho.empty()) {
        recado.Text(Rotulo("xbox.sources.no_drives"));
    }
    // **A seção do explorador só aparece quando serve para alguma coisa**: com unidade na tomada, ou
    // já dentro de uma pasta, que é onde estão o "Subir" e o "Usar esta pasta". No PC com a caixa do
    // sistema funcionando e nenhuma unidade ela some — e era ela, dizendo "nenhuma unidade
    // encontrada" sem ter o que fazer com isso, que fazia esta tela parecer quebrada. Um recado que
    // mora aqui dentro é quem a faz aparecer de volta, no `RecusaODialogo`.
    bool serve = !m_filhos.empty() || !m_caminho.empty();
    BlocoUnidades().Visibility(serve ? Visibility::Visible : Visibility::Collapsed);
    Pastas().ItemsSource(itens);
}

void MainPage::OnPastaEscolhida(IInspectable const& remetente, RoutedEventArgs const&) {
    // O índice sai do cartão que foi acionado, pela mesma razão do `OnJogoEscolhido`: no
    // controle é o A que abre a pasta, e o A aciona quem tem o foco.
    AbrePasta(IndiceDoCartao(Pastas(), remetente.as<DependencyObject>()));
}

void MainPage::OnPastaClicada(IInspectable const&, ItemClickEventArgs const& args) {
    // A outra rota do A na lista de pastas. Mesma razão do `OnJogoClicado`.
    AbrePasta(IndiceDoItem(Pastas(), args.ClickedItem()));
}

void MainPage::AbrePasta(int32_t alvo) {
    if (alvo < 0 || static_cast<size_t>(alvo) >= m_filhos.size()) {
        return;
    }
    m_caminho.push_back(m_filhos[static_cast<size_t>(alvo)]);
    AtualizaExplorador();
}

void MainPage::OnSubirPasta(IInspectable const&, RoutedEventArgs const&) {
    if (m_caminho.empty()) {
        return;
    }
    m_caminho.pop_back();
    AtualizaExplorador();
}

void MainPage::OnUsarPasta(IInspectable const&, RoutedEventArgs const&) {
    if (m_caminho.empty()) {
        return;
    }
    AcrescentaCaminho(std::wstring{m_caminho.back().Path()});
}

// ======================= O seletor do sistema =======================

void MainPage::AcrescentaCaminho(std::wstring const& caminho) {
    if (!m_app || caminho.empty()) {
        return;
    }
    // **O caminho vai em UTF-8, e não em `wchar_t`.** A ABI fala UTF-8 com o núcleo, como todo o
    // resto do emulador, e o `ParaUtf8` é o mesmo par que já leva o caminho da `LocalState`.
    std::string utf8 = ParaUtf8(caminho);
    int32_t resultado = zeebx_xbox_acrescenta_fonte(m_app, utf8.c_str());
    // **O resultado vai para o relatório.** É a linha que diz, num console, se a pasta entrou e
    // quantos jogos a varredura achou — e é a diferença entre "a pasta não entrou" e "a pasta entrou
    // e o núcleo não vê arquivo nenhum dentro dela", que são dois consertos diferentes.
    App::Anota(L"fonte: " + caminho + L" -> " + std::to_wstring(resultado) + L" (" +
               std::to_wstring(m_quantidade) + L" jogo(s) na varredura anterior)");
    // 0 entrou e -2 já estava: nos dois casos a pasta está na biblioteca, que é o que o jogador
    // queria, e os dois merecem a varredura. -1 e -3 são recusas, e ganham recado.
    if (resultado == 0 || resultado == -2) {
        // **A varredura de dentro da pasta que acabou de entrar.** Ela é pedida aqui, e o recado
        // de "não achei jogo" sai quando ela termina — é lá que o número existe. Ver o
        // `OnRelogioDaVarredura`.
        ComecaVarredura();
        AtualizaFontes();
        return;
    }
    FontesRecado().Text(Rotulo(resultado == -1 ? "common.folder_missing" : "xbox.sources.denied"));
}

void MainPage::AcrescentaPasta(StorageFolder const& pasta) {
    std::wstring caminho{pasta.Path()};
    // **O token é o que faz a pasta sobreviver ao fechamento do aplicativo.** O que a ABI guarda é
    // o caminho, e caminho não é permissão: quem guarda a permissão é o `FutureAccessList`, e é
    // dele que a pasta é reaberta a cada arranque. O `Add` devolve o token na hora, e o token vai
    // para o arquivo dos tokens — ver o `CaminhoDosTokens`.
    try {
        std::string token = to_string(StorageApplicationPermissions::FutureAccessList().Add(pasta));
        std::vector<std::string> tokens = LeTokens();
        bool jaEstava = false;
        for (auto const& guardado : tokens) {
            jaEstava = jaEstava || guardado == token;
        }
        if (!jaEstava) {
            tokens.push_back(token);
            GravaTokens(tokens);
        }
        // A linha que diz se a permissão ficou guardada: sem ela, "a pasta sumiu no arranque
        // seguinte" não se distingue de "o token nunca existiu", e o conserto de um caso não é o do
        // outro.
        App::Anota(L"permissao guardada para " + caminho);
    } catch (hresult_error const& falha) {
        // Sem token a pasta vale para esta execução, que é o que se tem quando nem o cofre do
        // sistema aceita a pasta. Recusar a pasta por causa disto seria pior.
        App::Anota(L"permissao NAO guardada para " + caminho + L": " +
                   std::wstring{falha.message().c_str()});
    }
    AcrescentaCaminho(caminho);
}

fire_and_forget MainPage::ReataFontes() {
    // **Sem isto a pasta escolhida pelo seletor some depois de fechar o aplicativo**: ela continua
    // escrita na configuração do núcleo, e a permissão não. O `FutureAccessList` é quem a devolve,
    // pelos tokens do arquivo; o `StorageFolder` que ele devolve fica vivo na `m_fontesVivas`, que é
    // como o aplicativo diz ao sistema que a pasta ainda lhe interessa.
    std::vector<std::string> tokens = LeTokens();
    if (tokens.empty()) {
        // Sem token não há permissão para reabrir, mas a varredura não pode deixar de acontecer: é
        // dela que sai a estante. `co_return`, e não `return`: um `return` antes do primeiro
        // `co_await` faz o compilador tratar a função como comum e acusar `C3773` — a função só se
        // declara co-rotina no `co_return`/`co_await`, e o caminho de saída antecipada vem antes
        // dos dois.
        ComecaVarredura();
        co_return;
    }
    std::vector<std::string> vivos;
    for (auto const& token : tokens) {
        try {
            auto pasta = co_await StorageApplicationPermissions::FutureAccessList().GetFolderAsync(
                to_hstring(token));
            m_fontesVivas.push_back(pasta);
            vivos.push_back(token);
            App::Anota(L"permissao reaberta: " + std::wstring{pasta.Path()});
        } catch (hresult_error const& falha) {
            // O token não vale mais: a pasta sumiu, ou o cofre foi limpo. Fica fora da lista
            // reescrita, e a permissão daquele caminho morre com ele.
            App::Anota(L"permissao perdida (" + DeUtf8(token.c_str()) + L"): " +
                       std::wstring{falha.message().c_str()});
        }
    }
    if (vivos.size() != tokens.size()) {
        GravaTokens(vivos);
    }
    // **A primeira varredura é a daqui, e não a do `OnCarregado`.** As pastas do seletor voltaram a
    // valer nas linhas de cima; varrer antes disto seria varrer sem enxergar a unidade de fora — "os
    // jogos do HD só aparecem depois de apertar varrer de novo" era exatamente isso.
    ComecaVarredura();
}

void MainPage::RecusaODialogo(std::wstring const& onde, hresult_error const& falha) {
    // **O código, e não só a frase.** "Este aparelho não tem seletor de pasta" e "o aplicativo não
    // estava em primeiro plano" querem consertos diferentes, e a tela mostra a mesma frase para os
    // dois. O código é o que separa uma causa da outra — e num console, onde não há depurador, esta
    // linha do relatório é a única testemunha que sobra.
    App::Anota(L"caixa do sistema recusada (" + onde + L"): " + std::wstring{falha.message().c_str()} +
               L", codigo " + std::to_wstring(static_cast<uint32_t>(falha.code().value)));
    // A tela diz o que fazer, e a caixa do explorador volta à vista: é dentro dela que o recado
    // mora, e um recado numa seção colapsada é um recado que ninguém lê.
    FontesRecado().Text(Rotulo("xbox.sources.picker_failed"));
    BlocoUnidades().Visibility(Visibility::Visible);
}

fire_and_forget MainPage::OnEscolherPasta(IInspectable const&, RoutedEventArgs const&) {
    if (!m_app) {
        co_return;
    }
    // **A caixa de pasta do sistema.** A Microsoft documenta que cenários de seletor de pasta não são
    // suportados no Xbox — é por isso que o botão ao lado existe. Uma recusa aqui não tenta
    // adivinhar o aparelho nem se guarda para a próxima execução: ela vira uma linha do relatório e
    // o jogador segue pelo outro botão, que está logo abaixo.
    try {
        FolderPicker seletor;
        seletor.SuggestedStartLocation(PickerLocationId::ComputerFolder);
        // O filtro de um seletor de pasta é de tipo de arquivo, e sem nenhum declarado a caixa não
        // deixa escolher pasta nenhuma: o curinga é o que abre a caixa.
        seletor.FileTypeFilter().Append(L"*");
        auto pasta = co_await seletor.PickSingleFolderAsync();
        // **O que a caixa devolveu vai para o relatório, inclusive o nada.** "Cancelou" e "a caixa
        // não abriu de verdade" são a mesma tela e o mesmo silêncio, e num console é esta linha que
        // separa os dois: sem ela, um seletor que devolve nada toda vez vira "o botão não faz nada",
        // que foi exatamente o defeito que esta tela veio consertar.
        App::Anota(L"seletor de pasta devolveu: " +
                   (pasta ? std::wstring{pasta.Path()} : std::wstring{L"nada"}));
        if (pasta) {
            AcrescentaPasta(pasta);
        }
        // Cancelar não é recusar: a caixa abriu, e quem desistiu foi o jogador.
    } catch (hresult_error const& falha) {
        RecusaODialogo(L"seletor de pasta", falha);
    }
}

fire_and_forget MainPage::OnEscolherJogo(IInspectable const&, RoutedEventArgs const&) {
    if (!m_app) {
        co_return;
    }
    try {
        FileOpenPicker seletor;
        seletor.SuggestedStartLocation(PickerLocationId::ComputerFolder);
        // **Só as extensões declaradas no manifesto.** No console a lista do seletor é filtrada por
        // elas — é a associação de tipos de arquivo que abre a unidade para o aplicativo —, e uma
        // extensão de fora deixaria o jogador procurar um arquivo que a caixa não mostra.
        for (auto const& extensao : {L".mod", L".mif", L".zip", L".7z"}) {
            seletor.FileTypeFilter().Append(extensao);
        }
        auto arquivo = co_await seletor.PickSingleFileAsync();
        // A mesma razão da linha gêmea do seletor de pasta: o nada também é notícia.
        App::Anota(L"seletor de arquivo devolveu: " +
                   (arquivo ? std::wstring{arquivo.Path()} : std::wstring{L"nada"}));
        if (!arquivo) {
            co_return;
        }
        // A pasta que contém o jogo escolhido. **Corte na mão, e não `GetParentAsync`:** é a mesma
        // resposta sem uma ida ao sistema de arquivos no meio, e sem um caminho de falha a mais
        // justamente no console, onde esta é a rota normal. É também o que dispensa um token para a
        // pasta: no console quem dá o acesso à unidade é a capacidade do manifesto, e não a caixa.
        std::wstring caminho{arquivo.Path()};
        size_t corte = caminho.find_last_of(L"\\/");
        if (corte == std::wstring::npos || corte == 0) {
            // Um arquivo na raiz da unidade não tem pasta acima que faça sentido ler: apontar a
            // biblioteca para a raiz é apontá-la para a unidade inteira.
            App::Anota(L"arquivo escolhido na raiz da unidade: " + caminho);
            FontesRecado().Text(Rotulo("xbox.sources.denied"));
            BlocoUnidades().Visibility(Visibility::Visible);
            co_return;
        }
        caminho.resize(corte);
        AcrescentaCaminho(caminho);
    } catch (hresult_error const& falha) {
        RecusaODialogo(L"seletor de arquivo", falha);
    }
}

void MainPage::OnTirarFonte(IInspectable const&, RoutedEventArgs const&) {
    if (!m_app) {
        return;
    }
    // A pasta escolhida é a da lista de cima; sem escolha, não há o que tirar.
    int32_t escolhida = FontesEmUso().SelectedIndex();
    if (escolhida < 0) {
        return;
    }
    if (zeebx_xbox_tira_fonte(m_app, escolhida) != 0) {
        return;
    }
    ComecaVarredura();
    AtualizaFontes();
}

void MainPage::OnTecla(IInspectable const&, Input::KeyRoutedEventArgs const& args) {
    // **O `P` é a pausa da bancada.** No console quem abre e fecha o menu de pausa é o `View`, que
    // vem do controle; no PC, que é onde este frontend é depurado, o `Esc` fazia isso — e deixou de
    // fazer, porque com o jogo rodando o `Esc` é o `B`, e o `B` agora é do jogo. Sem esta tecla, um
    // PC sem controle ligado não teria como abrir a pausa de jeito nenhum.
    auto tecla = args.Key();
    if (tecla == Windows::System::VirtualKey::P) {
        args.Handled(true);
        if (m_jogando && !args.KeyStatus().WasKeyDown) {
            AlternaPausa();
        }
        return;
    }
    // Teclado é o caminho do PC em modo de desenvolvimento, que é onde este frontend é depurado
    // antes de ir para o console. No console quem manda é o controle.
    //
    // **O B do controle chega aqui como `Escape`**, e é por isso que esta tecla já era o "voltar" —
    // mas o `GamepadB` cru também é aceito: o XAML do Xbox manda o B como as duas coisas conforme
    // o controle que o pegou, e aceitar as duas custa uma comparação a mais e evita um "voltar" que
    // não volta em algum modelo de controle.
    //
    // **O `Handled` é o que impede o sistema de levar o B.** O Xbox trata o B não consumido como
    // "voltar", e no fim da linha isso é sair do aplicativo — no meio da partida inclusive. Quem
    // decide o que o B faz é o `TrataVoltar`, e o que ele não consumir fica para o sistema.
    if (tecla != Windows::System::VirtualKey::Escape &&
        tecla != Windows::System::VirtualKey::GamepadB) {
        return;
    }
    args.Handled(true);
    // **A repetição da tecla presa não conta.** Segurar o B dispara `KeyDown` a cada repetição do
    // teclado, e sem esta guarda o B preso na pausa a abriria e fecharia em sequência. O `Handled`
    // acima já foi posto: o que não pode é o sistema ficar com o B repetido.
    if (args.KeyStatus().WasKeyDown) {
        return;
    }
    TrataVoltar();
}

bool MainPage::TrataVoltar() {
    if (m_varrendo) {
        // **O B não faz nada enquanto o aplicativo está trabalhando.** Não há para onde voltar: a
        // lista na tela é a da varredura anterior, e trocar de tela no meio de um trabalho que
        // termina noutra linha é o que deixa o foco num controle que acabou de sumir. O `true` é o
        // que impede o sistema de levar o B como "sair do aplicativo" no meio da leitura.
        return true;
    }
    // **Duas portas para o mesmo B, e uma ação só.** O B chega pela tecla (`Escape`/`GamepadB`) e
    // pelo "voltar" do sistema; o sistema levanta o segundo quando o primeiro não foi consumido,
    // mas nada garante que ele não levante os dois. A janela curta é o que impede a pausa de fechar
    // e reabrir no mesmo toque.
    uint64_t agora = ::GetTickCount64();
    if (agora - m_ultimoVoltar < 250) {
        return true;
    }
    m_ultimoVoltar = agora;

    if (m_jogando) {
        // **Com o jogo correndo, o B é do jogo.** Ele é o botão 2 do Zeebo e já foi para lá pelo
        // `Input`, que lê o controle direto e não passa pelo XAML. O que não pode é o sistema tomar
        // o mesmo B como "sair do aplicativo": era esse o defeito — o B tirava o jogador da partida
        // e o deixava no menu do Xbox. Quem sai do jogo é o `View`, pela pausa.
        if (m_pausado) {
            AlternaPausa();
            App::Anota(L"voltar: fechou a pausa");
        } else if (!m_voltarAnotado) {
            // **Uma linha por partida, e não uma por toque.** O B é botão de jogo: num jogo de
            // plataforma ele é apertado às centenas, e o relatório não pode virar isso.
            m_voltarAnotado = true;
            App::Anota(L"voltar: o jogo fica com o B (o sistema nao o leva)");
        }
        return true;
    }
    // O B sai de qualquer folha, e no explorador ele sobe um nível antes de sair da tela: é o
    // "voltar" de todo mundo, na mesma ordem em que a navegação desceu.
    if (Fontes().Visibility() == Visibility::Visible) {
        if (!m_caminho.empty()) {
            OnSubirPasta(nullptr, nullptr);
            App::Anota(L"voltar: subiu um nivel no explorador");
        } else {
            OnVoltar(nullptr, nullptr);
            App::Anota(L"voltar: saiu das fontes");
        }
        return true;
    }
    if (Configuracoes().Visibility() == Visibility::Visible ||
        Sobre().Visibility() == Visibility::Visible) {
        OnVoltar(nullptr, nullptr);
        App::Anota(L"voltar: saiu da folha");
        return true;
    }
    // **Na estante o B fica com o sistema.** Sair para o menu do Xbox é o que o B faz num
    // aplicativo de console, e quem está na estante não está no meio de nada. A linha existe para
    // que "o aplicativo saiu sozinho" tenha uma data e uma hora no relatório.
    App::Anota(L"voltar: nada a consumir nesta tela; o B fica com o sistema");
    return false;
}

void MainPage::OnVoltarDoSistema(IInspectable const&, winrt::Windows::UI::Core::BackRequestedEventArgs const& args) {
    // **É esta a porta que o Xbox conhece.** O "voltar" do sistema chega por fora do XAML, e um
    // evento não consumido leva o aplicativo embora: `Handled` é a diferença entre "o B é do jogo"
    // e "o B fecha o Zeebx".
    args.Handled(TrataVoltar());
}

fire_and_forget MainPage::MostraAviso(std::wstring titulo, std::wstring texto) {
    ContentDialog aviso;
    aviso.XamlRoot(Raiz().XamlRoot());
    // O `hstring` explícito é o que o `box_value` precisa: ele empacota o tipo que recebe, e um
    // `std::wstring` não tem interface do WinRT nenhuma para ser empacotada.
    aviso.Title(box_value(hstring(titulo)));
    aviso.Content(box_value(hstring(texto.empty() ? Texto("xbox.details_none") : texto)));
    aviso.CloseButtonText(Rotulo("xbox.ok"));
    co_await aviso.ShowAsync();
}

}  // namespace winrt::ZeebxXbox::implementation
