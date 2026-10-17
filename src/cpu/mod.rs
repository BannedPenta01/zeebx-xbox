//! Abstração do núcleo ARM.
//!
//! O emulador nunca fala com um núcleo concreto: fala com [`CpuBackend`]. A implementação padrão é
//! o `dynarmic`, que recompila blocos A32/Thumb para código nativo do host.

// Removido assim que o núcleo estiver ligado ao loop principal.
#![allow(dead_code)]

pub mod mem;

use crate::cpu::mem::GuestMemory;

/// Registradores que o despacho de API precisa ler e escrever.
///
/// Segue a AAPCS: argumentos em `r0..r3` e o resto na pilha, retorno em `r0`.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Reg {
    R0,
    R1,
    R2,
    R3,
    /// `r4..r11` não entram na convenção de chamada, mas são onde o compilador guarda o `this`
    /// e as variáveis vivas — sem eles não dá para reconstruir o contexto de uma falha.
    R4,
    R5,
    R6,
    R7,
    R8,
    R9,
    R10,
    R11,
    R12,
    Sp,
    Lr,
    Pc,
}

/// Por que a execução parou.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum StopReason {
    /// O guest saltou para o intervalo reservado às vtables do BREW — é uma chamada de API.
    ApiCall { addr: u32 },
    /// O guest retornou para o endereço-sentinela colocado em `lr` na entrada.
    Returned,
    /// Acesso a memória não mapeada. `addr` é o endereço acessado, `pc` a instrução que tentou.
    MemoryFault { addr: u32, pc: u32 },
    /// Instrução inválida, SWI ou outra exceção do núcleo.
    Exception { pc: u32 },
    /// Estourou o orçamento de instruções da fatia.
    Budget,
}

pub trait CpuBackend {
    /// O nome do núcleo, para o relatório: `"dynarmic"` ou `"interpretador"`.
    ///
    /// **Não é enfeite.** No `uwp` a escolha entre os dois acontece em execução (ver
    /// [`escolhido::Escolhido`]), e sem este nome o relatório de um console não diz qual dos dois
    /// está rodando — e uma velocidade abaixo do esperado não distingue "o JIT não entrou" de "o
    /// JIT entrou e não bastou", que são duas investigações diferentes.
    ///
    /// É `CStr` e não `&str` porque quem lê o nome é também o frontend em C: assim o mesmo texto
    /// atravessa a ABI sem cópia e sem uma segunda tabela de nomes do lado de lá. Quem quer texto
    /// usa `to_string_lossy`.
    ///
    /// Sem valor padrão de propósito: um núcleo novo tem de se nomear, e não aparecer como `?`.
    fn nome(&self) -> &'static std::ffi::CStr;

    /// Prepara o núcleo para executar com este mapa de memória.
    fn reset(&mut self, mem: &GuestMemory) -> Result<(), CpuError>;

    fn read_reg(&self, reg: Reg) -> u32;
    fn write_reg(&mut self, reg: Reg, value: u32);

    /// Executa a partir de `pc` até parar, gastando no máximo `max_instructions`.
    fn run(&mut self, pc: u32, max_instructions: u64) -> Result<StopReason, CpuError>;

    /// Lê memória do guest. Depois do `reset` a memória vive dentro do núcleo, então as
    /// implementações de API precisam passar por aqui em vez de consultar o [`GuestMemory`].
    /// Quantas instruções o guest já executou.
    ///
    /// É o relógio do emulador: o tempo que o jogo enxerga vem daqui, e não do host, para que
    /// duas execuções iguais deem o mesmo resultado.
    fn instructions(&self) -> u64;

    /// Entradas no JIT, o tempo estimado dentro dele e quantas amostras sustentam a estimativa.
    ///
    /// `None` quando o backend não tem entrada de JIT a separar — o interpretador não tem. Quem
    /// usa isto quer a diferença entre o tempo dentro do núcleo e o tempo no despacho; ver
    /// [`crate::cpu::dynarmic::DynarmicCpu::run`]. **O terceiro número é a honestidade da
    /// medida:** zero amostras significa que o relógio não foi ligado, e aí só a contagem vale.
    fn relato_do_jit(&self) -> Option<(u64, u64, u64)> {
        None
    }

