#ifndef ZEEBX_XBOX_H
#define ZEEBX_XBOX_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * O Zeebx no Xbox Series S|X, por dentro de um aplicativo UWP.
 *
 * Este header é o contrato entre o núcleo em Rust e o aplicativo em C++/WinRT, do mesmo modo
 * que `zeebx_ios.h` é o contrato com a UIKit. O aplicativo tem o ciclo de vida, a tela (D3D11),
 * a placa de som (XAudio2) e o controle (Windows.Gaming.Input); aqui só se pede uma varredura,
 * um passo, o quadro, o som e se devolve botão.
 *
 * **A chamada é da linha da interface**, uma só, como no iOS. Não é seguro chamar de outra.
 */

/* O que `zeebx_xbox_passo` devolve. Os números são o contrato com o C++. */
#define ZEEBX_SEM_SESSAO (-1)
#define ZEEBX_QUADRO 0
#define ZEEBX_RODANDO 1
#define ZEEBX_ADIANTADO 2
#define ZEEBX_PAROU 3
/*
 * A volta andou, o som saiu, e a tela é a mesma que já foi publicada.
 *
 * **É o caso comum, e não um caso de canto.** O Zeebo desenha a 30 quadros por segundo dentro de
 * uma tela que compõe a 60: na volta sem desenho novo o `zeebx_xbox_quadro` devolveria os mesmos
 * pixels, depois de uma conversão de 1,2 MB, e o `Present` repetiria o quadro que já está lá. Quem
 * recebe este código não apresenta — a imagem anterior continua na tela —, mas o som já foi
 * entregue por `zeebx_xbox_audio` e não pode ficar para trás.
 */
#define ZEEBX_SEM_QUADRO_NOVO 4

/*
 * Quantos eixos o núcleo aceita: 0 = X do manche esquerdo, 1 = Y; 2 = X do direito, 3 = Y.
 * Os gatilhos (ZL/ZR/L2/R2) não são eixos: são botões, e entram por `zeebx_xbox_botao` —
 * é assim que o controle do Zeebo é, e é o que o mapa do Android e do iOS já faz.
 */
#define ZEEBX_EIXOS 4

/* Ponteiro opaco. O C++ não olha para dentro. */
typedef struct ZeebxXbox ZeebxXbox;

/*
 * `dados` é o LocalState do pacote (ApplicationData::Current().LocalFolder()), onde ficam os
 * saves, o cache e o aparelho virtual. `roms` é a pasta que o jogador enxerga — no Xbox, a
 * `LocalFolder` também, mas separada de propósito: num build de PC ela pode ser outra coisa.
 * Os dois são caminhos absolutos em UTF-8, terminados em zero.
 *
 * Devolve nulo se um dos ponteiros for nulo ou vazio.
 */
ZeebxXbox *zeebx_xbox_cria(const char *roms, const char *dados);
void zeebx_xbox_destroi(ZeebxXbox *app);

/*
 * ==================== A memória executável ====================
 *
 * **Este par existe por causa de uma pergunta que só o aparelho responde.** O emulador tem dois
 * núcleos: o recompilador (Dynarmic), que escreve código de máquina e salta para ele, e o
 * interpretador. No desktop, no Android e no Libretro a escolha é feita na compilação — há página
 * executável, e o núcleo é o recompilador. Num AppContainer de Xbox ela depende do processo.
 *
 * E a diferença não é de velocidade: **um recompilador que não consegue alocar a página não falha
 * ao compilar, falha ao executar** — no meio de um jogo, sem caminho de volta. Por isso o
 * aplicativo sonda o caminho de verdade no arranque (`Jit.cpp`: aloca RW, escreve, troca para RX e
 * chama) e conta o resultado para o núcleo **antes** do primeiro `zeebx_xbox_comeca`.
 *
 * Quem não chama `zeebx_xbox_permite_jit` fica com o otimista, que é o de todos os outros
 * frontends: o recompilador.
 */

/* Diz se este processo pode executar a memória que ele mesmo escreve. Zero é "não pode". */
void zeebx_xbox_permite_jit(int32_t permitido);

/*
 * O nome do núcleo da sessão aberta: "dynarmic" ou "interpretador". Nulo sem sessão.
 *
 * É a linha do relatório: no Xbox o mesmo pacote roda recompilado ou interpretado conforme o
 * aparelho, e sem esta resposta o log do console não diz qual dos dois está rodando — que é a
 * primeira coisa a saber quando a velocidade não é a esperada. O texto é do núcleo, e não uma
 * segunda tabela daqui: é o mesmo nome que a linha de INFO dele escreve.
 */
const char *zeebx_xbox_nucleo(ZeebxXbox *app);

/* Quantos jogos a última varredura achou. Varre de novo a pasta de ROMs. */
int32_t zeebx_xbox_varre(ZeebxXbox *app);

