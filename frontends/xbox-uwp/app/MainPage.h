#pragma once

#include "pch.h"
// Gerado pelo compilador de XAML a partir do `MainPage.xaml`: é daqui que vem o `MainPageT`,
// com as propriedades e os eventos que o XAML nomeia.
#include "MainPage.g.h"

#include "Audio.h"
#include "Input.h"
#include "Renderer.h"

// `std::string_view` para o `Texto`, que recebe a chave como literal e não precisa de um
// `std::string` montado a cada chamada.
#include <string_view>

// O ponteiro opaco do núcleo. A definição mora no crate Rust; aqui ele nunca é desreferenciado.
struct ZeebxXbox;

namespace winrt::ZeebxXbox::implementation {

/// A única página do aplicativo: a estante, o jogo e a pausa, em camadas.
///
/// O laço é o `CompositionTarget::Rendering`, e não um `Timer`: ele dispara no compasso do
/// quadro da própria tela, que é o que evita o emulador e a composição do XAML brigarem pelo
/// mesmo ritmo — um `Timer` de 60 Hz numa tela de 60 Hz anda fora de fase e engasga.
struct MainPage : MainPageT<MainPage> {
    MainPage();
    ~MainPage();

    void OnCarregado(winrt::Windows::Foundation::IInspectable const&,
                     winrt::Windows::UI::Xaml::RoutedEventArgs const&);
    void OnTamanhoMudou(winrt::Windows::Foundation::IInspectable const&,
                        winrt::Windows::UI::Xaml::SizeChangedEventArgs const&);
    void OnTecla(winrt::Windows::Foundation::IInspectable const&,
                 winrt::Windows::UI::Xaml::Input::KeyRoutedEventArgs const&);
    /// A escala de composição do painel mudou: o quadro precisa ser remedido.
    ///
    /// **É o console que traz esta mudança.** No PC a escala já está posta quando o primeiro
    /// tamanho chega; no console ela chega depois, e é por isso que ouvir este evento é a
    /// diferença entre o quadro nascer no tamanho certo e nascer no tamanho lógico.
    void OnEscalaDaComposicaoMudou(winrt::Windows::UI::Xaml::Controls::SwapChainPanel const&,
                                   winrt::Windows::Foundation::IInspectable const&);
    /// **O "voltar" do sistema, que é a segunda porta do B e a que o sistema conhece.** A tecla
    /// chega pelo `OnTecla`; este evento é o que o Xbox levanta por fora do XAML, e um aplicativo
    /// que não o consome **sai para o menu do Xbox** — no meio da partida inclusive.
    void OnVoltarDoSistema(winrt::Windows::Foundation::IInspectable const&,
                           winrt::Windows::UI::Core::BackRequestedEventArgs const&);
    /// O A no cartão de um jogo, ou um clique nele.
    ///
    /// **É `RoutedEventArgs`, e não `ItemClickEventArgs`.** O `ItemClick` do `GridView` só chega
    /// pelo ponteiro; quem tem o foco — e quem o A aciona — é o `Button` de dentro do cartão. O
    /// jogo que ele representa sai do `IndiceDoCartao`, e não do argumento do evento.
    void OnJogoEscolhido(winrt::Windows::Foundation::IInspectable const&,
                         winrt::Windows::UI::Xaml::RoutedEventArgs const&);
    /// O A que a própria lista recebeu, quando quem tem o foco é o contêiner e não o cartão.
    ///
    /// **A segunda rota, e não a única.** Qual das duas o console aciona depende de onde o foco
    /// parou, e isso não se mede num PC; as duas resolvem o índice pelo caminho exato e as duas
    /// chegam ao mesmo `AbreJogo`, que ignora a segunda.
    void OnJogoClicado(winrt::Windows::Foundation::IInspectable const&,
                       winrt::Windows::UI::Xaml::Controls::ItemClickEventArgs const&);
    /// A arte do jogo, quando o cartão dele aparece na estante.
    ///
    /// **É o `ContainerContentChanging` da lista, e não um `Binding`.** Dentro de um `DataTemplate`
    /// o `{Binding}` entrega o título; pixels vêm do núcleo. O evento do contêiner é o único ponto
    /// em que o código sabe, ao mesmo tempo, qual índice da estante está ali (`ItemIndex`) e que o
    /// contêiner acabou de nascer — e ele é levantado de novo a cada rolagem, no contêiner
    /// reciclado, que é o que faz o cartão seguinte mostrar a arte dele.
    winrt::fire_and_forget OnConteinerMudou(
        winrt::Windows::UI::Xaml::Controls::ItemsControl const&,
        winrt::Windows::UI::Xaml::Controls::ContainerContentChangingEventArgs const&);
    /// O A numa pasta do explorador. Mesma razão do `OnJogoEscolhido`.
    void OnPastaEscolhida(winrt::Windows::Foundation::IInspectable const&,
                          winrt::Windows::UI::Xaml::RoutedEventArgs const&);
    /// A pasta que a própria lista recebeu. Mesma razão do `OnJogoClicado`.
    void OnPastaClicada(winrt::Windows::Foundation::IInspectable const&,
                        winrt::Windows::UI::Xaml::Controls::ItemClickEventArgs const&);
    /// O A no botão do seletor de pasta: abre a caixa de pasta do sistema.
    ///
    /// **A caixa do sistema, e não o nosso explorador.** Quem conhece as pastas é o aparelho, e uma
    /// caixa do sistema é a que o jogador já sabe usar. A Microsoft documenta que cenários de
    /// seletor de pasta não são suportados no Xbox — e é por isso que existe o botão ao lado, e não
    /// um malabarismo de tentativa e erro que adivinhe em que aparelho estamos.
    winrt::fire_and_forget OnEscolherPasta(winrt::Windows::Foundation::IInspectable const&,
                                           winrt::Windows::UI::Xaml::RoutedEventArgs const&);
    /// O A no botão do seletor de arquivo: escolhe um jogo, e a pasta é a que o contém.
    ///
    /// **É o caminho do console.** O seletor de arquivo existe no Xbox; a pasta sai do caminho do
    /// arquivo escolhido, e lá quem dá o acesso à unidade é a capacidade do manifesto, e não a
    /// caixa. Os dois botões ficam à vista de propósito: um aparelho que não tem o primeiro não
    /// pode deixar o jogador sem o segundo, e um botão que não faz nada e não diz por quê é pior do
    /// que um botão que explica o que faz.
    winrt::fire_and_forget OnEscolherJogo(winrt::Windows::Foundation::IInspectable const&,
                                          winrt::Windows::UI::Xaml::RoutedEventArgs const&);
    winrt::fire_and_forget OnAtualizar(winrt::Windows::Foundation::IInspectable const&,
                                       winrt::Windows::UI::Xaml::RoutedEventArgs const&);
    void OnAbrirFontes(winrt::Windows::Foundation::IInspectable const&,
                       winrt::Windows::UI::Xaml::RoutedEventArgs const&);
    void OnAbrirConfiguracoes(winrt::Windows::Foundation::IInspectable const&,
                              winrt::Windows::UI::Xaml::RoutedEventArgs const&);
    void OnAbrirSobre(winrt::Windows::Foundation::IInspectable const&,
                      winrt::Windows::UI::Xaml::RoutedEventArgs const&);
    void OnVoltar(winrt::Windows::Foundation::IInspectable const&,
                  winrt::Windows::UI::Xaml::RoutedEventArgs const&);
    void OnSalvar(winrt::Windows::Foundation::IInspectable const&,
                  winrt::Windows::UI::Xaml::RoutedEventArgs const&);
    void OnCarregar(winrt::Windows::Foundation::IInspectable const&,
                    winrt::Windows::UI::Xaml::RoutedEventArgs const&);
    void OnContinuar(winrt::Windows::Foundation::IInspectable const&,
                     winrt::Windows::UI::Xaml::RoutedEventArgs const&);
    void OnSairDoJogo(winrt::Windows::Foundation::IInspectable const&,
                      winrt::Windows::UI::Xaml::RoutedEventArgs const&);
    void OnVolumeMudou(winrt::Windows::Foundation::IInspectable const&,
                       winrt::Windows::UI::Xaml::Controls::Primitives::RangeBaseValueChangedEventArgs const&);
    void OnProporcaoMudou(winrt::Windows::Foundation::IInspectable const&,
                          winrt::Windows::UI::Xaml::Controls::SelectionChangedEventArgs const&);
    void OnEscalaMudou(winrt::Windows::Foundation::IInspectable const&,
                       winrt::Windows::UI::Xaml::RoutedEventArgs const&);
    void OnContadoresMudou(winrt::Windows::Foundation::IInspectable const&,
                           winrt::Windows::UI::Xaml::RoutedEventArgs const&);
    void OnIdiomaMudou(winrt::Windows::Foundation::IInspectable const&,
                       winrt::Windows::UI::Xaml::Controls::SelectionChangedEventArgs const&);
    void OnSubirPasta(winrt::Windows::Foundation::IInspectable const&,
                      winrt::Windows::UI::Xaml::RoutedEventArgs const&);
    void OnUsarPasta(winrt::Windows::Foundation::IInspectable const&,
                     winrt::Windows::UI::Xaml::RoutedEventArgs const&);
    void OnTirarFonte(winrt::Windows::Foundation::IInspectable const&,
                      winrt::Windows::UI::Xaml::RoutedEventArgs const&);

private:
    /// A volta do laço: entrega o controle, avança o jogo e publica quadro e som.
    void OnQuadro(winrt::Windows::UI::Xaml::Media::CompositionTarget const&,
                  winrt::Windows::Foundation::IInspectable const&);

