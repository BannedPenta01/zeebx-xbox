# Compila o Zeebx para o Xbox Series S|X: o núcleo em Rust e o aplicativo em C++/WinRT.
#
#   .\compilar.ps1 -Nucleo          só o núcleo (o `.lib`)
#   .\compilar.ps1 -App             só o aplicativo (espera o `.lib` já compilado)
#   .\compilar.ps1 -Pacote          monta o `.msix` depois de compilar os dois **e o assina**
#   .\compilar.ps1 -Registrar       registra o layout solto no PC, para testar sem console
#   .\compilar.ps1                  os dois, sem empacotar
#
# **O `-Pacote` assina de propósito.** Um `.msix` sem assinatura é recusado pelo Windows com
# `0x800B0100`, e foi o que fez o pacote parecer quebrado. O certificado é criado na primeira vez,
# é autoassinado e é do usuário — ver `Garante-Certificado`.
#
# **A política de execução do PowerShell barra este arquivo.** Ele é um `.ps1` que ninguém assinou,
# e o duplo-clique abre uma janela que fecha antes de dar para ler o erro. Chame-o assim:
#
#   powershell -ExecutionPolicy Bypass -File .\compilar.ps1
#
# **Este alvo é TIER 3 do Rust.** Não há `std` pronto para `x86_64-uwp-windows-msvc`: o
# `build-std` o compila junto, e isso exige `nightly` com o `rust-src` instalado. Ver o LEIAME.
#
# Alvo único de propósito: x64. O Xbox não aceita aplicativo de 32 bits, e o Series S|X é x64.
[CmdletBinding()]
param(
    [switch]$Nucleo,
    [switch]$App,
    [switch]$Pacote,
    [switch]$Registrar,
    [string]$Configuracao = "Release"
)

$ErrorActionPreference = "Stop"

$AQUI = Split-Path -Parent $MyInvocation.MyCommand.Path
$RAIZ = (Resolve-Path (Join-Path $AQUI "..\..")).Path
$ALVO = "x86_64-uwp-windows-msvc"
$PERFIL = "release"
$LIB = Join-Path $RAIZ "target\$ALVO\$PERFIL\zeebx_xbox.lib"
$PROJETO = Join-Path $AQUI "app\ZeebxXbox.vcxproj"

# --- O núcleo ---------------------------------------------------------------------------------

function Limpa-Dynarmic {
    # **O único jeito medido de obrigar o C++ da crate a ser recompilado.** Apaga três coisas, e as
    # três são necessárias:
    #
    #   - `build\dynarmic`, a árvore que o CMake monta — sem ela, o `configure` refaz tudo;
    #   - `.fingerprint\dynarmic-*`, para o cargo não achar a crate em dia e rodar o `build.rs`;
    #   - `deps\*dynarmic*`, os artefatos dela neste alvo.
    #
    # `cargo clean -p dynarmic` **não** basta, e isso custou uma tarde: ele não toca na árvore do
    # CMake, que mora em `build\dynarmic\*\out`, e o build seguinte termina dizendo "Finished" com o
    # objeto antigo no lugar — sem imprimir "Compiling dynarmic", que é o sinal de que nada foi
    # recompilado. Ver a nota grande em `Conserta-Dynarmic`.
    $saida = Join-Path $RAIZ "target\$ALVO\$PERFIL"
    Remove-Item -Recurse -Force (Join-Path $saida "build\dynarmic") -ErrorAction SilentlyContinue
    Get-ChildItem (Join-Path $saida ".fingerprint") -Directory -Filter "dynarmic-*" -ErrorAction SilentlyContinue |
        Remove-Item -Recurse -Force
    Get-ChildItem (Join-Path $saida "deps") -Filter "*dynarmic*" -ErrorAction SilentlyContinue |
        Remove-Item -Recurse -Force
}