    /// Se o núcleo parou com o guest em modo Thumb.
    ///
    /// Serve para retomar um trecho interrompido: quem retoma passa o endereço com o bit 0
    /// ligado, que é como o ARM diz "continue em Thumb". Sem isso, um jogo inteiro em Thumb —
    /// o Zenonia, a série Extreme — voltaria decodificado como ARM.
    /// O `CPSR`: os sinalizadores da última operação e o modo do processador.
    ///
    /// **Sem isto um save state fica errado de um jeito difícil de ver.** A memória volta, o
    /// programa volta, e as flags ficam as de outra execução: a comparação que o jogo fez antes de
    /// salvar continua valendo, mas a decisão seguinte pode tomar o outro caminho. O Dynarmic
    /// expõe isso por `get_cpsr`, e é por isso que o método não tem valor padrão: um
    /// backend que não saiba dizer o `CPSR` tem de dizer isso, e não devolver zero em silêncio.
    fn cpsr(&self) -> u32;

    /// Põe o `CPSR`. Ver [`CpuBackend::cpsr`].
    fn set_cpsr(&mut self, valor: u32);

    /// Põe o contador de instruções, que é o **relógio virtual** do emulador.
    ///
    /// Salvar o relógio e não o devolver deixaria o jogo depois do save state com outro tempo: os
    /// `SetTimer`, o áudio por quadro e o limite de passos do andamento dependem dele.
    fn set_instructions(&mut self, valor: u64);

    fn em_thumb(&self) -> bool {
        false
    }

    /// Arma um sinalizador de sujeira numa faixa: o hook o liga quando o guest escreve nela.
    ///
    /// Sem isto, descobrir se o jogo mexeu numa superfície exige **ler a faixa inteira e
    /// comparar byte a byte**. Medido na Z-Wheel: o `sync` do color buffer do pbuffer era
    /// chamado 93 mil vezes em treze segundos, e a leitura mais a comparação somavam seis
    /// segundos — mais de um terço de todo o tempo de API.
    ///
    /// O `id` identifica a faixa, e existe porque há **várias** ao mesmo tempo: o color buffer
    /// do pbuffer e uma por superfície do jogo. Armar de novo com o mesmo `id` troca a faixa de
    /// lugar, que é o que acontece quando um bitmap é reexposto com outro tamanho.
    ///
    /// O padrão responde "sempre sujo", que é exatamente o comportamento anterior: um backend
    /// que não saiba armar o hook continua correto, só não fica mais rápido.
    fn watch_dirty(&mut self, _id: u32, _base: u32, _len: u32) -> Result<(), CpuError> {
        Ok(())
    }

    /// Desarma a faixa de `id`. Sem isto, a superfície de um bitmap já liberado continuaria
    /// custando um hook em toda escrita do guest naquele endereço.
    fn unwatch_dirty(&mut self, _id: u32) {}

    /// Lê **e limpa** o sinalizador de `id`. `true` quando o guest pode ter escrito desde a
    /// última vez, e também quando não há faixa armada com esse `id` — na dúvida, sujo.
    fn take_dirty(&mut self, _id: u32) -> bool {
        true
    }

    /// Liga o sinalizador das faixas vigiadas que cruzam `addr..addr+len`.
    ///
    /// A vigia só enxerga escrita **do guest**, e isso é o que se quer para as escritas do
    /// próprio emulador. Mas um `MEMMOVE` que o jogo pede ao helper é escrita do jogo feita pelas
    /// nossas mãos: a Z-Wheel compõe o palco 3D assim, copiando o pbuffer para os pixels do
    /// bitmap de destino, e sem este aviso a superfície nunca importava a cópia — o palco ficava
    /// cinza.
    fn marca_sujo(&mut self, _addr: u32, _len: u32) {}

    fn read_mem(&self, addr: u32, buf: &mut [u8]) -> Result<(), CpuError>;

    fn write_mem(&mut self, addr: u32, data: &[u8]) -> Result<(), CpuError>;

