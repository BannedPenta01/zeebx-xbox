#pragma once

#include "pch.h"
// Gerado pelo compilador de XAML a partir do `App.xaml`: é daqui que vem o `AppT`, e é ele — e
// não o `ApplicationT` do SDK — que a `App` tem de estender. O `AppT` acrescenta o
// `IXamlMetadataProvider`: é por ele que o carregador do XAML resolve os tipos citados nos
// `.xaml` em tempo de execução. Com o `ApplicationT` direto, o aplicativo compila, abre e morre
// sem achar o próprio `MainPage`. Também é daqui que sai o `InitializeComponent` da `App`.
#include "App.xaml.g.h"

namespace winrt::ZeebxXbox::implementation {

struct App : AppT<App> {
    App() {
        // Sem isto as exceções do XAML somem numa tela preta sem explicação — e num console
        // sem depurador isso é indistinguível de "o emulador travou".
        // O primeiro parâmetro é `IInspectable`, e não `Application`: o `UnhandledException` é um
        // `EventHandler<T>`, cujo remetente é genérico. Com `Application` o compilador recusa com
        // `C2664: cannot convert argument 1 from 'const winrt::Windows::Foundation::IInspectable'`.
        UnhandledException([](winrt::Windows::Foundation::IInspectable const&,
                              winrt::Windows::UI::Xaml::UnhandledExceptionEventArgs const& args) {
            OutputDebugStringW(args.Message().c_str());
            // **A exceção também vai para o relatório.** O `OutputDebugString` na TV não existe, e
            // é numa exceção do XAML que um aplicativo do console morre sem dizer nada.
            Anota(std::wstring{L"excecao: "} + args.Message().c_str());
        });

        // Antes de qualquer página existir: é aqui que o aplicativo deixa de ser um aplicativo de
        // ponteiro. Ver `Inicializar`.
        Inicializar();
    }

    void OnLaunched(winrt::Windows::ApplicationModel::Activation::LaunchActivatedEventArgs const& args);

    /// Zera o relatório e escreve a primeira linha.
    ///
    /// **O relatório é o único instrumento que existe dentro de um console.** Não há depurador, e
    /// o File Explorer do Portal do Dispositivo lê `LocalState`: é por ali que se descobre o que o
    /// aparelho respondeu. O arquivo é reescrito a cada arranque de propósito — ele descreve
    /// **esta** execução, e um histórico que ninguém lê só atrapalha.
    static void ComecaRelato();

    /// Acrescenta uma linha ao relatório do arranque.
    ///
    /// **A escrita é do Windows, e não do WinRT.** Isto pode ser chamado do construtor do `App`, na
    /// linha da interface, e um `FileIO::AppendTextAsync(...).get()` ali é o abraço mortal clássico:
    /// a linha da interface espera uma operação que só termina nela mesma, e o aplicativo trava
    /// antes de abrir. `LocalFolder().Path()` é uma propriedade síncrona, e um `CreateFile2` resolve
    /// o resto. `CreateFile2`, e não `CreateFileW`, porque o AppContainer não tem o segundo — a
    /// explicação inteira está no `LeArquivoDoPacote`, no `Renderer.cpp`.
    static void Anota(std::wstring const& linha);

private:
    /// Tira o aplicativo do modo de ponteiro.
    ///
    /// **O Xbox abre todo aplicativo em modo de ponteiro**, e num controle isto é um cursor virtual
    /// que anda com o analógico: o direcional não anda o foco, e o A clica onde o cursor estiver. É
    /// a explicação de "o menu só destaca e não faz nada" — a tela não é feita para ponteiro
    /// nenhum, ela é feita de foco.
    ///
    /// `WhenRequested` (e não `Always`, que é o outro valor) devolve o ponteiro só quando um
    /// controle o pede de verdade — nenhum daqui pede, então na prática é o foco que manda.
    ///
    /// **No PC a chamada passa.** Medido: o relatório sai com "modo de entrada: foco
    /// (RequiresPointerMode = WhenRequested)", e não com a recusa — a propriedade existe também no
    /// Windows de mesa, e não só no console. O `try` fica de pé mesmo assim: o que se mediu foi este
    /// Windows, e uma propriedade que o sistema operacional aceita sem usar não é o mesmo que uma
    /// propriedade que o console honra. **Se o console a honra é o que ninguém mediu ainda** — o
    /// relatório de lá é quem vai dizer, e por isso a linha sai sempre.
    void Inicializar();
};

}  // namespace winrt::ZeebxXbox::implementation
