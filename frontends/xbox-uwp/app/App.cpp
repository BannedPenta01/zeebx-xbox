#include "pch.h"
#include "App.h"
#include "Jit.h"
#include "MainPage.h"
#include "zeebx_xbox.h"

#include <string>

using namespace winrt;
using namespace winrt::Windows::ApplicationModel::Activation;
using namespace winrt::Windows::UI::Xaml;

namespace winrt::ZeebxXbox::implementation {

namespace {

/// O caminho do relatório, guardado no arranque.
///
/// **O `RelataFalha` não pode descobri-lo na hora.** Chegar nele passa pelo WinRT
/// (`ApplicationData::Current`), que aloca — e o processo que entra ali já morreu, com o heap em
/// estado desconhecido. Guardado uma vez, é só leitura.
std::wstring g_relato;

/// Escreve uma linha no `LocalState\zeebx-xbox.log`, em UTF-8, por Win32.
///
/// `abrirDoZero` decide as duas metades do relatório: o arranque começa o arquivo, e tudo o mais
/// acrescenta a ele. Acrescentar sem abrir do zero precisa de `FILE_APPEND_DATA`, que grava no fim
/// sem mexer no ponteiro do arquivo.
///
/// **`CreateFile2`, e não `CreateFileW`** — o `CreateFileW` não existe no recorte de API de um
/// AppContainer: com `WINAPI_FAMILY=WINAPI_FAMILY_APP` ele nem chega a ser declarado, e o erro é
/// `C3861: 'CreateFileW': identifier not found`, que parece header faltando mas é a família de API.
/// A explicação inteira está no comentário do `LeArquivoDoPacote`, no `Renderer.cpp`.
void EscreveNoRelato(std::wstring const& linha, bool abrirDoZero) {
    try {
        std::wstring caminho{Windows::Storage::ApplicationData::Current().LocalFolder().Path()};
        caminho += L"\\zeebx-xbox.log";
        g_relato = caminho;

        HANDLE arquivo = ::CreateFile2(caminho.c_str(),
                                       abrirDoZero ? GENERIC_WRITE : FILE_APPEND_DATA, FILE_SHARE_READ,
                                       abrirDoZero ? CREATE_ALWAYS : OPEN_ALWAYS, nullptr);
        if (arquivo == INVALID_HANDLE_VALUE) {
            return;
        }
        std::string utf8 = to_string(linha);
        utf8 += "\r\n";
        DWORD escritos = 0;
        ::WriteFile(arquivo, utf8.data(), static_cast<DWORD>(utf8.size()), &escritos, nullptr);
        ::CloseHandle(arquivo);
    } catch (...) {
        // Se nem o registro sai, não há mais o que fazer daqui; o aplicativo segue.
    }
}

/// Escreve uma linha já pronta, sem alocar e sem exceções.
///
/// **É o caminho da falha.** Roda depois de um acesso inválido, com o processo a caminho da morte:
/// tudo o que possa alocar, pegar um bloqueio ou lançar atrapalha em vez de registrar. Daqui saem um
/// `CreateFile2` para acrescentar, um `WriteFile` e o arquivo fechado — nada mais.
void EscreveLinhaCrua(const wchar_t* linha) {
    if (g_relato.empty()) {
        return;
    }

    // A linha é montada em `wchar_t` e vira ASCII aqui, byte a byte. O que não couber em ASCII não
    // aparece: é um relatório de falha, e um endereço em hexadecimal caberia em qualquer lugar.
    char utf8[512];
    size_t quantos = 0;
    for (const wchar_t* letra = linha; *letra != L'\0' && quantos < sizeof(utf8) - 3; ++letra) {
        utf8[quantos++] = (*letra < 128) ? static_cast<char>(*letra) : '?';
    }
    utf8[quantos++] = '\r';
    utf8[quantos++] = '\n';

    HANDLE arquivo = ::CreateFile2(g_relato.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, OPEN_ALWAYS, nullptr);
    if (arquivo == INVALID_HANDLE_VALUE) {
        return;
    }
    DWORD escritos = 0;
    ::WriteFile(arquivo, utf8, static_cast<DWORD>(quantos), &escritos, nullptr);
    ::CloseHandle(arquivo);
}

/// Um hexadecimal dígito a dígito, sem `swprintf` e sem alocar.
void PoeHex(wchar_t*& fim, uint64_t valor) {
    const wchar_t* digitos = L"0123456789abcdef";
    wchar_t aoContrario[16];
    int quantos = 0;
    do {
        aoContrario[quantos++] = digitos[valor & 0xF];
        valor >>= 4;
    } while (valor != 0);
    while (quantos > 0) {
        *fim++ = aoContrario[--quantos];
    }
}

void PoeTexto(wchar_t*& fim, const wchar_t* texto) {
    while (*texto != L'\0') {
        *fim++ = *texto++;
    }
}

/// A falha que mata o processo, registrada antes de o sistema derrubá-lo.
///
/// **Por que isto existe.** Num console não há depurador para pendurar: o relatório de falha do
/// Windows (`MoAppCrash`) dá o código da exceção e o deslocamento do endereço, e mais nada — nem o
/// motivo, nem se a falha foi de leitura ou de escrita. Foi atrás de uma dessas linhas que uma tarde
/// inteira se foi, até o mapa de símbolos dar o nome da função. Aqui a mesma informação sai no
/// relatório do aplicativo, ao lado das outras linhas, com o deslocamento **relativo ao próprio
/// executável**, que é exatamente o que o mapa de símbolos indexa. Um `Fault offset` de `4e6534`
/// vira uma linha que diz o endereço, o tipo de acesso e o alvo — e o mapa, ao lado, diz a função.
///
/// O retorno é `EXCEPTION_CONTINUE_SEARCH`: registrado o que havia para registrar, o sistema segue
/// com o que faria, e o relatório dele continua sendo escrito.
LONG WINAPI RelataFalha(EXCEPTION_POINTERS* informacao) {
    if (informacao == nullptr || informacao->ExceptionRecord == nullptr) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    const EXCEPTION_RECORD* registro = informacao->ExceptionRecord;

    HMODULE proprio = ::GetModuleHandleW(nullptr);
    const uintptr_t base = reinterpret_cast<uintptr_t>(proprio);
    const uintptr_t endereco = reinterpret_cast<uintptr_t>(registro->ExceptionAddress);

    // **O tamanho da imagem sai do próprio PE, e não do `psapi`** — que não existe no recorte de API
    // de um AppContainer. Um cabeçalho não falta: isto é leitura de memória, e serve para dizer se o
    // endereço da falha é nosso ou de uma biblioteca do sistema.
    uintptr_t tamanho = 0;
    if (proprio != nullptr) {
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(proprio);
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
        tamanho = nt->OptionalHeader.SizeOfImage;
    }

    wchar_t linha[256];
    wchar_t* fim = linha;
    PoeTexto(fim, L"falha: ");
    if (registro->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && registro->NumberParameters >= 2) {
        const ULONG_PTR tipo = registro->ExceptionInformation[0];
        PoeTexto(fim, L"violacao de acesso (");
        PoeHex(fim, registro->ExceptionCode);
        PoeTexto(fim, L"): ");
        PoeTexto(fim, tipo == 1 ? L"escrita no endereco 0x"
                                : (tipo == 8 ? L"execucao em 0x" : L"leitura no endereco 0x"));
        PoeHex(fim, registro->ExceptionInformation[1]);
        PoeTexto(fim, L", codigo em 0x");
    } else {
        PoeTexto(fim, L"codigo ");
        PoeHex(fim, registro->ExceptionCode);
        PoeTexto(fim, L" em 0x");
    }
    PoeHex(fim, endereco);
    if (tamanho != 0 && endereco >= base && endereco < base + tamanho) {
        PoeTexto(fim, L", +0x");
        PoeHex(fim, endereco - base);
        PoeTexto(fim, L" no executavel");
    } else {
        PoeTexto(fim, L", fora do executavel");
    }
    *fim = L'\0';

    EscreveLinhaCrua(linha);
    return EXCEPTION_CONTINUE_SEARCH;
}

/// A exceção de C++ que ninguém pegou — o outro jeito de o processo morrer sem dizer por quê.
void Terminou() {
    EscreveLinhaCrua(L"falha: excecao de C++ nao tratada (o programa pediu para terminar)");
    ::abort();
}

/// `sim`/`nao` para o relatório — um `bool` cru num log é lido errado.
std::wstring Sim(bool valor) {
    return valor ? L"sim" : L"nao";
}

/// Um caminho percorrido pela sonda, passo a passo, para o relatório.
///
/// **Os passos vão todos, e não só o veredito.** "não deu" num console não diz o que tentar depois;
/// "reservar sim, confirmar sim, virar executavel nao (erro 87)" diz — e é a diferença entre uma
/// sessão e três.
std::wstring Passos(ZeebxUwp::PassosDaMemoria const& passos) {
    return L"reservar " + Sim(passos.reservou) + L", confirmar " + Sim(passos.confirmou) +
           L", escrever " + Sim(passos.escreveu) + L", virar executavel " +
           Sim(passos.virouExecutavel) + L", executar " + Sim(passos.rodou) +
           L" (retorno esperado 42), erro " + std::to_wstring(passos.erro);
}

}  // namespace

void App::ComecaRelato() {
    EscreveNoRelato(L"--- Zeebx no Xbox: relatorio desta execucao ---", true);
}

void App::Anota(std::wstring const& linha) {
    EscreveNoRelato(linha, false);
}

void App::Inicializar() {
    ComecaRelato();

    // **As duas redes que registram a morte, penduradas antes de tudo.** O `RelataFalha` pega o
    // acesso inválido — que num console é a diferença entre uma linha no relatório e um aplicativo
    // que "simplesmente fecha" —, e o `Terminou` pega a exceção de C++ que escapou. Nenhum dos dois
    // muda o que o sistema faz em seguida: os dois registram e devolvem a decisão para ele.
    ::SetUnhandledExceptionFilter(RelataFalha);
    std::set_terminate(Terminou);

    try {
        RequiresPointerMode(ApplicationRequiresPointerMode::WhenRequested);
        // Medido no PC: a chamada passa, e a linha sai assim. Não é prova de que o console a
        // honre — é o registro de que o pedido foi feito e não foi recusado, que é o que um
        // relatório pode afirmar.
        Anota(L"modo de entrada: foco (RequiresPointerMode = WhenRequested)");
    } catch (hresult_error const& falha) {
        // A recusa é notícia. `E_NOTIMPL` diz que a propriedade não existe neste aparelho; outro
        // código diz outra coisa, e é a linha do código que separa uma causa da outra — sem ela,
        // "não funciona" num console fica sem o porquê, que é o que faz um tropeço durar dias.
        Anota(L"modo de entrada: PONTEIRO (RequiresPointerMode recusado: " +
              std::wstring{falha.message().c_str()} + L", codigo " +
              std::to_wstring(static_cast<uint32_t>(falha.code().value)) + L")");
    }

    // **A sonda da memória executável.** Ela roda aqui, e não quando um jogo abre, por dois
    // motivos: é o primeiro instante em que dá para perguntar ao sistema, e a resposta é o que
    // decide se o núcleo do emulador pode recompilar o código do jogo ou tem de interpretá-lo.
    //
    // O relatório traz as duas medidas, passo a passo: a do bloco de dezesseis megabytes — o
    // tamanho que o alocador do JIT pede de verdade — e a de uma página. É a comparação entre as
    // duas que diz se o aparelho proíbe memória executável ou apenas limita quanto dela se pede.
    // Ver `Jit.h`, e a seção "A memória executável" do documento do porte.
    {
        auto prova = ZeebxUwp::SondaMemoriaExecutavel();
        Anota(L"memoria executavel (bloco de 16 MB, o tamanho que o JIT pede): " +
              Passos(prova.bloco));
        Anota(L"memoria executavel (uma pagina): " + Passos(prova.pagina));
        Anota(L"memoria executavel, RWX de uma vez: " + Sim(prova.rwxRecusado) +
              L" recusado (erro " + std::to_wstring(prova.erroDoRwx) + L")");

        // **A resposta vai para o núcleo aqui, e não na hora de abrir o jogo.** É o último instante
        // em que a escolha ainda é grátis: os dois núcleos estão compilados, e o emulador decide na
        // abertura da sessão. Feita depois, a escolha deixa de existir — um JIT que não consegue
        // alocar a página não falha ao compilar, falha ao executar, no meio do jogo, e daí não se
        // volta.
        //
        // A linha abaixo é a decisão; qual dos dois núcleos entrou de fato quem diz é o
        // `zeebx_xbox_nucleo` do arranque do jogo, que lê a resposta do próprio emulador. As duas
        // juntas é que fecham: uma sem a outra não distingue "a decisão não chegou" de "chegou e
        // não valeu".
        zeebx_xbox_permite_jit(prova.podeExecutar() ? 1 : 0);
        Anota(prova.podeExecutar()
                  ? L"nucleo do emulador: dynarmic (JIT), memoria executavel concedida"
                  : L"nucleo do emulador: interpretador, memoria executavel negada");
    }
}

void App::OnLaunched(LaunchActivatedEventArgs const&) {
    auto janela = Window::Current();
    janela.Content(make<ZeebxXbox::implementation::MainPage>());
    janela.Activate();
}

}  // namespace winrt::ZeebxXbox::implementation

// **O `wWinMain` não mora mais aqui, de propósito.** Ele agora vem do
// `Generated Files\App.xaml.g.hpp`, que o compilador de XAML escreve: faz o `init_apartment`,
// chama o `Application::Start` e constrói esta `App`. Duas definições dariam
// `LNK2005: wWinMain already defined`; a saída seria definir o `DISABLE_XAML_GENERATED_MAIN`,
// que desliga a versão gerada **inteira** — inclusive a inicialização do apartamento que o XAML
// espera, e que é justamente o que uma cópia nossa teria de repetir de memória. Ficar com a
// gerada é o que o modelo "Blank App (C++/WinRT)" do VS faz.
