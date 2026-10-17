// Jit.cpp — a sonda da memória executável. O porquê está no `Jit.h`.
#include "pch.h"
#include "Jit.h"

namespace ZeebxUwp {

namespace {

/// O que a sonda escreve na memória: `mov eax, 42` e `ret` — seis bytes.
///
/// **Código de máquina de verdade, e não um `nop`.** O que se quer saber é se este processo pode
/// executar o que ele mesmo escreveu; uma memória executável que nunca roda não responde isso, e é
/// por isso que a sonda chama a função e confere o resultado. Os bytes são a codificação de 64
/// bits, que é a única que este aplicativo usa.
constexpr std::uint8_t CODIGO[] = {0xb8, 0x2a, 0x00, 0x00, 0x00, 0xc3};
constexpr int RESULTADO_ESPERADO = 0x2a;

/// O tamanho de página que o sistema entrega.
constexpr std::size_t PAGINA = 4096;

/// **O tamanho que o alocador do dynarmic confirma e protege de uma vez.**
///
/// É o `PRELUDE_COMMIT_SIZE` do `block_of_code.cpp` dele: dezesseis megabytes, na construção do
/// `BlockOfCode`, e o mesmo número vale para a troca de proteção do `PreludeComplete`. Uma sonda
/// que mede uma página mede outro pedido, e foi essa diferença que deixou o aplicativo morrer
/// abrindo um jogo com o log dizendo que a memória executável tinha sido concedida.
constexpr std::size_t BLOCO_DO_JIT = 16u * 1024u * 1024u;

/// Percorre o caminho inteiro de que um JIT precisa, num tamanho, e conta onde parou.
///
/// **A reserva e a confirmação são dois passos, e não um `MEM_RESERVE | MEM_COMMIT`.** É a sequência
/// do dynarmic: o Xbyak reserva o cache inteiro e o dynarmic confirma os primeiros dezesseis
/// megabytes. Um pedido de uma vez só passaria por um teto de memória executável sem encostar nele.
PassosDaMemoria Percorre(std::size_t tamanho) {
    PassosDaMemoria passos;

    void* base = ::VirtualAllocFromApp(nullptr, tamanho, MEM_RESERVE, PAGE_READWRITE);
    if (base == nullptr) {
        passos.erro = ::GetLastError();
        return passos;
    }
    passos.reservou = true;

    if (::VirtualAllocFromApp(base, tamanho, MEM_COMMIT, PAGE_READWRITE) == nullptr) {
        passos.erro = ::GetLastError();
        ::VirtualFree(base, 0, MEM_RELEASE);
        return passos;
    }
    passos.confirmou = true;

    std::memcpy(base, CODIGO, sizeof(CODIGO));
    passos.escreveu = true;

    DWORD antiga = 0;
    if (!::VirtualProtectFromApp(base, tamanho, PAGE_EXECUTE_READ, &antiga)) {
        passos.erro = ::GetLastError();
        ::VirtualFree(base, 0, MEM_RELEASE);
        return passos;
    }
    passos.virouExecutavel = true;

    // O `memcpy` para uma variável de ponteiro de função, e não um `reinterpret_cast` direto: o
    // padrão da linguagem não promete que um ponteiro de dados vire ponteiro de função, e o
    // compilador avisa disso. Copiar os bytes não pede essa promessa.
    using Funcao = int (*)();
    static_assert(sizeof(Funcao) == sizeof(base), "ponteiro de funcao de tamanho diferente");
    Funcao funcao = nullptr;
    std::memcpy(&funcao, &base, sizeof(funcao));
    passos.rodou = (funcao() == RESULTADO_ESPERADO);

    ::VirtualFree(base, 0, MEM_RELEASE);
    return passos;
}

}  // namespace

ProvaDaMemoriaExecutavel SondaMemoriaExecutavel() {
    ProvaDaMemoriaExecutavel prova;

    // 1. **O pedido de uma vez só** — o do alocador do dynarmic sem
    //    `DYNARMIC_ENABLE_NO_EXECUTE_SUPPORT`. A recusa aqui é o esperado, e não o defeito: o
    //    AppContainer recusa uma página gravável e executável ao mesmo tempo, e é por isso que o
    //    caminho abaixo existe. Fica registrado porque é ele que explica uma falha do JIT: quem
    //    esquecer a definição de compilação chega aqui e morre no passo 1.
    if (void* rwx = ::VirtualAllocFromApp(nullptr, PAGINA, MEM_RESERVE | MEM_COMMIT,
                                          PAGE_EXECUTE_READWRITE)) {
        // Aceito: este aparelho não está sob a regra de W^X, ou a capacidade mudou. É notícia, e
        // não erro — a página sai daqui mesmo assim, para não deixar uma reserva pendurada.
        ::VirtualFree(rwx, 0, MEM_RELEASE);
    } else {
        prova.rwxRecusado = true;
        prova.erroDoRwx = ::GetLastError();
    }

    // 2. **O caminho do JIT no tamanho em que ele o usa.** É a medida que decide: ver o comentário
    //    do `BLOCO_DO_JIT`.
    prova.bloco = Percorre(BLOCO_DO_JIT);

    // 3. **O mesmo caminho numa página.** Continua aqui porque é o que separa "o aparelho proíbe
    //    memória executável" de "o aparelho limita a memória executável": com a página passando e o
    //    bloco não, a frase do relatório tem conserto e a causa está no tamanho.
    prova.pagina = Percorre(PAGINA);

    return prova;
}

}  // namespace ZeebxUwp
