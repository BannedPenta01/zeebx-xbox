//! A escolha do núcleo **em execução**.
//!
//! **Existe por causa de um alvo só: o Xbox.** No desktop, no Android e no Libretro a resposta é
//! sabida quando o projeto é compilado — há página executável, e o núcleo é o JIT. No aplicativo
//! UWP ela depende do processo: um AppContainer sem a capacidade `codeGeneration` recusa virar uma
//! página gravável em executável, e um JIT que descobre isso **não falha ao compilar, falha ao
//! executar** — no meio de um jogo, sem caminho de volta.
//!
//! Por isso o alvo de Xbox compila os **dois** núcleos e pergunta antes: a sonda do `app\Jit.cpp`
//! percorre o caminho inteiro no aparelho (aloca, escreve, troca a proteção e chama) e conta o
//! resultado por [`super::permite_jit`], durante a abertura do aplicativo. Aqui só se lê a resposta.
//!
//! **Todo método do [`CpuBackend`] é repassado, e é esta a regra do arquivo.** Metade do trait tem
//! valor padrão, e o padrão é *correto e lento*: "sempre sujo" nas faixas vigiadas, um `write_mem`
//! por bloco no `fill_mem`, uma alocação por string no `read_cbytes`. Um método esquecido aqui não
//! quebra nada — devolve o emulador ao comportamento anterior, em silêncio, e aparece só como
//! lentidão difusa. Por isso a lista abaixo é explícita e inteira, em vez de um `Deref` ou de uma
//! macro que esconderia o que ficou de fora.

use super::dynarmic::DynarmicCpu;
use super::interpretador::Interpretador;
use super::{CpuBackend, CpuError, GuestMemory, Reg, StopReason};

/// O núcleo da sessão, escolhido quando ela abre.
pub enum Escolhido {
    /// O JIT: recompila cada bloco A32 para o código do host e salta para ele.
    Jit(DynarmicCpu),
    /// O interpretador: decodifica e executa, instrução por instrução.
    Interpretador(Interpretador),
}

impl Escolhido {
    /// Abre o núcleo que **este** processo consegue sustentar. Ver [`super::permite_jit`].
    ///
    /// É `Escolhido` e não `Result` só para o JIT porque as duas variantes já devolvem `Result`:
    /// a falha de `DynarmicCpu::new` e a de `Interpretador::new` são a mesma coisa para quem
    /// chamou, e diferenciá-las não mudaria o que ele faria.
    pub fn new() -> Result<Self, CpuError> {
        if super::jit_permitido() {
            Ok(Self::Jit(DynarmicCpu::new()?))
        } else {
            Ok(Self::Interpretador(Interpretador::new()?))
        }
    }

    /// O texto que o guest escreveu pelo semihosting — a janela de depuração do emulador.
    ///
    /// Não é método do [`CpuBackend`]: é uma porta que os dois núcleos oferecem com o mesmo nome, e
    /// que a varredura e a tela de depuração procuram pelo nome. Ver
    /// [`DynarmicCpu::semihosting`](super::dynarmic::DynarmicCpu::semihosting).
    pub fn semihosting(&self) -> String {
        match self {
            Self::Jit(cpu) => cpu.semihosting(),
            Self::Interpretador(cpu) => cpu.semihosting(),
        }
    }
}