function Prepara-Nucleo {
    # **As duas definições que fazem o C++ do dynarmic linkar dentro de um AppContainer moram num
    # arquivo de toolchain, e não em variáveis de ambiente.** O `build.rs` do dynarmic compila a
    # árvore C++ dele pela crate `cmake`, que monta a linha de compilação a partir do que o `cc`
    # diz — e o `cc` não repassa `CXXFLAGS`. O que ele repassa é `CMAKE_TOOLCHAIN_FILE`, e é por
    # isso que `appcontainer.cmake` existe. Ver a seção "A memória executável" do documento do
    # porte.
    #
    # **Atenção ao editar aquele arquivo:** o `build.rs` do dynarmic não o declara como entrada, e o
    # cargo não sabe que ele mudou — o build seguinte sairia com o objeto velho, sem avisar. Esta
    # conferência faz a limpeza sozinha quando o arquivo é mais novo do que a árvore do CMake.
    $toolchain = Join-Path $AQUI "appcontainer.cmake"
    $env:CMAKE_TOOLCHAIN_FILE = $toolchain

    $arvores = Get-ChildItem (Join-Path $RAIZ "target\$ALVO\$PERFIL\build\dynarmic") -Directory -ErrorAction SilentlyContinue |
        ForEach-Object { Join-Path $_.FullName "out\build\build.ninja" } |
        Where-Object { Test-Path $_ }
    $ultima = $arvores | Sort-Object { (Get-Item $_).LastWriteTime } -Descending | Select-Object -First 1
    if ($ultima -and (Get-Item $toolchain).LastWriteTime -gt (Get-Item $ultima).LastWriteTime) {
        Write-Host "   o appcontainer.cmake e mais novo que o build do dynarmic; refazendo" -ForegroundColor DarkGray
        Limpa-Dynarmic
    }

    # **O Ninja, que o CMake do dynarmic usa como montador.** Ele vem com o Visual Studio mas não
    # fica no PATH; sem ele esta máquina não acha "nenhum programa de build" para o gerador Ninja, e
    # a mensagem do CMake não diz onde procurar.
    if (Get-Command ninja -ErrorAction SilentlyContinue) { return }
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path $vswhere) {
        $vsDir = & $vswhere -latest -products * -property installationPath
        if ($vsDir) {
            $direto = Join-Path $vsDir "Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe"
            if (Test-Path $direto) {
                $env:Path = (Split-Path -Parent $direto) + ";" + $env:Path
                Write-Host "   ninja: $direto" -ForegroundColor DarkGray
                return
            }
        }
    }
    throw @"
não achei o ninja, e o CMake precisa dele para montar o C++ do dynarmic.

Ele vem com a carga de C++ do Visual Studio. Procure `ninja.exe` em
  %ProgramFiles%\Microsoft Visual Studio\...\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja
e ponha essa pasta no PATH.
"@
}

