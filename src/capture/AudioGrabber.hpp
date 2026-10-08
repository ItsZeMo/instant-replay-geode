#pragma once

#include <Geode/Geode.hpp>
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
    std::vector<float> samples; // Interleaved 32-bit float PCM
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

    // Callbacks for receiving audio chunks
    using AudioCallback = std::function<void(std::shared_ptr<AudioChunk>)>;
    void setGameAudioCallback(AudioCallback cb);
    void setMicAudioCallback(AudioCallback cb);

    // Called by FMOD DSP callback
    void pushGameAudio(const float* inbuffer, unsigned int length, int channels, int sampleRate = 44100);

    // Called by WASAPI microphone capture thread
    void pushMicAudio(const float* inbuffer, unsigned int length, int channels, int sampleRate);

    void ensureDSPAttached();
    bool isDSPAttached() const { return m_fmodDSP != nullptr; }
    bool isMicActive() const { return m_micRunning.load(); }
    int getGameSampleRate() const { return m_gameSampleRate.load(); }
    void setGameSampleRate(int rate) { if (rate > 0) m_gameSampleRate.store(rate); }
    int getMicSampleRate() const { return m_micSampleRate.load(); }

private:
    AudioGrabber();
    ~AudioGrabber();

    void setupFmodDSP();
    void removeFmodDSP();

    void micCaptureThreadFunc();

    bool m_initialized = false;
    FMOD::DSP* m_fmodDSP = nullptr;

    AudioCallback m_gameCallback;
    AudioCallback m_micCallback;

    std::atomic<int> m_gameSampleRate{44100};
    std::atomic<int> m_micSampleRate{48000};
    std::atomic<bool> m_micRunning{false};
    std::thread m_micThread;
};

} // namespace replay
