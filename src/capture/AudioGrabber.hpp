#pragma once

#include <Geode/Geode.hpp>

#ifdef GEODE_IS_WINDOWS

#include <Geode/fmod/fmod.hpp>
#include <vector>
#include <memory>
#include <functional>
#include <thread>
#include <atomic>
#include <mutex>
#include <chrono>

namespace replay {

struct AudioChunk {
    std::vector<float> samples; // Interleaved 32-bit float PCM (stereo)
    int channels = 2;
    int sampleRate = 48000;
    std::chrono::steady_clock::time_point timestamp;
};

struct AudioDeviceInfo {
    std::string id;       // Endpoint device ID, or "default"
    std::string name;     // Sanitized friendly name for display
};

class AudioGrabber {
public:
    static AudioGrabber* get();

    void initialize();
    void cleanup();

    static std::vector<AudioDeviceInfo> getAvailableMicrophones();
    void restartMicCapture();
    void startMicCapture();
    void stopMicCapture();

    // Microphone callback (WASAPI thread -> ReplayManager worker queue)
    using AudioCallback = std::function<void(std::shared_ptr<AudioChunk>)>;
    void setMicAudioCallback(AudioCallback cb);

    // Called on the real-time FMOD mixer thread (lock-free, allocation-free SPSC ring buffer)
    void pushGameAudio(const float* inbuffer, unsigned int length, int channels, int sampleRate = 44100);

    // Drained by the ReplayManager worker thread; returns number of stereo frames read
    size_t drainGameAudio(std::vector<float>& outStereoSamples, int& outSampleRate,
                          std::chrono::steady_clock::time_point& outTimestamp);
    bool hasPendingGameAudio() const noexcept;
    void resetGameAudioBuffer() noexcept;

    // Called by WASAPI microphone capture thread
    void pushMicAudio(const float* inbuffer, unsigned int length, int channels, int sampleRate);

    void ensureDSPAttached();
    bool isDSPAttached() const { return m_fmodDSP != nullptr; }
    bool isMicActive() const { return m_micRunning.load(); }
    int getGameSampleRate() const { return m_gameSampleRate.load(std::memory_order_relaxed); }
    void setGameSampleRate(int rate) {
        if (rate > 0) m_gameSampleRate.store(rate, std::memory_order_relaxed);
    }
    int getMicSampleRate() const { return m_micSampleRate.load(std::memory_order_relaxed); }

private:
    AudioGrabber();
    ~AudioGrabber();

    void setupFmodDSP();
    void removeFmodDSP();

    void micCaptureThreadFunc();
    void resetMicResampler();

    bool m_initialized = false;
    FMOD::DSP* m_fmodDSP = nullptr;

    // Lock-free SPSC ring buffer for real-time FMOD DSP game audio (power-of-two stereo frames)
    static constexpr size_t kGameRingFrames = 65536; // ~1.36s at 48 kHz
    static constexpr size_t kGameRingMask = kGameRingFrames - 1;
    std::vector<float> m_gameRingBuffer;
    std::atomic<size_t> m_gameRingWriteFrame{0};
    std::atomic<size_t> m_gameRingReadFrame{0};
    std::atomic<int64_t> m_lastGamePushTimeNs{0};

    AudioCallback m_micCallback;

    // Stateful microphone resampler & scratch buffers (WASAPI thread only)
    double m_micResamplePos = 0.0;
    float m_micPrevSample[2] = {0.0f, 0.0f};
    bool m_micHasPrev = false;
    int m_micLastInputRate = 0;
    std::vector<float> m_micConvertScratch;
    std::vector<float> m_micStereoScratch;

    std::atomic<int> m_gameSampleRate{44100};
    std::atomic<int> m_micSampleRate{48000};
    std::atomic<bool> m_micRunning{false};
    std::thread m_micThread;
};

} // namespace replay

#endif // GEODE_IS_WINDOWS