function Conserta-Dynarmic {
    # **O dynarmic 0.1.3 grava dentro do bloco de código depois de fechá-lo para escrita.**
    #
    # `A32EmitX64` e `A64EmitX64`, ao se construírem, chamam `code.PreludeComplete()` — que com o
    # `DYNARMIC_ENABLE_NO_EXECUTE_SUPPORT` ligado protege o bloco inteiro como `PAGE_EXECUTE_READ` —
    # e só *depois* chamam `exception_handler.SetFastmemCallback`. Esse caminho grava
    # `except_info->ExceptionHandler`, e o `except_info` foi alocado dentro do próprio bloco de
    # código (`AllocateFromCodeSpace`). Num sistema de W^X essa escrita é uma violação de acesso, e o
    # preço dela foi medido: o aplicativo morre ao abrir o primeiro jogo, com o relatório dizendo,
    # três linhas antes, que a memória executável tinha sido concedida. Sem a definição ligada — que
    # é o caso do build de PC — `EnableWriting`/`DisableWriting` são vazios e o defeito não aparece;
    # é por isso que ele passou tanto tempo sem ser visto, e por isso o remendo não muda nada fora
    # deste alvo.
    #
    # A correção é a que o próprio dynarmic usa em volta de cada bloco emitido: reabrir a escrita em
    # volta da chamada. O trecho é conferido antes da troca: se a crate mudar de versão ou de texto,
    # isto falha em voz alta em vez de remendar outra coisa.
    #
    # **Por que remendar, e não trazer a crate para o repositório.** A crate traz o dynarmic inteiro,
    # com xbyak, zydis, fmt e mcl: são dezenas de megabytes de C++ que não são nossos, e uma cópia
    # deles aqui seria uma dívida que ninguém paga na próxima atualização. Quatro linhas aplicadas
    # onde já se mexe na dependência — o `Prepara-Nucleo` cuida do toolchain dela — ficam visíveis, e
    # somem sozinhas no dia em que a crate for corrigida.
    $crates = Join-Path $env:USERPROFILE ".cargo\registry\src"
    $crate = Get-ChildItem -Path (Join-Path $crates "*\dynarmic-0.1.3") -Directory -ErrorAction SilentlyContinue |
        Select-Object -First 1
    if (-not $crate) {
        throw "nao achei a crate dynarmic-0.1.3 em $crates. O Cargo.lock mudou de versao?"
    }

    # **A quebra de linha é a da crate, e ela é LF.** As fontes que vêm do crates.io não trazem o
    # `CR`, e montar a comparação com `CRLF` faz o trecho nunca ser encontrado — foi o que
    # aconteceu na primeira escrita deste remendo. O separador sai do próprio arquivo.
    $linhasAntes = @(
        '    exception_handler.SetFastmemCallback([this](u64 rip_) {'
        '        return FastmemCallback(rip_);'
        '    });'
    )

    $linhasDepois = @(
        '    // ZEEBX: o PreludeComplete do construtor acabou de fechar este bloco para escrita, e'
        '    // este caminho grava except_info, que mora aqui dentro. Ver Conserta-Dynarmic no'
        '    // compilar.ps1: sem isto, abrir o primeiro jogo e uma violacao de acesso.'
        '    code.EnableWriting();'
        '    exception_handler.SetFastmemCallback([this](u64 rip_) {'
        '        return FastmemCallback(rip_);'
        '    });'
        '    code.DisableWriting();'
    )

    $remendou = $false
    foreach ($arquivo in @("a32_emit_x64.cpp", "a64_emit_x64.cpp")) {
        $caminho = Join-Path $crate.FullName "dynarmic\src\dynarmic\backend\x64\$arquivo"
        if (-not (Test-Path $caminho)) {
            throw "nao achei $caminho - a crate do dynarmic mudou de arranjo?"
        }
        $texto = [System.IO.File]::ReadAllText($caminho)
        $quebra = if ($texto.Contains("`r`n")) { "`r`n" } else { "`n" }
        $antes = $linhasAntes -join $quebra
        $depois = $linhasDepois -join $quebra

        if ($texto.Contains($depois)) {
            continue
        }
        if (-not $texto.Contains($antes)) {
            throw @"
nao achei o trecho esperado em $caminho.

O dynarmic 0.1.3 escreve dentro do bloco de codigo depois de ele virar somente-leitura, e o
remendo deste script e o que evita a violacao de acesso ao abrir o primeiro jogo. O texto
esperado mudou: conferir a versao da crate e rever o remendo antes de qualquer build.
"@
        }
        [System.IO.File]::WriteAllText($caminho, $texto.Replace($antes, $depois))
        Write-Host "   remendei $arquivo" -ForegroundColor DarkGray
        $remendou = $true
    }

    # **A limpeza é o que faz o remendo valer.** Sem ela, o cargo considera a crate em dia e nem
    # chega a chamar o CMake, que é quem recompilaria o arquivo remendado — e o aplicativo volta a
    # morrer abrindo o primeiro jogo, sem nada na saída do build dizendo que o remendo foi ignorado.
    # O porquê de cada uma das três remoções está em `Limpa-Dynarmic`.
    if ($remendou) {
        Write-Host "   limpando o dynarmic, para o C++ ser recompilado" -ForegroundColor DarkGray
        Limpa-Dynarmic
    }
}

