//! O Zeebx no Xbox Series S|X: a mesma sessão, outra tela.
//!
//! Quem emula é a [`zeebx::session::Session`]. Este arquivo é a fronteira com o C++/WinRT: o
//! aplicativo pede uma varredura, um passo, um quadro e o som devido, e devolve botão. A janela
//! (o `SwapChainPanel`), a placa de som (XAudio2) e o controle (Windows.Gaming.Input) ficam no
//! projeto do Visual Studio, porque é ele quem tem o ciclo de vida do UWP.
//!
//! **O contraste com o iOS, que é o molde deste arquivo, é o áudio.** No iOS a placa é o `cpal`
//! (Core Audio) atrás da feature `audio`, e o núcleo toca sozinho. Aqui não: a placa é o XAudio2,
//! do lado C++, e as amostras saem por `Mixer::render` — o mesmo caminho do core Libretro. É o
//! que mantém a regra da casa, "o núcleo não linka biblioteca de host", verdadeira neste alvo.
//!
//! **O JIT, aqui, é decisão de execução.** O `dynarmic` escreve código de máquina e salta para
//! ele, e num AppContainer a página executável não é dada de graça: o alocador precisa subir a
//! página como RW, escrever o bloco e só então trocá-la para RX. O C++ do dynarmic já sabe fazer
//! isso — é a opção `DYNARMIC_ENABLE_NO_EXECUTE_SUPPORT`, ligada em `appcontainer.cmake` —, e os
//! `VirtualAlloc`/`VirtualProtect` que ele chama viram as variantes `FromApp` dentro de um
//! aplicativo.
//!
//! **O que não dá para saber antes de tentar é se o aparelho concede a página**, e um JIT que não
//! consegue alocar não falha ao compilar: falha ao executar, no meio do jogo. Por isso o aplicativo
//! percorre o caminho inteiro numa sonda (`app\Jit.cpp`) e conta o resultado antes do primeiro jogo:
//! os dois núcleos ficam compilados, e [`zeebx_xbox_permite_jit`] decide qual dos dois entra. Ver
//! `docs/implementacao/25-frontend-xbox-uwp.md`, seção "A memória executável".

use std::collections::HashSet;
use std::ffi::{CStr, CString, c_char};
use std::path::{Path, PathBuf};
use std::ptr;
use std::sync::{Arc, Mutex};
use std::time::Instant;

use zeebx::audio::Mixer;
use zeebx::input::{self, Pad};
use zeebx::library;
use zeebx::session::{self, FATIA_MAXIMA, Session, Step};
use zeebx::storage::StoragePaths;
use zeebx::ui::i18n;
use zeebx::ui::settings::Settings;
use zeebx::video::icon::Image;

// **O bloco vazio abaixo manda copiar o C++ de terceiro para dentro do `.lib`, e não declara
// função nenhuma.** O `dynarmic` do crates.io liga a árvore C++ dele com
// `cargo:rustc-link-lib=dynarmic`, **sem** modificador. Num binário isso basta: o linker lê o
// arquivo na hora e leva o que precisa. Num `staticlib` não: o rustc só copia para dentro do
// arquivo as bibliotecas nativas marcadas com `+whole-archive`, e as outras ficam de fora. O
// sintoma medido foi o pior possível — o núcleo saía com 61 MB **sem um objeto do dynarmic
// dentro**, e o erro só aparecia dois passos depois, na ligação do aplicativo, como `LNK2019` de
// nome C++ mangled, apontando para um arquivo que ninguém escreveu.
//
// É a mesma armadilha que o `frontends/switch/bundle-native-libs.sh` contorna do outro lado,
// copiando os objetos à mão para dentro do `.a`; aqui o aviso é do próprio rustc, e o `#[link]` é o
// canal dele.
//
// **Os quatro nomes são os que o `build.rs` do crate emite** — o C++ do dynarmic, o `fmt`, o `mcl`
// e o `Zydis` —, porque os objetos copiados do primeiro trazem referências pendentes para os
// símbolos dos outros três. O `Zydis` só existe em x86_64, que é o único alvo deste aplicativo.
#[cfg(target_vendor = "uwp")]
#[link(name = "dynarmic", kind = "static", modifiers = "+whole-archive")]
#[link(name = "fmt", kind = "static", modifiers = "+whole-archive")]
#[link(name = "mcl", kind = "static", modifiers = "+whole-archive")]
#[link(name = "Zydis", kind = "static", modifiers = "+whole-archive")]
unsafe extern "C" {}

/// Os mesmos números de `include/zeebx_xbox.h`. O header é o que o C++ inclui; o teste abaixo
/// confere o que dá para conferir deste lado.
const SEM_SESSAO: i32 = -1;
const QUADRO: i32 = 0;
const RODANDO: i32 = 1;
const ADIANTADO: i32 = 2;
const PAROU: i32 = 3;
const SEM_QUADRO_NOVO: i32 = 4;

/// A lista que uma varredura de fora deixa pronta para a linha da interface colher.
///
/// **É o único dado que atravessa fios neste arquivo.** A varredura de uma unidade cheia leva
/// segundos, e feita na linha da interface ela congela a estante — era o defeito. Quem varre é um
/// fio do Rust, que não toca no [`Aplicativo`] nenhum: ele monta a lista sozinho e a deixa aqui.
/// A linha da interface continua sendo a única que fala com o núcleo, como o header promete.
type Colheita = Arc<Mutex<Option<Vec<library::Game>>>>;

/// A taxa que o núcleo declara e o XAudio2 monta. É a mesma do core Libretro, e mudá-la aqui
/// sem mudar lá é o defeito que faz o som sair agudo — ver o comentário em `libretro/src/lib.rs`.
const TAXA: u32 = 44_100;

/// Quantos saves a GUI oferece. É o mesmo número que o cálculo do caminho lá em baixo usa.
const SLOTS: u32 = 10;

/// O aplicativo, do lado de cá da fronteira.
///
/// Público porque a assinatura `extern "C"` o nomeia. Os campos ficam privados: o C++ só vê o
/// ponteiro, e não há construtor fora das funções daqui.
pub struct Aplicativo {
    roms: PathBuf,
    dados: PathBuf,
    jogos: Vec<library::Game>,
    /// O que o quadro publicado representa: `(série, escritas, largura, altura)` da tela na
    /// última vez que ele foi montado.
    ///
    /// **É por ele que uma volta sem desenho novo não vira trabalho.** Um jogo que desenha a 30
    /// quadros por segundo dentro de um painel de 60 recebia, até aqui, uma conversão de 1,2 MB e
    /// um `Present` a mais em cada volta vazia: metade do trabalho de vídeo era refazer o mesmo
    /// quadro. `None` obriga a próxima publicação, e é o que um carregamento de estado pede.
    marca: Option<(u64, u64, u32, u32)>,
    /// A arte de cada jogo, em BGRA8 premultiplicado, montada só quando a estante pede um cartão.
    ///
    /// Preguiçosa de propósito: são até 2000 jogos, e converter todos os ícones de uma vez é
    /// trabalho que a maioria nunca vê. Quem já decodificou o arquivo foi a varredura, que guarda
    /// o RGBA em `Game::art`; aqui só se troca a ordem dos canais.
    imagens: Vec<Option<Imagem>>,
    /// A varredura que está acontecendo fora da linha da interface, se há alguma.
    varredura: Option<Colheita>,
    /// Um `CString` por título, para o ponteiro devolvido continuar válido.
    titulos: Vec<CString>,
    /// Um `CString` por caminho, pelo mesmo motivo.
    caminhos: Vec<CString>,
    /// As pastas de fora onde a varredura também procura: as que o jogador apontou na tela de
    /// fontes — uma unidade removível, uma pasta dentro dela.
    fontes: Vec<PathBuf>,
    /// Um `CString` por fonte, pelo mesmo motivo dos títulos.
    fontes_c: Vec<CString>,
    /// As preferências deste frontend, gravadas em `LocalState\settings.json`.
    config: Settings,
    /// Os idiomas de fábrica mais os arquivos que o jogador puser em `LocalState\lang`.
    catalogo: i18n::Catalog,
    /// Um `CString` por código de idioma, e um por nome de idioma.
    codigos: Vec<CString>,
    nomes: Vec<CString>,
    /// O último texto devolvido por `zeebx_xbox_texto`, para o ponteiro valer até a próxima
    /// chamada — o `String` do Rust não termina em zero, e a ABI devolve `*const c_char`.
    texto: CString,
    sessao: Option<Session>,
    pad: Pad,
    pad_anterior: Pad,
    ultimo: Instant,
    /// BGRA8 do quadro corrente. O C++ copia para a textura antes da próxima volta.
    quadro: Vec<u8>,
    largura: u32,
    altura: u32,
    /// A placa do núcleo. Não é um dispositivo: é o misturador, e é o C++ quem o esvazia.
    mixer: Option<Mixer>,
    /// Estéreo intercalado, já em i16: é o que o XAudio2 recebe.
    audio: Vec<i16>,
    /// O relógio virtual da volta anterior. O som devido é o que o jogo emulou desde então, e não
    /// um número fixo por quadro: um jogo que passa dois quadros virtuais precisa do dobro de som.
    ultimo_relogio_ms: u32,
    volume: f32,
    erro: CString,
    storage: StoragePaths,
}

impl Aplicativo {
    fn define_erro(&mut self, texto: &str) {
        self.erro = CString::new(texto).unwrap_or_else(|_| CString::new("erro").expect("sem nulo"));
    }

    fn varre(&mut self) -> i32 {
        let jogos = varre_pastas(&self.roms, &self.fontes);
        self.instala(jogos)
    }

