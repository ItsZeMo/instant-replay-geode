#pragma once

#include <Geode/Geode.hpp>

#ifdef GEODE_IS_WINDOWS

#include <Geode/fmod/fmod.hpp>
#include <vector>
#include <memory>
#include <functional>
#include <thread>
#include <atomic>
#include <chrono>
#include <algorithm>

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

// Stateful fractional-phase linear stereo resampler shared by AudioGrabber and ReplayManager (#28).
struct StereoResampler {
    double pos = 0.0;
    float prev[2] = {0.0f, 0.0f};
    bool hasPrev = false;
    int lastInRate = 0;
    int lastOutRate = 0;

    void reset() noexcept {
        pos = 0.0;
        prev[0] = 0.0f;
        prev[1] = 0.0f;
        hasPrev = false;
        lastInRate = 0;
        lastOutRate = 0;
    }

    // Resamples `inFrames` of interleaved stereo float PCM (`inStereo`) from `inRate` to `outRate` into `outStereo`.
    void process(const float* inStereo, size_t inFrames, int inRate, int outRate, std::vector<float>& outStereo) {
        outStereo.clear();
        if (!inStereo || inFrames == 0 || outRate <= 0) return;
        if (inRate <= 0) inRate = outRate;

        if (lastInRate != inRate || lastOutRate != outRate) {
            reset();
            lastInRate = inRate;
            lastOutRate = outRate;
        }

        if (inRate == outRate) {
            outStereo.assign(inStereo, inStereo + inFrames * 2);
            return;
        }

        double step = static_cast<double>(inRate) / static_cast<double>(outRate);
        size_t estOutFrames = static_cast<size_t>(static_cast<double>(inFrames) / step) + 2;
        outStereo.reserve(estOutFrames * 2);

        if (!hasPrev) {
            prev[0] = inStereo[0];
            prev[1] = inStereo[1];
            hasPrev = true;
            pos = 0.0;
        }

        // Position -1.0 corresponds to `prev`; positions [0, inFrames - 1] are in `inStereo`.
        double limit = static_cast<double>(inFrames) - 1.0;
        while (pos < limit) {
            float l0 = 0.0f, r0 = 0.0f, l1 = 0.0f, r1 = 0.0f;
            float frac = 0.0f;
            if (pos < 0.0) {
                frac = static_cast<float>(pos + 1.0);
                l0 = prev[0];
                r0 = prev[1];
                l1 = inStereo[0];
                r1 = inStereo[1];
            } else {
                size_t idx0 = static_cast<size_t>(pos);
                size_t idx1 = std::min<size_t>(idx0 + 1, inFrames - 1);
                frac = static_cast<float>(pos - static_cast<double>(idx0));
                l0 = inStereo[idx0 * 2];
                r0 = inStereo[idx0 * 2 + 1];
                l1 = inStereo[idx1 * 2];
                r1 = inStereo[idx1 * 2 + 1];
            }
            outStereo.push_back(l0 + frac * (l1 - l0));
            outStereo.push_back(r0 + frac * (r1 - r0));
            pos += step;
        }

        prev[0] = inStereo[(inFrames - 1) * 2];
        prev[1] = inStereo[(inFrames - 1) * 2 + 1];
        pos -= static_cast<double>(inFrames);
    }
};

class AudioGrabber {
public:
    static AudioGrabber* get();

    void initialize();
    void cleanup();

    static std::vector<AudioDeviceInfo> getAvailableMicrophones();
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

    // Called by WASAPI microphone capture thread with packet capture timestamp (#23)
    void pushMicAudio(const float* inbuffer, unsigned int length, int channels, int sampleRate,
                      std::chrono::steady_clock::time_point captureTime);

    void ensureDSPAttached();
    int getGameSampleRate() const { return m_gameSampleRate.load(std::memory_order_relaxed); }
    // Microsoft's AAC MFT only accepts 44100 or 48000 Hz (#18)
    int getEncodedGameSampleRate() const {
        int rate = m_gameSampleRate.load(std::memory_order_relaxed);
        return (rate == 44100) ? 44100 : 48000;
    }
    void setGameSampleRate(int rate) {
        if (rate > 0) m_gameSampleRate.store(rate, std::memory_order_relaxed);
    }

private:
    AudioGrabber();
    ~AudioGrabber();

    void setupFmodDSP();
    void removeFmodDSP();

    void micCaptureThreadFunc();

    bool m_initialized = false;
    FMOD::DSP* m_fmodDSP = nullptr;
    std::chrono::steady_clock::time_point m_nextDspRetryTime{};

    // Lock-free SPSC ring buffer for real-time FMOD DSP game audio (power-of-two stereo frames)
    static constexpr size_t kGameRingFrames = 65536; // ~1.36s at 48 kHz
    static constexpr size_t kGameRingMask = kGameRingFrames - 1;
    std::vector<float> m_gameRingBuffer;
    std::atomic<size_t> m_gameRingWriteFrame{0};
    std::atomic<size_t> m_gameRingReadFrame{0};
    std::atomic<int64_t> m_lastGamePushTimeNs{0};

    AudioCallback m_micCallback;

    // Stateful microphone resampler & scratch buffers (WASAPI thread only)
    StereoResampler m_micResampler;
    std::vector<float> m_micConvertScratch;
    std::vector<float> m_micStereoScratch;

    std::atomic<int> m_gameSampleRate{44100};
    std::atomic<bool> m_micRunning{false};
    std::thread m_micThread;
};

} // namespace replay

#endif // GEODE_IS_WINDOWS