function Compila-Nucleo {
    Write-Host "== núcleo (Rust, $ALVO) ==" -ForegroundColor Cyan

    if (-not (Get-Command cargo -ErrorAction SilentlyContinue)) {
        throw "não achei o cargo no PATH. Instale o Rust por rustup.ms/rustup"
    }
    Prepara-Nucleo
    Conserta-Dynarmic

    # O `build-std` é nightly-only, e o alvo é tier 3: sem este `rustc` com `rust-src` o build
    # morre em "can't find crate for `std`", que é a mensagem menos útil possível.
    # Sem `-ErrorAction Stop` nesta chamada: uma ferramenta que falha escrevendo em stderr é o
    # caminho normal de quem não tem o nightly, e o `$ErrorActionPreference` global a promote a
    # exceção antes de o `$LASTEXITCODE` ser lido.
    $anterior = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    & cargo +nightly --version *> $null
    $temNightly = ($LASTEXITCODE -eq 0)
    $ErrorActionPreference = $anterior
    if (-not $temNightly) {
        throw "não há toolchain nightly com o rust-src. Rode: rustup toolchain install nightly --component rust-src"
    }

    Push-Location $RAIZ
    try {
        # **Sem `--locked`.** O `Cargo.lock` do repositório ainda não conhece este membro do
        # workspace, e o primeiro build precisa atualizá-lo. Depois de commitar o lock, esta
        # linha pode ganhar o `--locked` — e deve, porque o CI usa ele.
        & cargo +nightly build --release -p zeebx-xbox --target $ALVO `
            -Z build-std=std,panic_abort
        if ($LASTEXITCODE -ne 0) {
            throw "o cargo falhou com o código $LASTEXITCODE"
        }
    } finally {
        Pop-Location
    }

    if (-not (Test-Path $LIB)) {
        throw "o build disse que deu certo, mas $LIB não existe"
    }
    Write-Host "   -> $LIB" -ForegroundColor Green
}

# --- O aplicativo -----------------------------------------------------------------------------

function Garante-CppWinRT {
    # **O `.nupkg` do C++/WinRT, que só o Visual Studio tem.**
    #
    # O `.vcxproj` importa `..\packages\Microsoft.Windows.CppWinRT.<versão>\build\native\...props` e
    # o `.targets` par. Sem eles o build morre lá na frente, em
    # `WMC1007: Cannot resolve 'Windows.metadata'` — um erro que parece do nosso código e é do
    # pacote que falta. E demora a aparecer: os dois `Import` têm um `Exists()`, então o MSBuild
    # segue em silêncio quando o diretório não está lá. É aqui que o silêncio é quebrado.
    #
    # A versão é a mesma dos dois `Import` do `.vcxproj`. Mudou lá, muda aqui.
    $versao = "2.0.220531.1"
    $destino = Join-Path $AQUI "packages\Microsoft.Windows.CppWinRT.$versao"
    $marca = Join-Path $destino "build\native\Microsoft.Windows.CppWinRT.props"
    if (Test-Path $marca) { return }

    $nome = "Microsoft.Windows.CppWinRT.$versao.nupkg"
    Write-Host "   procurando o pacote do C++/WinRT ($versao)..." -ForegroundColor DarkGray

    $origens = New-Object System.Collections.Generic.List[string]
    # 1. ao lado deste script: quem baixou o `.nupkg` à mão põe aqui.
    $origens.Add((Join-Path $AQUI "packages\$nome"))
    # 2. o cache do NuGet.
    $origens.Add((Join-Path $env:USERPROFILE ".nuget\packages\microsoft.windows.cppwinrt\$versao\$nome"))
    # 3. onde o Visual Studio guarda: `Common7\IDE\Extensions\<id>\Packages\`.
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path $vswhere) {
        $vsDir = & $vswhere -latest -products * -property installationPath
        if ($vsDir) {
            $extensoes = Join-Path $vsDir "Common7\IDE\Extensions"
            if (Test-Path $extensoes) {
                Get-ChildItem $extensoes -Recurse -File -Filter $nome -ErrorAction SilentlyContinue |
                    ForEach-Object { $origens.Add($_.FullName) }
            }
        }
    }

    $achado = $origens | Where-Object { Test-Path $_ } | Select-Object -First 1
    if (-not $achado) {
        throw @"
não achei $nome.
Ele vem com o Visual Studio; procure em Common7\IDE\Extensions\<id>\Packages\. Se não estiver aí,
baixe de https://www.nuget.org/packages/Microsoft.Windows.CppWinRT/$versao e ponha o .nupkg em
$AQUI\packages\.
"@
    }

    Write-Host "   desempacotando $achado" -ForegroundColor DarkGray
    if (Test-Path $destino) { Remove-Item $destino -Recurse -Force }
    New-Item -ItemType Directory -Path $destino -Force | Out-Null
    # Um `.nupkg` é um `.zip` com outro nome, e o `Expand-Archive` recusa a extensão. Já o
    # `ZipFile` olha o conteúdo, não o nome.
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    [System.IO.Compression.ZipFile]::ExtractToDirectory($achado, $destino)

    if (-not (Test-Path $marca)) {
        throw "$nome foi desempacotado em $destino, mas $marca não apareceu: o pacote não é o que este projeto espera."
    }
}

function Localiza-MSBuild {
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path $vswhere) {
        $achado = & $vswhere -latest -products * -requires Microsoft.Component.MSBuild `
            -find "MSBuild\**\Bin\MSBuild.exe" | Select-Object -First 1
        if ($achado) { return $achado }
    }
    $noPath = Get-Command msbuild -ErrorAction SilentlyContinue
    if ($noPath) { return $noPath.Source }
    throw "não achei o MSBuild. Precisa do Visual Studio com a carga de trabalho de C++/WinRT."
}