    /// Põe uma lista já varrida no lugar da atual, e refaz o que a ABI devolve por ponteiro.
    fn instala(&mut self, jogos: Vec<library::Game>) -> i32 {
        self.jogos = jogos;
        // A arte é montada sob demanda, e por índice: lista nova, arte nova.
        self.imagens.clear();
        self.titulos = self
            .jogos
            .iter()
            .map(|jogo| {
                CString::new(jogo.title.as_str())
                    .unwrap_or_else(|_| CString::new("jogo").expect("sem nulo"))
            })
            .collect();
        self.caminhos = self
            .jogos
            .iter()
            .map(|jogo| {
                CString::new(jogo.path.to_string_lossy().as_ref())
                    .unwrap_or_else(|_| CString::new("").expect("sem nulo"))
            })
            .collect();
        self.titulos.len() as i32
    }

    /// `true` quando o quadro publicado mudou nesta volta.
    fn publica(&mut self) -> bool {
        // Os campos são tomados em separado porque `tela` empresta a sessão e `quadro` precisa
        // ser escrito na mesma volta: com `&mut self` inteiro o compilador recusa, e com razão.
        let Aplicativo {
            sessao,
            quadro,
            largura,
            altura,
            marca,
            ..
        } = self;
        let Some(sessao) = sessao.as_ref() else {
            quadro.clear();
            *largura = 0;
            *altura = 0;
            *marca = None;
            return false;
        };
        let tela = sessao.screen();
        let agora = (tela.serie(), tela.escritas(), tela.width(), tela.height());
        if *marca == Some(agora) && !quadro.is_empty() {
            return false;
        }
        *marca = Some(agora);
        *largura = tela.width();
        *altura = tela.height();
        bgra_de_rgb565_para(quadro, tela.pixels());
        true
    }

    /// O caminho de um save. Fica junto do resto do que o aplicativo grava, que é o LocalState.
    fn caminho_do_slot(&self, slot: u32) -> PathBuf {
        self.dados.join("saves").join(format!("zeebx-slot-{slot}.zbx"))
    }

    /// O caminho das preferências deste frontend, dentro do `LocalState`.
    fn caminho_da_config(&self) -> PathBuf {
        self.dados.join("settings.json")
    }

    /// Grava as preferências. Falha de escrita não derruba nada: a escolha continua valendo nesta
    /// sessão, e o que se perde é ela na próxima abertura.
    fn grava_config(&self) -> std::io::Result<()> {
        self.config.save_to(&self.caminho_da_config())
    }

    /// Refaz os `CString` que os ponteiros da ABI devolvem: os idiomas e as fontes.
    ///
    /// **É por isto que a lista de idiomas não devolve o `String` do catálogo direto.** Um ponteiro
    /// para `String::as_str` não termina em zero, e o C++ leria além do fim; os textos ficam em
    /// memória do `Aplicativo`, como os títulos dos jogos já ficavam.
    fn atualiza_listas_de_texto(&mut self) {
        self.codigos = self
            .catalogo
            .languages()
            .iter()
            .map(|idioma| cstring(&idioma.code))
            .collect();
        self.nomes = self
            .catalogo
            .languages()
            .iter()
            .map(|idioma| cstring(&idioma.name))
            .collect();
        self.fontes_c = self
            .fontes
            .iter()
            .map(|fonte| cstring(&fonte.to_string_lossy()))
            .collect();
    }
}

/// A varredura de verdade, longe do [`Aplicativo`]: é ela que pode rodar num fio próprio.
///
/// **A pasta do pacote mais as fontes que o jogador apontou.** Para o núcleo uma unidade
/// removível é uma pasta como outra qualquer: quem a concede é o UWP, pela capacidade
/// `removableStorage`, e não este lado.
fn varre_pastas(roms: &Path, fontes: &[PathBuf]) -> Vec<library::Game> {
    let mut jogos = library::scan(roms);
    for fonte in fontes {
        jogos.extend(library::scan(fonte));
    }
    // Duas fontes podem se cobrir — a unidade inteira e uma pasta dentro dela —, e o mesmo
    // arquivo não pode virar dois cartões na estante. O caminho é a identidade do jogo.
    let mut vistos = HashSet::new();
    jogos.retain(|jogo| vistos.insert(jogo.path.clone()));
    jogos.sort_by(|a, b| a.title.to_lowercase().cmp(&b.title.to_lowercase()));
    jogos
}

/// RGB565 do console para BGRA8, que é o `DXGI_FORMAT_B8G8R8A8_UNORM` que o D3D11 recebe.
///
/// O canal de 5 bits vira 8 repetindo os bits altos nos baixos: senão o branco do console (31)
/// sai 248 e a tela fica lavada. O iOS faz a mesma conta para o `CGImage`, e é a mesma razão.
///
/// **A conversão escreve num vetor que já existe.** Ela roda a cada quadro, e um `Vec` novo por
/// quadro é 1,2 MB pedidos e devolvidos ao alocador sessenta vezes por segundo.
fn bgra_de_rgb565_para(saida: &mut Vec<u8>, pixels: &[u16]) {
    saida.clear();
    saida.reserve(pixels.len() * 4);
    for &pixel in pixels {
        let r5 = (pixel >> 11) & 0x1f;
        let g6 = (pixel >> 5) & 0x3f;
        let b5 = pixel & 0x1f;
        let r = ((r5 << 3) | (r5 >> 2)) as u8;
        let g = ((g6 << 2) | (g6 >> 4)) as u8;
        let b = ((b5 << 3) | (b5 >> 2)) as u8;
        saida.extend_from_slice(&[b, g, r, 255]);
    }
}

/// A mesma conversão, devolvendo um vetor novo — para os testes e ferramentas, onde a alocação
/// não importa e um vetor pronto é mais direto de usar.
#[cfg(test)]
fn bgra_de_rgb565(pixels: &[u16]) -> Vec<u8> {
    let mut saida = Vec::new();
    bgra_de_rgb565_para(&mut saida, pixels);
    saida
}

/// Um ícone pronto para o XAML: BGRA8 **premultiplicado**, que é como a composição do XAML espera
/// a cor e o alfa.
///
/// O ícone do `.mif` é RGBA direto, e é assim que ele aparece nas outras estantes. Aqui não: o
/// `SoftwareBitmapSource` recebe o quadro pronto e não o converte, e um alfa não-premultiplicado
/// com fundo transparente sai com a borda clara — o defeito clássico de recorte.
struct Imagem {
    largura: u32,
    altura: u32,
    bgra: Vec<u8>,
}

fn imagem_premultiplicada(arte: &Image) -> Imagem {
    let mut bgra = Vec::with_capacity(arte.rgba.len());
    for canal in arte.rgba.chunks_exact(4) {
        let (r, g, b, a) = (canal[0] as u32, canal[1] as u32, canal[2] as u32, canal[3] as u32);
        let p = |valor: u32| ((valor * a + 127) / 255) as u8;
        bgra.extend_from_slice(&[p(b), p(g), p(r), a as u8]);
    }
    Imagem {
        largura: arte.width as u32,
        altura: arte.height as u32,
        bgra,
    }
}

fn caminho(ptr: *const c_char) -> Option<PathBuf> {
    if ptr.is_null() {
        return None;
    }
    // O ponteiro veio do C++ nesta chamada e permanece válido até ela voltar.
    let texto = unsafe { CStr::from_ptr(ptr) }.to_string_lossy();
    if texto.is_empty() {
        return None;
    }
    Some(PathBuf::from(texto.as_ref()))
}

fn aplicativo<'a>(ptr: *mut Aplicativo) -> Option<&'a mut Aplicativo> {
    if ptr.is_null() {
        None
    } else {
        // O ponteiro é o que `zeebx_xbox_cria` devolveu e que `zeebx_xbox_destroi` ainda não
        // consumiu. O C++ o usa de uma linha só, na linha da interface.
        Some(unsafe { &mut *ptr })
    }
}

fn c_string(ptr: *const c_char) -> Option<String> {
    if ptr.is_null() {
        return None;
    }
    Some(unsafe { CStr::from_ptr(ptr) }.to_string_lossy().into_owned())
}

