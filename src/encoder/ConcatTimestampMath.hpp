#pragma once

#include <algorithm>
#include <cstdint>
#include <map>

namespace replay {

// Tracks cross-segment timestamp rebasing during lossless MP4 concatenation without masking overlaps.
struct ConcatTimestampState {
    int64_t outputBase = 0;
    int64_t segMaxEnd = 0;
    int64_t lastVideoDts = -1;
    std::map<uint32_t, int64_t> lastAudioEndByWriterStream;

    explicit ConcatTimestampState(int64_t initialShift = 0) noexcept
        : outputBase(initialShift), segMaxEnd(initialShift) {}

    void beginSegment() noexcept {
        segMaxEnd = outputBase;
    }

    int64_t computeSampleTime(int64_t srcPts, int64_t segCut) const noexcept {
        return std::max<int64_t>(0, srcPts - segCut + outputBase);
    }

    // Recomputes video DTS monotonically while keeping PTS >= DTS and preserving B-frame reorder lead.
    void adjustVideoTimestamps(int64_t rawDts, int64_t segCut, int64_t& inOutPts, int64_t& outNewDts) noexcept {
        int64_t newDts = std::max<int64_t>(0, rawDts - segCut + outputBase);
        if (lastVideoDts >= 0 && newDts <= lastVideoDts) {
            newDts = lastVideoDts + 1000LL;
        }
        if (inOutPts < newDts) {
            inOutPts = newDts;
        }
        lastVideoDts = newDts;
        outNewDts = newDts;
    }

    // Records written sample bounds; returns false if audio overlaps the prior written packet on the stream.
    bool recordWrittenSample(
        uint32_t writerIndex,
        bool isVideo,
        int64_t sampleTime,
        int64_t sampleDuration,
        int64_t* outPrevAudioEnd = nullptr
    ) {
        const int64_t dur = std::max<int64_t>(sampleDuration, 1LL);
        const int64_t sampleEnd = sampleTime + dur;
        bool overlap = false;
        if (!isVideo) {
            auto it = lastAudioEndByWriterStream.find(writerIndex);
            if (it != lastAudioEndByWriterStream.end()) {
                if (sampleTime < it->second) {
                    overlap = true;
                    if (outPrevAudioEnd) *outPrevAudioEnd = it->second;
                }
                it->second = std::max(it->second, sampleEnd);
            } else {
                lastAudioEndByWriterStream[writerIndex] = sampleEnd;
            }
        }
        segMaxEnd = std::max(segMaxEnd, sampleEnd);
        return !overlap;
    }

    void endSegment() noexcept {
        // Next segment base must never precede the end of any video or audio sample already written.
        outputBase = segMaxEnd;
    }
};

} // namespace replay
