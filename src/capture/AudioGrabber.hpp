#pragma once

#include <Geode/Geode.hpp>

#if defined(GEODE_IS_WINDOWS) || defined(GEODE_IS_MACOS)

#include "GameAudioRing.hpp"
#include "MicStatusPolicy.hpp"
#include <Geode/fmod/fmod.hpp>
#include <string>
#include <string_view>
#include <vector>
#include <memory>
#include <functional>
#include <thread>
#include <mutex>
#include <atomic>
#include <chrono>
#include <algorithm>

namespace replay {

struct AudioChunk {
    std::vector<float> samples; // Interleaved 32-bit float PCM (2-channel stereo at 48000 Hz)
    std::chrono::steady_clock::time_point timestamp;
};

struct AudioDeviceInfo {
    std::string id;   // Endpoint device ID, or "default"
    std::string name; // Sanitized friendly name for display
};

// Stateful fractional-phase linear stereo resampler shared by AudioGrabber and ReplayManager.
// Appends converted stereo samples to `outStereo` without clearing existing elements.
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

    void process(const float* inStereo, size_t inFrames, int inRate, int outRate, std::vector<float>& outStereo) {
        if (!inStereo || inFrames == 0 || inRate <= 0 || outRate <= 0) return;

        if (lastInRate != inRate || lastOutRate != outRate) {
            reset();
            lastInRate = inRate;
            lastOutRate = outRate;
        }

        if (inRate == outRate) {
            outStereo.insert(outStereo.end(), inStereo, inStereo + inFrames * 2);
            return;
        }

        double step = static_cast<double>(inRate) / static_cast<double>(outRate);
        size_t estOutFrames = static_cast<size_t>(static_cast<double>(inFrames) / step) + 2;
        outStereo.reserve(outStereo.size() + estOutFrames * 2);

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
    static constexpr int kMicSampleRate = 48000;

    void initialize();
    void cleanup();

    static std::vector<AudioDeviceInfo> getAvailableMicrophones();
    static std::string sanitizeDeviceNameUtf8(std::string_view rawName);
    void startMicCapture();
    void stopMicCapture();
    MicCaptureStatus getMicCaptureStatus() const noexcept {
        return m_micStatus.load(std::memory_order_acquire);
    }

    // Microphone callback (WASAPI thread -> ReplayManager worker queue)
    using AudioCallback = std::function<void(std::shared_ptr<AudioChunk>)>;
    void setMicAudioCallback(AudioCallback cb);

    // Called on the real-time FMOD mixer thread (lock-free, allocation-free SPSC ring buffer)
    void pushGameAudio(const float* inbuffer, unsigned int length, int channels, int sampleRate);

    // Drained by the ReplayManager worker thread; returns number of stereo frames read
    size_t drainGameAudio(std::vector<float>& outStereoSamples, int& outSampleRate,
                          std::chrono::steady_clock::time_point& outTimestamp);
    bool hasPendingGameAudio() const noexcept;
    size_t pendingGameAudioFrames() const noexcept;
    void resetGameAudioBuffer() noexcept;

    // Called by WASAPI microphone capture thread with packet capture timestamp
    void pushMicAudio(const float* inbuffer, unsigned int length, int channels, int sampleRate,
                      std::chrono::steady_clock::time_point captureTime);

    void ensureDSPAttached();
    // Plays an FMOD SFX through a dedicated ChannelGroup wired to Master HEAD DSP,
    // bypassing the tail capture DSP so notification sounds never enter future replays.
    bool playSaveSoundBypass(const char* soundPath);
    // Microsoft's AAC MFT only accepts 44100 or 48000 Hz
    int getEncodedGameSampleRate() const noexcept {
        int rate = m_gameSampleRate.load(std::memory_order_relaxed);
        return (rate == 44100) ? 44100 : 48000;
    }
    void setGameSampleRate(int rate) noexcept {
        if (rate > 0) m_gameSampleRate.store(rate, std::memory_order_relaxed);
    }

private:
    AudioGrabber();
    ~AudioGrabber();

    // Call with m_dspMutex held.
    void setupFmodDSP();
    void removeFmodDSP();
    void cleanupSaveBypass();

    void micCaptureThreadFunc();

    // Serializes the capture DSP / save-bypass state: FMODAudioEngine::update re-verifies the DSP on the
    // game thread while shutdown (macOS: $on_game(Exiting) on the AppKit main thread) tears it down (K5).
    // Recursive so a same-thread re-entry (e.g. GD code reached from playSaveSoundBypass calling back into
    // FMODAudioEngine::update) cannot self-deadlock. The FMOD mixer callback (pushGameAudio) never takes it.
    std::recursive_mutex m_dspMutex;
    std::atomic<bool> m_initialized{false};
    FMOD::DSP* m_fmodDSP = nullptr;
    FMOD::System* m_dspSystem = nullptr;            // system / master group the DSP was attached to (W18)
    FMOD::ChannelGroup* m_dspMasterGroup = nullptr;
    std::chrono::steady_clock::time_point m_nextDspCheckTime{};
    FMOD::ChannelGroup* m_saveBypassGroup = nullptr;
    FMOD::Sound* m_cachedSaveSound = nullptr;
    std::chrono::steady_clock::time_point m_nextDspRetryTime{};

    // Lock-free SPSC ring buffer + per-packet metadata descriptors for FMOD game audio
    GameAudioRing m_gameRing;

    AudioCallback m_micCallback;

    // Stateful microphone resampler & scratch buffers (WASAPI thread only)
    StereoResampler m_micResampler;
    std::vector<float> m_micConvertScratch;
    std::vector<float> m_micStereoScratch;

    std::atomic<int> m_gameSampleRate{48000};
    std::atomic<bool> m_micRunning{false};
    std::atomic<MicCaptureStatus> m_micStatus{MicCaptureStatus::Idle};
    std::thread m_micThread;
};

} // namespace replay

#endif // GEODE_IS_WINDOWS || GEODE_IS_MACOS
