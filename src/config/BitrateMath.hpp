#pragma once

#include "CodecUtils.hpp"

#include <algorithm>
#include <array>
#include <cstddef>

namespace replay {

// Pure bitrate math shared by ReplayConfig, the platform encoders and the host tests.
// The user-facing bitrate is a base value for 1080p60; VBR/CQ scale it by output size, FPS and codec.

// Rate control ordinals match ReplayConfig::RateControlMode (0 = Adaptive VBR, 1 = CBR, 2 = Constant Quality).
inline constexpr int kRateControlAdaptiveVbr = 0;
inline constexpr int kRateControlCbr = 1;
inline constexpr int kRateControlQuality = 2;

inline constexpr int kMinMeanBitrateMbps = 5;
inline constexpr int kMaxMeanBitrateMbps = 250;
inline constexpr int kMinMaxBitrateMbps = 10;
inline constexpr int kMaxMaxBitrateMbps = 300;

// Relative efficiency of each codec family against the 1080p60 base (H.264 adds packet overhead).
inline constexpr double codecBitrateMultiplier(VideoCodecFamily family) noexcept {
    switch (family) {
        case VideoCodecFamily::AV1:  return 0.70; // ~30% more efficient than H.264
        case VideoCodecFamily::HEVC: return 0.85; // ~15% more efficient than H.264
        case VideoCodecFamily::VP9:  return 0.85; // roughly HEVC-class efficiency
        case VideoCodecFamily::H264:
        default:                     return 1.10;
    }
}

inline constexpr double fpsBitrateMultiplier(int fps) noexcept {
    if (fps <= 60) return 1.00;
    if (fps == 120) return 1.40;
    if (fps >= 240) return 1.95;
    return 1.00 + (fps - 60) * 0.0053;
}

struct EffectiveBitrates {
    int meanMbps = 0;
    int maxMbps = 0;
};

inline constexpr std::size_t kCodecFamilyCount = 4;

// Effective bitrates for every codec family of one session, so a fallback encoder
// (e.g. AV1 -> HEVC) gets exactly what an explicit selection of that codec would get (W1).
struct CodecBitrateTable {
    std::array<EffectiveBitrates, kCodecFamilyCount> byFamily{};
    bool valid = false;

    EffectiveBitrates forFamily(VideoCodecFamily family) const noexcept {
        const auto idx = static_cast<std::size_t>(family);
        return (idx < byFamily.size()) ? byFamily[idx] : EffectiveBitrates{};
    }
};

// CBR is an explicit target and is never scaled; VBR and CQ scale the base by resolution, FPS and codec.
inline EffectiveBitrates computeEffectiveBitrates(
    int baseMbps,
    int baseMaxMbps,
    int qualityLevel,
    int rateControl,
    double resolutionMultiplier,
    double fpsMultiplier,
    VideoCodecFamily family
) noexcept {
    EffectiveBitrates out;
    if (rateControl == kRateControlCbr) {
        out.meanMbps = std::clamp(baseMbps, kMinMeanBitrateMbps, kMaxMeanBitrateMbps);
        out.maxMbps = out.meanMbps;
        return out;
    }

    const double mult = resolutionMultiplier * fpsMultiplier * codecBitrateMultiplier(family);
    double mean = static_cast<double>(baseMbps) * mult;
    if (rateControl == kRateControlQuality) {
        mean *= static_cast<double>(std::clamp(qualityLevel, 20, 100)) / 80.0;
    }
    out.meanMbps = std::clamp(static_cast<int>(mean + 0.5), kMinMeanBitrateMbps, kMaxMeanBitrateMbps);
    const int maxVal = static_cast<int>(static_cast<double>(baseMaxMbps) * mult + 0.5);
    out.maxMbps = std::max(std::clamp(maxVal, kMinMaxBitrateMbps, kMaxMaxBitrateMbps), out.meanMbps);
    return out;
}

inline CodecBitrateTable computeCodecBitrateTable(
    int baseMbps,
    int baseMaxMbps,
    int qualityLevel,
    int rateControl,
    double resolutionMultiplier,
    double fpsMultiplier
) noexcept {
    CodecBitrateTable table;
    for (std::size_t i = 0; i < table.byFamily.size(); ++i) {
        table.byFamily[i] = computeEffectiveBitrates(
            baseMbps, baseMaxMbps, qualityLevel, rateControl,
            resolutionMultiplier, fpsMultiplier, static_cast<VideoCodecFamily>(i));
    }
    table.valid = true;
    return table;
}

} // namespace replay
