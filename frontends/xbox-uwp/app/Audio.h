#pragma once

#include "pch.h"

// `ZeebxUwp` pela razão explicada em `Renderer.h`: `ZeebxXbox` já é o `struct` opaco da ABI.
namespace ZeebxUwp {

/// A placa de som do aplicativo: um XAudio2 com uma voz de origem em 44100 Hz, estéreo, 16 bits.
///
/// **O núcleo não toca.** Ele entrega as amostras por `Mixer::render` e quem as empurra para a
/// placa é este arquivo, pelo lado C++. É o mesmo desenho do core Libretro, e a razão de o
/// `cpal`/WASAPI não entrar num AppContainer, onde ele não funcionaria.
class Audio {
public:
    /// Abre a placa. Devolve `false` quando não há saída — e isto **não** pode impedir o jogo de
    /// rodar, só deixá-lo mudo.
    bool Inicializar(uint32_t taxa);

    /// Copia e enfileira um pedaço. Pede pouco: o que não couber no espaço livre é descartado,
    /// porque o atraso acumulado é pior do que a perda de um trecho.
    void Enfileira(int16_t const* amostras, uint32_t quadros);

    /// Esvazia o que está na fila. Usado na pausa e ao sair do jogo, para o som não continuar
    /// depois da imagem parada.
    void Silencia();

    void Desligar();

private:
    // O `XAudio2` é COM clássico, e o buffer que ele toca tem de viver até a voz devolvê-lo.
    winrt::com_ptr<IXAudio2> m_motor;
    IXAudio2MasteringVoice* m_mestre = nullptr;
    IXAudio2SourceVoice* m_origem = nullptr;
    /// Os buffers ficam num anel pequeno; `m_livres` conta quantos a placa já devolveu.
    std::vector<std::vector<uint8_t>> m_buffers;
    std::atomic<uint32_t> m_livres{0};
    uint32_t m_proximo = 0;
};

}  // namespace ZeebxUwp
