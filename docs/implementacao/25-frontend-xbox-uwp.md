# 25 — O frontend de Xbox (UWP)

O Zeebx num Xbox Series S|X. A parte de compilar e instalar está em
[`frontends/xbox-uwp/LEIAME.md`](../../frontends/xbox-uwp/LEIAME.md); aqui está **por que** cada
peça ficou onde ficou, e o que ainda não foi provado.

## Por que UWP, e não outra coisa

Não há caminho alternativo. Um Xbox de fábrica só executa o que a Microsoft assinou; o Modo de
Desenvolvedor aceita **aplicativo UWP** e mais nada. Não existe o equivalente de um softmod de
Wii ou de um homebrew de Switch: as duas portas que a Microsoft deixou são o Modo de
Desenvolvedor (UWP, x64, sideload por `MSIX`/Portal do Dispositivo) e o programa de
desenvolvedores do GDK, que é sob NDA e não serve para um projeto GPL.

Três consequências vêm daí, e moldam tudo:

- **Só x64.** Não há UWP de 32 bits no console, e o Series S|X é x64. Um alvo só.
- **O aplicativo roda num AppContainer.** As APIs que escrevem em disco, abrem socket ou alocam
  página executável são as versões "FromApp", ou não existem.
- **A memória é limitada — se a capacidade certa não for pedida.** Um aplicativo de UWP no Xbox
  tem cerca de **1 GB** de teto e até **45%** da GPU. Com a capacidade restrita
  `expandedResources` ele passa a rodar com os recursos de um jogo — é a mesma UWP, com o teto
  levantado. Um emulador num teto de 1 GB não vai longe, então o manifesto declara a capacidade.
  (O Cemu UWP Host mede esse teto perto de 5 GB no Series S, e é o número que se deve ter em mente
  ao dimensionar o cache de código.)

## O corte entre Rust e C++

O aplicativo é **C++/WinRT**, e o núcleo é o crate `zeebx` de sempre, compilado como
`staticlib` e ligado dentro dele. É o mesmo corte do iOS, que liga um `.a` numa UIKit.

```
frontends/xbox-uwp/
  src/lib.rs            a fronteira: uma ABI C, chamada da linha da interface
  include/zeebx_xbox.h  o contrato que o C++ inclui
  app/                  o aplicativo de UWP: XAML, D3D11, XAudio2, Windows.Gaming.Input
```

### Por que não um frontend Rust puro

O alvo `x86_64-uwp-windows-msvc` é **tier 3**: o Rust não distribui `std` para ele, e compilar
exige `nightly` com `-Z build-std`. Isso vale para *qualquer* código Rust neste alvo, inclusive o
aplicativo. Mas o que decidiu não foi o `build-std` — foi o resto: um aplicativo de UWP precisa
do `Application` do XAML e do seu ciclo de vida (`OnLaunched`, `Suspending`, `Resuming`), de um
`SwapChainPanel`, de um `XamlRoot`, e o `winit` não tem nada disso. Escrever a camada de
aplicativo em Rust significaria reescrever à mão o que o C++/WinRT gera a partir do `.xaml`.

O corte então é o mesmo dos outros alvos móveis: **Rust emula, o anfitrião apresenta.** A ABI é
estreita de propósito — varredura, passo, quadro, som, botão — e é a mesma forma da do iOS, o que
faz um defeito de uma valer para a outra.

### Por que o quadro vem do rasterizador de software

Não há contexto de OpenGL num aplicativo de UWP: o `gl`/`glutin` linka EGL/GLX/WGL, e nada disso
existe num AppContainer. O Xbox expõe Direct3D — 11, e 12 só para quem tem a capacidade de
"jogo". Então o núcleo entra pelo caminho que o iOS usa (`start_software_with_storage_installed`),
entrega RGB565, e a fronteira converte para BGRA8, que é o `DXGI_FORMAT_B8G8R8A8_UNORM` que o
D3D11 recebe sem trocar canal.

**Isto custa caro**, e é honesto dizer: o rasterizador de software é justamente o que o documento
[06-video-3d.md](06-video-3d.md) mostra sendo substituído pelo desenho na placa. Ligar o `gl` do
núcleo a um contexto de D3D11 não é um caminho: são APIs diferentes. O caminho seria um
rasterizador D3D11 no núcleo, que é um subsistema novo — não um ajuste deste porte.

## O som: o núcleo não toca

No iOS a placa é o `cpal`, atrás da feature `audio`, e o núcleo toca sozinho. Aqui **não**: o
`cpal` no Windows é WASAPI, e WASAPI é uma API de sessão de desktop, que um AppContainer não
abre. A placa é o **XAudio2**, do lado C++.

Isso caiu bem por acaso, porque o núcleo já tinha o caminho: `Session::grava_audio` e
`Mixer::render` são o que o core Libretro usa, e ele também não linka placa nenhuma. A fronteira
chama `Mixer::render` e entrega estéreo intercalado em i16; o `Audio.cpp` copia para um anel de
buffers e os submete à voz de origem.

**O tempo do som vem do relógio virtual, e não de um número fixo por quadro.** Um jogo que emula
dois quadros entre duas voltas precisa entregar o dobro de amostras, senão o som atrasa em relação
à imagem. É a mesma conta do `libretro/src/lib.rs`, e o comentário de lá explica por quê. O anel
tem cinco pedaços de 20 ms: o bastante para uma volta perdida não virar estalo, e pouco para a
latência não aparecer no botão.

## A GUI: XAML, e não desenho na placa

O menu é XAML comum — `GridView` de cartões, `Slider`, `ComboBox`, `ContentDialog`. **Não** é
uma GUI desenhada em D3D por cima do quadro.

A razão é o controle. Numa TV, o foco do controle tem de andar de botão em botão e o realce tem
de ser óbvio a três metros; o XAML faz isso sozinho, e uma GUI desenhada à mão teria de
reimplementar foco, navegação, realce e escalonamento — quatro coisas em que é fácil errar e que
não têm nada a ver com emular Zeebo.

As camadas são **seis `Grid` sobrepostos**, e não uma pilha de navegação com páginas: o vídeo, a
estante, as três folhas (fontes, configurações e sobre) e a pausa. Uma `Frame` com páginas para seis
estados é cerimônia que só atrapalha o foco — e é o foco que manda aqui.

**A interface é o foco.** Não há cursor: quem anda é o foco, e o A aciona quem o tem. Toda camada que
abre **põe o foco onde o jogador vai querer estar** — a estante no primeiro cartão, as fontes no botão
que acrescenta pasta, as configurações no comboio de idioma, a pausa no "Continuar" —, porque uma
camada que abre sem foco é uma camada em que o controle não faz nada. E
`XYFocusKeyboardNavigation="Enabled"` está ligado em cada camada, para o direcional poder sair de uma
lista e alcançar a fila de botões logo abaixo dela: sem isso o foco fica preso dentro da `GridView`. A
linha de baixo da tela diz o que cada botão faz, e as três palavras dela — jogar, voltar, menu —
também vêm do catálogo.

O `SwapChainPanel` fica embaixo de tudo e some quando a estante aparece — mas ele nunca é
recriado. Recriar a cadeia de troca a cada troca de tela custaria o dispositivo e o shader, e o
laço tem de estar quente quando o jogo começa.

## Os idiomas: a GUI não tem frase própria

**O aplicativo do Xbox não guarda uma linha de texto.** Todo rótulo, título e recado sai do
catálogo do núcleo, por chave, com as 39 chaves `xbox.*` que esta etapa acrescentou aos quatro
`assets/lang/*.json`. É a mesma decisão do `library` e do `settings`, e a razão é a mesma: o
catálogo é do emulador, não do frontend, e um frontend que traduz por conta própria cria uma
segunda lista para manter em dia.

A consequência prática vale o registro: **uma chave que falta aparece na tela como a própria
chave** — `xbox.sources.pick` no lugar de "Use this folder" —, e um defeito assim é visível no
primeiro olhar. Em compensação, `Text(Texto("chave"))` compila e roda com qualquer chave, e o
`chaves_da_interface.rs` — o teste que pega chave faltando — varre o Rust e o QML: **o C++ fica de
fora dele**. As chaves que o `.cpp` pede são conferidas à mão contra o `en.json` (foram 61 nesta
etapa, e a conferência é um `foreach` de dez linhas).

O idioma padrão é o **inglês** (`i18n::FALLBACK = "en"`), e a lista oferece os quatro catálogos de
fábrica: `en`, `es`, `es-MX` e `pt-BR`. O que o `settings.json` guarda é o **código** (`"pt-BR"`), e
não o índice da lista: o índice muda de significado no dia em que a lista mudar de ordem, e o
código, não.

Os nomes vêm do `language.name` de cada arquivo, e por isso saem no próprio idioma — "English",
"Português (Brasil)". Quem procura português procura por "Português", e não por "Portuguese".

**A troca foi vista em três línguas, e sobrevive a fechar o aplicativo.** Medido no PC, com a janela
dirigida pela Automação de Interface — o mesmo caminho que o Narrador usa, e que também serve para
apertar botão sem foco de teclado: escolhido "Español", a tela inteira se reescreveu ("Ajustes",
"Idioma de la interfaz", "Proporción del cuadro", "Estados guardados"), o `settings.json` passou a
dizer `"es"`, e reaberto o aplicativo abriu direto em espanhol. O mesmo com `pt-BR` e de volta para
`en`. O arquivo é escrito pelo núcleo (`Settings::save_to`), sem BOM.

**Os controles sem `Header` levam `AutomationProperties.Name`.** Nesta tela o rótulo de um `ComboBox`
e de um `Slider` é um `TextBlock` ao lado, e não o `Header` do controle: sem o nome acessível, a
árvore de acessibilidade trazia três "caixa de seleção" e um "controle deslizante" **sem dizer de
quê**. Os dois interruptores têm `Header` e já vinham nomeados de graça.

**E o comboio da proporção é remontado a cada troca de idioma.** A parte fechada de um `ComboBox`
guarda uma cópia do texto do item escolhido: trocar o `Content` de um item já desenhado não chega até
ela. A captura em português pegou "Proporção do quadro" com "Native (4:3)" — em inglês — embaixo,
enquanto o item da lista já dizia "Nativa (4:3)". Os dois itens agora nascem em código
(`PreencheProporcao`), como os dos slots, e a marcação é reposta com a trava `m_remontandoListas`,
que é a mesma que impede o laço da troca de idioma.

