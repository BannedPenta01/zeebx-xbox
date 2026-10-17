// Jit.h — a sonda da memória executável.
//
// **Ela existe para responder uma pergunta que nenhum documento responde por este aparelho:** um
// AppContainer daqui deixa o emulador escrever uma página, virá-la executável e chamá-la? É o
// caminho inteiro de que um JIT precisa, e é o que decide se o núcleo pode recompilar ou tem de
// interpretar. Ver `docs/implementacao/25-frontend-xbox-uwp.md`, "A memória executável".
#pragma once

#include <cstdint>

namespace ZeebxUwp {

/// O que este aparelho deixou fazer, passo a passo.
///
/// **Cada campo é um passo, e não um veredito.** Alocar, escrever, trocar a proteção e executar são
/// quatro decisões diferentes do sistema, e num console "não deu" vale muito menos do que "a troca
/// para RX foi negada, código 5": a primeira frase não diz o que tentar depois, a segunda diz.
struct PassosDaMemoria {
    /// A reserva do bloco saiu.
    bool reservou = false;
    /// A confirmação (`MEM_COMMIT`) do bloco saiu.
    bool confirmou = false;
    /// O código foi escrito no bloco.
    bool escreveu = false;
    /// `VirtualProtectFromApp(PAGE_EXECUTE_READ)` aceitou o bloco inteiro.
    bool virouExecutavel = false;
    /// A função escrita rodou e devolveu o valor que ela deveria devolver.
    bool rodou = false;
    /// `GetLastError` do primeiro passo que falhou; zero quando nenhum falhou.
    std::uint32_t erro = 0;

    /// Se o caminho inteiro ficou de pé, neste tamanho.
    bool inteiro() const { return reservou && confirmou && escreveu && virouExecutavel && rodou; }
};

/// O que este aparelho deixou fazer, nos dois tamanhos que importam.
///
/// **São dois tamanhos de propósito, e a diferença entre eles já custou uma sessão.** A primeira
/// versão desta sonda media uma página, e uma página passa em qualquer lugar — foi ela que disse
/// "pode executar" enquanto o aplicativo morria ao abrir um jogo. O alocador do dynarmic não pede
/// uma página: ele reserva o cache inteiro e confirma, e protege de uma vez só, os primeiros
/// dezesseis megabytes (`PRELUDE_COMMIT_SIZE`, em `block_of_code.cpp`). Um teto de memória
/// executável por processo aparece no bloco e não aparece na página, e é o bloco que decide.
struct ProvaDaMemoriaExecutavel {
    /// O caminho do JIT no tamanho que ele pede de verdade — a decisão sai daqui.
    PassosDaMemoria bloco;
    /// O mesmo caminho numa página só, que é o que a primeira versão media.
    PassosDaMemoria pagina;

    /// `PAGE_EXECUTE_READWRITE` de uma vez só.
    ///
    /// É o que o alocador do dynarmic pede quando o `DYNARMIC_ENABLE_NO_EXECUTE_SUPPORT` está
    /// desligado, e a recusa dele **não é defeito**: um AppContainer não aceita uma página que seja
    /// gravável e executável ao mesmo tempo, e é justamente por isso que o JIT precisa dos dois
    /// tempos. Registrar a recusa é o que separa "o aparelho proíbe JIT" de "o aparelho proíbe RWX".
    bool rwxRecusado = false;
    std::uint32_t erroDoRwx = 0;

    /// Se o caminho que o JIT usa ficou inteiro do começo ao fim, **no tamanho em que ele o usa**.
    bool podeExecutar() const { return bloco.inteiro(); }
};

/// Faz a prova, de verdade: aloca, escreve, vira executável e chama — duas vezes, num bloco de
/// dezesseis megabytes e numa página.
///
/// **Não é uma consulta a uma política; é o caminho.** Um `GetProcAddress`, um `IsApiSetPresent` ou
/// uma leitura da capacidade do manifesto diriam se a *chamada* existe, e não se ela passa — e no
/// console é o que passa que conta. A memória é liberada no fim, qualquer que seja o desfecho.
ProvaDaMemoriaExecutavel SondaMemoriaExecutavel();

}  // namespace ZeebxUwp
