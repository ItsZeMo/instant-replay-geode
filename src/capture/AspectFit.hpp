#pragma once

#include <algorithm>
#include <cstdint>

namespace replay {

// Destination rectangle of an aspect-preserving fit (letterbox / pillarbox bars when hasBars).
struct DestRect {
    int x0 = 0;
    int y0 = 0;
    int x1 = 0;
    int y1 = 0;
    bool hasBars = false;
};

// Fits a srcW x srcH frame into dstW x dstH, centred. Aspect differences within one pixel are treated
// as an exact fit so rounding never produces 1 px bars. Pure and Geode-free (shared by all capture
// backends and the host tests).
inline DestRect computeAspectFitRect(int srcW, int srcH, int dstW, int dstH) noexcept {
    if (srcW <= 0 || srcH <= 0 || dstW <= 0 || dstH <= 0) {
        return {0, 0, dstW, dstH, false};
    }
    int64_t lhs = static_cast<int64_t>(srcW) * dstH;
    int64_t rhs = static_cast<int64_t>(dstW) * srcH;
    int64_t diff = (lhs >= rhs) ? (lhs - rhs) : (rhs - lhs);
    if (diff <= std::max<int64_t>(srcH, dstH)) {
        return {0, 0, dstW, dstH, false};
    }

    int fitW = dstW;
    int fitH = dstH;
    if (lhs > rhs) {
        fitH = std::max(1, static_cast<int>((static_cast<int64_t>(dstW) * srcH + srcW / 2) / srcW));
    } else {
        fitW = std::max(1, static_cast<int>((static_cast<int64_t>(dstH) * srcW + srcH / 2) / srcH));
    }
    fitW = std::min(fitW, dstW);
    fitH = std::min(fitH, dstH);
    int x0 = (dstW - fitW) / 2;
    int y0 = (dstH - fitH) / 2;
    bool hasBars = (x0 > 0 || y0 > 0 || fitW < dstW || fitH < dstH);
    return {x0, y0, x0 + fitW, y0 + fitH, hasBars};
}

} // namespace replay
