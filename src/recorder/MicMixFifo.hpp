#pragma once

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace replay {

// Time-aligned interleaved-stereo FIFO for single-track microphone mixing.
// Tracks the timeline offset (in targetRate frames relative to current segment start)
// of the front unread sample so clock drift, pauses, overlaps, and segment switches stay synced.
struct MicMixFifo {
    std::vector<float> samples; // interleaved stereo (2 floats per frame)
    size_t readPos = 0;         // float index into `samples`
    int rate = 0;
    int64_t headSample = 0;     // frame offset relative to m_currentSegmentStart
    bool hasAnchor = false;

    void reset() noexcept {
        samples.clear();
        readPos = 0;
        rate = 0;
        headSample = 0;
        hasAnchor = false;
    }

    size_t availFrames() const noexcept {
        return (samples.size() > readPos) ? ((samples.size() - readPos) / 2) : 0;
    }

    void compact() {
        if (readPos >= samples.size()) {
            samples.clear();
            readPos = 0;
        } else if (readPos >= 16384 && readPos >= samples.size() / 2) {
            samples.erase(samples.begin(), samples.begin() + static_cast<std::ptrdiff_t>(readPos));
            readPos = 0;
        }
    }

    size_t advanceFrames(size_t frameCount) {
        size_t avail = availFrames();
        size_t drop = std::min(frameCount, avail);
        if (drop == 0) return 0;
        readPos += drop * 2;
        headSample += static_cast<int64_t>(drop);
        compact();
        if (availFrames() == 0) {
            hasAnchor = false;
        }
        return drop;
    }

    // Shift timeline origin when switching smoothly from segStartOld -> segStartNew on continuation segments.
    void rebaseSegmentOrigin(
        std::chrono::steady_clock::time_point prevSegStart,
        std::chrono::steady_clock::time_point nextSegStart
    ) noexcept {
        if (!hasAnchor || rate <= 0) return;
        double deltaSec = std::chrono::duration<double>(nextSegStart - prevSegStart).count();
        int64_t deltaSamples = static_cast<int64_t>(std::llround(deltaSec * rate));
        headSample -= deltaSamples;
    }

    // Appends resampled stereo PCM whose capture starts at `chunkStartSample` (frames from seg start).
    void appendResampled(
        const float* stereoPtr,
        size_t frameCount,
        int targetRate,
        int64_t chunkStartSample,
        size_t maxBufferedFrames
    ) {
        if (!stereoPtr || frameCount == 0 || targetRate <= 0) return;
        if (rate != targetRate) {
            reset();
            rate = targetRate;
        }

        const int64_t tolerance = std::max<int64_t>(1, targetRate / 25); // 40ms
        size_t curFrames = availFrames();

        if (curFrames == 0 || !hasAnchor) {
            samples.assign(stereoPtr, stereoPtr + frameCount * 2);
            readPos = 0;
            headSample = chunkStartSample;
            hasAnchor = true;
        } else {
            const int64_t fifoEnd = headSample + static_cast<int64_t>(curFrames);
            const int64_t drift = chunkStartSample - fifoEnd;

            if (drift > tolerance) {
                // Positive gap > 40ms between FIFO tail and incoming mic packet: insert aligned silence or re-anchor
                const size_t padFrames = static_cast<size_t>(drift);
                if (padFrames <= maxBufferedFrames / 2) {
                    samples.resize(samples.size() + padFrames * 2, 0.0f);
                } else {
                    samples.clear();
                    readPos = 0;
                    headSample = chunkStartSample;
                }
            } else if (drift < -tolerance) {
                // Incoming mic packet overlaps buffered tail beyond -40ms tolerance: trim overlapping head
                const size_t overlap = static_cast<size_t>(-drift);
                if (overlap >= frameCount) {
                    return;
                }
                stereoPtr += overlap * 2;
                frameCount -= overlap;
            }
            samples.insert(samples.end(), stereoPtr, stereoPtr + frameCount * 2);
        }

        size_t nowFrames = availFrames();
        if (maxBufferedFrames > 0 && nowFrames > maxBufferedFrames) {
            advanceFrames(nowFrames - maxBufferedFrames);
        } else {
            compact();
        }
    }

    // Discards buffered mic samples strictly older than targetPos - 40ms.
    void discardStaleBefore(int64_t targetSamplePos) {
        if (!hasAnchor || rate <= 0) return;
        const int64_t staleFloor = targetSamplePos - std::max<int64_t>(1, rate / 25); // P - 40ms
        if (headSample < staleFloor) {
            advanceFrames(static_cast<size_t>(staleFloor - headSample));
        }
    }

    // When game audio trims leading overlap frames before targetSamplePos, consume equivalent trailing mic.
    void advanceOnGameOverlap(int64_t targetSamplePos, int64_t overlapFrames) {
        if (!hasAnchor || overlapFrames <= 0) return;
        if (headSample < targetSamplePos) {
            int64_t maxShift = std::min(overlapFrames, targetSamplePos - headSample);
            if (maxShift > 0) {
                advanceFrames(static_cast<size_t>(maxShift));
            }
        }
    }

    // Mixes mic samples covering [targetSamplePos .. targetSamplePos + numFrames) into stereoOut.
    // Returns number of mic frames consumed.
    size_t mixInto(float* stereoOut, size_t numFrames, int64_t targetSamplePos) {
        if (!stereoOut || numFrames == 0 || !hasAnchor || rate <= 0) return 0;

        discardStaleBefore(targetSamplePos);
        size_t avail = availFrames();
        if (avail == 0 || !hasAnchor) return 0;

        const int64_t windowEnd = targetSamplePos + static_cast<int64_t>(numFrames);
        if (headSample >= windowEnd) {
            // Mic FIFO starts in the future relative to this game audio chunk; hold it until targetPos catches up.
            return 0;
        }

        const size_t dstOffset = (headSample > targetSamplePos)
            ? static_cast<size_t>(headSample - targetSamplePos)
            : 0;
        const size_t mixFrames = std::min(numFrames - dstOffset, avail);

        const float* micPtr = samples.data() + readPos;
        float* dstPtr = stereoOut + dstOffset * 2;
        for (size_t i = 0; i < mixFrames; ++i) {
            size_t base = i * 2;
            dstPtr[base] = std::clamp(dstPtr[base] + micPtr[base], -1.0f, 1.0f);
            dstPtr[base + 1] = std::clamp(dstPtr[base + 1] + micPtr[base + 1], -1.0f, 1.0f);
        }

        advanceFrames(mixFrames);
        return mixFrames;
    }
};

} // namespace replay