    void MontaLaco();
    void DesmontaLaco();
    /// Deixa só `alvo` visível entre as camadas de tela cheia.
    void Mostra(winrt::Windows::UI::Xaml::UIElement const& alvo);
    /// Começa uma varredura e volta na hora, com a camada de "procurando" de pé.
    ///
    /// **É esta função que tirou a varredura da linha da interface.** A chamada antiga
    /// (`zeebx_xbox_varre`) devolve quando terminou, e uma unidade removível cheia leva segundos:
    /// no arranque, era uma estante parada que não respondia a nada; no "Varrer de novo", a mesma
    /// coisa. O fio é do núcleo; daqui sai a camada de recado e um relógio que pergunta se acabou.
    void ComecaVarredura();
    /// O relógio da varredura: pergunta ao núcleo se a lista já pode ser colhida.
    void OnRelogioDaVarredura(winrt::Windows::Foundation::IInspectable const&,
                              winrt::Windows::Foundation::IInspectable const&);
    /// Monta os cartões da estante a partir da lista que o núcleo já tem. **Não varre nada.**
    void MontaEstante();
    /// Recua a GUI para dentro da área que a TV não corta, e não mexe no vídeo.
    ///
    /// **O quadro do jogo fica de fora de propósito.** Ele é a tela inteira do console, e uma
    /// margem nele seria uma borda preta em volta da imagem; quem precisa recuar é o que a TV
    /// costuma cortar — texto e botões, que ficavam a 20 DIPs da borda.
    void AplicaMargemSegura();
    /// A camada de "estou trabalhando", com o recado da chave dada.
    ///
    /// **Ela esconde as camadas de tela cheia enquanto está de pé**, e as devolve quando sai. Não
    /// é por estética: foco do controle anda por todo elemento visível da árvore, e um `Grid` não
    /// tem como ser desligado (`UIElement` não tem `IsEnabled`). O `Mostra`, que já sabe quais são
    /// as camadas e qual delas estava à vista, é quem faz isso — ver `m_camadaDoOcupado`.
    void MostraOcupado(std::string_view chave);
    void EscondeOcupado();
    /// O subtítulo da estante: a frase da casa sem jogo, e a contagem com jogo.
    ///
    /// **Vem separado da varredura de propósito.** Trocar de língua reescreve o subtítulo e não
    /// pode custar uma varredura: numa unidade removível ela leva segundos.
    void AtualizaSubtitulo();
    /// Abre o jogo `indice`: avisa na tela, carrega e monta o laço.
    ///
    /// **É uma co-rotina porque o carregamento é longo e a tela precisa dizer isso.** O `comeca` do
    /// núcleo descompacta e carrega na chamada; a camada de recado tem de estar pintada antes, e a
    /// volta ao despachante é o que garante isso.
    winrt::fire_and_forget AbreJogo(int32_t indice);
    void SaiDoJogo();
    void AlternaPausa();
    /// Remede o quadro: o tamanho da raiz em DIPs e a escala de composição do painel.
    ///
    /// **São duas medidas de duas fontes, e é de propósito.** O tamanho vem da raiz porque o painel
    /// do vídeo está colapsado enquanto a estante aparece e mediria zero; a escala vem do painel,
    /// porque é ele que compõe a cadeia de troca e é dele que sai o número que o D3D espera.
    void AplicaTamanhoDoVideo();
    /// O B, venha ele da tecla ou do "voltar" do sistema. Devolve `true` quando a tela o consumiu.
    ///
    /// **Uma ação só para duas portas.** No console o B chega como tecla e como o "voltar" do
    /// sistema, e as duas juntas fariam a pausa abrir e fechar no mesmo toque.
    bool TrataVoltar();
    void AtualizaEstadoDosSlots();
    /// A lista das pastas de fora que a biblioteca lê, com a marca de quando a pasta sumiu.
    void AtualizaFontes();
    /// O índice, na lista, do cartão que contém `elemento`.
    ///
    /// **É a resposta exata, e é por isso que ela caminha a árvore.** O caminho curto seria
    /// procurar o título do cartão na lista (`Items().IndexOf`), e dois jogos de mesmo nome
    /// abririam sempre o primeiro — um defeito que só aparece com a biblioteca cheia, que é
    /// exatamente quando ninguém está olhando. O `GridViewItem` sabe o próprio índice
    /// (`IndexFromContainer`), e o `Button` está a dois pais de distância dele.
    int32_t IndiceDoCartao(winrt::Windows::UI::Xaml::Controls::ItemsControl const& lista,
                           winrt::Windows::UI::Xaml::DependencyObject const& elemento);
    /// O índice, na lista, do item que o próprio `GridView` apontou no `ItemClick`.
    ///
    /// **`ContainerFromItem` e não um `IndexOf` no título.** O `ItemClick` entrega o item, e o
    /// contêiner dele é quem sabe o índice — o mesmo cuidado do `IndiceDoCartao`, pela outra ponta:
    /// dois jogos de mesmo nome não podem abrir o primeiro sempre.
    int32_t IndiceDoItem(winrt::Windows::UI::Xaml::Controls::ItemsControl const& lista,
                         winrt::Windows::Foundation::IInspectable const& item);
    /// Entra na pasta `alvo` do explorador. As duas rotas de A passam por aqui.
    void AbrePasta(int32_t alvo);
    /// Põe o foco onde o jogador está: na estante, ou no botão que a substitui quando ela está
    /// vazia.
    ///
    /// **Foco é o que faz o direcional andar.** Uma camada que abre sem foco nenhum é uma camada
    /// em que o controle não faz nada — e uma estante vazia não tem item em que pôr foco.
    void FocaEstante();
    /// Devolve ao aplicativo o acesso às pastas escolhidas pelo seletor, em execuções passadas.
    ///
    /// **É o preço de usar o seletor do sistema.** O que a ABI recebe é um caminho, e caminho não
    /// é permissão: quem guarda a permissão é o `FutureAccessList`, e é dele que a pasta é aberta
    /// de novo a cada arranque. Sem isto, a pasta fica na lista e a varredura não acha nada.
    winrt::fire_and_forget ReataFontes();
    /// Mete uma pasta na biblioteca e refaz as telas que dependem dela.
    ///
    /// **São duas entradas porque são dois caminhos.** A pasta vinda do seletor de pasta ganha um
    /// token no `FutureAccessList`, que é o que a mantém acessível no próximo arranque; a pasta
    /// vinda do explorador de unidades, não — ali quem dá o acesso é a capacidade do manifesto, e
    /// um token não teria o que guardar.
    void AcrescentaPasta(winrt::Windows::Storage::StorageFolder const& pasta);
    /// A parte comum das duas: acrescenta o caminho ao núcleo e refaz estante e fontes.
    void AcrescentaCaminho(std::wstring const& caminho);
    /// Anota no relatório que a caixa do sistema não abriu, e dá o recado na tela.
    ///
    /// **O relatório, e não só a tela.** Num console não há depurador, e o código do erro é a única
    /// coisa que diz *por que* a caixa não abriu: "este aparelho não tem seletor de pasta" e "o
    /// aplicativo não estava em primeiro plano" querem consertos diferentes, e a tela não distingue
    /// um do outro. Sem a linha do erro, a diferença se perde e o conserto vira palpite.
    void RecusaODialogo(std::wstring const& onde, winrt::hresult_error const& falha);
    /// Preenche o explorador: as unidades na raiz, as subpastas depois.
    ///
    /// **É assíncrono porque o `GetFoldersAsync` é.** Uma unidade removível recém-ligada pode
    /// levar séculos para responder, e travar a linha da interface esperando por ela congelaria a
    /// tela.
    winrt::fire_and_forget AtualizaExplorador();
    /// Os dois itens do comboio da proporção, remontados a cada idioma.
    void PreencheProporcao();
    /// Os dez comboios de slot, com o rótulo do idioma em uso.
    void PreencheSlots();
    /// A lista de idiomas, na ordem do núcleo, com o atual marcado.
    void PreencheIdiomas();
    /// Reescreve **todo** texto da tela a partir do catálogo do núcleo.
    void AplicaTextos();
    /// **Não é `const`, e não pode ser.** Os acessores que o compilador de XAML gera para os
    /// `x:Name` (`Slot()`, `SlotDaPausa()`) devolvem `ComboBox` por valor e não são `const`; um
    /// `SlotEscolhido() const` os chamaria por um `this` constante e o compilador recusaria com
    /// `C2663: no overloaded function has valid conversion for 'this' pointer`.
    int32_t SlotEscolhido();
    /// O texto da chave, no idioma em uso, já copiado para `std::wstring`.
    ///
    /// **A cópia é obrigatória, e não zelo:** a ABI devolve um ponteiro que vale até a chamada
    /// seguinte, e há quem peça dois textos para compor um terceiro.
    std::wstring Texto(std::string_view chave);
    /// O mesmo texto, já no `hstring` que os controles do XAML pedem.
    ///
    /// **São duas funções, e não uma, porque o `hstring` é `explicit`.** O construtor do `hstring`
    /// a partir de `std::wstring_view` é `explicit`, e o `hstring` só se converte *para*
    /// `wstring_view`: com uma só, `Text(Texto("chave"))` não compilaria — seriam duas conversões
    /// definidas pelo usuário encadeadas —, e todo rótulo carregaria um `hstring(...)` na frente.
    winrt::hstring Rotulo(std::string_view chave);
    winrt::fire_and_forget MostraAviso(std::wstring titulo, std::wstring texto);

