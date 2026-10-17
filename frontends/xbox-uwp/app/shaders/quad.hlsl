// quad.hlsl — o único desenho que este aplicativo faz na placa.
//
// A GUI é XAML, não D3D: o XAML dá foco de controle, texto, listas e escalonamento de TV sem
// uma linha nossa. Na placa fica só o quadro do Zeebo, num retângulo com tarja preta.
//
// O `fxc` compila este arquivo para `quad.vs.cso` e `quad.ps.cso` no build (ver o `.vcxproj`).
// Não há compilação em tempo de execução: o `d3dcompiler_47.dll` não é uma dependência que um
// pacote de UWP deva carregar, e o console não o traz.

struct EntradaDeVertice {
    float2 posicao : POSITION;
    float2 uv : TEXCOORD0;
};

struct SaidaDeVertice {
    float4 posicao : SV_POSITION;
    float2 uv : TEXCOORD0;
};

// Os quatro cantos chegam já em coordenadas normalizadas, com a tarja calculada na CPU: o
// retângulo depende da proporção escolhida e do tamanho do painel, e fazer essa conta aqui seria
// repetir por quadro o que muda raramente.
SaidaDeVertice VSMain(EntradaDeVertice entrada) {
    SaidaDeVertice saida;
    saida.posicao = float4(entrada.posicao, 0.0, 1.0);
    saida.uv = entrada.uv;
    return saida;
}

Texture2D Quadro : register(t0);
// Ponto para a escala inteira (o pixel do console é um bloco nítido) e linear para o resto.
SamplerState Amostra : register(s0);

float4 PSMain(SaidaDeVertice entrada) : SV_TARGET {
    return Quadro.Sample(Amostra, entrada.uv);
}
