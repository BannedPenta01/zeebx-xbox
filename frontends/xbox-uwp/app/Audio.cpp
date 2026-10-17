#include "pch.h"
#include "Audio.h"

#include <atomic>

using namespace winrt;

namespace ZeebxUwp {

namespace {

/// Quantos pedaços de 20 ms cabem. Cinco dá ~100 ms de folga: o bastante para uma volta perdida
/// do laço de desenho não virar um estalo, e pouco o bastante para a latência não aparecer no
/// botão de um jogo de ritmo.
constexpr uint32_t PEDACOS = 5;
constexpr uint32_t QUADROS_POR_PEDACO = 882;  // 20 ms a 44100 Hz

/// A voz avisa quando devolve um buffer. É o único jeito de saber quanto ainda está na fila sem
/// perguntar à placa a cada quadro.
struct AvisoDeVoz : IXAudio2VoiceCallback {
    std::atomic<uint32_t>* livres = nullptr;

    void STDMETHODCALLTYPE OnVoiceProcessingPassStart(UINT32) override {}
    void STDMETHODCALLTYPE OnVoiceProcessingPassEnd() override {}
    void STDMETHODCALLTYPE OnStreamEnd() override {}
    void STDMETHODCALLTYPE OnBufferStart(void*) override {}
    void STDMETHODCALLTYPE OnBufferEnd(void*) override {
        if (livres) {
            livres->fetch_add(1, std::memory_order_relaxed);
        }
    }
    void STDMETHODCALLTYPE OnLoopEnd(void*) override {}
    void STDMETHODCALLTYPE OnVoiceError(void*, HRESULT) override {}
};

AvisoDeVoz g_aviso;

}  // namespace

bool Audio::Inicializar(uint32_t taxa) {
    if (FAILED(::XAudio2Create(m_motor.put(), 0, XAUDIO2_DEFAULT_PROCESSOR))) {
        return false;
    }

    // O formato do console: dois canais, 16 bits com sinal. Os bytes por segundo e o alinhamento
    // de bloco **têm** de casar com a taxa, ou a voz recusa com `XAUDIO2_E_INVALID_CALL` e a
    // mensagem não diz qual campo estava errado.
    WAVEFORMATEX formato{};
    formato.wFormatTag = WAVE_FORMAT_PCM;
    formato.nChannels = 2;
    formato.nSamplesPerSec = taxa;
    formato.wBitsPerSample = 16;
    formato.nBlockAlign = static_cast<WORD>(formato.nChannels * formato.wBitsPerSample / 8);
    formato.nAvgBytesPerSec = formato.nSamplesPerSec * formato.nBlockAlign;

    if (FAILED(m_motor->CreateMasteringVoice(&m_mestre, 2, taxa, 0, nullptr, nullptr,
                                             AudioCategory_GameMedia))) {
        m_motor = nullptr;
        return false;
    }

    g_aviso.livres = &m_livres;
    m_buffers.assign(PEDACOS, std::vector<uint8_t>(QUADROS_POR_PEDACO * 4));
    m_livres.store(PEDACOS, std::memory_order_relaxed);

    if (FAILED(m_motor->CreateSourceVoice(&m_origem, &formato, 0, XAUDIO2_DEFAULT_FREQ_RATIO,
                                          &g_aviso))) {
        m_mestre->DestroyVoice();
        m_mestre = nullptr;
        m_motor = nullptr;
        return false;
    }
    m_origem->Start(0);
    return true;
}

void Audio::Enfileira(int16_t const* amostras, uint32_t quadros) {
    if (!m_origem || !amostras || quadros == 0) {
        return;
    }
    // Um pedaço maior do que o anel é cortado: o XAudio2 aceita, mas o atraso que ele cria é
    // exatamente o que se quer evitar.
    quadros = std::min<uint32_t>(quadros, QUADROS_POR_PEDACO);
    if (m_livres.load(std::memory_order_relaxed) == 0) {
        return;
    }

    auto& destino = m_buffers[m_proximo];
    m_proximo = (m_proximo + 1) % PEDACOS;
    const size_t bytes = static_cast<size_t>(quadros) * 4;
    std::memcpy(destino.data(), amostras, bytes);

    XAUDIO2_BUFFER pedaco{};
    pedaco.AudioBytes = static_cast<UINT32>(bytes);
    pedaco.pAudioData = destino.data();
    // O `pContext` carrega o índice para o `OnBufferEnd` — não é usado para nada além de
    // identificar o pedaço, mas o contrato do XAudio2 o exige como opaco.
    pedaco.pContext = destino.data();
    if (SUCCEEDED(m_origem->SubmitSourceBuffer(&pedaco))) {
        m_livres.fetch_sub(1, std::memory_order_relaxed);
    }
}

void Audio::Silencia() {
    if (m_origem) {
        m_origem->FlushSourceBuffers();
        m_livres.store(PEDACOS, std::memory_order_relaxed);
    }
}

void Audio::Desligar() {
    if (m_origem) {
        m_origem->Stop(0);
        m_origem->DestroyVoice();
        m_origem = nullptr;
    }
    if (m_mestre) {
        m_mestre->DestroyVoice();
        m_mestre = nullptr;
    }
    m_motor = nullptr;
    m_buffers.clear();
}

}  // namespace ZeebxUwp