/* Quantos jogos a lista corrente tem. **Não varre**: é o número que a GUI pede depois de colher
 * uma varredura de fora. */
int32_t zeebx_xbox_jogos(ZeebxXbox *app);

/*
 * A varredura de fora: começa num fio do núcleo e volta na hora.
 *
 * **É por estas duas que a estante não congela.** `zeebx_xbox_varre` devolve quando terminou, e uma
 * unidade cheia é segundo de janela parada — no arranque, e cada vez que o jogador pede de novo.
 * Quem chama `..._inicia` mostra a estante procurando e pergunta `..._colhe` a cada volta.
 *
 * 0 quando o fio saiu; -1 quando já havia uma varredura andando, ou quando o ponteiro é nulo.
 */
int32_t zeebx_xbox_varre_inicia(ZeebxXbox *app);

/*
 * Colhe a varredura de fora. 1 quando a lista entrou; 0 quando ainda anda; -1 quando não havia
 * nenhuma. A lista passa a valer aqui, nesta linha — o fio não toca no aplicativo.
 */
int32_t zeebx_xbox_varre_colhe(ZeebxXbox *app);

/* Título do jogo `indice`, válido até a próxima varredura. Nulo se o índice não existe. */
const char *zeebx_xbox_titulo(ZeebxXbox *app, int32_t indice);

/* Caminho do arquivo do jogo `indice`, para a GUI mostrar a extensão. Nulo se não existe. */
const char *zeebx_xbox_caminho(ZeebxXbox *app, int32_t indice);

/*
 * A arte do jogo `indice`: a capa que o jogador pôs ao lado do arquivo, com o mesmo nome e
 * extensão de imagem, ou o ícone que o próprio jogo traz dentro do `.mif`.
 *
 * BGRA8 **premultiplicado** — é o que o `SoftwareBitmapSource` recebe pronto. `largura` e `altura`
 * podem ser nulos. Ponteiro nulo quando o jogo não tem arte, quando o índice não existe, ou quando
 * a imagem é de um formato que o núcleo não lê; a GUI desenha o cartão sem capa, e não um buraco.
 *
 * O ponteiro vale enquanto o aplicativo viver e a lista da estante não for trocada por outra
 * varredura. Vale a pena saber o que ele **não** é: os ícones de fábrica são pequenos — os `.mif`
 * guardam de 16×16 a 65×42, e só um jogo conhecido passa disso, com 192×192. Um cartão que estica
 * isso a 300 pixels de largura mostra o que o arquivo tem, e nenhum milagre.
 */
const uint8_t *zeebx_xbox_imagem(ZeebxXbox *app, int32_t indice, uint32_t *largura, uint32_t *altura);

/*
 * Começa o jogo `indice`. 0 quando a sessão subiu; -1 quando não, e a razão fica em
 * `zeebx_xbox_erro` até a próxima chamada que a substitua.
 */
int32_t zeebx_xbox_comeca(ZeebxXbox *app, int32_t indice);
void zeebx_xbox_para(ZeebxXbox *app);

/*
 * Uma volta do laço: entrega o controle, avança o jogo o tempo real que passou e publica o
 * quadro e o som. A chamada é da linha da interface; não é segura de outra linha.
 */
int32_t zeebx_xbox_passo(ZeebxXbox *app);

/*
 * O quadro corrente, em BGRA8, com a origem no canto superior esquerdo — o que o
 * DXGI_FORMAT_B8G8R8A8_UNORM pede. O ponteiro vale até o próximo `zeebx_xbox_passo` ou
 * `zeebx_xbox_para`. `largura` e `altura` podem ser nulos. Devolve nulo quando não há quadro.
 */
const uint8_t *zeebx_xbox_quadro(ZeebxXbox *app, uint32_t *largura, uint32_t *altura);

/*
 * O som devido desde a volta anterior, em estéreo intercalado, 16 bits com sinal e 44100 Hz —
 * a mesma taxa que o core Libretro declara, e o que o XAudio2 espera de um WAVE_FORMAT_PCM.
 *
 * O ponteiro vale até o próximo `zeebx_xbox_passo`. `quadros` recebe o número de quadros de
 * áudio (um por canal), não o número de amostras. Devolve nulo quando não há som.
 */
const int16_t *zeebx_xbox_audio(ZeebxXbox *app, uint32_t *quadros);

/* A taxa do áudio, para a placa montar o formato. Hoje 44100. */
uint32_t zeebx_xbox_taxa_de_audio(void);

/* `indice` é a posição em `BUTTON_NAMES` do núcleo (`b1` é 0, `up` é 12). `apertado` é 0 ou 1. */
void zeebx_xbox_botao(ZeebxXbox *app, uint32_t indice, int32_t apertado);

/* Eixo do manche, centrado em zero. `indice` vai de 0 a `ZEEBX_EIXOS - 1`. O curso é -128..=128. */
void zeebx_xbox_eixo(ZeebxXbox *app, uint32_t indice, int32_t valor);

