#pragma once

#include <algorithm>
#include <cstdint>

namespace replay {

// Pure segment bookkeeping rules shared by ReplayManager and the host tests.

// A segment lasts until the later of its video and game-audio ends: with GD minimized (no swaps)
// FMOD keeps producing audio while video stops, and a video-only duration made exports pick extra
// segments and shifted the trim point (W14).
inline double segmentDurationSeconds(
    int64_t lastVideoHns,      // PTS of the last written frame, < 0 when no frame was written
    double frameDurationSec,
    int64_t gameAudioFrames,   // real game-audio frames written to the segment
    int gameSampleRate
) noexcept {
    const double videoEnd = (lastVideoHns >= 0)
        ? static_cast<double>(lastVideoHns) / 1e7 + std::max(0.0, frameDurationSec)
        : 0.0;
    const double audioEnd = (gameSampleRate > 0 && gameAudioFrames > 0)
        ? static_cast<double>(gameAudioFrames) / static_cast<double>(gameSampleRate)
        : 0.0;
    return std::max(videoEnd, audioEnd);
}

// Media Foundation cannot finalize an MP4 whose video stream received no samples, so at least one
// frame is still required; otherwise a segment is kept when any track lasts longer than 0.25 s.
inline bool shouldKeepSegment(bool finalizedOk, bool hasVideo, double durationSeconds) noexcept {
    return finalizedOk && hasVideo && durationSeconds > 0.25;
}

} // namespace replay
