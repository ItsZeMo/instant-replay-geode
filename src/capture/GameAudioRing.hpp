#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

namespace replay {

struct GameAudioPacketMeta {
    size_t endFrame = 0;
    int64_t endTimeNs = 0;
    int sampleRate = 48000;
};

// Lock-free, allocation-free SPSC ring binding each FMOD audio range to its exact capture timestamp and sample rate.
class GameAudioRing {
public:
    static constexpr size_t kRingFrames = 262144; // ~5.46s at 48 kHz
    static constexpr size_t kRingMask = kRingFrames - 1;
    static constexpr size_t kMetaCapacity = 8192;
    static constexpr size_t kMetaMask = kMetaCapacity - 1;

    static void downmixFrameToStereo(const float* frame, int channels, float vol, float& outL, float& outR) noexcept {
        float l = 0.0f;
        float r = 0.0f;
        switch (channels) {
            case 1:
                l = r = frame[0];
                break;
            case 2:
                l = frame[0];
                r = frame[1];
                break;
            case 3: { // 3.0: FL, FR, FC
                float c = frame[2] * 0.7071f;
                l = frame[0] + c;
                r = frame[1] + c;
                break;
            }
            case 4: // 4.0 Quad: FL, FR, BL, BR
                l = frame[0] + frame[2] * 0.7071f;
                r = frame[1] + frame[3] * 0.7071f;
                break;
            case 5: { // 5.0: FL, FR, FC, BL, BR
                float c = frame[2] * 0.7071f;
                l = frame[0] + c + frame[3] * 0.7071f;
                r = frame[1] + c + frame[4] * 0.7071f;
                break;
            }
            case 6: { // 5.1: FL, FR, FC, LFE, BL, BR
                float centerLfe = frame[2] * 0.7071f + frame[3] * 0.5f;
                l = frame[0] + centerLfe + frame[4] * 0.7071f;
                r = frame[1] + centerLfe + frame[5] * 0.7071f;
                break;
            }
            case 7: { // 6.1: FL, FR, FC, LFE, BC, SL, SR
                float mid = frame[2] * 0.7071f + frame[3] * 0.5f + frame[4] * 0.5f;
                l = frame[0] + mid + frame[5] * 0.7071f;
                r = frame[1] + mid + frame[6] * 0.7071f;
                break;
            }
            default: { // >= 8 (7.1: FL, FR, FC, LFE, BL, BR, SL, SR)
                float centerLfe = frame[2] * 0.7071f + frame[3] * 0.5f;
                l = frame[0] + centerLfe + (frame[4] + frame[6]) * 0.7071f;
                r = frame[1] + centerLfe + (frame[5] + frame[7]) * 0.7071f;
                break;
            }
        }
        outL = std::clamp(l * vol, -1.0f, 1.0f);
        outR = std::clamp(r * vol, -1.0f, 1.0f);
    }

    GameAudioRing()
        : m_pcm(kRingFrames * 2, 0.0f)
        , m_meta(kMetaCapacity) {}

    bool push(
        const float* inbuffer,
        unsigned int length,
        int channels,
        float volume,
        int sampleRate,
        int64_t endTimeNs
    ) noexcept {
        if (!inbuffer || length == 0 || channels <= 0 || sampleRate <= 0) return false;

        const size_t writePos = m_pcmWriteFrame.load(std::memory_order_relaxed);
        const size_t readPos = m_pcmReadFrame.load(std::memory_order_acquire);
        const size_t usedFrames = writePos - readPos;
        if (usedFrames >= kRingFrames) {
            return false; // PCM ring full; drop rather than overwrite unread frames or block FMOD
        }

        const size_t metaWrite = m_metaWriteIdx.load(std::memory_order_relaxed);
        const size_t metaRead = m_metaReadIdx.load(std::memory_order_acquire);
        if ((metaWrite - metaRead) >= kMetaCapacity) {
            return false; // Metadata ring full; drop rather than overwrite unread packet metadata
        }

        const size_t availFrames = kRingFrames - usedFrames;
        const size_t framesToWrite = std::min<size_t>(length, availFrames);

        for (size_t i = 0; i < framesToWrite; ++i) {
            float l = 0.0f;
            float r = 0.0f;
            downmixFrameToStereo(&inbuffer[i * static_cast<size_t>(channels)], channels, volume, l, r);
            const size_t slot = (writePos + i) & kRingMask;
            m_pcm[slot * 2] = l;
            m_pcm[slot * 2 + 1] = r;
        }

        m_meta[metaWrite & kMetaMask] = { writePos + framesToWrite, endTimeNs, sampleRate };
        m_pcmWriteFrame.store(writePos + framesToWrite, std::memory_order_release);
        m_metaWriteIdx.store(metaWrite + 1, std::memory_order_release);
        return true;
    }

