#pragma once

#include <cstddef>
#include <cstdint>

namespace replay {

// Bounds a queue of audio chunks by duration (frames) instead of chunk count (W15): drops the
// oldest chunks until the queued frames plus `incomingFrames` fit in `maxFrames`, always keeping
// room for the incoming chunk. Returns the number of frames dropped.
template <typename QueueT, typename FramesFn>
inline uint64_t enforceAudioQueueFrameLimit(QueueT& queue, size_t incomingFrames, size_t maxFrames, FramesFn&& framesOf) {
    size_t queued = 0;
    for (const auto& item : queue) {
        queued += framesOf(item);
    }
    uint64_t dropped = 0;
    while (!queue.empty() && queued + incomingFrames > maxFrames) {
        const size_t frames = framesOf(queue.front());
        queued -= (frames <= queued) ? frames : queued;
        dropped += frames;
        queue.pop_front();
    }
    return dropped;
}

} // namespace replay