function Confere-CargaUwp {
    # **A carga de trabalho que monta um aplicativo de UWP.** O `Microsoft.AppxPackage.targets` só
    # existe com ela instalada. Sem o arquivo, o MSBuild nem chega a carregar o `.vcxproj`: ele
    # para num `MSB4019` ("the imported project ... was not found"), que não diz o que instalar nem
    # que o problema é do Visual Studio, e não do núcleo. Vale conferir antes e dizer o que fazer.
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    if (-not (Test-Path $vswhere)) { return }
    $vsDir = & $vswhere -latest -products * -property installationPath
    if (-not $vsDir) { return }

    $alvo = Get-ChildItem (Join-Path $vsDir "MSBuild") -Recurse -Filter "Microsoft.AppxPackage.targets" `
        -ErrorAction SilentlyContinue | Select-Object -First 1
    if (-not $alvo) {
        throw @"
o Visual Studio está sem a carga de trabalho de **UWP do C++**: o Microsoft.AppxPackage.targets
não está em $vsDir\MSBuild. Sem ela o MSBuild nem abre o ZeebxXbox.vcxproj.

No Visual Studio Installer, em **Modificar**:
  - marque *Desenvolvimento de Plataforma Universal do Windows*;
  - dentro dela, marque *Ferramentas de C++ para UWP* (aparece como "C++ (v1xx) para UWP").
O SDK do Windows sozinho não basta: ele traz o C++/WinRT e o fxc, mas não estas metas.
"@
    }
}

function Compila-App {
    Write-Host "== aplicativo (C++/WinRT) ==" -ForegroundColor Cyan

    if (-not (Test-Path $LIB)) {
        throw "falta o núcleo em $LIB. Rode com -Nucleo antes."
    }

    # **A pasta `AppPackages` derruba o build seguinte.** O MSBuild apaga e refaz o layout dela a
    # cada vez, e um arquivo preso — o OneDrive sincronizando, o antivírus lendo — faz o
    # `RemoveDir` falhar com `MSB3231: Unable to remove directory ... Access to the path ... is
    # denied`. O erro não tem nada a ver com o nosso código e derruba o build inteiro; apagar
    # antes é mais barato do que redescobrir isso. Quando o `Remove-Item` também não consegue, ele
    # deixa a pasta: o MSBuild tenta de novo, e às vezes o arquivo já foi solto.
    $pacotes = Join-Path $AQUI "app\AppPackages"
    if (Test-Path $pacotes) {
        Remove-Item $pacotes -Recurse -Force -ErrorAction SilentlyContinue
    }

    Confere-CargaUwp
    Garante-CppWinRT
    $msbuild = Localiza-MSBuild
    & $msbuild $PROJETO /p:Configuration=$Configuracao /p:Platform=x64 `
        /p:AppxPackageSigningEnabled=false /m /v:minimal
    if ($LASTEXITCODE -ne 0) {
        throw "o MSBuild falhou com o código $LASTEXITCODE"
    }
    Write-Host "   -> $AQUI\app\AppPackages" -ForegroundColor Green
}

# --- O pacote ---------------------------------------------------------------------------------

function Monta-Pacote {
    Write-Host "== pacote (.appx) ==" -ForegroundColor Cyan
    $msbuild = Localiza-MSBuild
    Garante-CppWinRT
    # `/p:UapAppxPackageBuildMode=SideloadOnly` é o que evita o pacote querer ir para a Loja:
    # este aplicativo **não** é publicável lá (as duas capacidades restritas e os jogos de
    # terceiros), e o caminho é o Portal do Dispositivo.
    & $msbuild $PROJETO /p:Configuration=$Configuracao /p:Platform=x64 `
        /p:AppxBundle=Never /p:UapAppxPackageBuildMode=SideloadOnly `
        /p:AppxPackageSigningEnabled=false /p:GenerateAppxPackageOnBuild=true /m /v:minimal
    if ($LASTEXITCODE -ne 0) {
        throw "o MSBuild falhou com o código $LASTEXITCODE"
    }
    $pacotes = Join-Path $AQUI "app\AppPackages"
    # **`*.msix` e `*.appx`.** O VS 2026 monta `.msix` — o nome novo do mesmo pacote —, e as
    # dependências (`Dependencies\*.appx`, os `VCLibs`) ficam no meio da pasta e não são o que
    # interessa instalar. Também não são o que interessa **assinar**: elas são da Microsoft.
    $nossos = @(Get-ChildItem -Path $pacotes -Recurse -File -Include *.msix,*.appx -ErrorAction SilentlyContinue |
        Where-Object { $_.DirectoryName -notmatch "Dependencies" })
    foreach ($pacote in $nossos) {
        Assina-Pacote -Arquivo $pacote.FullName
        Write-Host "   -> $($pacote.FullName)" -ForegroundColor Green
    }

    # As dependências ficam em `Dependencies\<arquitetura>\`, e quem as monta é o MSBuild — este
    # passo não precisa copiar nada. O Portal do Dispositivo pede o `.appx` do framework quando o
    # console não tem a versão que o manifesto exige, e é de lá que ele sai.
}

# --- A assinatura ------------------------------------------------------------------------------

function Garante-Certificado {
    # **O `Publisher` do manifesto e o `Subject` do certificado têm de ser a mesma string.** É essa
    # igualdade que o Windows confere ao instalar, e não uma autoridade: um certificado autoassinado
    # serve, desde que os dois digam `CN=ZeebxTeam`. Com um `CN` diferente a instalação falha com a
    # assinatura tecnicamente válida, e o erro fala de "publisher" — o que manda procurar no lugar
    # errado.
    #
    # Ele mora no repositório **do usuário**, e não no da máquina: `LocalMachine` pede elevador, e
    # nada mais neste projeto pede — o `-App` e o `-Registrar` funcionam sem.
    $sujeito = "CN=ZeebxTeam"
    $existente = Get-ChildItem "Cert:\CurrentUser\My" -ErrorAction SilentlyContinue |
        Where-Object { $_.Subject -eq $sujeito -and $_.HasPrivateKey -and $_.NotAfter -gt (Get-Date).AddDays(30) } |
        Sort-Object NotAfter -Descending | Select-Object -First 1
    if ($existente) { return $existente }

    Write-Host "   criando um certificado de desenvolvimento ($sujeito)..." -ForegroundColor DarkGray
    New-SelfSignedCertificate -Type Custom -Subject $sujeito `
        -KeyUsage DigitalSignature -KeyExportPolicy Exportable `
        -FriendlyName "Zeebx (assinatura de desenvolvimento)" `
        -CertStoreLocation "Cert:\CurrentUser\My" `
        -TextExtension @("2.5.29.37={text}1.3.6.1.5.5.7.3.3", "2.5.29.19={text}")
}

function Assina-Pacote {
    param([string]$Arquivo)

    # **Assinar é o que separa "pacote" de "instalável".** Sem assinatura o Windows recusa o `.msix`
    # com `0x800B0100` ("no signature was present in the subject"), que era o estado anterior: por
    # isso o caminho do PC era só o layout solto. O `/p:AppxPackageSigningEnabled=false` do MSBuild
    # continua — ele existe para o build não pedir certificado a cada compilação; quem assina é este
    # passo, uma vez, no fim.
    $signtool = Get-ChildItem "${env:ProgramFiles(x86)}\Windows Kits\10\bin\*\x64\signtool.exe" `
        -ErrorAction SilentlyContinue | Sort-Object FullName -Descending | Select-Object -First 1
    if (-not $signtool) {
        throw "não achei o signtool.exe no SDK do Windows (Windows Kits\10\bin\<versão>\x64\). Ele vem no mesmo SDK que traz o fxc."
    }

    $cert = Garante-Certificado
    & $signtool.FullName sign /fd SHA256 /sha1 $cert.Thumbprint /d "Zeebx" $Arquivo
    if ($LASTEXITCODE -ne 0) { throw "o signtool falhou com o código $LASTEXITCODE" }

    # O `.cer` sai ao lado do pacote, com o nome dele: é ali que o `Add-AppDevPackage.ps1` — o
    # instalador que o Visual Studio gera — procura o certificado para instalar em quem vai abrir o
    # aplicativo.
    $cer = [System.IO.Path]::ChangeExtension($Arquivo, ".cer")
    Export-Certificate -Cert $cert -FilePath $cer -Type CERT | Out-Null

    # **A confiança que o AppX consulta é a da máquina, e não a do usuário.** Medido, e é o que faz
    # alguém concluir que assinar não adiantou: com o certificado em `CurrentUser\TrustedPeople`, o
    # `Add-AppxPackage` falha com `0x800B0109` ("terminated in a root certificate which is not
    # trusted by the trust provider"); pôr em `CurrentUser\Root` nem é permitido sem janela
    # ("UI is not allowed in this operation"). Quem instala pacote é o `AppXSvc`, um serviço, e
    # serviço não tem repositório de usuário.
    #
    # Isso vale para **este PC**. O console tem o repositório dele, e é lá que este certificado
    # precisa estar — no PC ele não serve para nada. Por isso aqui não se tenta instalar em
    # repositório de máquina (pediria elevador) nem se falha: só se diz o que falta, se faltar.
    $naMaquina = Get-ChildItem "Cert:\LocalMachine\TrustedPeople" -ErrorAction SilentlyContinue |
        Where-Object { $_.Thumbprint -eq $cert.Thumbprint }
    if (-not $naMaquina) {
        Write-Host "   (para instalar este .msix NESTE PC, falta confiar no certificado na maquina;" -ForegroundColor DarkGray
        Write-Host "    pede elevador uma vez: ver 'Instalando no console' no LEIAME)" -ForegroundColor DarkGray
    }

    Write-Host "   assinado ($($cert.Thumbprint.Substring(0,16))...), com o certificado em $cer" -ForegroundColor Green
}