    ZeebxUwp::Renderer m_renderer;
    ZeebxUwp::Audio m_audio;
    ZeebxUwp::Input m_input;
    // **`::ZeebxXbox`, com o `::`.** Sem ele, o nome resolve para o namespace do XAML
    // (`winrt::ZeebxXbox`), que é o mais próximo daqui, e o compilador acusa
    // `C2143: syntax error: missing ';' before '*'` — um erro que não sugere o `::` que falta.
    ::ZeebxXbox* m_app = nullptr;
    winrt::event_token m_assinaturaDoLaco{};
    bool m_lacoLigado = false;
    /// O "voltar" do sistema (o B do console), guardado para poder ser desassinado no fim.
    ///
    /// **Sem guardar o gerenciador não há como desassinar**, e um `BackRequested` que chega depois
    /// do fim da página chama um objeto morto.
    winrt::Windows::UI::Core::SystemNavigationManager m_navegacao{nullptr};
    winrt::event_token m_assinaturaDoVoltar{};
    /// Quando o B foi tratado pela última vez, em milissegundos do sistema.
    ///
    /// **Duas portas para a mesma tecla.** O B chega como `Escape`/`GamepadB` e como o "voltar" do
    /// sistema; sem esta janela, um toque fecharia a pausa e a abriria de volta.
    uint64_t m_ultimoVoltar = 0;
    /// Uma linha de tamanho por mudança de tamanho, e uma de quadro por partida.
    uint32_t m_larguraAnotada = 0;
    uint32_t m_alturaAnotada = 0;
    bool m_quadroAnotado = false;
    /// Se o relatório já viu um quadro com alguma coisa desenhada.
    ///
    /// **É o que separa "não tem vídeo" de "o jogo não desenhou".** Os dois são a mesma tela preta,
    /// e a diferença decide onde o defeito está: na apresentação ou no núcleo.
    bool m_imagemAnotada = false;
    /// Quantos quadros o núcleo já publicou nesta partida, para a linha da primeira imagem.
    uint32_t m_quadrosVistos = 0;
    /// Se o relatório já disse que o B, em jogo, ficou com o jogo.
    ///
    /// **Uma linha por partida e não uma por toque.** O B é botão de jogar: num jogo de plataforma
    /// ele é apertado às centenas, e o relatório não pode virar centenas de linhas iguais.
    bool m_voltarAnotado = false;
    bool m_jogando = false;
    bool m_pausado = false;
    bool m_escalaInteira = false;
    bool m_contadores = false;
    /// Enquanto o `AplicaTextos` remonta uma lista, a troca de marcação que ele dispara não vale.
    ///
    /// **Sem esta trava o programa entra em laço.** O `AplicaTextos` reescreve o nome dos idiomas
    /// e remarca o escolhido; remarcar dispara o `SelectionChanged`, que pede a troca de idioma,
    /// que chama o `AplicaTextos` — para sempre. A mesma trava serve ao comboio da proporção, cuja
    /// lista também é remontada a cada troca de idioma (`PreencheProporcao`).
    bool m_remontandoListas = false;
    float m_proporcao = 4.0f / 3.0f;
    /// Quantos jogos a última varredura achou. O subtítulo sai daqui, e não de uma varredura
    /// nova só para trocar de língua.
    int32_t m_quantidade = 0;
    /// A varredura de fora que está andando, e o relógio que a espera.
    ///
    /// **São dois estados, e não um.** Sem o `m_varrendo` a camada de "procurando" ficaria de pé
    /// para sempre quando o fio não sai; sem o relógio, ninguém perguntaria se ele acabou.
    bool m_varrendo = false;
    /// Quando a varredura de fora começou, para o relatório dizer quanto ela levou.
    ///
    /// **O número é a medida que decide o resto.** "A estante demora" num HD externo cheio pode ser
    /// a varredura de segundos ou a leitura de milhares de `.mif`, e as duas se parecem na tela: a
    /// linha com os milissegundos separa uma da outra.
    uint64_t m_inicioDaVarredura = 0;
    winrt::Windows::UI::Xaml::DispatcherTimer m_relogioDaVarredura{nullptr};
    /// A camada que estava à vista quando o "estou trabalhando" apareceu.
    ///
    /// **É ela que volta quando o trabalho acaba.** O aviso de ocupado esconde as camadas para o
    /// direcional não andar por botões que ninguém está vendo — e quem estava em Fontes volta para
    /// Fontes, e não para a estante.
    winrt::Windows::UI::Xaml::UIElement m_camadaDoOcupado{nullptr};
    /// As pastas por onde o explorador desceu. Vazia = a raiz, que são as unidades.
    ///
    /// **É uma pilha nossa, e não o `GetParentAsync` do WinRT.** Numa unidade removível a pasta
    /// raiz não tem pai que o WinRT devolva; quem sabe por onde se desceu é quem desceu.
    std::vector<winrt::Windows::Storage::StorageFolder> m_caminho;
    /// A listagem que está na tela, na mesma ordem do `Pastas().Items()`: é por ela que o
    /// `OnPastaEscolhida` sabe em que pasta o jogador tocou.
    std::vector<winrt::Windows::Storage::StorageFolder> m_filhos;
    /// As pastas escolhidas pelo seletor do sistema, abertas de novo nesta execução.
    ///
    /// **Guardar o `StorageFolder` é o que mantém o acesso vivo.** A ABI só leva o caminho, e
    /// caminho não é permissão: quem guarda a permissão é o `FutureAccessList`, e a pasta
    /// devolvida por ele é a prova de que ela existe para este aplicativo. Sem esta lista, a
    /// pasta continua escrita na configuração e a varredura não acha nada dentro dela.
    std::vector<winrt::Windows::Storage::StorageFolder> m_fontesVivas;
};

}  // namespace winrt::ZeebxXbox::implementation

namespace winrt::ZeebxXbox::factory_implementation {

struct MainPage : MainPageT<MainPage, implementation::MainPage> {};

}  // namespace winrt::ZeebxXbox::factory_implementation
