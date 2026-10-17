# O que o C++ de terceiro precisa para compilar no recorte de API de um AppContainer.
#
# **Por que um arquivo de cadeia de ferramentas, e não uma variável de ambiente.** O `cmake` do
# Cargo monta `-DCMAKE_CXX_FLAGS=...` a partir dos argumentos do `cc` e passa na linha de comando —
# e isso *vence* tanto o `CXXFLAGS` do ambiente quanto o `CMAKE_CXX_FLAGS_INIT`. O arquivo de cadeia
# de ferramentas é lido depois do cache e pode acrescentar ao valor que já existe: é o único canal
# por onde este alvo consegue dizer ao C++ do dynarmic como ele deve ser compilado.
#
# Quem usa isto é o `compilar.ps1`, e **só** no build do núcleo para `x86_64-uwp-windows-msvc`.
# Os builds de desktop, Android e Libretro não veem este arquivo, e continuam compilando o dynarmic
# do jeito de sempre.
#
# **Ao editar este arquivo, limpe o objeto velho:** `cargo clean -p dynarmic`. O `build.rs` do crate
# não declara o arquivo de cadeia de ferramentas como entrada, então o cargo não sabe que ele mudou
# e serve o objeto da compilação anterior — com as definições anteriores.

# 1. O recorte de API.
#
# Com `WINAPI_FAMILY=WINAPI_FAMILY_APP`, o `<memoryapi.h>` do SDK declara `VirtualAlloc` e
# `VirtualProtect` como funções **inline que chamam `VirtualAllocFromApp`/`VirtualProtectFromApp`**.
# É assim que o C++ embutido do dynarmic — que chama os dois nomes de sempre — passa a pedir a
# variante que um AppContainer aceita, **sem uma linha de patch no crate**: o que ele pede é o que o
# SDK entrega para este recorte.
#
# Sem esta linha o mesmo código compila e liga, e vai pedir `VirtualAlloc` a um processo que não
# pode chamá-lo — o erro aparece só em execução, que é o pior lugar para ele aparecer.
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /DWINAPI_FAMILY=WINAPI_FAMILY_APP")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} /DWINAPI_FAMILY=WINAPI_FAMILY_APP")

# 2. O caminho de W^X do próprio dynarmic.
#
# Ligada, esta opção faz o alocador confirmar a página como gravável, escrever o bloco e só então
# trocar a proteção para executável — nunca pedir uma página gravável **e** executável de uma vez,
# que é exatamente o que um AppContainer recusa. É a opção que o projeto do dynarmic oferece para
# sistemas que exigem W^X, e é o que a sonda do `app\Jit.cpp` confere no aparelho.
set(DYNARMIC_ENABLE_NO_EXECUTE_SUPPORT ON CACHE BOOL "" FORCE)

# 3. Os testes do dynarmic saem do build, e não é defeito deles.
#
# O `install` do projeto dele constrói `dynarmic_tests.exe` e mais três executáveis de bancada. Eles
# ligam contra o conjunto de bibliotecas de um aplicativo de console — `kernel32.lib` e companhia —,
# onde `VirtualAllocFromApp` não existe: sem o `WindowsApp.lib` de um pacote UWP, o link morre em
# `LNK2019: unresolved external symbol __imp_VirtualAllocFromApp`, e o `build.rs` do crate falha por
# causa de um alvo de teste que ninguém pediu construir. O `.lib` do núcleo é o mesmo com ou sem eles.
set(DYNARMIC_TESTS OFF CACHE BOOL "" FORCE)