# --- O registro no PC ---------------------------------------------------------------------------

function Registra-NoPC {
    # **O layout solto não recebe os recursos.** O MSBuild copia o `.exe`, o `.xbf`, o `.pri` e o
    # `.winmd` para `app\x64\<Config>\ZeebxXbox\`, e **não** copia o `Assets\` nem os `.cso` — nem
    # neste projeto, nem no modelo em branco do Visual Studio: é assim mesmo que o alvo de depuração
    # funciona, quem põe os recursos no lugar é a implantação do F5. Sem eles o D3D11 não sobe, e o
    # recado na tela é "Sem vídeo — o Direct3D 11 não subiu: Unspecified error": um erro que parece
    # do código e é arquivo que falta.
    #
    # O registro é por `Add-AppxPackage -Register`, que instala um layout solto em Modo de
    # Desenvolvedor **sem assinar nada** — e é o caminho de quem está mexendo no código: o `-App` já
    # deixou o `.exe` novo no lugar, e o registro só precisa apontar para ele. O `.msix` assinado
    # (ver `Assina-Pacote`) é para o console e para quem só quer instalar o aplicativo.
    # No PC esta era a **única** via enquanto o pacote saía sem assinatura; agora há as duas.
    Write-Host "== registro no PC ==" -ForegroundColor Cyan

    $layout = Join-Path $AQUI "app\x64\$Configuracao\ZeebxXbox"
    $manifesto = Join-Path $layout "AppxManifest.xml"
    if (-not (Test-Path $manifesto)) {
        throw "não há layout solto em $layout. Rode com -App antes."
    }

    Copy-Item (Join-Path $AQUI "app\Assets") $layout -Recurse -Force
    Copy-Item (Join-Path $AQUI "app\*.cso") $layout -Force

    # O nome do pacote e o do aplicativo saem do manifesto, e não de uma constante daqui: um
    # `Identity/@Name` novo no manifesto deixaria esta função removendo o pacote errado em silêncio.
    [xml]$xml = Get-Content $manifesto -Raw
    $nome = $xml.Package.Identity.Name
    $app = $xml.Package.Applications.Application.Id

    # Registrar por cima de um registro antigo falha, e o `-Register` não tem `-Force`: é preciso
    # tirar o antigo antes. É o que o Visual Studio faz por baixo quando se aperta F5 duas vezes.
    Get-AppxPackage -Name $nome -ErrorAction SilentlyContinue |
        ForEach-Object { Remove-AppxPackage -Package $_.PackageFullName -ErrorAction SilentlyContinue }

    Add-AppxPackage -Register $manifesto
    $familia = (Get-AppxPackage -Name $nome).PackageFamilyName
    Write-Host "   -> registrado" -ForegroundColor Green
    Write-Host "   -> abra com: Start-Process `"shell:appsFolder\$familia!$app`"" -ForegroundColor Green
}

# --- O caminho ---------------------------------------------------------------------------------

if (-not ($Nucleo -or $App -or $Pacote -or $Registrar)) {
    $Nucleo = $true
    $App = $true
}

if ($Nucleo) { Compila-Nucleo }
if ($App -or $Pacote) { Compila-App }
if ($Pacote) { Monta-Pacote }
if ($Registrar) { Registra-NoPC }

Write-Host ""
Write-Host "Pronto. Para instalar no console, use o Portal do Dispositivo:" -ForegroundColor Cyan
Write-Host "  https://<ip-do-xbox>:11443  ->  Home  ->  Add or install package"
Write-Host "As ROMs vão depois em LocalState\roms, pelo File Explorer do mesmo portal."
