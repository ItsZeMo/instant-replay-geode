#pragma once

#include "ConcatResult.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace replay::mfconcat {

// Windows-only, Geode-free Media Foundation segment exporter (compiled into the mod and the host tests).
//
// Video is remuxed without re-encoding (keyframe-aligned trim, B-frame DTS rebasing via
// ConcatTimestampState). Audio tracks are decoded to PCM, stripped of each segment's pre-roll and
// trailing AAC padding, stitched sample-exactly on the segment timeline and re-encoded as one
// continuous AAC stream per track, so seams carry neither AAC start-up dropouts nor overlap-add
// artefacts from independently encoded segments (W2).

struct SegmentInput {
    std::wstring path;
    double durationSeconds = 0.0;      // progress weighting only
    int64_t audioFrames[2] = {-1, -1}; // real frames per AAC track (0 = game, 1 = mic); -1 = unknown
};

struct Options {
    int64_t audioPrerollHns = 0;                  // pre-roll at the start of every segment audio track
    std::function<void(const std::string&)> logInfo;
    std::function<void(const std::string&)> logWarn;
    std::atomic<int>* progressPermille = nullptr; // 0..1000 while the export runs
};

ConcatResult concatenate(
    const std::vector<SegmentInput>& segments,
    const std::wstring& outputPath,
    double trimStartSeconds,
    const Options& options
);

} // namespace replay::mfconcat
