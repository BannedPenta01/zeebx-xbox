# Xbox 0.4.6.0

> O número aqui é o do **aplicativo de Xbox**, e não o do emulador: o núcleo é o **0.4.2** da
> ZeebxTeam, com este frontend por cima. É a primeira versão publicada neste repositório.

O Zeebx passa a rodar num **Xbox Series S ou X em Modo de Desenvolvedor**. O aplicativo é um menu de
console: quem anda de controle em controle é o **foco**, e não um cursor — o direcional caminha, o A
aciona o que está em foco, o View abre a pausa — e o jogo roda em tela cheia. Sai como um `.msix`
assinado, com o certificado ao lado para quem for instalar.

Novidades:

- **O frontend de Xbox**, em UWP com XAML: o núcleo em Rust vira um `.lib` estático e um aplicativo
  em C++/WinRT põe a janela, o som e o controle. O vídeo sai por um **rasterizador de software**, de
  RGB565 para BGRA8 — o console não dá contexto de vídeo próprio —, e o som vai pelo XAudio2, no
  frontend. O núcleo Libretro continua sem ligar biblioteca de host nenhuma
- **A estante mostra a arte de cada jogo.** Onde há capa em arquivo ao lado da ROM, é ela que
  aparece; onde não há, é o ícone do `.mif`, que o núcleo entrega já em BGRA8 pré-multiplicado
- **Os jogos podem vir de um pendrive ou de um HD externo**, por dois botões que abrem **caixas do
  próprio Windows** — nenhuma delas é um explorador nosso. A pasta escolhida volta na abertura
  seguinte, e a pasta que sumiu (unidade desplugada) aparece marcada na lista sem derrubar a
  varredura das outras. A unidade precisa estar em **NTFS ou exFAT**: o Xbox formata pendrive em
  FATX, que o Windows não lê
- **A varredura da estante não trava a janela.** Ela roda fora do fio da interface, com uma tela de
  espera enquanto dura; os arquivos de jogo vêm de `library::scan`, o mesmo do resto do emulador
- **O núcleo passa a recompilar código de verdade.** A v0.4.5.0 rodava no **interpretador** — o JIT
  entrava e derrubava o aplicativo, e ele caía para o caminho lento —, e era essa a explicação da
  engasgada. Nesta versão a recompilação acontece. Medido no PC: **28,1×** mais instruções por
  milissegundo que o interpretador (2.398.867 contra 85.295), e a abertura de um módulo de teste em
  332 e 603 ms contra 4407 ms. Quando o console recusa a memória executável, o aplicativo volta
  sozinho para o interpretador e **diz no relatório** que voltou
- **Quatro idiomas**, nos mesmos `assets/lang/*.json` do resto do emulador: inglês, espanhol,
  espanhol do México e português do Brasil. A interface nasce em inglês, a escolha sobrevive ao
  fechamento, e tradução nova é um `.json` na pasta `LocalState\lang`, sem recompilar. A GUI não tem
  frase própria: todo rótulo vem do catálogo do núcleo, por chave
- **Um relatório por execução**, em `LocalState\zeebx-xbox.log`, truncado a cada abertura, e um
  relator de falha que escreve a linha `falha:` com o código da exceção e o endereço, sem alocar
  memória no meio do estrago
- **O B é do jogo.** Com o jogo rodando, o B é o botão 2 do Zeebo; a pausa fica no View. Sem isso o
  "voltar" do sistema fechava o aplicativo no meio da partida

O que foi medido, e o que não foi:

- **medido no PC**, dentro do AppContainer: o JIT e o interpretador acima; o desenho, com um padrão
  sintético de 320×240 antes de existir ROM — a cadeia de troca medida em DIPs põe a borda esquerda
  em 306 (esperado 307), a cruz no centro em 970 (971) e a borda direita em 1635 (1635); o idioma
  escolhido, sobrevivendo a fechar e reabrir; e a falha provocada de propósito, para conferir que a
  linha `falha:` sai com o endereço certo
- **relatado de dentro do console**, por quem tem o aparelho: o aplicativo instala, sobe, dá som e
  roda jogo. **Não é medição minha.** A taxa de quadros do jogo no console, o som e os save states
  não foram medidos aqui — não há ROM neste PC, e é por isso que a engasgada não se reproduz nele
- **o seletor de pasta no console continua por medir.** A Microsoft documenta que cenários de
  seletor de pasta **não são suportados no Xbox**; é por isso que lá o caminho é "Escolher um
  arquivo de jogo", e quem dá o acesso à unidade é a capacidade `removableStorage` do manifesto,
  com a associação de tipos de arquivo — sem ela a unidade aparece vazia, em silêncio
- **um relato, e não uma bateria de testes**: 669 testes do núcleo e 15 do host deste frontend
  passam aqui, mas nenhum deles joga

Como instalar, o passo a passo inteiro está em
[`frontends/xbox-uwp/LEIAME.md`](../../frontends/xbox-uwp/LEIAME.md):

1. Ative o **Modo de Desenvolvedor** no console (pede uma conta do Partner Center). O Aplicativo
   Modo de Desenvolvedor mostra o IP e a senha do **Portal do Dispositivo**, o
   `https://<ip-do-xbox>:11443`
2. **Confie no certificado**: no PC, `Import-Certificate` no `LocalMachine\TrustedPeople` **e** no
   `Root`; no console, quem instala o certificado é a implantação pelo **Visual Studio**, que é o
   caminho mais garantido. Certificado autoassinado só é aceito por quem confia nele, e **não é
   `CurrentUser`**: quem instala pacote é o `AppXSvc`, um serviço, e serviço não tem repositório de
   usuário. Ele vale até **16 de outubro de 2027** — se um dia a instalação for recusada citando o
   certificado, é desta data que se trata, e a saída é assinar o pacote de novo (`compilar.ps1
   -Pacote`)
3. **Instale o pacote** — Home → Add or install package, no Portal do Dispositivo, ou
   `Add-AppxPackage` no PC
4. Ponha os jogos em `LocalState\roms` pelo Portal do Dispositivo, ou aponte uma pasta pelo próprio
   aplicativo, e aperte **Atualizar lista** na estante

**Instalar por cima precisa de versão maior**, e esta é: o console guarda a versão instalada e
recusa um pacote com o mesmo número. A outra saída é desinstalar antes — o que leva o `LocalState`
junto, com as pastas de jogos, o idioma e o que estiver em `LocalState\roms`.

**Não vem jogo nenhum neste pacote**, e não é descuido: o Zeebx é um emulador, e as ROMs do Zeebo
são obra de quem as fez. O que este repositório publica é o emulador.

O código é **GPL-2.0-or-later**, de quem o escreveu — a ZeebxTeam, no emulador, e este repositório,
no frontend de Xbox. O `LICENSE` e o `THIRD-PARTY-NOTICES.txt` acompanham o pacote nesta release. A
Loja do Xbox não aceitaria este pacote de qualquer forma: ele pede `expandedResources` e
`codeGeneration`, duas capacidades restritas que a Microsoft não aprova para emulador. O Modo de
Desenvolvedor existe exatamente para isto.
