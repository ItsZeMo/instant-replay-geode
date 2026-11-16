#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace replay {

template <typename ContainerT, typename ByteSizeFn>
inline size_t sumFrameBufferBytes(const ContainerT& frames, ByteSizeFn&& getBytes) noexcept {
    size_t total = 0;
    for (const auto& item : frames) {
        if (item) {
            total += getBytes(*item);
        }
    }
    return total;
}

// Scales a base byte budget up so at least `minFrames` of `frameBytes` fit (capped by `ceilingBytes`),
// keeping low-res budgets tight while preventing 4K+ frame pools/queues from shrinking to 1-2 buffers.
inline size_t effectiveFrameByteBudget(
    size_t baseMaxBytes,
    size_t frameBytes,
    size_t minFrames,
    size_t ceilingBytes
) noexcept {
    const size_t safeBase = std::min(baseMaxBytes, ceilingBytes);
    if (frameBytes == 0 || minFrames == 0) return safeBase;
    if (frameBytes > (ceilingBytes / minFrames)) {
        return ceilingBytes;
    }
    return std::max(safeBase, frameBytes * minFrames);
}

// Idle pool limit: bounded by maxCount and dynamic effectiveByteBudget across all retained buffers.
// A single frame larger than ceilingBytes is not cached in the idle pool.
inline bool canRetainPooledFrameBytes(
    size_t currentCount,
    size_t currentTotalBytes,
    size_t incomingBytes,
    size_t maxCount,
    size_t effectiveMaxBytes
) noexcept {
    if (incomingBytes == 0 || currentCount >= maxCount) return false;
    if (incomingBytes > effectiveMaxBytes) return false;
    return currentTotalBytes <= (effectiveMaxBytes - incomingBytes);
}

// Worker queue limit: bounded by maxCount and effectiveMaxBytes across all queued buffers,
// except an empty queue accepts at most 1 frame even if byteSize > effectiveMaxBytes so capture never hangs.
inline bool canEnqueueVideoFrameBytes(
    size_t currentCount,
    size_t currentTotalBytes,
    size_t incomingBytes,
    size_t maxCount,
    size_t effectiveMaxBytes
) noexcept {
    if (currentCount == 0) return true;
    if (incomingBytes == 0 || currentCount >= maxCount) return false;
    if (incomingBytes > effectiveMaxBytes) return false;
    return currentTotalBytes <= (effectiveMaxBytes - incomingBytes);
}

// Encoder in-flight limit: bounded by both maxCount and byte budget while guaranteeing at least minFrames.
inline bool canAcceptInFlightFrameBytes(
    size_t currentCount,
    size_t currentTotalBytes,
    size_t incomingBytes,
    size_t maxCount,
    size_t maxBytes,
    size_t minFrames
) noexcept {
    if (currentCount >= maxCount) return false;
    if (currentCount < minFrames) return true;
    if (incomingBytes > maxBytes) return false;
    return currentTotalBytes <= (maxBytes - incomingBytes);
}

} // namespace replay
