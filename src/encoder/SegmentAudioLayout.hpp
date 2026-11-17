#pragma once

#include <cstdint>

namespace replay {

// Windows segment audio layout (W2).
//
// Media Foundation's AAC encoder cannot reproduce the first ~11 ms (about 530 frames at 48 kHz)
// of every independently encoded stream: they decode as silence. Remuxing AAC packets of
// 30-second segments therefore put a dropout at every seam. Each Windows segment's AAC tracks
// now start with kSegmentAudioPrerollHns of silence and all real audio is shifted by it; the
// exporter decodes every segment, drops the pre-roll and the trailing AAC padding (using the
// real audio frame counts recorded per segment) and re-encodes one continuous AAC stream.
inline constexpr int64_t kSegmentAudioPrerollHns = 500000; // 50 ms

inline constexpr int64_t segmentAudioPrerollFrames(int sampleRate) noexcept {
    return (sampleRate > 0) ? (kSegmentAudioPrerollHns * sampleRate) / 10000000LL : 0;
}

} // namespace replay