#[unsafe(no_mangle)]
pub extern "C" fn zeebx_xbox_cria(roms: *const c_char, dados: *const c_char) -> *mut Aplicativo {
    let (Some(roms), Some(dados)) = (caminho(roms), caminho(dados)) else {
        return ptr::null_mut();
    };

    // **A pasta de ROMs é criada por nós.** No primeiro arranque ela não existe, e o aplicativo
    // de UWP não pode criar arquivos fora do seu container: quem grava é o jogador, pelo
    // `ApplicationData` ou por um IHM. Sem a pasta, a varredura devolveria zero para sempre e
    // pareceria defeito do emulador.
    let _ = std::fs::create_dir_all(&roms);
    let _ = std::fs::create_dir_all(dados.join("saves"));

    let storage = StoragePaths::for_frontend(&dados, Some(&dados));
    let erro_de_pasta = storage.create_dirs().err().map(|erro| erro.to_string());

    // **As preferências e os idiomas moram no `LocalState`.** Um AppContainer só escreve dentro do
    // seu container: o `settings.json` do desktop (`settings_path`, em `%APPDATA%`) aqui seria uma
    // gravação negada e uma escolha que não gruda. O idioma extra, que o desktop lê de ao lado do
    // executável, aqui é `LocalState\lang` — a pasta em que o jogador pode pôr um arquivo.
    let mut config = Settings::load_from(&dados.join("settings.json"));
    let mut catalogo = i18n::Catalog::new(&[dados.join("lang")]);

    // **O inglês é o idioma de fábrica deste alvo.** No desktop a primeira abertura segue o
    // sistema, e quem instalou é quem decide; num console não há de quem seguir, e o padrão que sai
    // do pacote é o inglês. Português e espanhol estão a um toque de distância, em `Configurações`,
    // e a escolha fica gravada.
    let escolhido = config.language.clone().unwrap_or_else(|| i18n::FALLBACK.to_string());
    if !catalogo.select(&escolhido) {
        catalogo.select_best(i18n::FALLBACK);
    }
    // Um idioma que não existe mais — um arquivo apagado do `LocalState\lang` — volta a ser o
    // inglês já na gravação, senão a abertura seguinte tentaria de novo o que não existe.
    let precisa_gravar = config.language.as_deref() != Some(catalogo.current());
    if precisa_gravar {
        config.language = Some(catalogo.current().to_string());
    }

    let mut app = Aplicativo {
        roms,
        dados,
        jogos: Vec::new(),
        marca: None,
        imagens: Vec::new(),
        varredura: None,
        titulos: Vec::new(),
        caminhos: Vec::new(),
        fontes: config.roms_dirs.clone(),
        fontes_c: Vec::new(),
        config,
        catalogo,
        codigos: Vec::new(),
        nomes: Vec::new(),
        texto: CString::new("").expect("vazio"),
        sessao: None,
        pad: Pad::default(),
        pad_anterior: Pad::default(),
        ultimo: Instant::now(),
        quadro: Vec::new(),
        largura: 0,
        altura: 0,
        mixer: None,
        audio: Vec::new(),
        ultimo_relogio_ms: 0,
        volume: 1.0,
        erro: CString::new("").expect("vazio"),
        storage,
    };
    app.atualiza_listas_de_texto();
    if let Some(texto) = erro_de_pasta {
        app.define_erro(&texto);
    }
    if precisa_gravar {
        // **A primeira abertura deixa o arquivo escrito.** Não é enfeite: é o que faz o
        // `settings.json` existir para quem quiser editar o idioma sem abrir o aplicativo, e o que
        // marca a escolha como explícita em vez de "o padrão de quem abriu primeiro".
        let _ = app.grava_config();
    }
    // **A varredura não acontece aqui.** Uma unidade de fora cheia leva segundos para ser lida, e
    // feita nesta chamada ela é a janela preta do arranque. Quem chama pede a varredura depois, pela
    // `zeebx_xbox_varre_inicia`, e a estante mostra "procurando" enquanto ela anda.
    Box::into_raw(Box::new(app))
}

#[unsafe(no_mangle)]
pub extern "C" fn zeebx_xbox_destroi(app: *mut Aplicativo) {
    if !app.is_null() {
        drop(unsafe { Box::from_raw(app) });
    }
}

/// Diz ao núcleo se **este processo** pode executar a memória que ele mesmo escreve.
///
/// **É a única diferença entre o jogo rodar recompilado ou interpretado no Xbox**, e a chamada tem
/// de vir antes do primeiro [`zeebx_xbox_comeca`]: num AppContainer que recusa virar uma página
/// gravável em executável, o Dynarmic não falha ao compilar — falha ao executar, no meio do jogo, e
/// não há caminho de volta. Quem responde é a sonda do `app\Jit.cpp`, que percorre o caminho inteiro
/// no aparelho durante a abertura do aplicativo.
///
/// Zero é "não pode"; qualquer outro valor é "pode". A resposta vale para todas as sessões: é do
/// processo, e não do jogo. Quem não chama fica com o otimista, que é o dos outros frontends.
#[unsafe(no_mangle)]
pub extern "C" fn zeebx_xbox_permite_jit(permitido: i32) {
    zeebx::cpu::permite_jit(permitido != 0);
}

/// O nome do núcleo da sessão aberta, em C: `"dynarmic"` ou `"interpretador"`. Nulo sem sessão.
///
/// Existe para o relatório do arranque: o mesmo pacote roda recompilado ou interpretado conforme o
/// aparelho, e sem esta resposta o log do console não diz qual dos dois está rodando. O texto é o
/// do núcleo — ver [`Session::nucleo`] —, e não uma segunda tabela deste lado.
#[unsafe(no_mangle)]
pub extern "C" fn zeebx_xbox_nucleo(app: *mut Aplicativo) -> *const c_char {
    let Some(app) = aplicativo(app) else {
        return ptr::null();
    };
    let Some(sessao) = app.sessao.as_ref() else {
        return ptr::null();
    };
    sessao.nucleo().as_ptr()
}

#[unsafe(no_mangle)]
pub extern "C" fn zeebx_xbox_varre(app: *mut Aplicativo) -> i32 {
    aplicativo(app).map(|app| app.varre()).unwrap_or(0)
}

/// Começa a varredura num fio do Rust e volta na hora.
///
/// 0 quando o fio saiu; -1 quando já havia uma varredura andando. **É esta chamada que impede a
/// estante de congelar:** a de cima devolve quando terminou, e uma unidade cheia é segundo de
/// janela parada. O fio não toca no [`Aplicativo`] — copia os caminhos, monta a lista sozinho e a
/// deixa na colheita —, então o contrato do header, "a chamada é da linha da interface", continua
/// valendo sem exceção.
#[unsafe(no_mangle)]
pub extern "C" fn zeebx_xbox_varre_inicia(app: *mut Aplicativo) -> i32 {
    let Some(app) = aplicativo(app) else {
        return -1;
    };
    if app.varredura.is_some() {
        return -1;
    }
    let colheita: Colheita = Arc::new(Mutex::new(None));
    let cesto = Arc::clone(&colheita);
    let roms = app.roms.clone();
    let fontes = app.fontes.clone();
    app.varredura = Some(colheita);
    std::thread::Builder::new()
        .name("zeebx-varredura".into())
        .spawn(move || {
            let jogos = varre_pastas(&roms, &fontes);
            // Um `Mutex` envenenado só acontece se o fio entrou em pânico, e aí não há lista
            // nenhuma para colher: a estante fica como estava e o jogador tenta de novo.
            if let Ok(mut cesto) = cesto.lock() {
                *cesto = Some(jogos);
            }
        })
        .map(|_| 0)
        .unwrap_or(-1)
}

/// Colhe a varredura do fio, se ela terminou.
///
/// 1 quando a lista entrou; 0 quando ainda anda; -1 quando não há varredura nenhuma. **Quem chama
/// isto está na linha da interface**: é aqui que a lista passa a valer, e não no fio.
#[unsafe(no_mangle)]
pub extern "C" fn zeebx_xbox_varre_colhe(app: *mut Aplicativo) -> i32 {
    let Some(app) = aplicativo(app) else {
        return -1;
    };
    let Some(colheita) = app.varredura.as_ref() else {
        return -1;
    };
    let Ok(mut cesto) = colheita.lock() else {
        app.varredura = None;
        return -1;
    };
    let Some(jogos) = cesto.take() else {
        return 0;
    };
    drop(cesto);
    app.varredura = None;
    app.instala(jogos);
    1
}

/// Quantos jogos a lista corrente tem. **Não varre nada.**
///
/// É o número que falta depois de uma varredura de fora: quem colhe a lista já sabe que ela
/// entrou, e é esta chamada que diz de que tamanho ela é — sem uma varredura a mais só para
/// contar.
#[unsafe(no_mangle)]
pub extern "C" fn zeebx_xbox_jogos(app: *mut Aplicativo) -> i32 {
    aplicativo(app)
        .map(|app| app.jogos.len() as i32)
        .unwrap_or(0)
}

#[unsafe(no_mangle)]
pub extern "C" fn zeebx_xbox_titulo(app: *mut Aplicativo, indice: i32) -> *const c_char {
    let Some(app) = aplicativo(app) else {
        return ptr::null();
    };
    let Ok(indice) = usize::try_from(indice) else {
        return ptr::null();
    };
    app.titulos
        .get(indice)
        .map(|titulo| titulo.as_ptr())
        .unwrap_or(ptr::null())
}

#[unsafe(no_mangle)]
pub extern "C" fn zeebx_xbox_caminho(app: *mut Aplicativo, indice: i32) -> *const c_char {
    let Some(app) = aplicativo(app) else {
        return ptr::null();
    };
    let Ok(indice) = usize::try_from(indice) else {
        return ptr::null();
    };
    app.caminhos
        .get(indice)
        .map(|caminho| caminho.as_ptr())
        .unwrap_or(ptr::null())
}

/// A arte do jogo `indice` — a capa que o jogador pôs ao lado do arquivo, ou o ícone que o próprio
/// jogo traz dentro do `.mif`.
///
/// Devolve BGRA8 **premultiplicado**, na largura e altura escritas nos ponteiros; ponteiro nulo
/// quando o jogo não tem arte nenhuma, quando o índice não existe, ou quando o `.mif` traz uma
/// imagem que o núcleo não sabe ler. A GUI desenha o cartão sem capa nesse caso, e não um buraco.
///
/// **A ordem dos canais é a do `SoftwareBitmapSource`, e é de propósito.** O RGBA do núcleo sai
/// direto no desktop; aqui a conversão é feita uma vez, na primeira vez que o cartão aparece, e
/// nunca mais — a arte fica guardada.
///
/// O ponteiro vale enquanto o [`Aplicativo`] viver e a lista da estante não for trocada por outra
/// varredura. É o mesmo contrato dos títulos.
#[unsafe(no_mangle)]
pub extern "C" fn zeebx_xbox_imagem(
    app: *mut Aplicativo,
    indice: i32,
    largura: *mut u32,
    altura: *mut u32,
) -> *const u8 {
    let Some(app) = aplicativo(app) else {
        return ptr::null();
    };
    let Ok(indice) = usize::try_from(indice) else {
        return ptr::null();
    };
    if indice >= app.jogos.len() {
        return ptr::null();
    }
    if app.imagens.len() != app.jogos.len() {
        app.imagens.resize_with(app.jogos.len(), || None);
    }
    if app.imagens[indice].is_none() {
        // A conversão é feita fora do empréstimo da lista: `art` está emprestado de `jogos`, e a
        // escrita vai em `imagens`.
        let pronta = app
            .jogos
            .get(indice)
            .and_then(|jogo| jogo.art.as_ref())
            .map(imagem_premultiplicada);
        app.imagens[indice] = pronta;
    }
    let Some(imagem) = app.imagens[indice].as_ref() else {
        return ptr::null();
    };
    if !largura.is_null() {
        unsafe { *largura = imagem.largura };
    }
    if !altura.is_null() {
        unsafe { *altura = imagem.altura };
    }
    imagem.bgra.as_ptr()
}