impl CpuBackend for Escolhido {
    fn nome(&self) -> &'static std::ffi::CStr {
        match self {
            Self::Jit(cpu) => cpu.nome(),
            Self::Interpretador(cpu) => cpu.nome(),
        }
    }

    fn reset(&mut self, mem: &GuestMemory) -> Result<(), CpuError> {
        match self {
            Self::Jit(cpu) => cpu.reset(mem),
            Self::Interpretador(cpu) => cpu.reset(mem),
        }
    }

    fn read_reg(&self, reg: Reg) -> u32 {
        match self {
            Self::Jit(cpu) => cpu.read_reg(reg),
            Self::Interpretador(cpu) => cpu.read_reg(reg),
        }
    }

    fn write_reg(&mut self, reg: Reg, value: u32) {
        match self {
            Self::Jit(cpu) => cpu.write_reg(reg, value),
            Self::Interpretador(cpu) => cpu.write_reg(reg, value),
        }
    }

    fn run(&mut self, pc: u32, max_instructions: u64) -> Result<StopReason, CpuError> {
        match self {
            Self::Jit(cpu) => cpu.run(pc, max_instructions),
            Self::Interpretador(cpu) => cpu.run(pc, max_instructions),
        }
    }

    fn instructions(&self) -> u64 {
        match self {
            Self::Jit(cpu) => cpu.instructions(),
            Self::Interpretador(cpu) => cpu.instructions(),
        }
    }

    fn relato_do_jit(&self) -> Option<(u64, u64, u64)> {
        match self {
            Self::Jit(cpu) => cpu.relato_do_jit(),
            Self::Interpretador(cpu) => cpu.relato_do_jit(),
        }
    }

    fn cpsr(&self) -> u32 {
        match self {
            Self::Jit(cpu) => cpu.cpsr(),
            Self::Interpretador(cpu) => cpu.cpsr(),
        }
    }

    fn set_cpsr(&mut self, valor: u32) {
        match self {
            Self::Jit(cpu) => cpu.set_cpsr(valor),
            Self::Interpretador(cpu) => cpu.set_cpsr(valor),
        }
    }

    fn set_instructions(&mut self, valor: u64) {
        match self {
            Self::Jit(cpu) => cpu.set_instructions(valor),
            Self::Interpretador(cpu) => cpu.set_instructions(valor),
        }
    }

    fn em_thumb(&self) -> bool {
        match self {
            Self::Jit(cpu) => cpu.em_thumb(),
            Self::Interpretador(cpu) => cpu.em_thumb(),
        }
    }

    fn watch_dirty(&mut self, id: u32, base: u32, len: u32) -> Result<(), CpuError> {
        match self {
            Self::Jit(cpu) => cpu.watch_dirty(id, base, len),
            Self::Interpretador(cpu) => cpu.watch_dirty(id, base, len),
        }
    }

    fn unwatch_dirty(&mut self, id: u32) {
        match self {
            Self::Jit(cpu) => cpu.unwatch_dirty(id),
            Self::Interpretador(cpu) => cpu.unwatch_dirty(id),
        }
    }

    fn take_dirty(&mut self, id: u32) -> bool {
        match self {
            Self::Jit(cpu) => cpu.take_dirty(id),
            Self::Interpretador(cpu) => cpu.take_dirty(id),
        }
    }

    fn marca_sujo(&mut self, addr: u32, len: u32) {
        match self {
            Self::Jit(cpu) => cpu.marca_sujo(addr, len),
            Self::Interpretador(cpu) => cpu.marca_sujo(addr, len),
        }
    }

    fn read_mem(&self, addr: u32, buf: &mut [u8]) -> Result<(), CpuError> {
        match self {
            Self::Jit(cpu) => cpu.read_mem(addr, buf),
            Self::Interpretador(cpu) => cpu.read_mem(addr, buf),
        }
    }

    fn write_mem(&mut self, addr: u32, data: &[u8]) -> Result<(), CpuError> {
        match self {
            Self::Jit(cpu) => cpu.write_mem(addr, data),
            Self::Interpretador(cpu) => cpu.write_mem(addr, data),
        }
    }

    fn fill_mem(&mut self, addr: u32, valor: u8, len: u32) -> Result<(), CpuError> {
        match self {
            Self::Jit(cpu) => cpu.fill_mem(addr, valor, len),
            Self::Interpretador(cpu) => cpu.fill_mem(addr, valor, len),
        }
    }

    fn suja_pilha(&mut self, addr: u32, len: u32) -> Result<(), CpuError> {
        match self {
            Self::Jit(cpu) => cpu.suja_pilha(addr, len),
            Self::Interpretador(cpu) => cpu.suja_pilha(addr, len),
        }
    }

    fn read_u32(&self, addr: u32) -> Result<u32, CpuError> {
        match self {
            Self::Jit(cpu) => cpu.read_u32(addr),
            Self::Interpretador(cpu) => cpu.read_u32(addr),
        }
    }

    fn write_u32(&mut self, addr: u32, value: u32) -> Result<(), CpuError> {
        match self {
            Self::Jit(cpu) => cpu.write_u32(addr, value),
            Self::Interpretador(cpu) => cpu.write_u32(addr, value),
        }
    }

    fn read_cbytes(&self, addr: u32, max_len: usize) -> Vec<u8> {
        match self {
            Self::Jit(cpu) => cpu.read_cbytes(addr, max_len),
            Self::Interpretador(cpu) => cpu.read_cbytes(addr, max_len),
        }
    }

    fn read_cstring(&self, addr: u32, max_len: usize) -> String {
        match self {
            Self::Jit(cpu) => cpu.read_cstring(addr, max_len),
            Self::Interpretador(cpu) => cpu.read_cstring(addr, max_len),
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::cpu::RETURN_MAGIC;

    /// A bancada das provas dos dois núcleos, a mesma: página de código gravável, dados e pilha.
    fn memoria(codigo: &[u8]) -> GuestMemory {
        let mut mem = GuestMemory::new();
        let mut bytes = codigo.to_vec();
        bytes.resize(0x1000, 0);
        mem.map("code", 0, bytes, true).unwrap();
        mem.map_zeroed("data", 0x1000, 0x1000).unwrap();
        mem.map_zeroed("stack", 0x2000_0000, 0x1000).unwrap();
        mem
    }

    fn arm(palavras: &[u32]) -> Vec<u8> {
        palavras.iter().flat_map(|p| p.to_le_bytes()).collect()
    }

    fn pronto(cpu: &mut Escolhido, mem: &GuestMemory) {
        cpu.reset(mem).unwrap();
        cpu.write_reg(Reg::Sp, 0x2000_0f00);
    }

    /// Os dois núcleos, para as provas que não querem saber do interruptor global.
    fn os_dois() -> [Escolhido; 2] {
        [
            Escolhido::Jit(DynarmicCpu::new().unwrap()),
            Escolhido::Interpretador(Interpretador::new().unwrap()),
        ]
    }

    /// **A prova que interessa neste arquivo: o repasse não muda a resposta.** Os dois núcleos
    /// rodam o mesmo programa pela mesma porta, e o que sai tem de ser igual.
    #[test]
    fn os_dois_nucleos_respondem_o_mesmo_pela_porta_do_repasse() {
        let mem = memoria(&arm(&[0xe3a0_0037, 0xeaff_fffe])); // mov r0,#0x37 ; b .
        let [mut jit, mut interp] = os_dois();

        // O que o guest vê é o que tem de bater: por que parou e o que ficou no registrador.
        for cpu in [&mut jit, &mut interp] {
            pronto(cpu, &mem);
            assert_eq!(cpu.run(0, 1).unwrap(), StopReason::Budget);
            assert_eq!(cpu.read_reg(Reg::R0), 0x37);
        }

        // **O contador de instruções não bate, e não é defeito.** O interpretador confere o
        // orçamento instrução a instrução e para na primeira; o bloco emitido só confere na
        // fronteira do bloco, então ele passa do teto por até o tamanho do bloco — aqui, dois. É
        // diferença de *despacho*, não de resultado: quem consome o orçamento o consome como fatia
        // de tempo, e uma instrução a mais numa fatia de milhões não muda nada. A prova ficou
        // vermelha na primeira rodada por afirmar a conta exata; o que se afirma agora é o que de
        // fato tem de valer.
        assert_eq!(interp.instructions(), 1);
        assert!(
            (1..=2).contains(&jit.instructions()),
            "o bloco do JIT passou do tamanho esperado: {} instruções para um orçamento de uma",
            jit.instructions()
        );
    }

    /// O nome é do núcleo de dentro, e não da porta que repassa: é ele que o relatório da sessão
    /// escreve, e é ele que atravessa a ABI a caminho do log do aplicativo.
    #[test]
    fn o_nome_e_o_do_nucleo_que_esta_rodando() {
        let [jit, interp] = os_dois();
        assert_eq!(jit.nome().to_bytes(), b"dynarmic");
        assert_eq!(interp.nome().to_bytes(), b"interpretador");
    }

    /// O semihosting não é do trait e sai pela porta própria — que também tem de repassar.
    #[test]
    fn o_semihosting_atravessa_a_porta_propria() {
        // mov r0,#SYS_WRITEC ; mov r1,#0x1000 ; svc #0xab ; bx lr
        let mem = memoria(&arm(&[0xe3a0_0003, 0xe3a0_1a01, 0xef00_00ab, 0xe12f_ff1e]));
        for cpu in os_dois().iter_mut() {
            pronto(cpu, &mem);
            cpu.write_mem(0x1000, b"Z").unwrap();
            cpu.write_reg(Reg::Lr, RETURN_MAGIC);
            assert_eq!(cpu.run(0, 10).unwrap(), StopReason::Returned);
            assert_eq!(cpu.semihosting(), "Z");
        }
    }

    /// **Quanto o JIT ganha do interpretador, nesta máquina.**
    ///
    /// Existe porque a escolha deste arquivo é entre dois núcleos e a distância entre eles é o que
    /// decide se um jogo roda a trinta quadros por segundo ou a oito. É a medição que dá sentido à
    /// escolha: sem ela, "o JIT entrou" e "o JIT bastou" continuam indistinguíveis.
    ///
    /// O programa é um laço que **conta na memória** — ler, somar, escrever e decidir, que é o que
    /// um jogo faz o tempo todo —, e não um laço só de contas: é na memória que os dois núcleos mais
    /// se separam, porque o interpretador paga uma chamada por acesso e o bloco emitido não.
    ///
    /// Ignorado por padrão, como o `quanto_custa_o_relogio` da varredura: não é invariante do
    /// emulador, é propriedade da máquina.
    ///
    /// ```text
    /// cargo test --release --lib quanto_o_jit_ganha -- --ignored --nocapture
    /// ```
    #[test]
    #[ignore = "mede a máquina, não o emulador"]
    fn quanto_o_jit_ganha_do_interpretador() {
        /// Instruções por chamada a `run`. Alto o bastante para o custo da chamada sumir.
        const LOTE: u64 = 2_000_000;
        /// Chamadas, por núcleo.
        const LOTES: u64 = 10;

        // O laço, de cinco instruções: lê o contador da memória, soma um, grava de volta, compara e
        // volta. `bne` enquanto não der a volta nos 32 bits — some 4 bilhões de voltas, e quem para
        // é o teto de instruções, não o programa.
        //
        //   mov r2, #0x1000   ; o contador mora na página de dados da bancada
        //   ldr r1, [r2]
        //   add r1, r1, #1
        //   str r1, [r2]
        //   cmp r1, #0
        //   bne -4            ; de volta ao ldr
        let codigo = arm(&[
            0xe3a0_2a01, // mov r2, #0x1000
            0xe592_1000, // ldr r1, [r2]
            0xe281_1001, // add r1, r1, #1
            0xe582_1000, // str r1, [r2]
            0xe351_0000, // cmp r1, #0
            0x1aff_fffa, // bne -4
        ]);
        let mem = memoria(&codigo);

        let mut medir = |cpu: &mut Escolhido| {
            pronto(cpu, &mem);
            // O aquece: a primeira volta é a que compila o bloco, e ela não é o que se mede.
            cpu.run(0, LOTE).unwrap();
            let comeco = std::time::Instant::now();
            for _ in 0..LOTES {
                assert_eq!(cpu.run(0, LOTE).unwrap(), StopReason::Budget);
            }
            let gasto = comeco.elapsed();
            (cpu.instructions(), gasto)
        };

        let [mut jit, mut interp] = os_dois();
        let (instrucoes_jit, tempo_jit) = medir(&mut jit);
        let (instrucoes_interp, tempo_interp) = medir(&mut interp);

        let por_ms = |instrucoes: u64, tempo: std::time::Duration| {
            instrucoes as f64 / tempo.as_secs_f64() / 1000.0
        };
        let do_jit = por_ms(instrucoes_jit, tempo_jit);
        let do_interp = por_ms(instrucoes_interp, tempo_interp);

        eprintln!(
            "laço de memória, {instrucoes_jit} instruções: JIT {do_jit:.0}/ms ({tempo_jit:?}), \
             interpretador {do_interp:.0}/ms ({tempo_interp:?}) — o JIT é {:.1}x o interpretador",
            do_jit / do_interp
        );
    }

    /// **A única prova deste arquivo que mexe no interruptor do processo**, e é uma só de
    /// propósito: `permite_jit` é global, as provas de um binário correm em paralelo, e duas
    /// provas mexendo nele fariam o resultado de uma depender do instante da outra.
    #[test]
    fn a_escolha_segue_a_resposta_do_sistema() {
        let antes = crate::cpu::jit_permitido();

        crate::cpu::permite_jit(true);
        assert!(matches!(
            Escolhido::new().unwrap(),
            Escolhido::Jit(_)
        ));

        crate::cpu::permite_jit(false);
        assert!(matches!(
            Escolhido::new().unwrap(),
            Escolhido::Interpretador(_)
        ));

        crate::cpu::permite_jit(antes);
    }
}
