#pragma once

#include <cstddef>

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

// Idle pool limit: strictly bounded by maxCount and maxBytes across all retained buffers.
// A single frame larger than maxBytes is not cached in the idle pool.
inline bool canRetainPooledFrameBytes(
    size_t currentCount,
    size_t currentTotalBytes,
    size_t incomingBytes,
    size_t maxCount,
    size_t maxBytes
) noexcept {
    if (incomingBytes == 0 || currentCount >= maxCount) return false;
    if (incomingBytes > maxBytes) return false;
    return currentTotalBytes <= (maxBytes - incomingBytes);
}

// Worker queue limit: strictly bounded by maxCount and maxBytes across all queued buffers,
// except an empty queue accepts at most 1 frame even if byteSize > maxBytes so capture never hangs.
inline bool canEnqueueVideoFrameBytes(
    size_t currentCount,
    size_t currentTotalBytes,
    size_t incomingBytes,
    size_t maxCount,
    size_t maxBytes
) noexcept {
    if (currentCount == 0) return true;
    if (incomingBytes == 0 || currentCount >= maxCount) return false;
    if (incomingBytes > maxBytes) return false;
    return currentTotalBytes <= (maxBytes - incomingBytes);
}

} // namespace replay