#[unsafe(no_mangle)]
pub extern "C" fn zeebx_xbox_comeca(app: *mut Aplicativo, indice: i32) -> i32 {
    let Some(app) = aplicativo(app) else {
        return -1;
    };
    let Ok(indice) = usize::try_from(indice) else {
        app.define_erro("jogo fora da lista");
        return -1;
    };
    let Some(jogo) = app.jogos.get(indice) else {
        app.define_erro("jogo fora da lista");
        return -1;
    };
    let caminho = jogo.path.clone();
    let instalados: Vec<(u32, String)> = app
        .jogos
        .iter()
        .filter_map(|jogo| Some((jogo.clsid?, library::id_do_modulo(&jogo.path)?)))
        .collect();

    // Sem placa no núcleo: no UWP o quadro é o do rasterizador de software e o som sai pelo
    // `Mixer`, que o C++ esvazia para o XAudio2. Passar pela feature `audio` aqui traria o
    // `cpal`/WASAPI para dentro de um AppContainer, que é onde ele não funciona.
    match Session::start_software_with_storage_installed(
        &caminho,
        zeebx::PORTAS_PADRAO,
        zeebx::config::ZWheel::default(),
        &app.storage,
        &instalados,
    ) {
        Ok(mut sessao) => {
            let mixer = sessao.grava_audio(TAXA);
            mixer.set_master(app.volume, false);
            app.mixer = Some(mixer);
            app.audio.clear();
            app.ultimo_relogio_ms = sessao.clock_ms();
            app.sessao = Some(sessao);
            app.pad = Pad::default();
            app.pad_anterior = Pad::default();
            app.ultimo = Instant::now();
            app.erro = CString::new("").expect("vazio");
            // O primeiro quadro sai já montado, para o `zeebx_xbox_quadro` ter o que devolver; a
            // marca volta a `None` para a primeira volta publicar de novo e o `Present` acontecer.
            app.publica();
            app.marca = None;
            0
        }
        Err(erro) => {
            app.define_erro(&erro.to_string());
            -1
        }
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn zeebx_xbox_para(app: *mut Aplicativo) {
    let Some(app) = aplicativo(app) else {
        return;
    };
    app.sessao = None;
    app.mixer = None;
    app.audio.clear();
    app.pad = Pad::default();
    app.pad_anterior = Pad::default();
    app.quadro.clear();
    app.largura = 0;
    app.altura = 0;
    app.marca = None;
}

#[unsafe(no_mangle)]
pub extern "C" fn zeebx_xbox_passo(app: *mut Aplicativo) -> i32 {
    let Some(app) = aplicativo(app) else {
        return SEM_SESSAO;
    };
    if app.sessao.is_none() {
        return SEM_SESSAO;
    }

    let antes = app.pad_anterior;
    let pad = app.pad;
    {
        let sessao = app.sessao.as_mut().expect("conferido acima");
        sessao.set_port_pad(0, pad);
        for (avk, apertada) in input::teclas_do_controle(&antes, &pad) {
            sessao.set_key(avk, apertada);
        }
    }
    app.pad_anterior = pad;

    // O orçamento é o tempo real desde a volta anterior, preso ao teto: é quanto o jogo precisa
    // emular para acompanhar o relógio do mundo. O teto existe para uma volta que demorou — a
    // aplicação voltou do segundo plano, ou o jogador abriu o menu — não virar um salto de minutos.
    let agora = Instant::now();
    let fatia = agora
        .saturating_duration_since(app.ultimo)
        .min(FATIA_MAXIMA);
    app.ultimo = agora;

    let intermediario = app
        .sessao
        .as_mut()
        .expect("conferido acima")
        .mostra_quadro_intermediario();
    if intermediario {
        return if app.publica() { QUADRO } else { SEM_QUADRO_NOVO };
    }

    let passo = app
        .sessao
        .as_mut()
        .expect("conferido acima")
        .step(fatia, true);

    // O som é montado **antes** do retorno de parada: no quadro em que o jogo para ainda há o que
    // tocar, e cortá-lo ali é o que faz o desfecho sair mudo.
    monta_audio(app);

    if passo == Step::Stopped {
        let normal = app
            .sessao
            .as_ref()
            .is_some_and(session::Session::saiu_normalmente);
        let motivo = app
            .sessao
            .as_ref()
            .and_then(session::Session::stopped_reason)
            .unwrap_or_default();
        app.sessao = None;
        app.mixer = None;
        app.pad = Pad::default();
        app.pad_anterior = Pad::default();
        if !normal {
            app.define_erro(&motivo);
        }
        return PAROU;
    }
    // **O som sai em toda volta, o quadro só quando muda.** O jogo desenha a 30 quadros por
    // segundo e a tela compõe a 60: na volta sem desenho novo o `Present` repetiria o mesmo quadro,
    // depois de uma conversão de 1,2 MB. O XAudio2, esse, precisa do som devido nas duas.
    if !app.publica() {
        return SEM_QUADRO_NOVO;
    }
    match passo {
        Step::Presented => QUADRO,
        Step::Ahead => ADIANTADO,
        Step::Running => RODANDO,
        Step::Stopped => PAROU,
    }
}

/// Monta o som devido desde a volta anterior, já em i16 estéreo intercalado.
///
/// **O tempo vem do relógio virtual, não de um número fixo.** Um jogo que passa dois quadros
/// virtuais entre duas voltas precisa entregar o dobro de amostras, senão o som atrasa em relação
/// à imagem. É a mesma conta do core Libretro, e a razão é a mesma.
fn monta_audio(app: &mut Aplicativo) {
    let Some(sessao) = app.sessao.as_ref() else {
        app.audio.clear();
        return;
    };
    let agora = sessao.clock_ms();
    let decorrido = u64::from(agora.wrapping_sub(app.ultimo_relogio_ms));
    app.ultimo_relogio_ms = agora;
    let Some(mixer) = app.mixer.as_ref() else {
        app.audio.clear();
        return;
    };
    // O teto de um segundo existe para uma volta que demorou não pedir um buffer absurdo: o jogo
    // já perdeu o compasso, e o que se quer é que o som volte, não que ele se atrase mais.
    let devidas = (decorrido.min(1000) * u64::from(TAXA) / 1000) as usize;
    app.audio.clear();
    for amostra in mixer.render(devidas) {
        app.audio
            .push((amostra.clamp(-1.0, 1.0) * f32::from(i16::MAX)) as i16);
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn zeebx_xbox_quadro(
    app: *mut Aplicativo,
    largura: *mut u32,
    altura: *mut u32,
) -> *const u8 {
    let Some(app) = aplicativo(app) else {
        return ptr::null();
    };
    if app.quadro.is_empty() || app.largura == 0 || app.altura == 0 {
        return ptr::null();
    }
    if !largura.is_null() {
        unsafe { *largura = app.largura };
    }
    if !altura.is_null() {
        unsafe { *altura = app.altura };
    }
    app.quadro.as_ptr()
}

#[unsafe(no_mangle)]
pub extern "C" fn zeebx_xbox_audio(app: *mut Aplicativo, quadros: *mut u32) -> *const i16 {
    let Some(app) = aplicativo(app) else {
        return ptr::null();
    };
    if app.audio.is_empty() {
        return ptr::null();
    }
    if !quadros.is_null() {
        // Intercalado estéreo: dois valores por quadro.
        unsafe { *quadros = (app.audio.len() / 2) as u32 };
    }
    app.audio.as_ptr()
}

#[unsafe(no_mangle)]
pub extern "C" fn zeebx_xbox_taxa_de_audio() -> u32 {
    TAXA
}

#[unsafe(no_mangle)]
pub extern "C" fn zeebx_xbox_botao(app: *mut Aplicativo, indice: u32, apertado: i32) {
    let Some(app) = aplicativo(app) else {
        return;
    };
    let indice = indice as usize;
    if indice >= input::BUTTONS {
        return;
    }
    app.pad.press(indice, apertado != 0);
}

#[unsafe(no_mangle)]
pub extern "C" fn zeebx_xbox_eixo(app: *mut Aplicativo, indice: u32, valor: i32) {
    let Some(app) = aplicativo(app) else {
        return;
    };
    if indice as usize >= ZEEBX_EIXOS {
        return;
    }
    app.pad.set_axis(indice as usize, valor);
}

/// O mesmo número do `#define ZEEBX_EIXOS` do header. O teste abaixo o confere.
const ZEEBX_EIXOS: usize = 4;

#[unsafe(no_mangle)]
pub extern "C" fn zeebx_xbox_botao_por_nome(nome: *const c_char) -> i32 {
    let Some(nome) = c_string(nome) else {
        return -1;
    };
    Pad::button_by_name(&nome)
        .map(|indice| indice as i32)
        .unwrap_or(-1)
}

#[unsafe(no_mangle)]
pub extern "C" fn zeebx_xbox_volume(app: *mut Aplicativo, volume: u32) {
    let Some(app) = aplicativo(app) else {
        return;
    };
    let nivel = (volume.min(100) as f32) / 100.0;
    app.volume = nivel;
    if let Some(mixer) = app.mixer.as_ref() {
        mixer.set_master(nivel, false);
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn zeebx_xbox_erro(app: *mut Aplicativo) -> *const c_char {
    aplicativo(app)
        .map(|app| app.erro.as_ptr())
        .unwrap_or(ptr::null())
}

#[unsafe(no_mangle)]
pub extern "C" fn zeebx_xbox_relogio_ms(app: *mut Aplicativo) -> u32 {
    aplicativo(app)
        .and_then(|app| app.sessao.as_ref().map(Session::clock_ms))
        .unwrap_or(0)
}

#[unsafe(no_mangle)]
pub extern "C" fn zeebx_xbox_quadros(app: *mut Aplicativo) -> u64 {
    aplicativo(app)
        .and_then(|app| app.sessao.as_ref().map(Session::quadros_apresentados))
        .unwrap_or(0)
}

#[unsafe(no_mangle)]
pub extern "C" fn zeebx_xbox_instrucoes(app: *mut Aplicativo) -> u64 {
    aplicativo(app)
        .and_then(|app| app.sessao.as_ref().map(Session::instrucoes))
        .unwrap_or(0)
}

#[unsafe(no_mangle)]
pub extern "C" fn zeebx_xbox_salva_estado(app: *mut Aplicativo, slot: u32) -> i32 {
    let Some(app) = aplicativo(app) else {
        return -1;
    };
    if slot >= SLOTS {
        app.define_erro("slot fora da faixa");
        return -1;
    }
    let Some(sessao) = app.sessao.as_mut() else {
        app.define_erro("sem jogo aberto");
        return -1;
    };
    // O `pode_salvar` é quem sabe se o ponto atual é salvavel — salvar no meio de uma carga de
    // arquivo grava um estado que não restaura. Perguntar antes é mais barato que descobrir depois.
    if let Err(motivo) = sessao.pode_salvar() {
        app.define_erro(&motivo);
        return -1;
    }
    let arquivo = sessao.grava_estado();
    let caminho = app.caminho_do_slot(slot);
    if let Some(pai) = caminho.parent() {
        let _ = std::fs::create_dir_all(pai);
    }
    // Gravação atômica: um `.tmp` ao lado e o rename por cima. Um save interrompido no meio não
    // pode deixar um arquivo do tamanho errado no lugar, que é o que o restore leria.
    let temporario = caminho.with_extension("zbx.tmp");
    if let Err(erro) = std::fs::write(&temporario, &arquivo) {
        app.define_erro(&erro.to_string());
        return -1;
    }
    if let Err(erro) = std::fs::rename(&temporario, &caminho) {
        let _ = std::fs::remove_file(&temporario);
        app.define_erro(&erro.to_string());
        return -1;
    }
    app.erro = CString::new("").expect("vazio");
    0
}

#[unsafe(no_mangle)]
pub extern "C" fn zeebx_xbox_carrega_estado(app: *mut Aplicativo, slot: u32) -> i32 {
    let Some(app) = aplicativo(app) else {
        return -1;
    };
    if slot >= SLOTS {
        app.define_erro("slot fora da faixa");
        return -1;
    }
    let caminho = app.caminho_do_slot(slot);
    let Ok(arquivo) = std::fs::read(&caminho) else {
        // 1 e não -1: o slot vazio não é falha, é ausência — a GUI não mostra erro por isso.
        return 1;
    };
    let Some(sessao) = app.sessao.as_mut() else {
        app.define_erro("sem jogo aberto");
        return -1;
    };
    match sessao.restaura_estado(&arquivo) {
        Ok(()) => {
            // O relógio virtual saltou: o próximo `monta_audio` pediria o som do tempo que passou
            // enquanto o jogo tinha outro conteúdo, e sairia um estouro. Reancora o marco aqui.
            app.ultimo_relogio_ms = sessao.clock_ms();
            // O estado restaurado pode ser o mesmo quadro que já está publicado — a marca é
            // (série, escritas, largura, altura), e as escritas voltam ao número do save. Sem
            // isto, carregar um save no mesmo ponto não redesenha a tela.
            app.marca = None;
            app.erro = CString::new("").expect("vazio");
            0
        }
        Err(erro) => {
            app.define_erro(&erro.to_string());
            -1
        }
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn zeebx_xbox_tem_estado(app: *mut Aplicativo, slot: u32) -> i32 {
    let Some(app) = aplicativo(app) else {
        return 0;
    };
    if slot >= SLOTS {
        return 0;
    }
    Path::new(&app.caminho_do_slot(slot)).is_file() as i32
}

// ==================== A língua da interface ====================
//
// **A GUI deste alvo não tem texto próprio.** Ela pergunta ao núcleo, chave por chave, e o núcleo
// responde do mesmo catálogo que o desktop usa — os `assets/lang/*.json` embutidos, mais o que o
// jogador puser em `LocalState\lang`. É o que faz o emulador falar inglês, espanhol e português sem
// uma segunda tabela de traduções para manter em dia.

/// Quantos idiomas o catálogo tem.
#[unsafe(no_mangle)]
pub extern "C" fn zeebx_xbox_idiomas(app: *mut Aplicativo) -> i32 {
    aplicativo(app)
        .map(|app| app.catalogo.languages().len() as i32)
        .unwrap_or(0)
}

/// O código do idioma `indice` — "en", "es", "es-MX", "pt-BR". Nulo fora da faixa.
#[unsafe(no_mangle)]
pub extern "C" fn zeebx_xbox_idioma_codigo(app: *mut Aplicativo, indice: i32) -> *const c_char {
    let Some(app) = aplicativo(app) else {
        return ptr::null();
    };
    let Ok(indice) = usize::try_from(indice) else {
        return ptr::null();
    };
    app.codigos
        .get(indice)
        .map(|codigo| codigo.as_ptr())
        .unwrap_or(ptr::null())
}

/// O nome do idioma **no próprio idioma** — "English", "Português". É o que a lista mostra: quem
/// procura português procura "Português", e não "Portuguese".
#[unsafe(no_mangle)]
pub extern "C" fn zeebx_xbox_idioma_nome(app: *mut Aplicativo, indice: i32) -> *const c_char {
    let Some(app) = aplicativo(app) else {
        return ptr::null();
    };
    let Ok(indice) = usize::try_from(indice) else {
        return ptr::null();
    };
    app.nomes
        .get(indice)
        .map(|nome| nome.as_ptr())
        .unwrap_or(ptr::null())
}

/// O código do idioma em uso. Vale até a próxima chamada.
#[unsafe(no_mangle)]
pub extern "C" fn zeebx_xbox_idioma(app: *mut Aplicativo) -> *const c_char {
    let Some(app) = aplicativo(app) else {
        return ptr::null();
    };
    let atual = app.catalogo.current().to_string();
    app.texto = cstring(&atual);
    app.texto.as_ptr()
}

/// Passa a usar `codigo` e grava a escolha. 0 quando trocou; -1 quando o núcleo não tem esse
/// idioma — e aí nada muda, nem o idioma nem o arquivo.
#[unsafe(no_mangle)]
pub extern "C" fn zeebx_xbox_define_idioma(app: *mut Aplicativo, codigo: *const c_char) -> i32 {
    let Some(app) = aplicativo(app) else {
        return -1;
    };
    let Some(codigo) = c_string(codigo) else {
        return -1;
    };
    if !app.catalogo.select(&codigo) {
        app.define_erro("idioma desconhecido");
        return -1;
    }
    app.config.language = Some(app.catalogo.current().to_string());
    // A troca vale nesta sessão mesmo que a gravação falhe; o que se perde é ela na próxima.
    let _ = app.grava_config();
    // Quem troca de idioma troca também o nome dos idiomas na lista: "Português" vira "English".
    app.atualiza_listas_de_texto();
    0
}

/// O texto de `chave`, no idioma em uso. Vale até a próxima chamada.
///
/// Uma chave que falta aparece como ela mesma, e não vazia: quem mexe na GUI vê
/// `xbox.sources.title` na tela em vez de um botão em branco. É o mesmo que o desktop já faz.
#[unsafe(no_mangle)]
pub extern "C" fn zeebx_xbox_texto(app: *mut Aplicativo, chave: *const c_char) -> *const c_char {
    let Some(app) = aplicativo(app) else {
        return ptr::null();
    };
    let Some(chave) = c_string(chave) else {
        return ptr::null();
    };
    let texto = app.catalogo.get(&chave).to_string();
    app.texto = cstring(&texto);
    app.texto.as_ptr()
}

// ==================== As pastas de fora ====================
//
// A pasta de dentro do pacote — a que `zeebx_xbox_cria` recebe — é a que o jogador alcança pelo
// Portal do Dispositivo. As de fora são as que ele aponta com o controle: uma unidade removível,
// ou uma pasta dentro dela. Quem concede a leitura é o manifesto, não este lado.

/// Quantas pastas de fora a varredura procura, além da pasta de dentro do pacote.
#[unsafe(no_mangle)]
pub extern "C" fn zeebx_xbox_fontes(app: *mut Aplicativo) -> i32 {
    aplicativo(app)
        .map(|app| app.fontes.len() as i32)
        .unwrap_or(0)
}

/// A pasta de fora `indice`, para a tela de fontes mostrar. Nulo fora da faixa.
#[unsafe(no_mangle)]
pub extern "C" fn zeebx_xbox_fonte(app: *mut Aplicativo, indice: i32) -> *const c_char {
    let Some(app) = aplicativo(app) else {
        return ptr::null();
    };
    let Ok(indice) = usize::try_from(indice) else {
        return ptr::null();
    };
    app.fontes_c
        .get(indice)
        .map(|fonte| fonte.as_ptr())
        .unwrap_or(ptr::null())
}

/// Se a pasta de fora `indice` ainda está lá.
///
/// A unidade pode ter sido desplugada desde a última varredura, e a tela de fontes precisa dizer
/// isso em vez de listar uma pasta que não abre mais.
#[unsafe(no_mangle)]
pub extern "C" fn zeebx_xbox_fonte_existe(app: *mut Aplicativo, indice: i32) -> i32 {
    let Some(app) = aplicativo(app) else {
        return 0;
    };
    let Ok(indice) = usize::try_from(indice) else {
        return 0;
    };
    app.fontes
        .get(indice)
        .map(|fonte| fonte.is_dir() as i32)
        .unwrap_or(0)
}

/// Acrescenta uma pasta de fora, grava e varre de novo.
///
/// 0 quando entrou; -1 quando o caminho não é uma pasta que exista; -2 quando já estava lá; -3
/// quando a pasta existe e não abre. **O -3 existe porque o erro que ele evita é silencioso**: numa
/// unidade removível quem concede a leitura é a capacidade `removableStorage` do manifesto, e sem
/// ela a varredura devolveria zero jogos — a estante vazia de quem tem a unidade cheia.
#[unsafe(no_mangle)]
pub extern "C" fn zeebx_xbox_acrescenta_fonte(app: *mut Aplicativo, pasta: *const c_char) -> i32 {
    let Some(app) = aplicativo(app) else {
        return -1;
    };
    let Some(nova) = caminho(pasta) else {
        return -1;
    };
    if !nova.is_dir() {
        app.define_erro("a pasta de jogos não existe");
        return -1;
    }
    if app.fontes.iter().any(|fonte| mesma_pasta(fonte, &nova)) {
        return -2;
    }
    if std::fs::read_dir(&nova).is_err() {
        app.define_erro("não deu para abrir a pasta de jogos");
        return -3;
    }
    app.fontes.push(nova);
    app.config.roms_dirs = app.fontes.clone();
    let _ = app.grava_config();
    app.atualiza_listas_de_texto();
    // **Sem varredura aqui.** Quem acrescentou a pasta pede a varredura pela `..._varre_inicia` e
    // mostra a estante procurando; varrer dentro desta chamada é a janela parada de quem acabou de
    // escolher uma unidade cheia.
    0
}

/// Tira a pasta de fora `indice`, grava e varre de novo. 0 quando saiu; -1 fora da faixa.
#[unsafe(no_mangle)]
pub extern "C" fn zeebx_xbox_tira_fonte(app: *mut Aplicativo, indice: i32) -> i32 {
    let Some(app) = aplicativo(app) else {
        return -1;
    };
    let Ok(indice) = usize::try_from(indice) else {
        return -1;
    };
    if indice >= app.fontes.len() {
        return -1;
    }
    app.fontes.remove(indice);
    app.config.roms_dirs = app.fontes.clone();
    let _ = app.grava_config();
    app.atualiza_listas_de_texto();
    // A varredura também não acontece aqui, pela mesma razão do `acrescenta`.
    0
}

/// Um `CString` de um texto qualquer.
///
/// Um texto com byte nulo dentro vira vazio: nenhuma tradução de fábrica tem um, e devolver
/// ponteiro nulo obrigaria o C++ a conferir toda chamada de texto.
fn cstring(texto: &str) -> CString {
    CString::new(texto).unwrap_or_else(|_| CString::new("").expect("vazio"))
}

/// Se dois caminhos são a mesma pasta.
///
/// **Compara sem a barra do fim e sem maiúscula**, porque é assim que o Windows lê caminho: a GUI
/// devolve `E:\jogos\` onde o jogador escolheu `E:\jogos`, e o `PathBuf` guardaria as duas como
/// pastas diferentes — a estante mostraria o mesmo jogo duas vezes.
fn mesma_pasta(a: &Path, b: &Path) -> bool {
    let limpa = |caminho: &Path| {
        caminho
            .to_string_lossy()
            .trim_end_matches(['\\', '/'])
            .to_lowercase()
    };
    limpa(a) == limpa(b)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn branco_e_preto_do_rgb565_viram_bgra_cheio() {
        let pixels = [0u16, 0xffff, 0xf800];
        let bgra = bgra_de_rgb565(&pixels);
        assert_eq!(&bgra[0..4], &[0, 0, 0, 255]);
        assert_eq!(&bgra[4..8], &[255, 255, 255, 255]);
        assert_eq!(bgra[8], 0, "azul");
        assert_eq!(bgra[9], 0, "verde");
        assert_eq!(bgra[10], 255, "vermelho");
        assert_eq!(bgra[11], 255);
    }

    #[test]
    fn ponteiro_nulo_nao_cria() {
        assert!(zeebx_xbox_cria(ptr::null(), ptr::null()).is_null());
        let vazio = CString::new("").unwrap();
        assert!(zeebx_xbox_cria(vazio.as_ptr(), vazio.as_ptr()).is_null());
    }

    #[test]
    fn a_taxa_e_a_do_core_libretro() {
        assert_eq!(zeebx_xbox_taxa_de_audio(), 44_100);
    }

    #[test]
    fn os_eixos_do_header_batem_com_o_limite_daqui() {
        // O `#define ZEEBX_EIXOS` do header não entra no Rust; este teste é o par.
        assert_eq!(ZEEBX_EIXOS, 4);
    }

    #[test]
    fn os_nomes_dos_botoes_seguem_o_controle() {
        assert_eq!(zeebx_xbox_botao_por_nome(c"b1".as_ptr()), 0);
        assert_eq!(
            zeebx_xbox_botao_por_nome(c"up".as_ptr()) as usize,
            Pad::button_by_name("up").unwrap()
        );
        assert_eq!(
            zeebx_xbox_botao_por_nome(c"home".as_ptr()),
            zeebx_xbox_botao_por_nome(c"back".as_ptr())
        );
        assert_eq!(zeebx_xbox_botao_por_nome(ptr::null()), -1);
        assert_eq!(zeebx_xbox_botao_por_nome(c"nao-existe".as_ptr()), -1);
    }

    fn pastas_de_teste(nome: &str) -> (PathBuf, CString, CString) {
        let raiz = std::env::temp_dir().join(format!("zeebx-xbox-{nome}-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&raiz);
        let roms = raiz.join("roms");
        let dados = raiz.join("LocalState");
        std::fs::create_dir_all(&roms).unwrap();
        std::fs::create_dir_all(&dados).unwrap();
        let roms_c = CString::new(roms.to_str().unwrap()).unwrap();
        let dados_c = CString::new(dados.to_str().unwrap()).unwrap();
        (raiz, roms_c, dados_c)
    }

    #[test]
    fn pasta_vazia_lista_zero_e_nao_comeca_jogo() {
        let (raiz, roms, dados) = pastas_de_teste("vazia");
        let app = zeebx_xbox_cria(roms.as_ptr(), dados.as_ptr());
        assert!(!app.is_null());
        assert_eq!(zeebx_xbox_varre(app), 0);
        assert!(zeebx_xbox_titulo(app, 0).is_null());
        assert!(zeebx_xbox_caminho(app, 0).is_null());
        assert_eq!(zeebx_xbox_comeca(app, 0), -1);
        assert!(!zeebx_xbox_erro(app).is_null());
        assert_eq!(zeebx_xbox_passo(app), SEM_SESSAO);
        assert!(zeebx_xbox_quadro(app, ptr::null_mut(), ptr::null_mut()).is_null());
        assert!(zeebx_xbox_audio(app, ptr::null_mut()).is_null());
        zeebx_xbox_botao(app, 0, 1);
        zeebx_xbox_eixo(app, 0, 40);
        zeebx_xbox_volume(app, 50);
        assert_eq!(zeebx_xbox_relogio_ms(app), 0);
        assert_eq!(zeebx_xbox_quadros(app), 0);
        assert_eq!(zeebx_xbox_salva_estado(app, 0), -1);
        assert_eq!(zeebx_xbox_carrega_estado(app, 0), 1, "slot ausente devolve 1");
        assert_eq!(zeebx_xbox_tem_estado(app, 0), 0);
        assert_eq!(zeebx_xbox_salva_estado(app, 99), -1, "slot fora da faixa");
        zeebx_xbox_para(app);
        zeebx_xbox_destroi(app);
        let _ = std::fs::remove_dir_all(raiz);
    }

    #[test]
    fn um_save_gravado_no_slot_aparece_como_presente() {
        let (raiz, roms, dados) = pastas_de_teste("slot");
        let app = zeebx_xbox_cria(roms.as_ptr(), dados.as_ptr());
        // Sem sessão não há o que salvar — mas o slot vazio é ausência, e não erro.
        assert_eq!(zeebx_xbox_tem_estado(app, 3), 0);
        assert_eq!(zeebx_xbox_carrega_estado(app, 3), 1);
        // Um arquivo qualquer no lugar do save faz o `tem_estado` responder que sim; o restore
        // é que recusa. É o que a GUI precisa para desenhar o botão ligado.
        let slot = dados_do_slot(&dados, 3);
        std::fs::create_dir_all(slot.parent().unwrap()).unwrap();
        std::fs::write(&slot, b"nao e um save").unwrap();
        assert_eq!(zeebx_xbox_tem_estado(app, 3), 1);
        zeebx_xbox_destroi(app);
        let _ = std::fs::remove_dir_all(raiz);
    }

    fn dados_do_slot(dados: &CString, slot: u32) -> PathBuf {
        PathBuf::from(dados.to_str().unwrap())
            .join("saves")
            .join(format!("zeebx-slot-{slot}.zbx"))
    }

    /// O texto de uma chave, já copiado: o ponteiro da ABI vale só até a chamada seguinte.
    fn texto(app: *mut Aplicativo, chave: &str) -> String {
        let chave = CString::new(chave).unwrap();
        let ponteiro = zeebx_xbox_texto(app, chave.as_ptr());
        assert!(!ponteiro.is_null(), "`{chave:?}` sem texto");
        unsafe { CStr::from_ptr(ponteiro) }.to_string_lossy().into_owned()
    }

    /// O código do idioma em uso, já copiado.
    fn codigo_atual(app: *mut Aplicativo) -> String {
        let ponteiro = zeebx_xbox_idioma(app);
        assert!(!ponteiro.is_null());
        unsafe { CStr::from_ptr(ponteiro) }.to_string_lossy().into_owned()
    }

    /// A pasta de fora `indice`, já copiada.
    fn fonte(app: *mut Aplicativo, indice: i32) -> String {
        let ponteiro = zeebx_xbox_fonte(app, indice);
        assert!(!ponteiro.is_null(), "fonte {indice} sem caminho");
        unsafe { CStr::from_ptr(ponteiro) }.to_string_lossy().into_owned()
    }

    #[test]
    fn o_catalogo_do_nucleo_e_quem_da_os_textos_da_gui() {
        let (raiz, roms, dados) = pastas_de_teste("idiomas");
        let app = zeebx_xbox_cria(roms.as_ptr(), dados.as_ptr());
        assert!(zeebx_xbox_idiomas(app) >= 4, "os quatro idiomas de fábrica");

        let mut codigos = Vec::new();
        let mut nomes = Vec::new();
        for indice in 0..zeebx_xbox_idiomas(app) {
            let codigo = zeebx_xbox_idioma_codigo(app, indice);
            let nome = zeebx_xbox_idioma_nome(app, indice);
            assert!(!codigo.is_null(), "idioma {indice} sem código");
            assert!(!nome.is_null(), "idioma {indice} sem nome");
            codigos.push(unsafe { CStr::from_ptr(codigo) }.to_string_lossy().into_owned());
            nomes.push(unsafe { CStr::from_ptr(nome) }.to_string_lossy().into_owned());
        }
        for esperado in ["en", "es", "es-MX", "pt-BR"] {
            assert!(codigos.iter().any(|codigo| codigo == esperado), "falta {esperado}");
        }
        // O nome sai no próprio idioma — "English", "Português (Brasil)" —, que é o que a lista
        // mostra: quem procura português procura "Português", e não "Portuguese".
        assert!(nomes.iter().any(|nome| nome == "English"));
        assert!(
            nomes.iter().any(|nome| nome.starts_with("Português")),
            "os nomes são {nomes:?}"
        );

        // **O inglês é o padrão deste alvo**, e o texto sai traduzido em vez da chave.
        assert_eq!(codigo_atual(app), "en");
        assert_ne!(texto(app, "library.rescan"), "library.rescan");
        // Uma chave que não existe aparece como ela mesma: interface feia, nunca vazia.
        assert_eq!(texto(app, "xbox.chave.que.nao.existe"), "xbox.chave.que.nao.existe");

        // Fora da faixa não há idioma.
        assert_eq!(zeebx_xbox_idioma_codigo(app, 99) as usize, 0);
        assert_eq!(zeebx_xbox_idioma_nome(app, -1) as usize, 0);
        assert_eq!(zeebx_xbox_define_idioma(app, ptr::null()), -1);

        zeebx_xbox_destroi(app);
        let _ = std::fs::remove_dir_all(raiz);
    }

    #[test]
    fn trocar_o_idioma_grava_e_sobrevive_ao_fechar() {
        let (raiz, roms, dados) = pastas_de_teste("troca-de-idioma");
        let app = zeebx_xbox_cria(roms.as_ptr(), dados.as_ptr());
        assert_eq!(codigo_atual(app), "en");
        let em_ingles = texto(app, "library.rescan");

        assert_eq!(zeebx_xbox_define_idioma(app, c"pt-BR".as_ptr()), 0);
        assert_eq!(codigo_atual(app), "pt-BR");
        assert_ne!(texto(app, "library.rescan"), em_ingles, "o texto muda com o idioma");

        // Um idioma que o núcleo não tem não troca nada.
        assert_eq!(zeebx_xbox_define_idioma(app, c"tlh".as_ptr()), -1);
        assert_eq!(codigo_atual(app), "pt-BR");
        zeebx_xbox_destroi(app);

        // O `settings.json` do LocalState guardou a escolha...
        let arquivo = PathBuf::from(dados.to_str().unwrap()).join("settings.json");
        assert!(arquivo.is_file(), "as preferências ficam no LocalState");
        // ...e a abertura seguinte já nasce em português.
        let app = zeebx_xbox_cria(roms.as_ptr(), dados.as_ptr());
        assert_eq!(codigo_atual(app), "pt-BR");
        zeebx_xbox_destroi(app);
        let _ = std::fs::remove_dir_all(raiz);
    }

    #[test]
    fn uma_pasta_de_fora_entra_na_varredura_e_fica_gravada() {
        let (raiz, roms, dados) = pastas_de_teste("fontes");
        let unidade = raiz.join("unidade").join("Zeebo");
        std::fs::create_dir_all(&unidade).unwrap();
        let unidade_c = CString::new(unidade.to_str().unwrap()).unwrap();
        let fantasma = CString::new(raiz.join("nao-existe").to_str().unwrap()).unwrap();
        // A barra do fim é como a GUI devolve o caminho de uma unidade.
        let com_barra = CString::new(format!("{}\\", unidade.to_str().unwrap())).unwrap();

        let app = zeebx_xbox_cria(roms.as_ptr(), dados.as_ptr());
        assert_eq!(zeebx_xbox_fontes(app), 0);
        assert_eq!(zeebx_xbox_acrescenta_fonte(app, fantasma.as_ptr()), -1, "pasta que não existe");
        assert_eq!(zeebx_xbox_acrescenta_fonte(app, ptr::null()), -1);
        assert_eq!(zeebx_xbox_acrescenta_fonte(app, unidade_c.as_ptr()), 0);
        assert_eq!(zeebx_xbox_fontes(app), 1);
        assert_eq!(fonte(app, 0), unidade.to_string_lossy().as_ref());
        assert_eq!(zeebx_xbox_fonte_existe(app, 0), 1);
        assert_eq!(
            zeebx_xbox_acrescenta_fonte(app, unidade_c.as_ptr()),
            -2,
            "a mesma pasta duas vezes"
        );
        assert_eq!(zeebx_xbox_acrescenta_fonte(app, com_barra.as_ptr()), -2, "e com a barra do fim");
        zeebx_xbox_destroi(app);

        // A abertura seguinte já encontra a fonte.
        let app = zeebx_xbox_cria(roms.as_ptr(), dados.as_ptr());
        assert_eq!(zeebx_xbox_fontes(app), 1);
        assert_eq!(fonte(app, 0), unidade.to_string_lossy().as_ref());
        assert_eq!(zeebx_xbox_tira_fonte(app, 1), -1, "fora da faixa");
        assert_eq!(zeebx_xbox_tira_fonte(app, 0), 0);
        assert_eq!(zeebx_xbox_fontes(app), 0);
        zeebx_xbox_destroi(app);

        // E a remoção também fica gravada.
        let app = zeebx_xbox_cria(roms.as_ptr(), dados.as_ptr());
        assert_eq!(zeebx_xbox_fontes(app), 0);
        zeebx_xbox_destroi(app);
        let _ = std::fs::remove_dir_all(raiz);
    }

    #[test]
    fn uma_fonte_desplugada_aparece_como_ausente_sem_derrubar_a_varredura() {
        let (raiz, roms, dados) = pastas_de_teste("desplugada");
        let unidade = raiz.join("unidade");
        std::fs::create_dir_all(&unidade).unwrap();
        let unidade_c = CString::new(unidade.to_str().unwrap()).unwrap();

        let app = zeebx_xbox_cria(roms.as_ptr(), dados.as_ptr());
        assert_eq!(zeebx_xbox_acrescenta_fonte(app, unidade_c.as_ptr()), 0);
        // A unidade sai do ar com o aplicativo aberto — o caso do HD desligado no meio.
        std::fs::remove_dir_all(&unidade).unwrap();
        assert_eq!(zeebx_xbox_fonte_existe(app, 0), 0);
        assert_eq!(zeebx_xbox_fontes(app), 1, "a pasta continua na lista, para o jogador ver");
        assert_eq!(zeebx_xbox_varre(app), 0);
        zeebx_xbox_destroi(app);
        let _ = std::fs::remove_dir_all(raiz);
    }

    /// Um BMP de 24 bits, do jeito que os `.mif` guardam: sem compressão e com as linhas de baixo
    /// para cima. Dois pixels — vermelho e azul — para a cor provar que os canais não trocaram.
    fn bmp_vermelho_e_azul() -> Vec<u8> {
        let mut bmp = vec![0u8; 54 + 8];
        bmp[0..2].copy_from_slice(b"BM");
        bmp[2..6].copy_from_slice(&(62u32).to_le_bytes());
        bmp[10..14].copy_from_slice(&(54u32).to_le_bytes());
        bmp[14..18].copy_from_slice(&(40u32).to_le_bytes());
        bmp[18..22].copy_from_slice(&(2u32).to_le_bytes());
        bmp[22..26].copy_from_slice(&(1u32).to_le_bytes());
        bmp[26..28].copy_from_slice(&(1u16).to_le_bytes());
        bmp[28..30].copy_from_slice(&(24u16).to_le_bytes());
        bmp[34..38].copy_from_slice(&(8u32).to_le_bytes());
        // BMP grava azul-verde-vermelho, e a linha é completada até quatro bytes.
        bmp[54..57].copy_from_slice(&[0, 0, 255]);
        bmp[57..60].copy_from_slice(&[255, 0, 0]);
        bmp
    }

    /// Um `.mif` com uma imagem, montado à mão.
    ///
    /// **O layout é o que os 24 arquivos reais confirmaram**, e é o que `MifFile::parse` lê: o
    /// cabeçalho de 0x20 bytes, a tabela de seções com `n + 1` limites (o último fecha a última
    /// seção), e uma seção de imagem que começa com o `u16` do tamanho do próprio cabeçalho, o
    /// MIME terminado em zero e o arquivo logo depois.
    fn mif_com_imagem(imagem: &[u8]) -> Vec<u8> {
        let secao = 0x28u32;
        let mut mif = vec![0u8; secao as usize];
        mif[0..2].copy_from_slice(&0x0011u16.to_le_bytes());
        mif[2..4].copy_from_slice(&1u16.to_le_bytes());
        mif[4..6].copy_from_slice(&1u16.to_le_bytes());
        mif[8..12].copy_from_slice(&0x20u32.to_le_bytes());
        mif[16..20].copy_from_slice(&0x20u32.to_le_bytes());
        mif[20..24].copy_from_slice(&1u32.to_le_bytes());
        mif[24..28].copy_from_slice(&secao.to_le_bytes());
        mif[28..32].copy_from_slice(&(12 + imagem.len() as u32).to_le_bytes());
        mif[32..36].copy_from_slice(&secao.to_le_bytes());
        let fim = secao + 12 + imagem.len() as u32;
        mif[36..40].copy_from_slice(&fim.to_le_bytes());
        mif.extend_from_slice(&12u16.to_le_bytes());
        // "image/bmp\0" são os dez bytes que, com o `u16` acima, fecham o cabeçalho de 12 — o
        // arquivo começa logo depois. Um par de zeros a mais aqui empurra a imagem e o leitor
        // entrega dois bytes de lixo no lugar do `BM`: foi assim que este teste nasceu torto.
        mif.extend_from_slice(b"image/bmp\0");
        mif.extend_from_slice(imagem);
        mif
    }

    /// Cria `roms\<nome>.mod` ao lado do `.mif` com a imagem dada.
    fn planta_um_jogo(roms: &CString, nome: &str, imagem: &[u8]) {
        let pasta = PathBuf::from(roms.to_str().unwrap());
        std::fs::write(pasta.join(format!("{nome}.mod")), b"nao e um modulo de verdade").unwrap();
        std::fs::write(pasta.join(format!("{nome}.mif")), mif_com_imagem(imagem)).unwrap();
    }

    /// Um `.mif` sem imagem nenhuma: cabeçalho, tabela de uma entrada e nenhuma seção.
    fn mif_sem_imagem() -> Vec<u8> {
        let mut mif = vec![0u8; 0x24];
        mif[0..2].copy_from_slice(&0x0011u16.to_le_bytes());
        mif[2..4].copy_from_slice(&1u16.to_le_bytes());
        mif[4..6].copy_from_slice(&1u16.to_le_bytes());
        mif[8..12].copy_from_slice(&0x20u32.to_le_bytes());
        mif[16..20].copy_from_slice(&0x20u32.to_le_bytes());
        // O único limite é o fim do arquivo: sem seções, não há imagem para decodificar.
        mif[32..36].copy_from_slice(&0x24u32.to_le_bytes());
        mif
    }

    #[test]
    fn a_estante_mostra_a_arte_que_o_jogo_traz_dentro_do_mif() {
        let (raiz, roms, dados) = pastas_de_teste("arte");
        let pasta = PathBuf::from(roms.to_str().unwrap());
        planta_um_jogo(&roms, "Com Arte", &bmp_vermelho_e_azul());
        // **O cartão sem capa, que é o caso comum.** Este tem `.mif`, e o `.mif` dele não traz
        // imagem nenhuma.
        //
        // Um `.mod` sem `.mif` nenhum não serve para este caso, e a razão é do núcleo: sem um
        // manifesto de nome casado, a busca aceita qualquer `.mif` da pasta — e acharia o do jogo
        // ao lado. Ver o `manifest_paths`, em `src/library.rs`.
        std::fs::write(pasta.join("Sem Arte.mod"), b"nao e um modulo de verdade").unwrap();
        std::fs::write(pasta.join("Sem Arte.mif"), mif_sem_imagem()).unwrap();

        let app = zeebx_xbox_cria(roms.as_ptr(), dados.as_ptr());
        // Os dois são jogos, e a ordem é a dos títulos.
        assert_eq!(zeebx_xbox_varre(app), 2);

        // A arte sai em BGRA8, e o vermelho do BMP chega vermelho.
        let mut largura = 0u32;
        let mut altura = 0u32;
        let pixels = zeebx_xbox_imagem(app, 0, &mut largura, &mut altura);
        assert!(!pixels.is_null(), "o ícone do `.mif` devia estar aqui");
        assert_eq!((largura, altura), (2, 1));
        let pixels = unsafe { std::slice::from_raw_parts(pixels, 8) };
        assert_eq!(pixels, &[0, 0, 255, 255, 255, 0, 0, 255], "vermelho e azul, na ordem do BMP");

        // Sem arte naquele jogo, e fora da faixa, o ponteiro nulo é o contrato: a GUI desenha o
        // cartão sem capa em vez de um buraco.
        assert!(zeebx_xbox_imagem(app, 1, &mut largura, &mut altura).is_null());
        assert!(zeebx_xbox_imagem(app, 9, &mut largura, &mut altura).is_null());
        assert!(zeebx_xbox_imagem(app, -1, &mut largura, &mut altura).is_null());
        // Os ponteiros de tamanho são opcionais, como os do quadro.
        assert!(!zeebx_xbox_imagem(app, 0, ptr::null_mut(), ptr::null_mut()).is_null());

        zeebx_xbox_destroi(app);
        let _ = std::fs::remove_dir_all(raiz);
    }

    #[test]
    fn o_alfa_do_icone_sai_premultiplicado() {
        let arte = Image {
            width: 2,
            height: 1,
            // Meio transparente: sem premultiplicar, a metade branca sairia branca e o XAML
            // comporia a borda clara por cima do cartão.
            rgba: vec![255, 255, 255, 128, 200, 100, 50, 0],
        };
        let pronta = imagem_premultiplicada(&arte);
        assert_eq!((pronta.largura, pronta.altura), (2, 1));
        assert_eq!(&pronta.bgra[0..4], &[128, 128, 128, 128], "branco pela metade");
        assert_eq!(&pronta.bgra[4..8], &[0, 0, 0, 0], "e o que é transparente some");
    }

    /// A varredura de fora é a que **não** congela a estante: a chamada volta na hora e a lista
    /// entra na colheita.
    #[test]
    fn a_varredura_de_fora_volta_na_hora_e_entrega_a_lista() {
        let (raiz, roms, dados) = pastas_de_teste("varredura-de-fora");
        planta_um_jogo(&roms, "Um Jogo", &bmp_vermelho_e_azul());
        let app = zeebx_xbox_cria(roms.as_ptr(), dados.as_ptr());

        // Sem varredura nenhuma não há o que colher.
        assert_eq!(zeebx_xbox_varre_colhe(app), -1);
        assert_eq!(zeebx_xbox_varre_inicia(app), 0);
        // Uma segunda, com a primeira andando, é recusada em vez de virar dois fios.
        assert_eq!(zeebx_xbox_varre_inicia(app), -1);

        // `0` é "ainda anda", `1` é "entrou" — e o título já sai da lista nova.
        let mut colhido = 0;
        for _ in 0..500 {
            colhido = zeebx_xbox_varre_colhe(app);
            if colhido != 0 {
                break;
            }
            std::thread::sleep(std::time::Duration::from_millis(10));
        }
        assert_eq!(colhido, 1, "a varredura não terminou");
        assert!(!zeebx_xbox_titulo(app, 0).is_null());
        assert_eq!(zeebx_xbox_varre_colhe(app), -1, "colhida uma vez, acabou");

        zeebx_xbox_destroi(app);
        let _ = std::fs::remove_dir_all(raiz);
    }

    /// **A única prova deste binário que mexe no interruptor de processo do JIT**, e é uma só de
    /// propósito: `permite_jit` é global, as provas correm em paralelo, e duas mexendo nele fariam o
    /// resultado de uma depender do instante da outra.
    #[test]
    fn a_abi_do_jit_chega_ao_nucleo_e_o_relatorio_sabe_qual_nucleo_e() {
        let antes = zeebx::cpu::jit_permitido();

        // Sem sessão não há núcleo para nomear, e a resposta é nula — não um texto inventado.
        assert!(zeebx_xbox_nucleo(ptr::null_mut()).is_null());

        zeebx_xbox_permite_jit(0);
        assert!(!zeebx::cpu::jit_permitido(), "zero tem de chegar como 'não pode'");
        zeebx_xbox_permite_jit(1);
        assert!(zeebx::cpu::jit_permitido(), "qualquer coisa menos zero é 'pode'");

        zeebx_xbox_permite_jit(i32::from(antes));
    }
}