    /// Preenche `len` bytes com `valor`, sem alocar.
    ///
    /// Existe por causa de uma medição: o `memset` do guest é chamado em laço de espera, e a
    /// implementação por `write_mem` **alocava um `Vec` do tamanho pedido em cada chamada**. No
    /// Zeebo Extreme Rolima isso era 731 ms dos 1.149 ms gastos em chamadas de API — 63% —, para
    /// 12 mil chamadas de limpeza. Aqui a limpeza vai em blocos de um buffer de pilha.
    fn fill_mem(&mut self, addr: u32, valor: u8, len: u32) -> Result<(), CpuError> {
        const BLOCO: usize = 4096;
        let pilha = [valor; BLOCO];
        let mut restante = len as usize;
        let mut onde = addr;
        while restante > 0 {
            let passo = restante.min(BLOCO);
            self.write_mem(onde, &pilha[..passo])?;
            onde = onde.wrapping_add(passo as u32);
            restante -= passo;
        }
        Ok(())
    }

    /// Zera `len` bytes da pilha do guest **sem** invalidar código do JIT.
    ///
    /// Só para a sujeira de pilha (ver `Machine::gasta_a_pilha`): o intervalo está confinado à
    /// pilha do jogo, onde nunca há código compilado, e sujar em toda chamada de API com a
    /// invalidação junto custava 30% da velocidade no Crash Nitro Kart em corrida (de 385% para
    /// 296% no `bench`, medido) — quase tudo no `jit_mut` e nos empréstimos da marcação, que
    /// aqui sempre sai vazia. Fora da pilha, use o [`Self::fill_mem`], que invalida.
    fn suja_pilha(&mut self, addr: u32, len: u32) -> Result<(), CpuError> {
        self.fill_mem(addr, 0, len)
    }

    fn read_u32(&self, addr: u32) -> Result<u32, CpuError> {
        let mut buf = [0u8; 4];
        self.read_mem(addr, &mut buf)?;
        Ok(u32::from_le_bytes(buf))
    }

    fn write_u32(&mut self, addr: u32, value: u32) -> Result<(), CpuError> {
        self.write_mem(addr, &value.to_le_bytes())
    }

    /// Lê uma string terminada em zero. Para na primeira falha de acesso e no limite dado,
    /// para que um ponteiro corrompido não vire leitura infinita.
    /// Os bytes de uma string do guest, até o terminador — sem interpretar codificação.
    ///
    /// É esta a versão que as funções de string do C precisam. A conversão para `String` troca
    /// cada byte inválido em UTF-8 por `U+FFFD`, que ocupa **três** bytes: `strlen("\x89PNG")`
    /// devolveria 6 em vez de 4, e foi exatamente esse erro que fez o Bejeweled Twist rejeitar
    /// os próprios PNGs.
    fn read_cbytes(&self, addr: u32, max_len: usize) -> Vec<u8> {
        /// Quanto ler de uma vez. Cada leitura é uma chamada ao núcleo, e byte a byte isso
        /// custava caro onde mais dói: o `strtoul` pede até `MAX_STRING` bytes e o Turma da
        /// Mônica o chama cem mil vezes, o que dava centenas de milhões de chamadas só para
        /// converter números. Sessenta e quatro bytes cobrem a string curta típica numa
        /// leitura só.
        const BLOCO: usize = 64;

        let mut bytes = Vec::new();
        while bytes.len() < max_len {
            let quer = BLOCO.min(max_len - bytes.len());
            let base = addr + bytes.len() as u32;
            let mut buf = vec![0u8; quer];
            if self.read_mem(base, &mut buf).is_err() {
                // O bloco pode cruzar o fim da região mapeada, e aí a leitura inteira falha
                // mesmo havendo bytes válidos antes. Byte a byte só neste caso.
                for offset in 0..quer {
                    let mut byte = [0u8; 1];
                    if self.read_mem(base + offset as u32, &mut byte).is_err() || byte[0] == 0 {
                        return bytes;
                    }
                    bytes.push(byte[0]);
                }
                continue;
            }
            match buf.iter().position(|&b| b == 0) {
                Some(fim) => {
                    bytes.extend_from_slice(&buf[..fim]);
                    return bytes;
                }
                None => bytes.extend_from_slice(&buf),
            }
        }
        bytes
    }

