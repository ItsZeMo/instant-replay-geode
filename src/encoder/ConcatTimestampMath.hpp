#pragma once

#include <algorithm>
#include <cstdint>
#include <map>

namespace replay {

// Tracks cross-segment timestamp rebasing during lossless MP4 concatenation without masking overlaps.
struct ConcatTimestampState {
    int64_t outputBase = 0;
    int64_t segMaxEnd = 0;
    int64_t segMaxVideoPts = -1;
    int64_t segMaxVideoEnd = 0;
    int64_t segLastVideoFrameDurHns = 0;
    int64_t lastVideoDts = 0;
    bool hasLastVideoDts = false;
    std::map<uint32_t, int64_t> lastAudioEndByWriterStream;
    std::map<uint32_t, int64_t> segAudioEndByWriterStream;

    explicit ConcatTimestampState(int64_t initialShift = 0) noexcept
        : outputBase(initialShift), segMaxEnd(initialShift), segMaxVideoEnd(initialShift) {}

    void beginSegment() noexcept {
        segMaxEnd = outputBase;
        segMaxVideoPts = -1;
        segMaxVideoEnd = outputBase;
        segLastVideoFrameDurHns = 0;
        segAudioEndByWriterStream.clear();
    }

    int64_t computeSampleTime(int64_t srcPts, int64_t segCut) const noexcept {
        return std::max<int64_t>(0, srcPts - segCut + outputBase);
    }

    // Recomputes video DTS monotonically. Windows MF requires non-negative DTS and PTS >= DTS
    // (clampPtsToDts = true, allowNegativeDts = false); macOS AVAssetWriter preserves negative B-frame
    // decode lead directly without shifting movie start_time (allowNegativeDts = true, clampPtsToDts = false).
    void adjustVideoTimestamps(
        int64_t rawDts,
        int64_t segCut,
        int64_t& inOutPts,
        int64_t& outNewDts,
        bool clampPtsToDts = true,
        bool allowNegativeDts = false
    ) noexcept {
        int64_t newDts = rawDts - segCut + outputBase;
        if (!allowNegativeDts && newDts < 0) {
            newDts = 0;
        }
        if (hasLastVideoDts && newDts <= lastVideoDts) {
            newDts = lastVideoDts + 1000LL;
        }
        if (clampPtsToDts && inOutPts < newDts) {
            inOutPts = newDts;
        }
        lastVideoDts = newDts;
        hasLastVideoDts = true;
        outNewDts = newDts;
    }

    // One-sample HNS tolerance (ceil(1e7 / sampleRate)) absorbs independent 100ns integer rounding
    // of packet PTS vs duration (e.g. 1024/48000s = 213333.33 HNS, N4) without masking real overlaps.
    static constexpr int64_t audioOverlapToleranceHns(int sampleRate = 44100) noexcept {
        if (sampleRate <= 0) return 227LL;
        return (10'000'000LL + sampleRate - 1) / sampleRate;
    }

    // Records written sample bounds; returns false if audio overlaps the prior written packet beyond toleranceHns.
    bool recordWrittenSample(
        uint32_t writerIndex,
        bool isVideo,
        int64_t sampleTime,
        int64_t sampleDuration,
        int64_t* outPrevAudioEnd = nullptr,
        int64_t toleranceHns = audioOverlapToleranceHns(44100)
    ) {
        const int64_t dur = std::max<int64_t>(sampleDuration, 1LL);
        const int64_t sampleEnd = sampleTime + dur;
        bool overlap = false;
        if (isVideo) {
            segMaxVideoPts = std::max(segMaxVideoPts, sampleTime);
            segMaxVideoEnd = std::max(segMaxVideoEnd, sampleEnd);
            if (sampleDuration > 0) {
                segLastVideoFrameDurHns = sampleDuration;
            }
        } else {
            auto it = lastAudioEndByWriterStream.find(writerIndex);
            if (it != lastAudioEndByWriterStream.end()) {
                const int64_t tol = std::max<int64_t>(0, toleranceHns);
                if (sampleTime + tol < it->second) {
                    overlap = true;
                    if (outPrevAudioEnd) *outPrevAudioEnd = it->second;
                }
                it->second = std::max(it->second, sampleEnd);
            } else {
                lastAudioEndByWriterStream[writerIndex] = sampleEnd;
            }

            auto segIt = segAudioEndByWriterStream.find(writerIndex);
            if (segIt != segAudioEndByWriterStream.end()) {
                segIt->second = std::max(segIt->second, sampleEnd);
            } else {
                segAudioEndByWriterStream[writerIndex] = sampleEnd;
            }
        }
        segMaxEnd = std::max(segMaxEnd, sampleEnd);
        return !overlap;
    }

    // Advances outputBase to the end of the primary audio track (smallest audio writerIndex),
    // clamped >= last video PTS + 1 to keep video monotonically ordered across the join.
    // Falls back to segMaxEnd (and signals *outAnomalyWarn) if video outlasts audio by > 1.5 frames.
    void endSegment(
        int64_t expectedVideoFrameDurHns = 0,
        bool* outAnomalyWarn = nullptr,
        int64_t* outExcessOverAudioHns = nullptr
    ) noexcept {
        if (outAnomalyWarn) *outAnomalyWarn = false;
        if (outExcessOverAudioHns) *outExcessOverAudioHns = 0;

        if (segAudioEndByWriterStream.empty()) {
            outputBase = segMaxEnd;
            return;
        }

        const int64_t primaryAudioEnd = segAudioEndByWriterStream.begin()->second;
        const int64_t frameDur = (expectedVideoFrameDurHns > 0)
            ? expectedVideoFrameDurHns
            : segLastVideoFrameDurHns;

        if (segMaxVideoPts >= 0 && frameDur > 0) {
            const int64_t excess = segMaxVideoEnd - primaryAudioEnd;
            const int64_t anomalyThreshold = (frameDur * 3) / 2;
            if (excess > anomalyThreshold) {
                if (outAnomalyWarn) *outAnomalyWarn = true;
                if (outExcessOverAudioHns) *outExcessOverAudioHns = excess;
                outputBase = segMaxEnd;
                return;
            }
        }

        outputBase = (segMaxVideoPts >= 0)
            ? std::max(primaryAudioEnd, segMaxVideoPts + 1)
            : primaryAudioEnd;
    }
};

} // namespace replay