/* -1 quando o nome não é botão do Zeebo. */
int32_t zeebx_xbox_botao_por_nome(const char *nome);

/* Volume de 0 a 100. Vale para a próxima volta. */
void zeebx_xbox_volume(ZeebxXbox *app, uint32_t volume);

/* Texto da última falha, ou string vazia. Vale até a próxima chamada que o substitua. */
const char *zeebx_xbox_erro(ZeebxXbox *app);

/* O relógio virtual da sessão, em milissegundos. Zero sem sessão. */
uint32_t zeebx_xbox_relogio_ms(ZeebxXbox *app);

/* Os contadores que a GUI mostra no canto: quadros apresentados e instruções executadas. */
uint64_t zeebx_xbox_quadros(ZeebxXbox *app);
uint64_t zeebx_xbox_instrucoes(ZeebxXbox *app);

/*
 * Save state no `slot` (0..=9), gravado em `dados/saves/zeebx-slot-N.zbx`.
 * 0 quando deu certo; -1 quando não, com a razão em `zeebx_xbox_erro`.
 * `zeebx_xbox_carrega_estado` devolve 1 quando o slot não existe.
 */
int32_t zeebx_xbox_salva_estado(ZeebxXbox *app, uint32_t slot);
int32_t zeebx_xbox_carrega_estado(ZeebxXbox *app, uint32_t slot);

/* Se o slot tem save, para a GUI desenhar o botão ligado. */
int32_t zeebx_xbox_tem_estado(ZeebxXbox *app, uint32_t slot);

/*
 * ==================== A língua da interface ====================
 *
 * **A GUI deste alvo não guarda texto próprio.** Ela pede a chave, e o núcleo responde do catálogo
 * que o desktop usa — os `assets/lang/*.json` embutidos, mais o que o jogador puser em
 * `LocalState\lang`. `zeebx_xbox_texto` é a chamada que faz a tela inteira falar três línguas sem
 * uma segunda tabela de traduções para manter em dia.
 *
 * O inglês é o padrão, e a escolha fica em `LocalState\settings.json`.
 */

/* Quantos idiomas o catálogo tem. */
int32_t zeebx_xbox_idiomas(ZeebxXbox *app);

/* O código ("en", "pt-BR") e o nome ("English", "Português") do idioma `indice`. */
const char *zeebx_xbox_idioma_codigo(ZeebxXbox *app, int32_t indice);
const char *zeebx_xbox_idioma_nome(ZeebxXbox *app, int32_t indice);

/* O código do idioma em uso. Vale até a próxima chamada. */
const char *zeebx_xbox_idioma(ZeebxXbox *app);

/* Passa a usar `codigo` e grava. 0 quando trocou; -1 quando o núcleo não tem esse idioma. */
int32_t zeebx_xbox_define_idioma(ZeebxXbox *app, const char *codigo);

/*
 * O texto de `chave`, no idioma em uso e em UTF-8. Vale até a próxima chamada — quem guarda o
 * ponteiro guarda lixo. Uma chave que falta volta como ela mesma, e não vazia.
 */
const char *zeebx_xbox_texto(ZeebxXbox *app, const char *chave);

/*
 * ==================== As pastas de fora ====================
 *
 * A pasta de dentro do pacote vem no `zeebx_xbox_cria`. Estas são as que o jogador aponta com o
 * controle: um HD externo, um pendrive, uma pasta dentro deles. Quem concede a leitura é a
 * capacidade `removableStorage` do manifesto — deste lado elas são pastas como outra qualquer, e a
 * varredura soma todas.
 */

/* Quantas pastas de fora a varredura procura. */
int32_t zeebx_xbox_fontes(ZeebxXbox *app);

/* O caminho da pasta `indice`. Nulo fora da faixa. */
const char *zeebx_xbox_fonte(ZeebxXbox *app, int32_t indice);

/* Se a pasta `indice` ainda está lá: a unidade pode ter sido desplugada desde a última varredura. */
int32_t zeebx_xbox_fonte_existe(ZeebxXbox *app, int32_t indice);

/*
 * Acrescenta a pasta e grava. **A varredura não acontece aqui**: quem acrescentou pede a de fora
 * (`..._varre_inicia`) e mostra a estante procurando.
 *
 * 0 quando entrou; -1 pasta que não existe; -2 já estava na lista; -3 existe e não abre — falta de
 * permissão, que é o que acontece com uma unidade removível sem a capacidade no manifesto.
 */
int32_t zeebx_xbox_acrescenta_fonte(ZeebxXbox *app, const char *caminho);

/* Tira a pasta `indice` e grava, também sem varrer. 0 quando saiu; -1 fora da faixa. */
int32_t zeebx_xbox_tira_fonte(ZeebxXbox *app, int32_t indice);

#ifdef __cplusplus
}
#endif

#endif