**As frases de erro do núcleo continuam em português**, e isso está anotado no LEIAME: elas não
moram no catálogo, e sim no meio do caminho que carrega o jogo (`session`, `loader`), e traduzi-las
é outro serviço. O que esta etapa traduz é o que é da GUI: o título da caixa, o rótulo, o recado.

## As fontes de jogos de fora

A estante lê `LocalState\roms` e mais as pastas que o jogador apontar, e essas pastas podem estar
num pendrive ou num HD externo. O caminho é o `KnownFolders.RemovableDevices`, e ele custa **duas**
declarações no `Package.appxmanifest` — e nenhuma das duas serve sozinha:

| Declaração | O que ela dá | O que acontece sem ela |
|---|---|---|
| `<DeviceCapability Name="removableStorage" />` | a permissão de ler arquivo em unidade removível | nenhuma unidade aparece |
| `<uap:Extension Category="windows.fileTypeAssociation">` com `.mod`, `.mif`, `.zip` e `.7z` | *quais* arquivos o aplicativo pode ler de lá | a unidade aparece **vazia** |

A segunda é a que engana. Ela tem cara de "Abrir com" — é o que a Microsoft chama de *File Type
Association* —, mas em mídia removível ela é a lista de extensões que o acesso enxerga, e a
documentação da capacidade diz isso com todas as letras ("Your app can only read and write files of
file types that the app has registered to handle"). As quatro extensões são as que o
`library::scan` abre: o `.mod` é o jogo, o `.mif` é de onde sai o título, e o `.zip`/`.7z` é o jogo
embalado. Tirar uma tira a leitura daquele tipo no console — e o modo de falhar é o pior possível:
unidade cheia que aparece vazia não dá erro nenhum, e parece defeito da varredura.

**`broadFileSystemAccess` não é alternativa**: a Microsoft lista essa capacidade como não suportada
no Xbox, e é o console o alvo.

No console, o primeiro acesso abre uma caixa do sistema — é o
`KnownFolders::RequestAccessAsync(KnownFolderId::RemovableDevices)`, que devolve um
`KnownFoldersAccessStatus`. No PC não há caixa nenhuma e o pedido pode nem existir, então **a falha
do pedido não é tratada como recusa**: quem responde se o acesso veio é a listagem logo abaixo, com
o `try`/`catch` em volta do `GetFoldersAsync`.

As pastas escolhidas vão para o `LocalState\settings.json`, e não para o `settings.json` do desktop:
o `settings_path()` do núcleo aponta para o `%APPDATA%`, que o AppContainer bloqueia. O
`Settings::load_from`/`save_to` já recebiam o caminho por parâmetro — quem o passa é o Xbox.

**O núcleo ganhou um campo, e um só: `Settings::roms_dirs`**, ao lado do `roms_dir` que o desktop já
tinha. O desktop tem *uma* pasta de jogos, escolhida numa caixa de diálogo; no console a lista cresce
e encolhe com o que estiver plugado. Acrescentar o campo em vez de trocar `PathBuf` por
`Vec<PathBuf>` é o que mantém os dois `settings.json` legíveis um pelo outro: um arquivo do desktop
aberto no console não perde a pasta, e um do console aberto no desktop não perde nada além da lista
extra — que fica `null`, no arquivo do console, por não ser usada lá.

A varredura de várias pastas é um laço **no frontend**, e não no núcleo: `library::scan` continua
recebendo uma raiz, e o `varre` do Xbox chama uma vez por fonte e tira as repetições pelo caminho do
arquivo — duas fontes podem se cobrir (a unidade inteira e uma pasta dentro dela), e o mesmo `.mod`
não pode virar dois cartões na estante. Para o núcleo, uma unidade removível é uma pasta como outra
qualquer: quem a concede é o UWP.

**As duas caixas do sistema, e o explorador como reserva.** A tela de fontes tem dois botões — abrir a
caixa de pasta e abrir a caixa de arquivo —, e nenhum dos dois é um explorador nosso. A caixa de pasta
existe no PC e é a rota de lá; a Microsoft documenta que cenários de seletor de pasta **não são
suportados no Xbox**, e por isso a rota do console é escolher um arquivo de jogo, cuja pasta passa a
ser a fonte. Os dois botões ficam à vista porque nenhum dos dois pode ser escondido a partir de uma
tentativa que falhou: o aparelho que não tem um precisa do outro, e um botão que não faz nada e não
diz por quê foi exatamente a queixa que esta tela veio consertar. O explorador de pastas que sobrou
(com a pilha de navegação, o "Subir" e o "Usar esta pasta") é o terceiro caminho, e só aparece quando
tem o que mostrar — unidade na tomada, uma pasta aberta, ou um recado dentro dele.

**A pasta que o jogador escolhe nem sempre é uma pasta que o núcleo lê.** Medido no PC: escolhida
`C:\Users\janmi\Downloads` — que existe, e abre no Explorer —, a ABI recusou com **`-1`**. O recorte
do UWP é quem manda: do lado de dentro do aplicativo, `is_dir()` responde `false` para o que o
AppContainer não enxerga, por mais que a caixa do sistema o tenha concedido. No PC isso é o esperado
(quem lê pastas de usuário é o desktop, que não é sandbox nenhum), e a tela diz que a pasta não
existe; no console quem abre a unidade é a capacidade `removableStorage`, e é por isso que a rota de
lá é a do arquivo. **Se a capacidade dá ao núcleo leitura de diretório — e não só de arquivo — na
unidade removível é o que só o console diz**: o `read_dir` da varredura é o que separa `0` de `-3`, e
a linha do relatório traz o código.

**A permissão das pastas escolhidas mora num arquivo, e não no `LocalSettings`.** Medido: o `Insert`
no `LocalSettings` não chegou ao `settings.dat` — a gravação ficou no log de transação do registro, e
um fechamento brusco a levou embora —, e o arranque seguinte não achou token nenhum: a pasta sumia
sem deixar recado. Os tokens do `FutureAccessList` passaram a morar em `LocalState\fontes.txt`, uma
linha por token, lido e reescrito pelo próprio aplicativo: um arquivo que dá para conferir, e que o
Portal do Dispositivo mostra no dia em que a pasta não reaparecer. É o mesmo raciocínio do
`settings.json` do núcleo, que já é um arquivo comum pelo mesmo motivo.

A navegação é uma **pilha de pastas nossa**, e não o `GetParentAsync`: numa unidade removível a pasta
raiz não tem pai que o WinRT devolva, e quem sabe por onde se desceu é quem desceu. O `Subir` é um
`pop`. Na rota da caixa de arquivo a pasta sai do caminho do próprio arquivo escolhido — corte na
última barra, sem `GetParentAsync` e sem uma ida ao sistema de arquivos no meio.

Três decisões pequenas, cada uma com o seu teste:

- **A identidade da pasta é comparada sem a barra final e sem caixa.** A ABI devolve `E:\jogos\`
  onde o jogador escolheu `E:\jogos`; sem isso, apontar duas vezes para a mesma pasta a acrescenta
  duas vezes, e a estante mostra os mesmos jogos repetidos.
- **A pasta que existe e não pode ser lida devolve `-3`, e não `0`.** Uma unidade desplugada tem de
  aparecer como ausente, com o recado ao lado; e um caminho sem permissão não pode virar "varri e
  não achei nada", que é uma resposta que mente.
- **A varredura continua síncrona**, como no desktop — não há scanner em segundo plano no núcleo. O
  "Atualizar lista" escreve `library.scanning` no subtítulo e devolve o controle ao despachante antes
  de varrer: sem isso, a tela congela sem dizer por quê. É o mesmo motivo pelo qual a contagem do
  subtítulo sai de um campo guardado, e não de uma varredura nova a cada troca de idioma.

## O laço

`CompositionTarget::Rendering`, e não um `Timer`. Ele dispara no compasso do quadro da própria
tela, o que mantém o emulador e a composição do XAML no mesmo ritmo. Um `Timer` de 60 Hz numa
tela de 60 Hz anda fora de fase e engasga — e a diferença entre os dois é justamente o que um
jogo de ritmo sente.

O orçamento de cada volta é o tempo real decorrido, preso a `FATIA_MAXIMA` (100 ms), como no
resto do projeto: uma volta que demorou — o aplicativo voltou do segundo plano — não pode virar um
salto de minutos.

### O quadro, e quem manda no tamanho dele

O painel do vídeo é um `SwapChainPanel`: o aplicativo desenha num buffer e o XAML compõe esse buffer na
árvore, embaixo da GUI.

**A cadeia de troca é medida em DIPs, e o painel compõe um pixel por DIP.** Isto contraria a leitura
que parece óbvia — a de que o buffer é medido em pixels físicos e quem converte é o `CompositionScale`
—, e contrariou este documento por uma rodada inteira:

    cadeia de troca (dip) = tamanho da raiz (dip)

Medido com um quadro sintético de 320x240 apresentado pela mesma `Apresenta`: com a cadeia multiplicada
pela escala de composição do painel (1,5 neste PC), o centro da cruz do padrão caiu em **1440 px** de uma
janela de 1920 (o esperado eram 960), a borda esquerda em **444** (296) e a tarja direita saiu fora da
tela — tudo exatamente 1,5x. Com a cadeia em DIPs, os três números voltaram ao lugar: a tarja esquerda
termina em 306 (esperado 307), a cruz fica em 970 (971) e a tarja direita começa em 1635 (1635).

O `CompositionScale` continua sendo lido — ele vai para o relatório, porque é ele que diz em que
contexto de DPI o console abriu o aplicativo —, mas **não dimensiona mais nada**. `CompositionScaleChanged`
segue ouvido pela mesma razão: é onde o número do relatório se atualiza.

Quatro momentos remedem o quadro, e todos existem porque a ordem em que as coisas ficam prontas muda
com o aparelho: o `SizeChanged` da raiz (chega **antes** do `Loaded`, quando o dispositivo ainda não
existe — o `DefineTamanho` só guarda o número), o fim do `Inicializar` (o dispositivo existe e o
número guardado é aplicado), o `CompositionScaleChanged` do painel, e a abertura do jogo (o painel
acaba de sair de colapsado, com um `UpdateLayout` para ele ter escala).

Três linhas respondem no relatório: `video:` (em que DIPs o quadro é apresentado), `primeiro quadro:`
(o que o núcleo publicou) e `primeira imagem:` (que só sai quando o jogo desenha algo). **Sem a
terceira, o defeito não é de apresentação** — o jogo nunca mandou pixels.

### O desenho, e os dois silêncios que ele escondeu

Uma tela preta é indistinguível de "o desenho não aconteceu": as duas mostram a mesma coisa, e o
relatório fica limpo nas duas. Duas linhas do `Renderer.cpp` garantiam a segunda, e nenhuma delas
falha com erro:

1. **O alvo de saída era ligado com `m_alvo.put()`.** O `put()` de um `com_ptr` **solta o ponteiro
   antes de devolver o endereço dele** — é a forma de parâmetro de *saída* —, então o
   `OMSetRenderTargets` recebia um alvo nulo. A chamada de desenho sem alvo de saída não falha e não
   escreve, e o `Clear` não passa pela saída: ele continuava aparecendo, e o `Apresenta` seguinte saía
   cedo pela guarda do `m_alvo` nulo. As outras ligações do arquivo (`IASetVertexBuffers`,
   `PSSetShaderResources`, `PSSetSamplers`) sempre usaram `get()`.
2. **O layout de entrada nunca era criado.** O `quad.hlsl` lê `POSITION` e `TEXCOORD0` de um buffer de
   vértices; sem o layout declarado, o D3D11 não rasteriza — em silêncio, e sem erro na ABI.

**O instrumento que os achou foi um quadro sintético**, não uma leitura de código: um padrão com
moldura vermelha, marcadores de canto e uma cruz no centro, desenhado pela mesma `Apresenta` que
apresenta o jogo, numa captura de tela do PC. Ele responde as três perguntas de uma vez — o desenho
aconteceu, onde ele caiu, para que lado está virado — e transforma "parece deslocado" em "970 px, e o
esperado era 971". O padrão não fica no aplicativo; ele existe enquanto a medição dura.

O que **não** é defeito, e ficou medido: o descarte de face. Um estado de rasterizador com
`CULL_NONE` foi posto e depois tirado — com o estado padrão o quadrilátero desenha igual, e a ordem em
que o `Apresenta` escreve os vértices não é descartada.

## O B, e o voltar do sistema

No primeiro teste em console **o B fechava o aplicativo**, com o jogo rodando inclusive. Não era mapa
de botões errado: no Xbox o B é o **"voltar" do sistema**, e um aplicativo que não consome o
`BackRequested` do `SystemNavigationManager` **é fechado por ele**. O aplicativo não o consumia.

O conserto tem duas partes que se parecem e não são a mesma coisa:

1. **A porta que o sistema conhece.** `SystemNavigationManager::GetForCurrentView().BackRequested` é
   levantado por fora do XAML, e o `Handled` dele é o que decide se o aplicativo fica ou sai.
2. **A porta do XAML, no lugar certo.** O `KeyDown` estava na raiz de dentro (o `Grid`), e com o jogo
   rodando **o foco é a própria página** — o `AbreJogo` faz `Focus` no `this`, porque um `Grid` é
   `Panel` e painel nenhum recebe foco. Um `KeyDown` que borbulha a partir da página **nunca passa por
   um filho**: o tratador não rodava, o `Handled` não era posto, e o B seguia para o sistema. O
   tratador mudou para a página, que é o último elemento da linha de borbulhamento.

O que o B faz agora é decisão de tela, e não de tecla: com o jogo rodando ele **fica com o jogo** (é o
botão 2 do Zeebo, e o `Input` o entrega ao núcleo sem passar pelo XAML); com a pausa aberta, fecha a
pausa; nas folhas, volta um passo — no explorador, sobe um nível antes de sair da tela; e **na
estante fica com o sistema**, porque sair para o menu do Xbox é o que o B faz num aplicativo de
console. Quem sai do jogo é o `View`, pela pausa.

**Uma ação por toque, e não duas.** O mesmo B chega pelas duas portas — a tecla e o "voltar" do
sistema —, e as duas juntas fariam a pausa fechar e reabrir. O `TrataVoltar` guarda o instante do
último toque e ignora o segundo dentro de 250 ms; a repetição da tecla presa
(`KeyStatus().WasKeyDown`) também é ignorada, senão o B preso viraria um pisca-pisca de pausa.

**No PC** o teclado é a bancada: o `Esc` é o mesmo B, e o `P` abre e fecha a pausa. O `Esc` abria a
pausa antes, e deixou de abrir quando passou a ser o B do jogo — sem o `P`, um PC sem controle ligado
não teria como abrir a pausa de jeito nenhum.

## A memória executável: como o JIT entrou

**Neste porte o núcleo é o `dynarmic` — menos onde o aparelho não deixar.** Este documento dizia o
contrário: que o JIT ficava para depois, porque o alocador de memória executável está dentro do C++
embutido, que chama `VirtualAlloc` e `VirtualProtect` direto, e ensiná-lo o caminho de um
AppContainer pediria uma cópia vendorizada inteira. **Isso estava errado**, e o que derrubou a
suposição foi ir ler duas coisas em vez de repetir a conclusão: o `<memoryapi.h>` do SDK, e as
opções do próprio dynarmic.

O que existe, e o que foi medido em cada passo:

1. **O dynarmic já traz o caminho de W^X.** A opção `DYNARMIC_ENABLE_NO_EXECUTE_SUPPORT`, do CMake
   dele, troca o alocador de "pede uma página gravável e executável de uma vez" para "confirma como
   RW, escreve o bloco e só então troca a proteção para RX". É exatamente o que um AppContainer
   aceita, e não é código nosso: é uma opção de quem escreveu o recompilador.

2. **Os nomes de sempre viram as variantes `FromApp` sem uma linha de patch.** No recorte de API de
   um aplicativo (`WINAPI_FAMILY=WINAPI_FAMILY_APP`), o `<memoryapi.h>` do SDK declara `VirtualAlloc`
   e `VirtualProtect` como funções **inline que chamam** `VirtualAllocFromApp` e
   `VirtualProtectFromApp`. O C++ de terceiro continua chamando o nome que ele conhece, e o SDK
   entrega a chamada que este processo pode fazer.

3. **Os testes do dynarmic saem do build.** O alvo `install` dele constrói quatro executáveis de
   bancada que ligam contra o conjunto de bibliotecas de um aplicativo de console, onde
   `VirtualAllocFromApp` não existe: o link morre em `LNK2019: unresolved external symbol
   __imp_VirtualAllocFromApp`, por causa de um alvo de teste que ninguém pediu construir.

**Não há árvore vendorizada nem `[patch.crates-io]`.** O que entra é um arquivo de cadeia de
ferramentas — `appcontainer.cmake` — com as três definições acima e o porquê de cada uma, além de
**quatro linhas remendadas num `.cpp` do dynarmic**, aplicadas pelo `compilar.ps1` com o trecho
conferido antes da troca. As quatro linhas não são preferência: sem elas o JIT **derruba o aplicativo**
ao abrir o primeiro jogo, e o porquê é a seção seguinte.

### O JIT entrava e derrubava o aplicativo: um defeito do dynarmic sob W^X

**A sonda passava nos dois tamanhos, e mesmo assim o JIT não salvava o aplicativo.** Com a memória
executável concedida — o relatório diz `reservar sim, confirmar sim, escrever sim, virar executavel
sim, executar sim` —, abrir um jogo matava o `ZeebxXbox.exe` em poucos segundos, com `c0000005` e
sempre no mesmo deslocamento. Os fatos que fecharam o caso, na ordem em que apareceram:

1. **O mesmo deslocamento com dois programas-guest diferentes.** Um `.mod` de bancada que só faz
   aritmética e outro que mexe em memória morriam no mesmo endereço: é caminho de código do
   hospedeiro, e não do jogo.
2. **Com o JIT desligado à força** (`zeebx_xbox_permite_jit(0)`), a mesma abertura sobrevive e o
   relatório diz o motivo pelo qual o jogo não abriu. É o que separa "o JIT" de "qualquer outra
   coisa".
3. **O mapa de símbolos deu o nome.** O relatório de falha do Windows traz só o deslocamento
   (`P9: 00000000004e6534`); com `/MAP` no ligador, o `.map` que fica ao lado do `.exe` diz que
   aquele endereço está dentro de `Dynarmic::Backend::ExceptionHandler::SetFastmemCallback`.
4. **O defeito, lido no fonte da crate:** o construtor do `A32EmitX64` chama `code.PreludeComplete()`
   — que, com o `DYNARMIC_ENABLE_NO_EXECUTE_SUPPORT` ligado, protege o bloco de código inteiro como
   `PAGE_EXECUTE_READ` — e **só depois** chama `exception_handler.SetFastmemCallback`. Esse caminho
   grava `except_info->ExceptionHandler`, e o `except_info` foi alocado *dentro* do bloco de código
   (`AllocateFromCodeSpace`). Num sistema de W^X, escrever ali é violação de acesso — e é por isso que
   o defeito não aparece no PC comum nem no build de bancada: **sem a definição ligada,
   `EnableWriting`/`DisableWriting` são vazios.**

**O conserto é o que o próprio dynarmic usa em volta de cada bloco emitido:** reabrir a escrita em
volta da chamada.

```cpp
    code.EnableWriting();
    exception_handler.SetFastmemCallback([this](u64 rip_) { return FastmemCallback(rip_); });
    code.DisableWriting();
```

**Medido depois do remendo:** a mesma abertura que morria passa a terminar com
`jogo nao abriu (indice 0, 332 ms): parou antes de comecar (Budget)` — o mesmo orçamento de 500 milhões
de instruções que o interpretador gastava **4.407 ms** para esgotar. E a bancada de CPU, no PC, mede o
JIT em **28,1× o interpretador** (2.398.867 instruções por milissegundo contra 85.295, num laço de
22 milhões).

**O remendo é aplicado na hora do build, e não copiado para dentro do repositório.** A crate traz o
dynarmic inteiro — xbyak, zydis, fmt, mcl —, dezenas de megabytes de C++ que não são nossos; uma cópia
deles aqui seria uma dívida que ninguém paga na próxima atualização. O `Conserta-Dynarmic` do
`compilar.ps1` troca o trecho **conferindo antes**: se a crate mudar de versão ou de texto, o build
pára com o motivo em vez de remendar outra coisa. E a limpeza que faz o remendo valer **não** é
`cargo clean -p dynarmic`: medido, aquele comando não toca na árvore do CMake, e o build seguinte
termina dizendo `Finished` com o objeto velho no lugar. O `Limpa-Dynarmic` apaga as três coisas que
obrigam — a árvore, o `fingerprint` e os artefatos da crate naquele alvo.

### Por que um arquivo de cadeia de ferramentas

O `cmake` do Cargo monta `-DCMAKE_CXX_FLAGS=...` a partir dos argumentos do `cc` e passa na linha de
comando, e isso vence tanto o `CXXFLAGS` do ambiente quanto o `CMAKE_CXX_FLAGS_INIT`. O arquivo de
cadeia de ferramentas é lido depois do cache e pode *acrescentar* ao valor que já existe: é o único
canal por onde este alvo consegue dizer ao C++ do dynarmic como ele deve ser compilado. Quem o
aponta é o `CMAKE_TOOLCHAIN_FILE`, que o `build.rs` da crate `cmake` respeita — e o `compilar.ps1`
o define, junto com o Ninja, no passo do núcleo.

**Medido:** compilados os **369 objetos** do C++ do dynarmic e ligado o `dynarmic.lib` para
`x86_64-uwp-windows-msvc`; as `FLAGS` do `build.ninja` carregam
`/DWINAPI_FAMILY=WINAPI_FAMILY_APP /DDYNARMIC_ENABLE_NO_EXECUTE_SUPPORT`, e os objetos referenciam
`__imp_VirtualAllocFromApp` e `__imp_VirtualProtectFromApp`. O núcleo `zeebx_xbox.lib` sai com os
dois núcleos dentro.

### A escolha é de execução, e não de compilação

**Um recompilador que não consegue alocar a página não falha ao compilar: falha ao executar**, no
meio de um jogo, e daí não se volta. A capacidade `codeGeneration` já está declarada no
`Package.appxmanifest`, mas declarar não é garantir — quem concede a página é o processo, e a
resposta pode ser diferente num aparelho e noutro, ou entre modos do console.

Por isso o alvo de Xbox **compila os dois núcleos** e escolhe em execução:

- `app\Jit.h` / `app\Jit.cpp` — a sonda. Ela percorre o caminho inteiro **em dois tamanhos**: o bloco
  de **16 MB**, que é o que o alocador do JIT pede de verdade (`PRELUDE_COMMIT_SIZE`, do próprio
  dynarmic), e **uma página**. Aloca como RW, escreve `mov eax, 42; ret`, confirma, troca a proteção
  para `PAGE_EXECUTE_READ`, chama e confere o resultado — devolvido 42. Além disso tenta **RWX de uma
  vez**, que é o caminho que o dynarmic usaria sem a definição de W^X. Alocar, confirmar, escrever,
  virar executável e executar são cinco decisões diferentes do sistema, e o relatório guarda cada uma
  com o código de erro do primeiro passo que falhou — "não deu" vale muito menos que "a troca para RX
  foi negada, erro 5".
- `App::Inicializar` roda a sonda no arranque e conta o resultado ao núcleo por
  `zeebx_xbox_permite_jit`. A resposta vale para todas as sessões: é do processo, e não do jogo.
- `src/cpu/escolhido.rs` lê essa resposta na abertura da sessão e escolhe entre o `dynarmic` e o
  interpretador. `Escolhido` repassa **todo** método do `CpuBackend` — um método esquecido ali não
  quebra nada, só devolve o emulador ao comportamento padrão do trait, que é correto e lento.
- O relatório tem duas linhas para isso, e as duas juntas é que fecham: a decisão
  (`nucleo do emulador: …`, no arranque) e a confirmação (`nucleo dynarmic`, na linha que diz em
  quanto tempo o jogo abriu). Uma sem a outra não distingue "a decisão não chegou" de "chegou e não
  valeu".

**Medido no PC, que também é um AppContainer, e é onde o porte se mede antes de ir ao console:** as
duas medidas passam — `reservar sim, confirmar sim, escrever sim, virar executavel sim, executar sim` —
e o **RWX de uma vez é recusado com erro 87**. É exatamente o retrato que a definição de W^X espera:
memória gravável *e* executável não se consegue de uma vez, e a política do AppContainer é a de dois
passos. **O que segue sem medição é o console**: se ele concede as mesmas duas páginas, quem diz é a
primeira execução no aparelho, e a linha do relatório que a anota é `nucleo do emulador: …`.

**A interface diz o que está rodando**, e a frase do `xbox.cpu.hint` acompanhou: ela deixou de
prometer o JIT "na próxima etapa" e passou a explicar que o núcleo é escolhido pelo aparelho.

### O `std` tier 3 e as bibliotecas que ele esconde

Aqui há uma surpresa que não é do Zeebx, e vale o parágrafo: **um `staticlib` do Rust não carrega
as bibliotecas nativas que o `std` usa.** O `cargo` as conhece e as passaria no link de um binário;
num `staticlib` não há link, e a responsabilidade passa para quem liga do outro lado. É exatamente
o que o `OTHER_LDFLAGS` do iOS faz com os frameworks, e sem alguém nomeando essas bibliotecas o link
morre com um `LNK2019` de símbolos que não têm nada a ver com o código escrito.

O que o `std` do Windows pede, e que este núcleo alcança de verdade:

| Símbolo | Quem o traz | Onde ele está |
|---|---|---|
| `NtSetInformationFile` | `std::fs::rename` | `ntdll.lib` |
| `NtCreateNamedPipeFile` | o cano do `std::process` | `ntdll.lib` |
| `QueryDosDeviceW` | o caminho das unidades (`sys/pal/windows/api.rs`) | `mincore.lib` |

Nem o `WindowsApp.lib` nem o `onecore.lib` exportam as duas primeiras: só o `ntdll.lib`, que é o que
o `ZeebxXbox.vcxproj` acrescenta ao lado do núcleo. **Medido:** sem o `ntdll.lib` o link acusa oito
símbolos não resolvidos; com ele e com a CRT dinâmica, zero.

O terceiro é de outra família e tem uma diferença que vale o parágrafo: **quem escolhe a biblioteca é
o próprio alvo do Rust.** O `x86_64-uwp-windows-msvc` passa `mincore.lib` nos `pre-link-args` — medido
com o JSON do alvo que o `rustc` imprime, cujo `pre-link-args.msvc` é
`["/NOLOGO", "/APPCONTAINER", "mincore.lib"]`. Quando quem liga é um binário do `cargo`, essa linha
acontece sozinha; quando é um `.vcxproj`, ninguém a repete, e o símbolo fica órfão:
`LNK2001: unresolved external symbol __imp_QueryDosDeviceW`, apontando para dentro do `std-*.rcgu.o`.
O `WindowsApp.lib` não exporta o `QueryDosDeviceW`; o `mincore.lib` exporta, e é o do alvo.

Sobre o `NtOpenFile` que este documento antes apontava como o risco número um: **ele não apareceu.**
O `remove_dir_all`, que é quem o usa, não é alcançado pelo que o frontend chama; quem é alcançado é
o `fs::rename`, e o símbolo que ele traz é o irmão `NtSetInformationFile`. A aposta chegou perto e o
remédio é o mesmo — a `ntdll.lib` cobre a família inteira.

**O link de verdade agora foi feito, e o que ele pediu foi só o `mincore.lib`.** O `msbuild` ligou o
`.vcxproj` inteiro — núcleo, C++/WinRT, XAML gerado — e produziu um `ZeebxXbox.exe` de 6,0 MB. A
previsão deste documento de que os `ntdll` bastavam estava certa quanto à família `Nt` e incompleta
quanto ao resto: faltava a biblioteca que o próprio alvo do Rust nomeia.

### O que a revisão do C++ achou

Estes só apareceram quando o código foi entregue a um compilador de verdade. Os dois primeiros
erram **em silêncio** — compilam, e o defeito só se vê na tela.

| Defeito | Sintoma | Conserto |
|---|---|---|
| `L"--release"` num comentário XML do `.vcxproj` | O MSBuild **nem carrega** o projeto (`--` é proibido em comentário de XML) | Reescrito sem os dois traços |
| Sem `/utf-8` | `"Não deu para abrir"` vira `"NÃ£o deu para abrir"` na tela, sem erro de compilação | `/utf-8` no `AdditionalOptions` |
| `std::max` com `<windows.h>` | `C2589: '(' illegal token on right side of '::'` — não aponta para a macro | `NOMINMAX` antes do `<windows.h>` |
| `CreateFileW` | `C3861: identifier not found` — o AppContainer recusa o recorte | `CreateFile2` |
| `namespace ZeebxXbox` ao lado de `struct ZeebxXbox` | `C2757: a symbol with this name already exists` | O namespace vira `ZeebxUwp` |
| `ZeebxXbox* m_app` dentro de `winrt::ZeebxXbox` | `C2143: syntax error: missing ';' before '*'` | `::ZeebxXbox*` |
| `<SubSystem>Console</SubSystem>` com `wWinMain` | `LNK2019: unresolved external symbol main` | `Windows` |
| `<LanguageStandard>stdcpp17</LanguageStandard>` | `STL1011`: o `<experimental/coroutine>` do C++/WinRT foi descontinuado | `stdcpp20` |
| `<PlatformToolset>v143</PlatformToolset>` | O VS de 2026 traz o `v145` e não instala o `v143` | Em branco, o toolset é o da instalação |
| `WindowsTargetPlatformVersion` fixo | Não abre em quem tem outro SDK | `10.0` = o mais novo instalado |
| `fxc` pelo `PATH` no CustomBuild | O MSBuild não põe o `bin` do SDK no `PATH` | Caminho completo pelo `$(WindowsSdkDir)` |
| Sem a CRT dinâmica | Oito `LNK2019` no `sqlite3.o`, que parecem do SQLite | `MultiThreadedDLL` |
| `..\include` fora do caminho de busca | `C1083: Cannot open include file: 'zeebx_xbox.h'` | `AdditionalIncludeDirectories` |
| `compilar.ps1` sem BOM UTF-8 | O Windows PowerShell 5.1 lê como ANSI e escreve `nA�cleo` | BOM UTF-8 |

A lição que fica: **o `XAML` foi o único pedaço que não deu para compilar sem a carga de trabalho**,
e é justamente onde estão o `MainPage.cpp` e o `App.cpp`. Tudo o que deu para compilar compilou, e
ainda assim havia onze defeitos — o que é o argumento para instalar a carga e compilar o resto antes
de tentar rodar.

### A segunda rodada: com a carga instalada, o `XAML` compila

Instalada a carga de trabalho de **UWP do C++**, o `msbuild` abre o `.vcxproj` e o XAML gerado
compila. O que apareceu no caminho não é defeito de C++: é o **contrato do compilador de XAML**, que
não está escrito em nenhum lugar que o build aponte.

O compilador de XAML escreve o conector da página em **C++/CX** ou em **C++/WinRT**, e a escolha
depende do projeto. No C++/WinRT ele emite, ao lado do `MainPage.h`, um `MainPage.xaml.g.h` com o
`MainPageT` e um acessor por `x:Name`; no C++/CX, um `ref class MainPage : ... IComponentConnector`
que o `cl` recusa sem `/ZW` — e que ainda ocupa o nome `MainPage.g.h`, de que o `cppwinrt` precisa.
Três coisas decidem, e as três moram no `.vcxproj`:

| O que | Por quê |
|---|---|
| Um `.idl` por página, declarando o `runtimeclass` | Sem ele a página não existe no metadado do projeto. É este `.idl` que faz o `XamlTypeInfo.g.cpp` conhecer `ZeebxXbox.MainPage` — e o `App.idl` existe só para o `cppwinrt` emitir a projeção do namespace. |
| `App.h`/`MainPage.h` e `App.cpp`/`MainPage.cpp`, **sem o `.xaml`** | É a convenção do C++/WinRT; o `MainPage.xaml.h` é nome do C++/CX. |
| `<DependentUpon>` em cada par `.xaml`/`.h`/`.cpp`/`.idl` | **É assim que o gerador do `XamlTypeInfo.g.cpp` acha o código-fonte de cada página**, e é o que produz o `#include "MainPage.h"` de que a tabela `TypeInfos[]` depende. Sem o metadado o `#include` não sai, e o build morre em `C2039: 'MainPage': is not a member of 'winrt::ZeebxXbox::implementation'` **dentro de um arquivo gerado**. |

Os defeitos desta rodada, todos medidos:

| Defeito | Sintoma | Conserto |
|---|---|---|
| Sem `.idl` de página | O conector sai em C++/CX, e o `cl` para em `C4430: missing type specifier` na linha 14 de um arquivo que ninguém escreveu | `App.idl` + `MainPage.idl` |
| Sem `<DependentUpon>` | `C2039` e `C2737` no `XamlTypeInfo.g.cpp` | O metadado, nos três grupos |
| `struct App : ApplicationT<App>` | `C2039`: quem traz o `IXamlMetadataProvider` é o `AppT` | `App : AppT<App>` |
| `wWinMain` escrito à mão | Briga com o que o compilador de XAML gera no `App.xaml.g.hpp` (Pass2) | Apagado: o ponto de entrada é gerado |
| O tratador de `UnhandledException` tomando `Application const&` | `C2664`: o remetente do evento é `IInspectable` | Assinatura corrigida |
| `pch.h` sem `winrt/Windows.UI.Xaml.Markup.h` | `C2079: ... uses undefined struct 'winrt::impl::produce<D,I>'`, um erro que cai dentro do `base.h` do SDK | O `include` (com o `Data.h` e o `Interop.h` ao lado) |
| `Raiz().Focus(...)` e `Pausa().Focus(...)` | `C2039: 'Focus': is not a member of Grid` — painel não recebe foco | O foco vai para a página, ou para um botão com `x:Name` |
| `SlotEscolhido() const` chamando `Slot()` | `C2663: no overloaded function has valid conversion for 'this' pointer` — os acessores do XAML não são `const` | Deixou de ser `const` |
| Sem `mincore.lib` | `LNK2001: __imp_QueryDosDeviceW` | `mincore.lib` |
| `Package.appxmanifest` sem `<mp:PhoneIdentity>` | `APPX1673: App manifest is missing required element 'PhoneIdentity'` — **depois** de o `.exe` estar pronto | O elemento, com um GUID qualquer |

**O `PhoneIdentity` é o mais estranho dos dez, e vale a nota.** Ele vem do Windows Phone 8.1 e não
diz nada sobre um emulador de Xbox. Mas a ferramenta de empacotamento o exige de **todo** aplicativo
UWP — e só dele: um pacote de desktop bridge passa sem, porque o `runFullTrust` o dispensa. Tirá-lo
não resolve: medido, sem a declaração `xmlns:mp` e sem o elemento, o erro é exatamente o mesmo, então
não é a declaração do namespace que o dispara. Os valores não são validados — a documentação da
Microsoft diz que qualquer GUID serve para o `PhoneProductId`, e o `PhonePublisherId` de zeros é o
que o modelo do VS traz.

Um último, que não é defeito e vale como nota de leitura: **o `MainPage.xaml.cpp` do C++/CX e o
`MainPage.cpp` do C++/WinRT não são o mesmo arquivo com outro nome.** O `wWinMain` que este projeto
tinha escrito à mão existia porque o modelo antigo o pedia; no C++/WinRT quem o traz é o
`App.xaml.g.hpp`, gerado. Escrever o ponto de entrada virou um defeito, e não um requisito.

## A varredura fora da linha da interface

**O sintoma era "o menu trava", e o defeito era a varredura dentro da chamada da ABI.** Numa estante
de bancada com 801 jogos — um `.mod` com `.mif` de 128×128 e oitocentos sem manifesto — a varredura
leva **2.406 ms**, medido e escrito no relatório. Rodando na linha da interface, isso é a janela sem
repintar por dois segundos e meio, e o jogador apertando de novo.

Duas funções novas, e um estado dentro do `Aplicativo`:

| Peça | O que faz |
|---|---|
| `zeebx_xbox_varre_inicia(app) -> i32` | Sobe uma `std::thread` que varre as fontes e deixa o `Vec<library::Game>` pronto numa `Colheita` (um `Mutex<Option<...>>`). Devolve `0` se começou, `-1` se já havia uma andando |
| `zeebx_xbox_varre_colhe(app) -> i32` | `1` pronto (e **instala** a lista: troca os jogos, zera as imagens e a marca de quadro), `0` ainda andando, `-1` nada pedido |
| `zeebx_xbox_jogos(app) -> i32` | Quantos jogos há agora, **sem varrer**. Existe porque o `varre_colhe` já usa o `1` para "pronto" |

**A regra da casa vale aqui: todas as chamadas da ABI ficam na linha da interface.** O fio de dentro
só monta o `Vec<library::Game>`; quem troca a lista, quem conta e quem lê os títulos é a interface. Um
deles só — o `jogos` — precisa saber disso, e por isso ele responde sem tocar em disco.

O C++ pergunta pelo `varre_colhe` a cada **120 ms**, num `DispatcherTimer` que só existe enquanto há
varredura. O intervalo é uma medida de compromisso: uma pasta pequena termina antes disso, e perguntar
mais rápido gastaria mais do que a varredura. A camada de "procurando" (`ProgressRing` mais o texto de
`library.scanning`) fica de pé enquanto o relógio pergunta.

**A primeira varredura é a do `ReataFontes`.** Ela espera as pastas do `FutureAccessList` voltarem a
valer; varrer antes seria varrer sem enxergar a unidade de fora. Sem isso, o HD externo só apareceria
na segunda varredura — o defeito que o relatório do console ia mostrar como "os jogos do HD não
aparecem até apertar varrer de novo".

**O B e o A ficam com a interface durante o trabalho.** `TrataVoltar` devolve `true` enquanto
`m_varrendo` (não há para onde voltar com a lista trocando embaixo, e um `false` deixaria o sistema
fechar o aplicativo no meio da leitura) e o `AbreJogo` recusa. A camada de recado **esconde as telas**
em vez de desligá-las: `IsEnabled` é do `Control`, e um `Grid` não é `Control` — com as camadas
colapsadas, o direcional não tem por onde andar. Qual delas volta fica guardado, então quem estava em
Fontes volta para Fontes.

**Abrir jogo continua travando a linha, e é de propósito.** A `Session` não é `Send`, e movê-la para
outro fio pediria mudar o núcleo. O que mudou é que a chamada agora acontece **com o recado já
pintado**: o `AbreJogo` virou uma co-rotina (`fire_and_forget`) que mostra a camada, dá uma volta ao
despachante e só então chama o `zeebx_xbox_comeca` — e escreve no relatório quanto tempo levou
(`jogo abriu (indice N) em X ms`). O número é o que separa "o emulador está lento" de "o carregamento
é lento", duas coisas que na tela se parecem.

## O quadro que o jogo não desenhou

O Zeebo desenha a 30 quadros por segundo dentro de uma composição de 60. Nas voltas em que o jogo não
desenhou, o núcleo publicava **o mesmo quadro** — e o frontend convertia 1,2 MB de RGB565 para BGRA8 e
chamava o `Present` de novo, por nada.

O detector é do lado do núcleo, no `publica()`, e compara quatro números: a **série** do framebuffer
(que muda a cada `flush`), as **escritas** (o contador de pixels tocados), a **largura** e a **altura**.
`publica` passou a devolver `bool`, e o `passo` devolve

    #define ZEEBX_SEM_QUADRO_NOVO 4

— um código **acrescentado ao fim** da lista, para não mexer no valor de nenhum existente. O C++ trata
esse código como "volte ao topo do laço": não chama o `quadro`, não apresenta, e **continua chamando o
`audio`** (o som não pode esperar por desenho).

A `marca` fica no `Aplicativo` e é zerada em tudo o que muda a tela por fora: `comeca`,
`carrega_estado` e `para`. Sem isso, um save state carregado na mesma série de framebuffer seria
tratado como "quadro velho" e a tela ficaria com o quadro anterior.

Duas economias vieram junto, pelo mesmo motivo (o custo por volta):

- o buffer de BGRA é **reusado** — `bgra_de_rgb565_para(&mut Vec<u8>, &[u16])` escreve num `Vec` que
  já existe, em vez de alocar 1,2 MB por quadro. A `bgra_de_rgb565` antiga continua existindo, só para
  os testes, que querem um vetor pronto;
- o `Input` guarda a `IVectorView<Gamepad>` entre quadros e só a refaz quando ela volta vazia (um
  controle conectado no meio do jogo), com um `catch (hresult_error)` que limpa o cache quando o
  `GetCurrentReading` falha.

**O ganho não foi medido.** Sem ROM nesta máquina o laço não roda até o fim: a detecção de quadro novo
tem teste de unidade, e a apresentação foi medida com um quadro sintético, mas "quantos quadros por
segundo a mais" é medida de console.

## A arte dos cartões

`zeebx_xbox_imagem(app, indice, &largura, &altura) -> *const u8` entrega os pixels crus em **BGRA8
premultiplicado**, e o C++ os transforma em `SoftwareBitmapSource`. **O trabalho já estava pago pela
varredura**: quem lê o `.mif` é o `library::scan`, que já guarda a `Image` decodificada em `Game.art`.
A conversão para BGRA é feita **uma vez por jogo**, na primeira vez que o cartão aparece, e fica
guardada em `imagens: Vec<Option<Imagem>>` (`Imagem { largura, altura, bgra }`).

Por que premultiplicado: é o único formato com alfa que o `SoftwareBitmapSource` aceita junto de
`BitmapAlphaMode::Premultiplied`, e o alfa existe (há ícone com transparência). A conversão tem teste
próprio — um ícone de alfa 128 sai com o canal multiplicado pelo alfa, e não com os valores originais.

No C++, os pixels viram `IBuffer` pelo `DataWriter::WriteBytes(array_view)` mais `DetachBuffer()`,
porque `SoftwareBitmap::CopyFromBuffer` não aceita memória crua.

**O gancho é o `ContainerContentChanging` da lista, e não o `Loaded` do `Image`.** Dentro de um
`DataTemplate` o `{Binding}` traz o título e mais nada, e um nome de dentro do modelo não é alcançável
do código. O evento do contêiner é o único ponto em que se sabe, ao mesmo tempo:

- **qual item** está ali — o `ItemIndex` do argumento, sem precisar resolver o índice pela árvore;
- **que o contêiner acabou de nascer** — e ele volta a ser levantado a cada rolagem, no contêiner
  reciclado, que é o que faz o cartão seguinte mostrar a arte dele.

Como o `SetBitmapAsync` é assíncrono, o índice é conferido **de novo depois da espera**
(`IndiceDoCartao(Jogos(), conteiner) != indice` → desiste). Sem essa conferência, a arte de um jogo
apareceria no cartão de outro — o defeito clássico de lista virtualizada.

**O tamanho é a limitação honesta.** Os `.mif` de fábrica trazem de **16×16 a 65×42**, com um 192×192
no melhor caso: é ícone de menu do BREW, não arte de caixa. Num cartão de 300 DIPs um ícone de 32×32
aparece borrado, e é por isso que a faixa do cartão tem degradê próprio — o fundo é o que faz o ícone
parecer posto ali de propósito. **A saída para capa de verdade é um PNG ao lado do jogo**, com o mesmo
nome do `.mod`: o `cover()` do núcleo é tentado **antes** do `manifest_art()`, e o PNG não tem o limite
de tamanho do `.mif`.

**Medido no PC**: com um `.mif` de bancada (um PNG de 128×128 dentro do formato), a estante de 801
jogos mostra a arte em todos os cartões, e o `Stretch="Uniform"` a apresenta suave. Cartão sem arte
(um `.mod` cujo `.mif` não traz imagem) sai como faixa e título, sem buraco.

## A margem segura da TV, e o menu de pausa

Duas causas, medidas separadas:

1. **A caixa da pausa era estreita demais para o próprio conteúdo.** O menu era um `StackPanel` de
   `Width="360"`, e a linha dos saves — o `ComboBox` de slot mais dois botões — pedia mais que isso: o
   "Carregar" saía pela borda. A caixa agora tem `MinWidth` 380, `MaxWidth` 560, `MaxHeight` 620, um
   `ScrollViewer` por dentro, e a linha dos saves virou um `Grid` de três colunas (`Auto`/`*`/`*`) com
   os dois botões em `MinWidth` 96: o seletor mede o que precisa e os botões repartem o resto.
2. **O console come a borda da tela** (overscan), e o que estava a 20–28 DIPs dela — a linha das
   dicas, os contadores, o pé das telas — caía exatamente nessa faixa.

A margem é calculada no código e aplicada como `Margin`:

    recuo = clamp(min(largura, altura) * 0.05, 12, 48)   // DIPs

Ela vai em todas as **camadas de tela cheia** e na caixa da pausa, e **não** vai em dois lugares, cada
um por um motivo diferente:

- no **quadro do jogo**: ele é a tela inteira do console, e recuá-lo seria uma moldura preta em volta
  do que o jogo desenhou;
- na **cortina da pausa**: ela escurece a tela toda; com margem, sobraria uma moldura clara em volta
  da parte escurecida.

Medido neste PC (raiz de 1280×664 DIP, escala 1,5): o recuo sai **33 DIP** — 5% de 664 —, e o título
da estante, que o XAML põe a 48 DIPs da borda, aparece a 81. O menu de pausa foi fotografado com a
caixa dentro do seguro: slot, Salvar e Carregar lado a lado, nada cortado. **O tamanho real da TV do
console é outro**, e é a linha `video: raiz ... dip` do relatório que o diz.

## O que o porte mexe fora do frontend

Um frontend novo costuma caber em `frontends/`. Este não cabe, e vale registrar exatamente o que
ele toca — são **poucos arquivos**, e a maioria é núcleo:

| Arquivo | Mudança | Por quê |
|---|---|---|
| `Cargo.toml` (raiz) | `frontends/xbox-uwp` entra em `members`; o `dynarmic` deixa de ser excluído por `target_vendor = "uwp"` | Ele voltou ao alvo quando o caminho de memória executável apareceu — ver "A memória executável" |
| `src/cpu/mod.rs` | o módulo `dynarmic` entra no `uwp`; entra o `escolhido`; entram o interruptor `permite_jit`/`jit_permitido` e o `nome()` do trait | É o que deixa o alvo compilar os dois núcleos e escolher em execução |
| `src/cpu/escolhido.rs` | arquivo novo: o `Escolhido` repassa **todo** o [`CpuBackend`] para o `dynarmic` ou para o interpretador | É onde a resposta da sonda vira núcleo |
| `src/cpu/dynarmic.rs`, `src/cpu/interpretador.rs` | cada um responde ao `nome()` — `"dynarmic"` e `"interpretador"` | O relatório precisa dizer qual dos dois está rodando |
| `src/session.rs` | o `use` do backend em **três** `#[cfg]` (`uwp` pede o `Escolhido`) e um método público `nucleo()`; a linha de INFO da sessão passa a nomear o núcleo | O `use` acompanha o `cpu::mod`, e o nome é o que o frontend lê pela ABI |
| `frontends/xbox-uwp/*` | a ABI ganha `zeebx_xbox_permite_jit` e `zeebx_xbox_nucleo`; a sonda `Jit.h`/`Jit.cpp`; o `appcontainer.cmake`; o `#[link]` com `+whole-archive` no `src/lib.rs`; o relator de falhas do `App.cpp` | É o lado do aplicativo da mesma decisão |
| `assets/lang/*.json` | a frase do `xbox.cpu.hint` nos quatro idiomas | Ela prometia o JIT "para a próxima etapa"; agora diz quem escolhe |
| `src/ui/settings.rs` | o campo `roms_dirs: Vec<PathBuf>`, ao lado do `roms_dir` | É onde a lista de fontes de fora é gravada (ver "As fontes de jogos de fora") |
| `src/ui/settings.rs`, `src/ui/navegacao.rs` | um literal de teste em cada um | O `cargo test -p zeebx` não compilava: o `Settings` do teste ficou sem o campo novo, e o teste da navegação passava **seis** posições para um `comandos` que já pedia oito (defeito anterior, que só apareceu ao rodar a suíte inteira) |
| `src/audio/mod.rs` | os três contadores da placa passam a existir só com a feature `audio` | Num alvo sem ela — o núcleo do Xbox — eles ficavam sem uso e o aviso de código morto saía em toda compilação, no meio dos avisos que importam |

**As condições dos `#[cfg]` têm de andar juntas com a do `Cargo.toml`.** Se o `Cargo.toml` tira o
`dynarmic` de um alvo e um `#[cfg]` do `cpu::mod` não tira, o erro é `cannot find module or crate
'dynarmic'` — e só no alvo esquecido, porque no desktop a crate existe. Foi assim que o primeiro
`cargo check` para `uwp` falhou: a dependência já estava fora e o `pub mod dynarmic;` ainda estava
lá. Não é um erro que o CI pegue sozinho; por isso a condição está escrita por extenso em cada
lugar, e não escondida atrás de um `cfg` próprio.

O discriminador é `target_vendor`, e não `target_os`: no alvo `x86_64-uwp-windows-msvc` o
`target_os` continua sendo `windows` — quem muda é o *vendor*, que passa a ser `uwp`. Usar
`target_os` teria desligado o JIT no frontend de desktop do Windows também.

## A assinatura: o que ela resolve, e o que não resolve

O pacote saía **sem assinatura** (`/p:AppxPackageSigningEnabled=false`), e um `.msix` sem assinatura
não instala em lugar nenhum: o Windows devolve `0x800B0100` ("no signature was present in the
subject"). Era por isso que o caminho do PC era só o layout solto — e por isso o `-Pacote` passou a
assinar.

O certificado é **autoassinado**, e isso não é problema: o que o Windows confere é que o `Subject`
do certificado seja **igual** ao `Publisher` do manifesto — `CN=ZeebxTeam` —, e não que ele venha de
uma autoridade. Ele é criado na primeira vez (assinatura de código, sem restrição básica), fica no
repositório **do usuário** e sai exportado em `.cer` ao lado do pacote, com o mesmo nome: é onde o
`Add-AppDevPackage.ps1` o procura.

**E assinar não basta para o PC.** Medido, em ordem: com o certificado em `CurrentUser\TrustedPeople`
o `Add-AppxPackage` falha com `0x800B0109` ("terminated in a root certificate which is not trusted by
the trust provider"); pôr o certificado em `CurrentUser\Root` nem é permitido, e a resposta é
`UI is not allowed in this operation`. A razão é que quem instala pacote é o `AppXSvc`, um serviço —
serviço não tem repositório de usuário, e a confiança que ele consulta é a da **máquina**. Instalar o
`.msix` neste PC pede um passo elevado, uma vez (`LocalMachine\TrustedPeople` e `LocalMachine\Root`),
que é exatamente o que o `Add-AppDevPackage.ps1` faz; o `-Registrar` continua sendo o caminho sem
elevador, e é o de quem está mexendo no código.

**O console é outro repositório.** O certificado deste PC não vale lá: quem tem de confiar nele é o
**console**, e é o que a implantação pelo **Visual Studio** faz — ela instala o aplicativo e o
certificado. Se o Portal do Dispositivo recusar o pacote citando o certificado, é este o caminho.

Nada disto foi exercitado no console: está na lista abaixo.

## O estado real, sem arredondar

Isto já passou por um compilador, por um ligador, por um empacotador **e agora abre no PC** em modo de
desenvolvimento. "Compila", "liga" e "roda" continuam sendo três coisas diferentes — e a quarta,
"roda um jogo", ainda não aconteceu: não há ROM nenhuma nesta máquina para carregar.

**Verificado, com a cadeia de verdade:**

- **O núcleo compila para o alvo.** `cargo +nightly build --release -p zeebx-xbox --target
  x86_64-uwp-windows-msvc -Z build-std=std,panic_abort` termina em `Finished`, e sai um
  `zeebx_xbox.lib` de **159.981.164 bytes** (152,6 MB) com as **39** funções da ABI. A árvore de
  dependências inteira compila no alvo — inclusive o SQLite embutido do `rusqlite`, que é C e passa
  pelo `cl`. Era a dúvida que mais podia dar errado, e não deu.
- **O objeto do dynarmic chega ao `.lib` de verdade.** O `rustc` só copia para um `staticlib` as
  bibliotecas nativas marcadas com `+whole-archive`; as quatro do dynarmic — ele, o `fmt`, o `mcl` e
  o `Zydis` — são declaradas assim no `src/lib.rs` do frontend, e é por isso que o `.lib` saltou de
  60.992.066 para **159.981.044 bytes**: medido antes e depois do `+whole-archive`. Sem isso o JIT
  fica compilado e não chega ao aplicativo, e o sintoma é um `LNK2019` de símbolos do dynarmic que
  parece erro de ligação nosso.
- **Os testes passam nos dois lados.** `cargo test -p zeebx-xbox` são **15** — entre eles a arte sair
  do `.mif`, o alfa do ícone sair premultiplicado e a varredura de fundo voltar na hora e entregar a
  lista —, e `cargo test -p zeebx --lib` são **669**, com 7 ignorados. Um dos ignorados é a medida do
  JIT contra o interpretador, e é assim de propósito: é uma medida, não um teste, e não pode custar
  minutos em toda execução da suíte. (A suíte do núcleo **não compilava** numa etapa anterior, com dois
  literais de teste desatualizados; voltou a compilar, e a passar.)
- **A ABI bate nos dois lados, nome a nome.** As **39** funções do `src/lib.rs` e as **39** do
  `zeebx_xbox.h` são o mesmo conjunto — conferido nome a nome, sem faltar nem sobrar. Que o `.lib`
  também as traga não é conferência de lista: é o **link do aplicativo**, que chama as que precisa e
  termina com código 0. (Uma delas, `zeebx_xbox_caminho`, é exportada e ainda não é chamada por
  ninguém.)
- **O dynarmic sai só no `uwp`.** Confirmado por `cargo tree`: ausente no `x86_64-uwp-windows-msvc`,
  presente no host, no libretro, no headless e no android.
- **O aplicativo inteiro compila, liga e empacota.** O `msbuild` do `ZeebxXbox.vcxproj` termina com
  código 0 e sem um aviso: saem o `ZeebxXbox.exe` (**11.126.272 bytes**) e o pacote de **4.248.010
  bytes** (`0.4.6.0`). O `MainPage.cpp` e o `App.cpp` — os dois que dependem do XAML gerado —
  compilam pela primeira vez, e o XAML gerado é C++/WinRT: um `MainPage.xaml.g.h` com o `MainPageT`
  e um acessor por `x:Name`, e zero `ref class`.
- **O JIT é escolhido, e roda.** No PC, que também é um AppContainer, a sonda concede as duas medidas
  e o núcleo da sessão é o `dynarmic`. Medida de ponta a ponta: a abertura de bancada — carregar o
  módulo e esgotar os 500 milhões de instruções do orçamento, porque o `.mod` plantado não chega a
  criar um *applet* — terminou em **332 ms** com o JIT, e em **603 ms** numa segunda medida (a mesma
  máquina, com carga), contra os **4.407 ms** que o interpretador gastava no mesmo trabalho. O
  relatório registra as duas metades da escolha: `nucleo do emulador: dynarmic (JIT), memoria
  executavel concedida` no arranque, e o `nucleo dynarmic` da linha da abertura. O mesmo laço, na
  bancada de CPU, mede o JIT em **28,1× o interpretador**, e é uma medida de teste
  (`#[ignore]`), como manda a casa — ver [*O JIT entrava…*](#o-jit-entrava-e-derrubava-o-aplicativo-um-defeito-do-dynarmic-sob-wx).
  **O que isto não mede é quadro por quadro**: sem ROM, o que os 332 ms medem é o caminho do núcleo, e
  não o desenho.
- **O pacote assinado contém o que o binário é.** Medido no `.msix` de 0.4.6.0: o manifesto embutido
  diz `Version="0.4.6.0"`, as quatro capacidades estão lá (`expandedResources`, `codeGeneration`,
  `internetClient` e o `DeviceCapability` `removableStorage`, **por último**), o conteúdo traz
  `quad.vs.cso`, `quad.ps.cso` e `ZeebxXbox.exe`, e o **SHA-256 do `.exe` de dentro do pacote é o
  mesmo** do `.exe` compilado (`9F8D2C8775D3B92B…`) — a assinatura não é de outro binário. **O que
  fica de fora também foi conferido**: o `.map` e o `.pdb` de 110 MB aparecem na pasta de saída ao
  lado do `.exe` e **não** entram no pacote, porque o empacotador segue o `.appxrecipe` e não a pasta
  inteira. É por isso que o mapa de símbolos pode ficar ligado no ligador sem engordar o que o
  console instala.
- **A estante com arte foi medida no PC, com uma bancada de 801 jogos.** A captura da janela mostra o
  ícone do `.mif` em todos os cartões visíveis, o cartão sem arte sair como faixa mais título, a
  contagem certa no subtítulo e o recuo seguro no lugar. O relatório fecha a medida:
  `varredura de fora: 801 jogo(s) em 2406 ms`. Ver "A varredura fora da linha da interface" e "A arte
  dos cartões".
- **O menu de pausa foi fotografado, e cabe.** Com a caixa dentro do recuo seguro, o seletor de slot,
  o Salvar e o Carregar aparecem lado a lado e nada sai pela borda. A foto exigiu um instrumento
  temporário (não há ROM para chegar à pausa pelo jogo), e ele **saiu do código** depois da captura:
  uma linha `Pausa().Visibility(Visible)` no fim do `MontaEstante`, conferida por busca no diretório
  antes de a etapa fechar.
- **O `compilar.ps1` monta tudo numa máquina limpa.** Ele acha o `.nupkg` do C++/WinRT na pasta do
  Visual Studio, desempacota em `packages\` e só então chama o `msbuild` — medido com o diretório
  apagado antes.
- **O `Cargo.lock` da raiz já conhece o membro novo**, como o `AGENTS.md` pede.
- **O aplicativo abre, e as quatro camadas foram percorridas.** Com o pacote registrado no PC
  (`compilar.ps1 -App -Registrar`), a estante abre no estado vazio, e dali se chega a **Fontes de
  jogos**, **Configurações** e **Sobre**. Os rótulos vieram todos do catálogo — conferido lendo os
  **nomes acessíveis** pela Automação de Interface, que é a mesma lista que o Narrador lê: "Scan
  again", "Game sources", "Whole pixels", "Save states" são o `en.json`, e não texto escrito no C++.
- **O A abre jogo, e as duas rotas foram medidas.** Quem o A aciona é quem tem o foco, e num cartão da
  estante pode ser o `Button` de dentro dele (`Click`) ou o contêiner da lista (`ItemClick`): os dois
  caminhos existem, os dois resolvem o índice pelo caminho exato — `IndexFromContainer` subindo a
  árvore, ou `ContainerFromItem` descendo —, e os dois chegam ao mesmo `AbreJogo`, que ignora a
  segunda chamada. Medido pela Automação de Interface, que aciona o mesmo caminho que o botão do
  console: acionado o cartão, o núcleo tentou carregar o jogo e recusou o arquivo de teste ("parou
  antes de começar", `Exception { pc: 65588 }`), que é a prova de que o pedido chegou ao núcleo. A
  segunda medição foi por tecla, com o foco no **contêiner**: um espaço — o que o A manda — chegou ao
  mesmo lugar, e o relatório registrou `jogo nao abriu (indice 0)`. **Antes disto, o direcional andava
  pela estante, o cartão se destacava, e o A não abria nada**: o `ItemClick` que estava ligado só
  chega pelo ponteiro, e o `Button` do cartão estava sem `Click`.
- **O foco ao abrir a estante vai para o cartão, e o caminho até lá foi medido.** `Jogos().Focus()`
  **não** para no cartão: medido no PC, o foco escorregou para o botão seguinte da ordem de tabulação
  ("Varrer de novo"), porque os contêineres da lista ainda não existiam quando a página carregou. O
  foco passou a ser dado ao primeiro controle focável **dentro** do primeiro contêiner, depois de um
  `UpdateLayout()` que força a passagem de layout — e a medição seguinte mostra o foco no
  `GridViewItem` do cartão. (`FocusManager::FindFirstFocusableElement` não serve: ele devolve a
  própria lista, que é quem recusa o foco.)
- **A caixa de pasta do sistema abre no PC, e o que ela devolve chega ao núcleo.** Medido: a caixa
  abriu (uma janela `#32770`, "Select Folder"), o jogador escolheu `C:\Users\janmi\Downloads`, a
  permissão foi guardada, e a ABI recusou o caminho com `-1` pela razão do sandbox acima. O caminho
  inteiro do seletor foi percorrido, e parou onde tinha de parar neste PC. O código que a lista
  devolve quando o jogador cancela — `nada` — também é anotado: "cancelou" e "a caixa não abriu de
  verdade" são a mesma tela e o mesmo silêncio, e é a linha do relatório que separa os dois.
- **A permissão é gravada, lida de volta, e limpa quando não vale mais.** Medido pelo lado do erro:
  plantado um token inválido em `LocalState\fontes.txt`, o arranque seguinte registrou
  `permissao perdida (nao-e-um-token-valido): The parameter is incorrect.` — a mensagem é do WinRT, o
  que prova que a chamada chegou ao sistema — e reescreveu o arquivo sem o token morto. **Não
  medido:** o lado do acerto, um token de verdade reabrindo a pasta, que pede uma escolha de pasta que
  este PC recusa mais adiante.
- **O modo de ponteiro.** `RequiresPointerMode(WhenRequested)` passa no PC — o relatório sai com
  `modo de entrada: foco (RequiresPointerMode = WhenRequested)` —, e a recusa é registrada com a
  mensagem e o código. **Se o console honra o pedido não foi medido**: um sistema que aceita a
  propriedade sem usá-la é indistinguível, daqui, de um que a usa. É a primeira linha a conferir no
  relatório de lá.
- **O relatório do arranque existe, e é lido de dentro do `LocalState`.** `zeebx-xbox.log`, reescrito a
  cada arranque, com o modo de entrada, a sonda da memória executável passo a passo, cada pasta
  devolvida pelas caixas do sistema, cada permissão guardada ou perdida, o código da ABI em cada
  tentativa, os jogos que o núcleo recusou e as exceções do XAML. Depois do primeiro teste em console
  ele ganhou as três linhas do quadro — o tamanho da raiz, a escala de composição do painel e a cadeia
  de troca em DIPs; o tamanho do primeiro quadro que o núcleo publicou; e a primeira vez que o jogo
  desenhou algo — e uma linha por decisão sobre o B. Num console não há depurador na TV, e é este o
  instrumento que sobra: "não tem imagem" e "o jogo não desenhou" são a mesma tela preta, e são as
  linhas do relatório que separam as duas.
- **A falha que mata o processo também sai no relatório, com o número que o mapa de símbolos lê.** Um
  `SetUnhandledExceptionFilter` e um `std::set_terminate` escrevem a linha `falha: …` antes de o
  sistema derrubar o aplicativo: o código da exceção, **se o acesso foi de leitura ou de escrita**, o
  endereço alvo e o endereço da falha **relativo ao próprio executável**. Medido, provocando a falha
  de propósito: `falha: violacao de acesso (c0000005): escrita no endereco 0x10, codigo em
  0x7ff7c033860a, +0x31860a no executavel`. O relatório de falha do Windows traz só o deslocamento
  (`P9`), e foi atrás de um deles que uma tarde se foi — é o que dá nome à função culpada, com o
  `.map` ao lado (ver [*O JIT entrava…*](#o-jit-entrava-e-derrubava-o-aplicativo-um-defeito-do-dynarmic-sob-wx)).
  Escrever ali é deliberadamente pobre: `CreateFile2` e `WriteFile` sobre o caminho já guardado, sem
  alocar e sem exceções, porque o processo que entra naquele caminho já morreu.
- **A carga de recursos do `App.xaml` acontece.** Era a maior aposta que restava — o
  `AppT::InitializeComponent()` é vazio, e quem carrega o dicionário é o próprio XAML. Os títulos
  saem no estilo `Titulo`, os botões e interruptores saem pintados e os cartões usam o
  `CartaoDeJogo`: se o dicionário não carregasse, a tela sairia sem estilo nenhum e nenhum passo do
  build avisaria.
- **A janela se mede, e o XAML concorda com ela.** A dúvida que atravessou duas sessões — um painel
  centralizado que "aparecia deslocado" — era **instrumento, e não defeito**. Numa tela de 1920×1080 a
  150%, um processo que não declara DPI recebe as medidas da tela já divididas pela escala (ele jura
  que a tela mede 1280×720) e tira a captura do retângulo errado, que é a fatia do canto superior
  esquerdo. Medido **por dentro** do aplicativo, a raiz mede **1280×664 DIP** com
  `RasterizationScale()` de **1,5**, e o painel do estado vazio fica em x = **343 DIP** — centrado,
  como devia. A captura passou a sair inteira quando o processo que fotografa declarou DPI.

**Não verificado, e por quê:**

- **Nenhum jogo de verdade rodou neste PC.** Os `.mod` que existem aqui são de **bancada** — arquivos
  de mentira, plantados para a estante ter tamanho (801 jogos, um com `.mif` de verdade e oitocentos
  sem manifesto). Eles exercitam a contagem, o título e a arte, e **não** o carregador, que recusa o
  conteúdo desses arquivos na hora de abrir. O som (XAudio2), o mapa do controle, os save states e o
  carregamento continuam por exercitar aqui, e **nenhum quadro de jogo foi visto**. O caminho do
  desenho, esse, deixou de estar nessa lista: ele foi exercitado com um quadro sintético (ver
  [*O laço*](#o-laço)).
- **O ganho do quadro pulado não foi medido**, e não é um detalhe: sem jogo, o laço de quadro não roda
  até o fim. O que existe é a medida do que saiu do caminho (uma conversão de 1,2 MB e um `Present` por
  volta em que nada foi desenhado) e o teste de unidade do detector; "quantos quadros por segundo a
  mais" é pergunta de console.
- **A varredura foi medida com uma pasta local, e não com uma unidade removível.** Os 2.406 ms são de
  801 arquivos no disco do sistema. Um HD externo é outra ordem de grandeza, e é lá que a camada de
  "procurando" e o B bloqueado mostram se valem o que custam.
- **O menu de pausa foi medido com um instrumento, e não pelo jogo.** Não há ROM para chegar nele, então
  a caixa foi fotografada com a pausa forçada visível na estante — o que prova o layout, e não o
  caminho. **O corte que o jogador viu era na TV do console**: o recuo seguro é calculado do tamanho
  medido da raiz, e a linha `video: raiz ... dip` do relatório de lá é o número que confirma a margem
  certa.
- **Nenhuma unidade removível foi lida.** Não há pendrive nem HD externo ligados: a tela de fontes foi
  vista no estado "nenhuma unidade", que é o único que um PC vazio oferece. A caixa de permissão do
  `RemovableDevices` e a `fileTypeAssociation` valendo como lista de extensões são comportamento de
  console, e só o console os prova.
- **O primeiro teste em console aconteceu, e ele é o que trouxe a terceira rodada de consertos.** Quem
  usa instalou o pacote num Series S em Modo de Desenvolvedor — a instalação passou, e por qual das
  duas rotas (Portal do Dispositivo ou implantação pelo Visual Studio) não está registrado — e o que
  voltou foram **quatro fatos**: o som toca, **um jogo de verdade roda** (nada disto tinha rodado
  antes, nem aqui nem lá), a imagem não aparece, e o B fecha o aplicativo, com o jogo rodando
  inclusive. Os dois últimos têm conserto nesta etapa: o B porque o aplicativo **não consumia** o
  `BackRequested` do sistema — o que é leitura do código, e não medição de console —, e a imagem porque
  **o desenho nunca acontecia**: o alvo de saída chegava nulo ao D3D e faltava o layout de entrada, os
  dois em silêncio (ver [*O laço*](#o-laço)). A tela preta do console era o `Clear`, e não um erro de
  tamanho: a hipótese da escala de composição, que era a da rodada anterior, **estava errada**.
- **A correção do B foi medida no PC, e a da imagem também — com um quadro sintético.** O B foi medido
  pelo teclado (o `Esc` é o mesmo B): na estante ele fica com o sistema, no explorador ele sobe um nível
  antes de sair, e com o jogo rodando ele não chega ao sistema. A imagem foi medida com um padrão de
  320×240 apresentado pela mesma função que apresenta o jogo, e os três números fecham: a tarja
  esquerda em 306 (esperado 307), a cruz no centro em 970 (971) e a tarja direita em 1635 (1635). **O
  que continua sem medição é o quadro de um jogo de verdade**, que é o que o relatório do console traz
  na linha `primeira imagem` — e é ela que diz se o núcleo publica pixels.
- **O modo de ponteiro, no console, segue sem medição direta**: o relato do primeiro teste não
  mencionou cursor, o que é indício e não medida de que o `RequiresPointerMode` foi honrado. A linha
  `modo de entrada` do relatório é o que fecha a dúvida.
- O `release.yml` ainda não monta este alvo. Como ele é o único gatilho automático e os workflows de
  alvo são `workflow_dispatch`, vale disparar um build antes de qualquer tag.

**O caminho, em ordem:**

1. **A tentativa no console com o `0.4.6.0`** — é esta que mede a engasgada, e é o único lugar onde
   ela se mede. O relatório do console diz, na primeira linha que interessa, se o aparelho entrou no
   caminho rápido: `nucleo do emulador: dynarmic (JIT), memoria executavel concedida`. Se em vez dela
   vier o interpretador, o que o console recusou está escrito logo acima, passo a passo — e o próximo
   passo passa a ser a política de memória do aparelho, e não o dynarmic. As outras linhas que a
   rodada nova pede são as de sempre: `jogo abriu (indice N) em X ms` (o carregamento, que agora é
   JIT) e `primeira imagem`, que diz se o núcleo publicou pixels.
2. **Uma ROM e uma unidade removível**, que é o que fecha o resto de uma vez: o controle, os save
   states, a escolha de pasta e a `fileTypeAssociation`. É também com um jogo rodando que se mede o
   ganho do quadro pulado e se vê a pausa pelo caminho de verdade.
3. **A taxa, que o relatório não mede.** Ele conta o custo do carregamento e o tamanho do quadro, não
   quadros por segundo. Se a engasgada continuar com o JIT ligado e confirmado no relatório, o que
   separa as duas causas prováveis — emulação e desenho — é medir cada uma: o `zeebx_xbox_quadros` já
   conta os quadros que o núcleo publicou, e comparar esse número com o que a tela mostrou diz qual
   das duas está atrás.
