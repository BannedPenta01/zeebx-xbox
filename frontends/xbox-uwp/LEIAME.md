# O Zeebx no Xbox Series S|X

Um aplicativo UWP: o núcleo em Rust vira um `.lib` estático, e um aplicativo em C++/WinRT põe a
janela, o som e o controle. Este documento é o passo a passo de compilar e instalar, e o que
esperar de cada etapa.

O desenho — por que cada peça está onde está — está em
[`docs/implementacao/25-frontend-xbox-uwp.md`](../../docs/implementacao/25-frontend-xbox-uwp.md).

## O que é preciso ter

| Peça | Por quê |
|---|---|
| **Xbox Series S ou X em Modo de Desenvolvedor** | É o único jeito de rodar código nosso num Xbox. O Modo de Desenvolvedor pede uma conta do Partner Center (uma vez, US$ 19). |
| **Windows 11** com o **SDK 10.0.22621** ou mais novo | Desde agosto de 2023 o Xbox recusa as ferramentas antigas: ele exige o canal seguro do SDK novo, e as versões antigas falavam SHA1. |
| **Visual Studio** (2022 ou mais novo) com as cargas **Desenvolvimento de Aplicativos Universais do Windows**, **C++/WinRT** e **Ferramentas de C++ para UWP** | É o que traz o `MSBuild`, o `fxc` e as projeções `winrt/`. A versão exata não importa — o que importa é o SDK abaixo; o Community serve. |
| **Rust nightly** com o componente `rust-src` | O alvo `x86_64-uwp-windows-msvc` é **tier 3**: não há `std` pronto, e o `build-std` compila um junto. Só o nightly faz `build-std`. |

```powershell
rustup toolchain install nightly --component rust-src
```

O `Rust 1.88` ou mais novo que o resto do projeto pede não basta aqui: o canal tem de ser o
`nightly`, pela razão acima.

## O que vai para onde

O aplicativo só pode ler e escrever dentro do próprio pacote. A pasta que ele enxerga é a
`LocalState`, e é por ela que o **Portal do Dispositivo** (Device Portal) deixa o jogador mexer:

```
LocalState\
  roms\            <- os .mod e .zip que vierem pelo portal
  saves\           <- os save states (zeebx-slot-N.zbx)
  lang\            <- traduções extras: um .json por idioma
  settings.json    <- o idioma escolhido e as pastas de jogos de fora
  zeebx-xbox.log   <- o relatório desta execução (ver adiante, "Quando algo não funciona")
  ...              <- cache, aparelho virtual e o que o núcleo grava
```

O Portal do Dispositivo é o `https://<ip-do-xbox>:11443`, com o usuário e a senha que o Modo de
Desenvolvedor mostra na tela. Em **File Explorer** dele há a árvore dos pacotes instalados, e a
`LocalState` do Zeebx é uma pasta como outra qualquer: dá para arrastar uma ROM de dentro dela.

### Os jogos podem vir de um pendrive ou de um HD externo

`LocalState\roms` é o caminho do portal, e não é o único. Na estante, **Fontes de jogos** tem dois
botões, e os dois abrem uma caixa do próprio Windows — nenhum deles é um explorador nosso:

- **Escolher uma pasta** abre a caixa de pasta do sistema: navegue até `E:\Zeebo` e escolha a pasta.
  É o caminho do PC, e a pasta fica guardada com a permissão que a reabre na próxima vez.
- **Escolher um arquivo de jogo** abre a caixa de arquivo: aponte para um `.mod` ou um `.zip` e o
  aplicativo passa a ler **a pasta em que ele está**. É o caminho do console — a Microsoft documenta
  que cenários de seletor de pasta não são suportados no Xbox —, e lá quem dá o acesso à unidade é a
  capacidade do manifesto, e não a caixa.

Os dois botões ficam à vista de propósito: um aparelho que recusa o primeiro não pode deixar o
jogador com um botão que não faz nada e não diz por quê. As pastas escolhidas ficam no
`LocalState\settings.json` e voltam na próxima abertura; a permissão que as reabre fica em
`LocalState\fontes.txt`, uma linha por pasta. A pasta que sumiu — unidade desplugada — aparece na
lista com "This folder does not exist." ao lado, e não derruba a varredura das outras.

**A unidade tem de estar em NTFS ou exFAT.** O Xbox formata pendrive em FATX, que o Windows não lê —
nem o Explorer, nem o aplicativo. Não é limitação do Zeebx: é o formato.

O Windows não dá o acesso de graça, e são **duas** declarações no `Package.appxmanifest`, não uma:

- a capacidade **`removableStorage`**, que é a permissão de ler arquivo em unidade removível;
- uma **associação de tipos de arquivo** (`.mod`, `.mif`, `.zip`, `.7z`), porque é ela que diz
  *quais* arquivos o aplicativo pode ler de lá. Sem a lista, o `KnownFolders.RemovableDevices` nem
  devolve as unidades, e o resultado é uma unidade cheia que aparece vazia — o pior tipo de erro,
  porque é silencioso.

No console, o primeiro acesso abre uma caixa do sistema pedindo a permissão; no PC não há caixa
nenhuma, e a mesma tela funciona sem pedir nada. E não há `broadFileSystemAccess` no lugar disso: a
Microsoft lista essa capacidade como **não suportada no Xbox**.