    size_t drain(
        std::vector<float>& outStereoSamples,
        int& outSampleRate,
        std::chrono::steady_clock::time_point& outTimestamp,
        const std::function<void()>& afterSnapshotHook = {}
    ) {
        const size_t metaWrite = m_metaWriteIdx.load(std::memory_order_acquire);
        const size_t metaRead = m_metaReadIdx.load(std::memory_order_relaxed);
        if (metaWrite <= metaRead) {
            outStereoSamples.clear();
            return 0;
        }

        if (afterSnapshotHook) {
            afterSnapshotHook();
        }

        const size_t readPos = m_pcmReadFrame.load(std::memory_order_relaxed);
        const int firstRate = m_meta[metaRead & kMetaMask].sampleRate;
        size_t batchEndIdx = metaRead + 1;
        while (batchEndIdx < metaWrite && m_meta[batchEndIdx & kMetaMask].sampleRate == firstRate) {
            ++batchEndIdx;
        }

        const GameAudioPacketMeta tailMeta = m_meta[(batchEndIdx - 1) & kMetaMask];
        const size_t targetWritePos = tailMeta.endFrame;
        if (targetWritePos <= readPos) {
            outStereoSamples.clear();
            m_pcmReadFrame.store(targetWritePos, std::memory_order_release);
            m_metaReadIdx.store(batchEndIdx, std::memory_order_release);
            return 0;
        }

        const size_t frames = targetWritePos - readPos;
        outStereoSamples.resize(frames * 2);
        for (size_t i = 0; i < frames; ++i) {
            const size_t slot = (readPos + i) & kRingMask;
            outStereoSamples[i * 2] = m_pcm[slot * 2];
            outStereoSamples[i * 2 + 1] = m_pcm[slot * 2 + 1];
        }

        m_pcmReadFrame.store(targetWritePos, std::memory_order_release);
        m_metaReadIdx.store(batchEndIdx, std::memory_order_release);

        const int rate = (tailMeta.sampleRate > 0) ? tailMeta.sampleRate : 48000;
        outSampleRate = rate;

        const int64_t endNs = tailMeta.endTimeNs;
        const int64_t durationNs = (static_cast<int64_t>(frames) * 1000000000LL) / rate;
        if (endNs > durationNs) {
            outTimestamp = std::chrono::steady_clock::time_point(std::chrono::nanoseconds(endNs - durationNs));
        } else {
            outTimestamp = std::chrono::steady_clock::now();
        }
        return frames;
    }

    bool hasPending() const noexcept {
        return m_metaWriteIdx.load(std::memory_order_acquire) > m_metaReadIdx.load(std::memory_order_relaxed);
    }

    size_t pendingFrames() const noexcept {
        const size_t writePos = m_pcmWriteFrame.load(std::memory_order_acquire);
        const size_t readPos = m_pcmReadFrame.load(std::memory_order_relaxed);
        return (writePos > readPos) ? (writePos - readPos) : 0;
    }

    // Discards buffered frames without moving metadata out of sync with PCM positions.
    void reset() noexcept {
        const size_t metaWrite = m_metaWriteIdx.load(std::memory_order_acquire);
        const size_t metaRead = m_metaReadIdx.load(std::memory_order_relaxed);
        if (metaWrite > metaRead) {
            const size_t endFrame = m_meta[(metaWrite - 1) & kMetaMask].endFrame;
            m_pcmReadFrame.store(endFrame, std::memory_order_release);
            m_metaReadIdx.store(metaWrite, std::memory_order_release);
        }
    }

private:
    std::vector<float> m_pcm;
    std::vector<GameAudioPacketMeta> m_meta;
    std::atomic<size_t> m_pcmWriteFrame{0};
    std::atomic<size_t> m_pcmReadFrame{0};
    std::atomic<size_t> m_metaWriteIdx{0};
    std::atomic<size_t> m_metaReadIdx{0};
};

} // namespace replay