    /// A mesma string, interpretada como texto.
    fn read_cstring(&self, addr: u32, max_len: usize) -> String {
        latin1_decode(&self.read_cbytes(addr, max_len))
    }
}

/// Interpreta bytes de uma string `char` do BREW como texto.
///
/// O `char` do BREW é ISO-8859-1: um byte, um caractere. Ler como UTF-8 destruía tudo que
/// tivesse acento — `0xE7` não é UTF-8 válido, virava `U+FFFD` e voltava para a memória do
/// jogo como três bytes de lixo. É por isso que a acentuação saía quebrada na tela do
/// Resident Evil 4 e do Double Dragon, que passam o texto por `strtowstr`.
///
/// O mapeamento é direto porque os 256 primeiros pontos do Unicode *são* o ISO-8859-1, e por
/// isso a ida e a volta nunca perdem byte nenhum.
pub fn latin1_decode(bytes: &[u8]) -> String {
    bytes.iter().map(|&b| b as char).collect()
}

/// O caminho de volta: cada caractere vira um byte.
///
/// O que não couber em um byte não veio da memória do guest e não tem representação lá; vira
/// `?`, que é o que a libc faz com um caractere fora da página de código.
pub fn latin1_encode(text: &str) -> Vec<u8> {
    text.chars()
        .map(|c| u8::try_from(c as u32).unwrap_or(b'?'))
        .collect()
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct CpuError(pub String);

impl std::fmt::Display for CpuError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(f, "falha no núcleo ARM: {}", self.0)
    }
}

impl std::error::Error for CpuError {}

/// O JIT, onde ele pode emitir código que o processo executa.
///
/// **Dois alvos ficam de fora**, e a razão de cada um está em [`interpretador`]: o `wasm32`, onde o
/// navegador não salta para o bloco emitido, e o iOS, onde o kernel recusa a página executável.
///
/// **O `uwp` saiu desta lista, e o que o pôs de volta foi uma medição.** Um AppContainer de fato
/// recusa uma página que seja gravável e executável ao mesmo tempo — mas quem pedia isso era o
/// *alocador*, e o dynarmic já traz o caminho de W^X pronto atrás de uma opção do CMake dele. Ver
/// `appcontainer.cmake` e `docs/implementacao/25-frontend-xbox-uwp.md`, "A memória executável".
#[cfg(not(any(target_arch = "wasm32", target_os = "ios")))]
pub mod dynarmic;

/// Interpretador A32/T32. Entra onde o JIT não pode emitir código que o processo execute: no
/// `wasm32` o navegador não salta para o bloco, e no iOS o kernel recusa a página executável.
///
/// **No `uwp` ele deixou de ser o único, mas continua compilado** — é para ele que o alvo recua
/// quando a sonda da memória executável volta negativa, e por isso a escolha de lá é em execução,
/// em [`escolhido`].
///
/// Também entra nos testes do próprio arquivo. O desktop continua no JIT.
#[cfg(any(test, target_arch = "wasm32", target_os = "ios", target_vendor = "uwp"))]
pub mod interpretador;

/// A escolha do núcleo **em execução**, para onde o sistema pode recusar a página executável.
///
/// Só existe onde os **dois** núcleos existem, porque é lá que há o que escolher: no `uwp`, e nos
/// testes do desktop. No `wasm32` e no iOS não há JIT para escolher; no desktop sem testes não há
/// interpretador.
#[cfg(all(
    any(test, target_vendor = "uwp"),
    not(any(target_arch = "wasm32", target_os = "ios"))
))]
pub mod escolhido;