**O núcleo só lê o que o recorte do UWP deixa.** Medido no PC: escolhida uma pasta comum
(`%USERPROFILE%\Downloads`, que existe e abre no Explorer), a ABI recusou com **`-1`** — daqui de
dentro, `is_dir()` responde `false` para uma pasta fora do que o AppContainer enxerga, e a tela diz
que a pasta não existe. No PC isso é o esperado; no console quem abre a unidade é a capacidade
`removableStorage`, e é por isso que a rota de lá é a do arquivo. Cada tentativa vira uma linha do
relatório, com o código — ver [*Quando algo não funciona*](#quando-algo-nao-funciona).

### Os idiomas

A interface nasce em **inglês** e oferece os quatro catálogos de fábrica — inglês, espanhol,
espanhol do México e português do Brasil —, que são os mesmos `assets/lang/*.json` do resto do
emulador. Quem escolhe é o comboio em **Configurações**; a escolha vai para o `settings.json` e
sobrevive ao fechamento — medido: escolhido espanhol, fechado e reaberto, o aplicativo abre em
espanhol, sem tocar em arquivo nenhum. Tradução nova é um `.json` em `LocalState\lang`, sem
recompilar.

**A GUI do Xbox não tem frase própria.** Todo rótulo, título e recado vem do catálogo do núcleo,
por chave: é o que faz a mesma tela falar as quatro línguas, e é o que faz uma chave que falta
aparecer na tela como a própria chave — visível, e não mentindo em português.

## Compilando

Na pasta deste frontend:

```powershell
# o PowerShell barra um `.ps1` que ninguém assinou; o `Bypass` vale só para esta chamada
powershell -ExecutionPolicy Bypass -File .\compilar.ps1            # o núcleo e o aplicativo
powershell -ExecutionPolicy Bypass -File .\compilar.ps1 -Pacote    # e mais o pacote, pronto
powershell -ExecutionPolicy Bypass -File .\compilar.ps1 -Registrar # registra no PC, para testar
```

O `-Registrar` é o caminho de testar **no PC**, sem console: ele copia o `Assets\` e os `.cso` para
o layout solto — o build não os copia, e sem eles o D3D11 não sobe — e registra o pacote no Modo de
Desenvolvedor, **sem assinar nada**. É o caminho do PC que funciona sem elevador: o registro só
aponta para o `.exe` que o `-App` acabou de escrever, e cada build novo aparece no aplicativo
seguinte, sem instalar nada.

O script cuida do que é fácil esquecer. Ele confere a carga de trabalho de **UWP do C++**, desempacota
o `.nupkg` do **C++/WinRT** que o Visual Studio traz — sem ele o build morre em `WMC1007: Cannot
resolve 'Windows.metadata'`, um erro que não aponta para o pacote que falta — e diz o que instalar
quando falta alguma peça, em vez de deixar o `msbuild` falar sozinho.

**Três coisas entram no passo do núcleo, e todas são do C++ do dynarmic**, que o `build.rs` da
crate compila com CMake: o `CMAKE_TOOLCHAIN_FILE`, apontando para `appcontainer.cmake` — as
definições que fazem esse C++ compilar e alocar memória do jeito que um AppContainer aceita —, o
`ninja` no `PATH`, que é o montador de que esse CMake precisa, e **quatro linhas remendadas num `.cpp`
da crate**, sem as quais o JIT derruba o aplicativo ao abrir o primeiro jogo (o defeito está contado
por inteiro em [*A memória executável*](../../docs/implementacao/25-frontend-xbox-uwp.md#a-memória-executável-como-o-jit-entrou);
e o `compilar.ps1` confere o trecho antes de trocar, e pára com o motivo se a crate mudar). O
`compilar.ps1` põe as duas primeiras; o ninja ele acha na carga de C++ do Visual Studio. É por isso
que os comandos crus abaixo só funcionam depois delas, e é lá também que o `.lib` sai com o C++ de
terceiro dentro.

**Ao mexer em `appcontainer.cmake`, não é `cargo clean -p dynarmic`.** Esse comando **não** força a
recompilação: ele não toca na árvore do CMake, que mora em `build\dynarmic\*\out`, e o build seguinte
termina dizendo `Finished` com o objeto velho no lugar, sem imprimir `Compiling dynarmic` — medido. O
que obriga é o `Limpa-Dynarmic` do `compilar.ps1`, que apaga a árvore, o `fingerprint` e os artefatos
da crate naquele alvo; e o script já o chama sozinho quando o `appcontainer.cmake` é mais novo que o
build.

Ou, peça por peça (o `msbuild` precisa do pacote acima em `packages\`, que é o que o `compilar.ps1`
prepara):

```powershell
# o núcleo: só o `.lib`. As duas linhas de ambiente são o que o `compilar.ps1` faria por você, e
# sem elas o C++ do dynarmic sai com as definições erradas
$env:CMAKE_TOOLCHAIN_FILE = "$PWD\appcontainer.cmake"
cargo +nightly build --release -p zeebx-xbox --target x86_64-uwp-windows-msvc `
    -Z build-std=std,panic_abort

# o aplicativo: espera o `.lib` acima em target\x86_64-uwp-windows-msvc\release\
msbuild frontends\xbox-uwp\app\ZeebxXbox.vcxproj /p:Configuration=Release /p:Platform=x64
```

O pacote — `.msix` no Visual Studio 2026, `.appx` nos mais velhos — sai em `app\AppPackages\`, ao
lado de um `Add-AppDevPackage.ps1` (o instalador que o próprio Visual Studio gera) e do certificado
`.cer`. **O `-Pacote` monta o pacote e o assina**: sem assinatura o Windows recusa o arquivo com
`0x800B0100`, e era o estado anterior — o pacote parecia quebrado e não estava. O certificado é
criado na primeira vez, é autoassinado e é do usuário: `CN=ZeebxTeam`, que é exatamente o
`Publisher` do manifesto, e é essa igualdade que o Windows confere — não uma autoridade.

## Instalando no console

1. No console, abra o **Aplicativo Modo de Desenvolvedor** e veja o IP e as credenciais.
2. No PC, abra o Portal do Dispositivo no endereço mostrado e entre.
3. **Home → Add or install package**, e escolha o pacote (`.msix` ou `.appx`) já **assinado**. Se ele
   pedir as dependências, elas estão na pasta `Dependencies\` ao lado do pacote. Se recusar o
   certificado, veja a seção seguinte.
4. Abra o aplicativo pela lista do Modo de Desenvolvedor, ou pelo atalho que ele cria.
5. Ponha as ROMs na `LocalState\roms` (ver acima) e aperte **Atualizar lista** na estante.

**Instalar por cima do que já está lá precisa de uma versão maior.** O console guarda a versão
instalada, e um pacote com a **mesma** versão não se instala por cima: o `AppXSvc` recusa, e o Portal
do Dispositivo devolve um erro de pacote já instalado. Duas saídas, e as duas funcionam: o pacote novo
sair com um número maior — é o caso deste, que subiu para `0.4.6.0` —, ou **desinstalar** o aplicativo
no console antes de instalar. A segunda leva o `LocalState` junto: as pastas de jogos, o idioma
escolhido e as ROMs que estiverem em `LocalState\roms` vão embora com ele.

### O certificado, e por que ele confunde

O pacote é assinado com um certificado **autoassinado**, e um autoassinado só é aceito por quem
confia nele. Isso vale em **dois lugares diferentes**, e trocá-los custa uma tarde:

| Onde | O que precisa confiar | Quem faz |
|---|---|---|
| O **console** | o repositório de certificados do console | a implantação pelo **Visual Studio** instala o certificado lá; é o caminho mais garantido. Se o Portal do Dispositivo recusar o pacote citando o certificado, é este o caminho |
| **Este PC** | `LocalMachine\TrustedPeople` **e** `LocalMachine\Root` | você, **uma vez**, com elevador — é o que o `Add-AppDevPackage.ps1` faz |

**Não é `CurrentUser`.** Medido, e é o erro que faz concluir que assinar não adiantou: com o
certificado no repositório do usuário, o `Add-AppxPackage` falha com `0x800B0109` ("terminated in a
root certificate which is not trusted by the trust provider"), e escrever no `Root` do usuário nem é
permitido sem janela ("UI is not allowed in this operation"). Quem instala pacote é o `AppXSvc`, um
serviço — e serviço não tem repositório de usuário.

Para instalar o `.msix` **neste PC**, rode uma vez **como administrador** (ajuste o caminho do
pacote se ele mudou de versão):

```powershell
$cer = (Get-ChildItem .\app\AppPackages -Recurse -Filter *.cer | Select-Object -First 1).FullName
Import-Certificate -FilePath $cer -CertStoreLocation "Cert:\LocalMachine\TrustedPeople"
Import-Certificate -FilePath $cer -CertStoreLocation "Cert:\LocalMachine\Root"
Add-AppxPackage -Path ([IO.Path]::ChangeExtension($cer, ".msix"))
```

Sem esse passo o PC continua funcionando pelo `-Registrar`, que não usa certificado nenhum. **O
certificado deste PC não serve para o console**: cada máquina confia na sua lista.

### Se o console recusar o pacote por causa do certificado

O caminho conhecido para o console é a **implantação pelo Visual Studio**: ela instala o aplicativo
**e** o certificado no aparelho — é para isso que o Modo de Desenvolvedor serve.

1. Abra `frontends\xbox-uwp\app\ZeebxXbox.vcxproj` no Visual Studio. **Não há `.sln`**: o VS abre o
   projeto direto, e ele é o único que existe.
2. Escolha o destino **Remote Machine** e aponte para o IP que o Aplicativo Modo de Desenvolvedor
   mostra, com a autenticação **Universal (Unencrypted Protocol)**. Na primeira vez o Visual Studio
   pede o **PIN** que o console exibe na tela.
3. **Depurar → Iniciar Sem Depuração** (`Ctrl+F5`). Ele compila, implanta e abre; a partir daí o
   aplicativo aparece na lista do Modo de Desenvolvedor, como qualquer instalado.

**A primeira instalação em console já aconteceu** — quem usa instalou o pacote num Series S em Modo de
Desenvolvedor, e o aplicativo subiu, tocou som e rodou um jogo. Por qual das duas rotas a instalação
foi feita não está registrado; o que se sabe é que o pacote assinado por este `CN=ZeebxTeam` foi aceito
pelo aparelho.

### Se o console recusar o pacote por dependência

O pacote declara uma dependência de framework, e ela não é enfeite:

```xml
<PackageDependency Name="Microsoft.VCLibs.140.00" MinVersion="14.0.33519.0" ... />
```

É o runtime do C++ que o `ZeebxXbox.exe` usa — ele é ligado dinamicamente, e não estático. **No PC
ela já está satisfeita**: `Get-AppxPackage Microsoft.VCLibs.140.00` mostra a `14.0.33519.0`, que é
exatamente a versão mínima que o manifesto pede, e é por isso que o registro local funciona de
primeira. Se o console não tiver essa versão, o Portal do Dispositivo recusa o pacote citando o
nome dela, e a saída é instalar o framework **antes**. O `.appx` dele já vem ao lado do pacote, na
pasta `Dependencies\` que o MSBuild monta (o de x64 é o `Dependencies\x64\`); o SDK tem o mesmo
arquivo, em

```
C:\Program Files (x86)\Microsoft SDKs\Windows Kits\10\ExtensionSDKs\Microsoft.VCLibs\14.0\Appx\Retail\x64\Microsoft.VCLibs.x64.14.00.appx
```

(0,9 MB), e vai pelo mesmo **Add or install package**.

### Por que não dá para publicar na Loja

**Assinado não é o mesmo que publicável.** O pacote agora é assinado para poder ser instalado, e
publicá-lo na Loja continuaria impossível: exigiria uma conta de desenvolvedor da Store e duas
capacidades restritas que a Microsoft não aprova para um emulador — `expandedResources` e
`codeGeneration`. O Modo de Desenvolvedor aceita pacote assinado por um certificado em que ele
confie: é para isso que ele serve. O `Add-AppDevPackage.ps1` que o Visual Studio gera ao lado do
pacote faz os mesmos passos por linha de comando — e pede elevador, porque instala o certificado no
repositório da máquina.

## O controle

O aplicativo é um menu de console: quem anda é o **foco**, e não um cursor. O direcional e o manche
andam de controle em controle, o A aciona o que está em foco, e o View abre e fecha a pausa. A linha
de baixo da tela diz o que cada botão faz, nas palavras do catálogo — `A para jogar`, `B Voltar`,
`View Menu` no idioma escolhido.

**O Xbox abre todo aplicativo em modo de ponteiro**, e o Zeebx pede o contrário na primeira linha do
aplicativo (`Application::RequiresPointerMode(WhenRequested)`): em modo de ponteiro, o direcional não
anda o foco e o A clica onde o cursor estiver, que é o "o menu só destaca e não faz nada". Medido no
PC: a chamada passa e o relatório a registra. **Se o console a honra é o que ainda ninguém mediu** —
é a primeira linha a conferir no relatório de lá.

### O B, e a partida que ele interrompia

No console o B não é só uma tecla: é o **"voltar" do sistema**, e um aplicativo que não o consome é
fechado por ele — **no meio do jogo inclusive**, que foi exatamente o que aconteceu no primeiro teste
em console. O aplicativo agora ouve o "voltar" (`SystemNavigationManager::BackRequested`) e decide o
que fazer com ele:

- **com o jogo rodando, o B é do jogo** — é o botão 2 do Zeebo, e o `Input` já o manda para lá sem
  passar pelo XAML;
- com a pausa aberta, o B fecha a pausa;
- nas telas de fontes, configurações e sobre, o B volta um passo — e no explorador de pastas, sobe um
  nível antes de sair da tela;
- **na estante o B fica com o sistema**, porque sair para o menu do Xbox é o que o B faz num
  aplicativo de console. Quem sai do jogo é o `View`, pela pausa.

| Xbox | Zeebo |
|---|---|
| A, B, X, Y | botões 1, 2, 3 e 4 |
| LB / RB | ZL / ZR |
| LT / RT | L2 / R2 |
| direcional | direcional |
| manche esquerdo e direito | os dois manches do console |
| **Menu** | HOME |
| **View** | abre e fecha o menu de pausa do aplicativo |

No PC, em modo de desenvolvimento, o teclado também responde: o `Esc` é o mesmo `B` — volta um passo
nas telas e, com o jogo rodando, fica com o jogo —, e o `P` abre e fecha a pausa.

## A imagem, e o tamanho do quadro

O primeiro teste em console trouxe som e não trouxe imagem. Não era escala: **o desenho nunca
acontecia**, e a tela preta era só o `Clear` — que aparece mesmo quando a chamada de desenho é jogada
fora.

**Dois bloqueios mudos, os dois no `Renderer.cpp`.** Nenhum dos dois falha com um erro; os dois
deixam a tela preta e o relatório limpo:

1. **O alvo de saída era ligado com `put()`.** Num `com_ptr`, o `put()` **solta o ponteiro antes de
   devolver o endereço dele** — ele é para parâmetro de *saída* —, e o D3D recebia um alvo nulo. Uma
   chamada de desenho sem alvo de saída **não falha e não escreve**: o `Clear` não passa pela saída e
   continuava aparecendo, e o `Apresenta` do quadro seguinte saía cedo pela guarda do `m_alvo` nulo.
   As outras ligações do arquivo sempre usaram `get()`; esta era a que fugia do padrão.
2. **O layout de entrada nunca era criado nem posto.** O `quad.hlsl` lê `POSITION` e `TEXCOORD0` de um
   buffer de vértices, e sem layout de entrada o D3D11 também **não rasteriza nada, em silêncio**.

**Como isso apareceu: um quadro sintético.** Nenhum quadro deste aplicativo jamais tinha sido desenhado,
aqui ou no console — não há ROM neste PC, e o teste de lá só mostrou a tela preta. O que destravou foi
desenhar **um padrão de 320x240** (moldura vermelha, verde no canto superior esquerdo, azul no direito,
cruz branca no centro) pela mesma `Apresenta`, e capturar a tela do PC. Uma captura responde o que três
linhas de relatório não respondem: se o desenho aconteceu, onde ele caiu e para que lado está virado. O
padrão **não** fica no aplicativo; ele existe enquanto a medição dura.

**Aí a conta do tamanho apareceu errada, e a hipótese anterior caiu.** Com a cadeia medida em "pixels"
— tamanho da raiz × escala de composição —, o padrão saiu **1,5x maior e deslocado**: o centro da cruz
em 1440 px de uma janela de 1920 (deviam ser 960), a borda esquerda em 444 (deviam ser 296) e a tarja
direita fora da tela. O `SwapChainPanel` **compõe um pixel da cadeia por DIP**: quem quiser 1:1 com os
pixels físicos deixa o XAML escalar, e a cadeia nasce medida em DIPs. Medido de novo, com a cadeia em
DIPs:

    tarja esquerda termina em 306   (esperado 307)
    cruz no centro em 970           (esperado 971)
    tarja direita começa em 1635    (esperado 1635)

A leitura do `CompositionScale` continua indo para o relatório — é ela que diz em que contexto de DPI o
console abriu o aplicativo —, mas ela não dimensiona mais nada. A hipótese anterior, de que a escala
entrava na conta e era lida do lugar errado, **estava errada**, e a multiplicação que ela pediu foi
desfeita no mesmo lugar em que foi escrita.

### As três linhas que respondem

    video: raiz ... dip, escala do painel ..., cadeia de troca ... dip
    primeiro quadro: ... px
    primeira imagem: ... px, no quadro ...

A primeira diz em que tamanho o quadro foi apresentado; a segunda, que tamanho o **núcleo** publicou; e
a terceira só sai quando o jogo desenha alguma coisa. **Sem `primeira imagem`, o defeito não está na
apresentação**: o jogo nunca mandou pixels, e não há tamanho de cadeia que resolva isso.

## A estante, e a arte que cada jogo traz

**O ícone do `.mif` é o que existe, e ele é pequeno.** A estante mostrava só títulos; agora mostra a
arte de cada jogo. O caminho é o `zeebx_xbox_imagem`: o núcleo entrega os pixels crus em **BGRA8
premultiplicado** e o C++ monta um `SoftwareBitmapSource`. O trabalho já estava pago — quem lê o `.mif`
é a mesma varredura que lê o título —, e o que sai daí é o ícone **de menu** do BREW.

O que dá para esperar de tamanho, medido nos `.mif`: **de 16×16 a 65×42, com um 192×192 no melhor
caso**. Não é arte de caixa, é ícone de menu. Um ícone de 32×32 num cartão de 300 DIPs aparece
borrado, e é por isso que a faixa do cartão tem degradê próprio: **o que faz o ícone parecer posto ali
de propósito é o fundo, não a escala.**

**Quem quiser capa de verdade põe um PNG ao lado do jogo**, com o mesmo nome do `.mod` (`MeuJogo.png`
ao lado de `MeuJogo.mod`). O núcleo já preferia o arquivo ao ícone — o `cover()` é tentado antes do
`manifest_art()` —, e é a única saída para uma arte maior do que o `.mif` consegue guardar.

A arte chega ao cartão pelo **`ContainerContentChanging` da lista**, e não por um `Loaded` do `Image`:
dentro de um `DataTemplate` o `{Binding}` traz o título e mais nada, e nome dentro de modelo não é
alcançável do código. O evento do contêiner é o único ponto em que se sabe, ao mesmo tempo, **qual
índice** está ali (o `ItemIndex`) e **que o contêiner acabou de nascer** — e ele volta a ser levantado
a cada rolagem, no contêiner reciclado. Como o `SetBitmapAsync` é assíncrono, o índice é conferido
**depois** da espera: sem isso, a arte de um jogo apareceria no cartão de outro.

Medido neste PC com um `.mif` de bancada (um PNG de 128×128 dentro do formato): a estante de 801 jogos
mostra o ícone em todos os cartões, e o `scale` do XAML o apresenta suave — a pixelização que se teme
só aparece com ícone pequeno esticado, que é o caso dos `.mif` de fábrica.

## A varredura fora da linha da interface

**"O menu trava" era a varredura.** A lista de 801 jogos desta bancada leva **2.406 ms** para ser lida
com os `.mif` decodificados — medido, e é o número que está no relatório. Antes desta etapa ela rodava
dentro da chamada da ABI, ou seja **na linha da interface**: a janela ficava parada, sem repintar, e o
jogador apertava de novo (e o segundo A também não fazia nada).

A ABI ganhou duas funções e a varredura virou trabalho de fundo: `zeebx_xbox_varre_inicia` sobe uma
linha e devolve na hora; `zeebx_xbox_varre_colhe` responde `0` (ainda andando), `1` (pronto) ou `-1`
(nada pedido). O C++ pergunta a cada 120 ms por um `DispatcherTimer`, e a camada de "procurando"
(`ProgressRing` mais o recado) fica de pé enquanto isso. **Todas as chamadas da ABI continuam na linha
da interface** — é ela que lê a lista, e a linha de fundo só monta o `Vec`.

O `zeebx_xbox_jogos` responde quantos jogos há **sem varrer**: ele existe porque o `varre_colhe`
devolve `1` para "pronto", e a estante precisava de um número que não se confundisse com esse código.

**A primeira varredura é a do `ReataFontes`, e não a do `OnCarregado`.** As pastas do seletor só
voltam a valer depois que o `FutureAccessList` as reabre; varrer antes disso seria varrer sem enxergar
a unidade de fora — "os jogos do HD só aparecem depois de apertar varrer de novo" era exatamente isso.

Enquanto a varredura anda, **o B não faz nada** (não há para onde voltar com a lista trocando embaixo) e
o A não abre jogo. A camada de recado esconde as telas de tela cheia, e não é por estética: o
direcional anda por todo elemento visível da árvore, e um `Grid` não tem como ser desligado —
`IsEnabled` é do `Control`. Com as camadas colapsadas, o que o controle alcança é o próprio recado.
Quem lembra qual delas volta é o `m_camadaDoOcupado`: quem estava em **Fontes** quando a pasta entrou
volta para **Fontes**.

**Abrir um jogo continua sendo uma chamada que trava** — a `Session` não é `Send`, e mandá-la para
outra linha pediria mudar o núcleo —, mas agora ela acontece com o recado na tela e o tempo no
relatório: o `zeebx_xbox_comeca` roda depois de uma volta ao despachante, que é o que deixa o
compositor pintar a camada antes de a linha parar.

## O quadro que o jogo não desenhou

**O Zeebo desenha a 30 quadros por segundo dentro de uma composição de 60.** Na volta em que o jogo
não desenhou, o núcleo devolvia o mesmo quadro — e o aplicativo convertia 1,2 MB de RGB565 para BGRA8
e chamava o `Present` de novo, por nada. Metade do trabalho de vídeo era refazer um quadro que já
estava na tela.

O `publica()` do núcleo agora compara `(série, escritas, largura, altura)` com o que ele publicou da
última vez e devolve `bool`; quando não há quadro novo, o `passo` devolve **`ZEEBX_SEM_QUADRO_NOVO`
(4)** e o C++ **não apresenta**. O som, esse, não pode esperar: o `zeebx_xbox_audio` roda em toda
volta, tenha havido desenho ou não.

Duas coisas vieram junto, pelo mesmo motivo — o custo de cada volta: o buffer de BGRA passou a ser
**reusado** (o `bgra_de_rgb565_para` escreve num `Vec` que já existe, em vez de alocar 1,2 MB por
quadro) e o `Gamepad::Gamepads()` do `Input` deixou de ser uma lista nova a cada quadro.

**O que não foi medido: o ganho.** Sem ROM neste PC o laço de quadro não roda até o fim — a detecção
de quadro novo tem teste de unidade, e o caminho da apresentação foi medido com um quadro sintético,
mas "quantos quadros por segundo a mais" é pergunta que só o console responde.

## A engasgada, e o núcleo que recompila

**O que faz o emulador engasgar é o núcleo lento, e não o console.** Até esta etapa o alvo `uwp` rodava
o jogo **interpretado** — uma instrução por vez —, porque o recompilador (o JIT) não cabia no recorte
de memória de um aplicativo UWP sem um remendo no C++ de terceiro. Num console modesto de emular, o
interpretador é o que se mede; a mesma abertura de bancada, esgotando os 500 milhões de instruções do
orçamento:

| Núcleo | O mesmo trabalho |
|---|---|
| interpretador | **4.407 ms** |
| `dynarmic` (JIT) | **332 ms** |

O mesmo laço, medido na bancada de CPU deste PC, dá **28,1×** (2.398.867 instruções por milissegundo
contra 85.295). É o número que explica a queda de taxa: o Zeebo não é um console exigente, mas cada
instrução dele custa caro quando quem a executa é um laço em Rust, e barato quando é código nativo
gerado na hora.

**A escolha é do aparelho, e acontece no arranque.** O aplicativo sonda a memória executável — o
sistema concede? em que tamanho? — e conta a resposta ao núcleo; se o aparelho recusar, o emulador
continua rodando, interpretado, em vez de não rodar. A linha que responde "o console entrou no caminho
rápido?" é a do relatório:

```
nucleo do emulador: dynarmic (JIT), memoria executavel concedida
```

Se ali vier `interpretador`, a engasgada continua — e o que o console recusou está escrito **logo
acima** dessa linha, passo a passo (`reservar`, `confirmar`, `escrever`, `virar executavel`,
`executar`, cada um com o seu `sim`/`nao` e o código de erro).

**O que foi preciso consertar para o JIT funcionar foram quatro linhas** num `.cpp` do dynarmic. Sem
elas o aplicativo **fecha** ao abrir o primeiro jogo: o `dynarmic` escreve dentro do bloco de código
depois de ele virar somente-leitura, e num sistema que não deixa a mesma memória ser gravável e
executável isso é violação de acesso. O `compilar.ps1` aplica o remendo, conferindo o trecho antes de
trocar; o caso inteiro, com o endereço e o nome da função, está em
[`docs/implementacao/25-frontend-xbox-uwp.md`](../../docs/implementacao/25-frontend-xbox-uwp.md),
*A memória executável*. E, se um dia o aplicativo fechar sozinho, o relatório agora diz **onde**: a
linha `falha: …` sai antes de o sistema derrubá-lo.

**O que segue sem medição é a taxa no console** — o número que o relato do jogador descreveu como
"engasgada". O relatório conta o custo do carregamento e o tamanho do quadro, e não quadros por
segundo; é o olho na TV, com o JIT confirmado na linha acima, que fecha a questão.

## O menu de pausa, e a margem segura

**O menu saía cortado por duas razões, e as duas foram medidas.**

A primeira era ele mesmo: um `StackPanel` de **360 DIPs** de largura com uma linha que pedia mais do
que isso — o seletor de slot mais os dois botões de save —, e o "Carregar" saía pela borda direita. A
caixa agora tem `MinWidth` 380, `MaxWidth` 560, um `ScrollViewer` por dentro e **três colunas**
(`Auto`/`*`/`*`) na linha dos saves: o seletor mede o que precisa e os dois botões repartem o resto.

A segunda era a TV: **overscan**. O console come uma faixa da borda, e o que estava a 20 e 28 DIPs
dela — a linha das dicas, os contadores, o pé das folhas — caía exatamente ali. A raiz agora aplica um
recuo de **5% do lado menor, entre 12 e 48 DIPs**, em todas as camadas de tela cheia e na caixa da
pausa — e **nunca no quadro do jogo** (que é a tela inteira do console, e recuá-lo seria uma moldura
preta em volta do que o jogo desenhou) **nem na cortina da pausa** (que escurece a tela toda; com
margem sobraria uma moldura clara).

Medido neste PC a 150%: a raiz abre em 1280×664 DIP, o recuo sai **33 DIP** (5% de 664), e o título da
estante — que o XAML põe a 48 DIPs da borda — aparece a 81. O menu de pausa foi fotografado com a
caixa dentro do seguro: slot, Salvar e Carregar lado a lado, nada cortado. **O tamanho real da TV do
console é outro**, e é a linha `video: raiz ... dip` do relatório que o diz.

## O que já funciona, e o que não

**A etapa que compila está fechada, a estante abre no PC, e o A já abre jogo.** O que dá para afirmar
hoje, com a medida ao lado:

- **O núcleo compila.** O `cargo` produz o `zeebx_xbox.lib` (**159.981.164 bytes**, 152,6 MB) para o
  alvo, com as **39** funções da ABI: as 22 do desenho original mais as que as etapas seguintes
  trouxeram — os idiomas, as pastas de jogos, a varredura de fundo, a arte dos cartões e a escolha do
  núcleo. O tamanho é o dynarmic inteiro lá dentro: o C++ de terceiro só entra num `staticlib` com o
  `+whole-archive` do `src/lib.rs`, e é ele que responde pela maior parte do arquivo.
- **O aplicativo compila, liga e empacota.** `ZeebxXbox.exe` (**11.126.272 bytes**) e o pacote assinado
  de **4.248.010 bytes** (`0.4.6.0`) — quase tudo dele é o aplicativo, e o resto é a assinatura — saem
  de um `msbuild` sem um aviso. Até
  a etapa passada nada disso existia: os dois arquivos que dependem do XAML gerado — `App.cpp` e
  `MainPage.cpp` — nunca tinham sido compilados.
- **O pacote sai assinado, e a assinatura confere.** O `signtool` só reclama da confiança
  (`0x800B0109`, a raiz fora do repositório da máquina) e não do conteúdo — que é o erro que
  apareceria se o pacote tivesse sido alterado depois de assinado. Instalar o `.msix` **neste PC**
  ainda pede o certificado no repositório **da máquina**, um passo com elevador (ver
  [*O certificado*](#o-certificado-e-por-que-ele-confunde)), e é por isso que o PC segue pelo
  `-Registrar`.
- **A estante abre, e foi vista.** Com o pacote registrado no PC, a janela mostra o título, a linha
  de estado, a fila de botões e o estado vazio — e o núcleo sobe de verdade: ele cria
  `LocalState\{roms,saves,zeebx}` e o D3D11 enche o cache de sombreadores (`DxCache\*.parc`).
  **Dois defeitos só apareceram aí**, e os dois são de ordem de acontecimento: o alvo do D3D11 era
  criado antes de a janela ter tamanho (o `SizeChanged` da raiz chega **antes** do `Loaded`), e o
  registro de layout solto não recebe nem o `Assets\` nem os `.cso` — o build não os leva para lá, e
  a tela responde "Sem vídeo" até eles serem copiados à mão (é o que o `-Registrar` do
  `compilar.ps1` faz).
- **As quatro telas foram percorridas, e os três idiomas foram trocados.** A estante, **Fontes de
  jogos**, **Configurações** e **Sobre** abrem, e cada palavra delas vem do catálogo do núcleo:
  trocado para espanhol, a tela inteira se reescreveu na hora ("Ajustes", "Idioma de la interfaz"),
  e o `settings.json` ficou com `"es"`. Fechado e reaberto, o aplicativo abriu em espanhol — e de
  volta em inglês, que é o padrão. Nada disso precisou de recompilar: as frases são as do
  `assets/lang/`.
- **O A abre jogo, e foi medido nas duas rotas.** Quem o A aciona é quem tem o foco, e num cartão da
  estante isso pode ser o `Button` de dentro dele ou o contêiner da lista: os dois caminhos existem,
  os dois resolvem o índice pelo caminho exato — dois jogos de mesmo nome não se confundem — e os
  dois chegam ao mesmo `AbreJogo`. Medido no PC pela Automação de Interface, que aciona o mesmo
  caminho que o botão do console: acionado o cartão, o núcleo tentou abrir o jogo e recusou o arquivo
  de teste ("parou antes de começar"), que é a prova de que o pedido chegou lá. **Antes disto o
  direcional andava pela estante, o cartão se destacava, e o A não abria nada** — o `ItemClick` que
  estava ligado só chega pelo ponteiro, e o `Button` do cartão estava sem `Click`.
- **A caixa de pasta do sistema abre, e o que ela devolve chega ao núcleo.** Medido no PC: a caixa
  abriu, a pasta escolhida foi `%USERPROFILE%\Downloads`, a permissão foi guardada, e a ABI recusou
  o caminho com `-1` — o caminho inteiro do seletor foi percorrido, e parou onde tinha de parar neste
  PC (ver [*Os jogos podem vir de um pendrive*](#os-jogos-podem-vir-de-um-pendrive-ou-de-um-hd-externo)).
  A caixa de arquivo — a rota do console — usa a mesma mecânica, e a pasta sai do caminho do arquivo
  escolhido.
- **A permissão que reabre a pasta sobrevive ao fechamento.** As pastas escolhidas vão para
  `LocalState\fontes.txt`, uma linha por token do `FutureAccessList`, e o arranque seguinte reabre
  cada uma. Medido pelo lado do erro: plantado um token inválido no arquivo, o relatório registrou
  `permissao perdida (nao-e-um-token-valido): The parameter is incorrect.`, e o arquivo foi reescrito
  sem ele. **Não medido:** o lado do acerto — um token de verdade reabrindo a pasta —, que pede uma
  escolha de pasta que este PC recusa mais adiante (o `-1` acima).
- **O relatório conta o que aconteceu**, e é a única janela para dentro de um console. Ver
  [*Quando algo não funciona*](#quando-algo-nao-funciona).
- **O caminho do vídeo desenha, e isso foi medido neste PC com um quadro sintético.** A cadeia de troca
  compõe (o `Clear` aparece), o quadrilátero cai no lugar previsto e a orientação está certa — as três
  coisas que o desenho tem de acertar, e as três que estavam erradas. **No console, um jogo de verdade
  já apareceu, com imagem e som** — é o que diz quem usa, e não uma medida daqui: o relato é de um
  emulador "quase perfeito" em que o que atrapalha é a engasgada. **Neste PC, o quadro de um jogo
  continua sem medição**, porque não há ROM aqui. Ver
  [*A imagem*](#a-imagem-e-o-tamanho-do-quadro).
- **Os testes passam nos dois lados.** O `cargo test -p zeebx-xbox` são **15**, e entre eles estão os
  três desta etapa: a estante mostrar a arte que o jogo traz dentro do `.mif` (com um `.mif` de bancada
  montado à mão no próprio teste), o alfa do ícone sair premultiplicado, e a varredura de fora voltar na
  hora e entregar a lista depois. O `cargo test -p zeebx --lib` do núcleo são **669**, com 7 ignorados —
  e um dos ignorados é a própria medida do JIT contra o interpretador, que é medida e não teste, e por
  isso não roda em toda execução da suíte.
- **A estante com arte, e a varredura longa, foram medidas no PC.** Com 801 jogos de bancada — um
  `.mod` com um `.mif` de 128×128 e oitocentos `.mod` sem manifesto —, a captura mostra o ícone em
  todos os cartões do XAML e o recuo seguro nos lugares certos, e o relatório diz `801 jogo(s) em 2406
  ms`. É a medida que separa "a estante é lenta" de "ler oito mil `.mif` é lento". Ver
  [*A estante*](#a-estante-e-a-arte-que-cada-jogo-traz) e
  [*A varredura*](#a-varredura-fora-da-linha-da-interface).
- **O menu de pausa foi fotografado, e cabe.** Com a caixa dentro do recuo seguro, o seletor de slot,
  o Salvar e o Carregar aparecem lado a lado e nada sai pela borda — o defeito que o jogador viu no
  console. A medição foi feita com um instrumento temporário que mostrava a pausa na estante (não há
  ROM neste PC para chegar nela pelo jogo), e ele saiu do código depois da captura. Ver
  [*O menu de pausa*](#o-menu-de-pausa-e-a-margem-segura).
- **O quadro sem desenho novo deixou de ser apresentado**, e o ganho disso **não foi medido** — o laço
  de quadro não roda até o fim sem ROM. Ver [*O quadro que o jogo não desenhou*](#o-quadro-que-o-jogo-nao-desenhou).
- **O que fazia o XAML sair em C++/CX eram três coisas, e as três estão no `.vcxproj`:** um `.idl`
  por página, os nomes `App.h`/`MainPage.h` (sem o `.xaml`) e o `DependentUpon` de cada par
  `.xaml`/`.h`/`.cpp`/`.idl`. Faltando qualquer uma, o compilador escreve no lugar do conector de
  C++/WinRT um `ref class` — que o `cl` não compila sem `/ZW`, e que ainda ocupa o nome de que o
  `cppwinrt` precisa.
- **O núcleo é o `dynarmic`, e quem decide é o aparelho.** O alvo `uwp` compila os **dois** núcleos,
  e a escolha acontece em execução: no arranque o aplicativo sonda o caminho da memória executável
  (`app\Jit.cpp` — aloca 16 MB e uma página, escreve, troca a proteção e chama, e ainda tenta RWX de
  uma vez, que o sistema recusa) e conta o resultado ao núcleo por `zeebx_xbox_permite_jit`; a sessão
  abre então no recompilador ou no interpretador, conforme o que o sistema concedeu. O relatório tem as
  duas linhas que fecham a questão: a decisão, no arranque (`nucleo do emulador: dynarmic (JIT),
  memoria executavel concedida`), e a confirmação, na linha que diz em quanto tempo o jogo abriu.
  **Medido neste PC:** a mesma abertura de bancada que o interpretador esgotava em **4.407 ms** termina
  em **332 ms** com o JIT. E o que essa escolha custou foi um remendo: **quatro linhas** num `.cpp` do
  dynarmic, sem as quais o JIT derruba o aplicativo ao abrir o primeiro jogo — o caso inteiro, com o
  endereço e o defeito, está em
  [`docs/implementacao/25-frontend-xbox-uwp.md`](../../docs/implementacao/25-frontend-xbox-uwp.md),
  *A memória executável*.
- **`ntdll.lib` e `mincore.lib` são obrigatórias na ligação.** Um `staticlib` do Rust **não** carrega
  as bibliotecas nativas que o `std` pede: o `fs::rename` e o cano do `process` pedem funções `Nt`,
  que só o `ntdll` exporta, e o `QueryDosDeviceW` do caminho das unidades pede o `mincore` — que é a
  biblioteca que o próprio alvo `x86_64-uwp-windows-msvc` do Rust passa. Se você mexer na lista,
  não as tire.
- **Nenhum jogo de verdade rodou neste PC, e nenhuma unidade de fora foi lida.** Os `.mod` desta
  bancada são arquivos de mentira: eles exercitam a **estante** — a contagem, o título, a arte do
  `.mif` — e não o carregador, que recusa o conteúdo na hora de abrir. Também não há pendrive nem HD
  externo ligados: a tela de fontes foi vista no estado "nenhuma unidade", que é o único que dá para
  ver sem uma. Então a conversão do vídeo, o som (XAudio2), o mapa do controle, os save states e o
  carregamento continuam sem medição de ponta a ponta.
- **O primeiro teste em console aconteceu, e trouxe quatro fatos.** O pacote foi instalado num Series
  S em Modo de Desenvolvedor por quem usa: **o som toca**, **um jogo de verdade roda** (nada disto
  tinha rodado antes, nem aqui nem lá), **a imagem não apareceu** e **o B fechava o aplicativo**, com
  o jogo rodando inclusive. Os dois últimos viraram conserto numa etapa seguinte — [*O B, e a partida
  que ele interrompia*](#o-b-e-a-partida-que-ele-interrompia) e
  [*A imagem*](#a-imagem-e-o-tamanho-do-quadro) —, e cada um deles deixou uma linha no relatório: **a
  imagem preta era o desenho que nunca acontecia**, e foi um quadro sintético, e não o console, que
  mostrou isso (ver [*A imagem*](#a-imagem-e-o-tamanho-do-quadro)).
- **E o relato que abriu esta etapa veio de lá:** com aqueles consertos no lugar, quem joga disse que
  o emulador ficou **quase perfeito**, e que o que atrapalha é a **engasgada** — a taxa de quadros cai
  muito, num console modesto de emular. É relato de uso, e não medição daqui; mas é ele que explica
  por que o JIT deixou de ser "a segunda etapa" e virou esta: até então o jogo rodava **interpretado**,
  um a um, e o interpretador é o que se mede em **4.407 ms** no lugar dos **332 ms** do recompilador.

### Quando algo não funciona

O aplicativo escreve `LocalState\zeebx-xbox.log` a cada arranque, e reescreve o arquivo do zero — ele
descreve **esta** execução, e não um histórico. Num console não há depurador na TV, e o File Explorer
do Portal do Dispositivo lê `LocalState`: é aquela a janela. Ele registra:

- o que o sistema respondeu sobre o modo de ponteiro (foco, ou a recusa com a mensagem e o código);
- cada pasta que uma caixa do sistema devolveu — inclusive quando devolveu **nada** —, cada permissão
  guardada ou reaberta, e o código que a ABI devolveu ao acrescentar a pasta (`0` entrou, `-1` não
  existe para o aplicativo, `-2` já estava, `-3` existe e não abre);
- cada jogo que o núcleo recusou, com o motivo que ele deu — e, desde esta etapa, **quanto tempo a
  abertura levou** (`jogo abriu (indice N) em X ms`), que é o número que separa "o emulador está lento"
  de "o carregamento é lento";
- a varredura de fora: o começo, e o fim com a contagem **e o tempo** (`801 jogo(s) em 2406 ms`);
- o tamanho em que o quadro é apresentado — a raiz em DIPs, a escala de composição do painel e a
  cadeia de troca em DIPs —, a cada mudança de tamanho;
- o tamanho do primeiro quadro que o núcleo publicou, e em que quadro da partida o jogo desenhou pela
  primeira vez (`primeira imagem`);
- cada decisão sobre o B: se ele ficou com o jogo, se fechou a pausa, se voltou um passo numa tela, ou
  se ficou com o sistema;
- a memória executável, **passo a passo** — reservar, confirmar, escrever, virar executável, executar,
  cada um com o seu `sim`/`nao` e o código de erro —, e o núcleo que essa resposta escolheu
  (`nucleo do emulador: dynarmic (JIT)` ou `interpretador`). É a linha que responde "o console entrou
  no caminho rápido?";
- as exceções que o XAML não tratou;
- e, quando a falha **mata** o processo, a linha `falha:`, escrita antes de o sistema derrubar o
  aplicativo: o código da exceção, se o acesso foi de leitura ou de escrita, o endereço alvo e o
  endereço da falha **relativo ao próprio executável** — que é o número que o `ZeebxXbox.map` (ao lado
  do `.exe`, e fora do pacote) transforma em nome de função.

É a primeira coisa a olhar quando algo não funcionar. `-1` e `-3` na linha da pasta querem consertos
diferentes, e `nada` quer um terceiro; na imagem, a linha do tamanho diz **em que pixels** o quadro foi
apresentado e a linha `primeira imagem` diz **se o jogo desenhou** — e são dois defeitos que se
parecem, mas moram em lugares diferentes.

### Quatro armadilhas de quem for repetir esta medição

- **O `-Registrar` apaga o `LocalState`.** Não é um efeito colateral do `compilar.ps1`: é o registro
  de layout solto, que recria o estado do aplicativo. Medido duas vezes, sempre igual — e é por isso
  que o `compilar.ps1 -Registrar` avisa que as ROMs vão depois, pelo portal. Consequência prática:
  **re-registrar no PC zera as pastas de jogos e o idioma escolhido**, e no console o mesmo vale para
  uma instalação nova do pacote. A estante, os catálogos e o núcleo ficam; o `LocalState`, não.
- **A captura de tela precisa vir de um processo DPI-ciente.** Numa tela a 150%, um `GetWindowRect` +
  `CopyFromScreen` de um processo que não declara DPI recebe as medidas da janela já divididas pela
  escala e copia só a fatia fisicamente correspondente àquele retângulo — o canto superior esquerdo. O
  sintoma é uma janela que "aparece cortada à direita" com um painel centralizado "deslocado": o
  defeito é do instrumento, e custou duas sessões de investigação. Uma chamada a
  `SetProcessDpiAwarenessContext(PER_MONITOR_AWARE_V2)` antes de qualquer `Add-Type` resolve, e a
  captura sai inteira.
- **O caminho do vídeo pode ser medido sem ROM, com um quadro sintético.** Uma tela preta é
  indistinguível de "o desenho não aconteceu" — foi por aí que dois defeitos mudos do `Renderer.cpp`
  passaram por duas leituras de código. Desenhar um padrão de geometria conhecida pela **mesma**
  função que apresenta o jogo, e capturar a tela, separa as três perguntas de uma vez: o desenho
  aconteceu, onde ele caiu e para que lado está virado. **Meça com o número, e não com o olho:** o
  padrão com uma cruz no centro transforma "parece deslocado" em "970 px, e o esperado era 971".
- **O `settings.json` editado à mão tem de ser UTF-8 sem BOM.** O `serde_json` recusa um BOM, o
  `Settings::load_from` cai no padrão e a abertura seguinte grava o inglês por cima — que é o
  comportamento desejado (uma escolha ilegível não pode travar o aplicativo), mas some com o idioma
  escolhido sem avisar. O `Set-Content` do PowerShell 5.1 escreve **com** BOM; o caminho é
  `[System.IO.File]::WriteAllText($caminho, $texto, (New-Object System.Text.UTF8Encoding($false)))`.