/// O alias que o resto do código usa para pedir "o backend padrão".
///
/// **Os três `#[cfg]` são exclusivos.** O `uwp` é o único alvo em que o alias é uma escolha, e não
/// um núcleo: os outros dois são a resposta fixa de sempre.
#[cfg(target_vendor = "uwp")]
pub type BackendPadrao = escolhido::Escolhido;
#[cfg(any(target_arch = "wasm32", target_os = "ios"))]
pub type BackendPadrao = interpretador::Interpretador;
#[cfg(all(
    not(target_vendor = "uwp"),
    not(any(target_arch = "wasm32", target_os = "ios"))
))]
pub type BackendPadrao = dynarmic::DynarmicCpu;

/// Se **este processo** pode executar a memória que ele mesmo escreveu.
///
/// É a única pergunta que separa o JIT do interpretador onde ela é dúvida. No desktop, no Android e
/// no Libretro ninguém mexe nisto e o padrão vale — o JIT. No `uwp` quem responde é a sonda do
/// aplicativo (`app\Jit.cpp`), que tenta o caminho de verdade no aparelho e chama [`permite_jit`]
/// com o resultado antes de qualquer jogo abrir.
///
/// **O padrão é `true`, e ele é otimista de propósito**: quem não tem dúvida não precisa dizer nada,
/// e é o comportamento que os outros quatro frontends já tinham. Quem tem dúvida *tem* de dizer, e
/// o frontend de Xbox diz — um JIT que aloca errado não falha ao compilar, falha em execução.
static JIT_PERMITIDO: std::sync::atomic::AtomicBool = std::sync::atomic::AtomicBool::new(true);

/// Diz se este processo pode executar a memória que ele mesmo escreve. Ver [`JIT_PERMITIDO`].
pub fn permite_jit(permitido: bool) {
    JIT_PERMITIDO.store(permitido, std::sync::atomic::Ordering::Relaxed);
}

/// A resposta de [`permite_jit`].
pub fn jit_permitido() -> bool {
    JIT_PERMITIDO.load(std::sync::atomic::Ordering::Relaxed)
}

// As constantes da faixa de vtables do BREW são parte do contrato entre o backend e o despachante.
pub use faixas_do_brew::{API_BASE, API_SIZE, RETURN_MAGIC};


/// As três constantes da faixa reservada às vtables do BREW.
///
/// Ficam num módulo próprio porque são **do contrato**, não de um backend: é assim que o despachante
/// Rust descobre qual API o jogo chamou.
mod faixas_do_brew {
    /// Base da faixa reservada às vtables do BREW.
    pub const API_BASE: u32 = 0xf000_0000;
    /// Tamanho da faixa.
    pub const API_SIZE: u32 = 0x0100_0000;
    /// Endereço-sentinela que o `lr` recebe para marcar "voltou da API".
    pub const RETURN_MAGIC: u32 = 0xfff0_0000;
}

/// Quanto do log por semihosting fica guardado, em bytes.
const MAX_SEMIHOSTING: usize = 256 * 1024;

/// Mantém o log por semihosting dentro de [`MAX_SEMIHOSTING`], jogando fora a metade mais antiga.
///
/// Era uma `String` que só crescia: o Peggle e o Zuma escrevem por ali a sessão inteira, e o
/// relatório clona o texto todo a cada quadro em que a janela de log está aberta.
pub(crate) fn apara_semihosting(texto: &mut String) {
    if texto.len() <= MAX_SEMIHOSTING {
        return;
    }
    let mut corte = texto.len() - MAX_SEMIHOSTING / 2;
    while !texto.is_char_boundary(corte) {
        corte += 1;
    }
    texto.drain(..corte);
}

#[cfg(test)]
mod tests {
    use super::*;

    /// O texto acentuado tem de sobreviver à ida e à volta byte a byte: é isso que o jogo
    /// guarda na memória dele.
    #[test]
    fn latin1_ida_e_volta_preserva_acento() {
        let bytes = b"Configura\xe7\xf5es";
        let text = latin1_decode(bytes);
        assert_eq!(text, "Configurações");
        assert_eq!(latin1_encode(&text), bytes);
    }

    #[test]
    fn latin1_cobre_todos_os_bytes() {
        let bytes: Vec<u8> = (1..=255).collect();
        assert_eq!(latin1_encode(&latin1_decode(&bytes)), bytes);
    }
}
