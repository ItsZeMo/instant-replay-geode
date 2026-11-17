#pragma once

#include <algorithm>
#include <cctype>
#include <cmath>
#include <string>
#include <string_view>

namespace replay {

enum class VideoCodecFamily : int {
    H264 = 0,
    HEVC = 1,
    AV1 = 2,
    VP9 = 3
};

struct CodecTargetSpec {
    VideoCodecFamily family = VideoCodecFamily::H264;
    bool hardware = true;
};

struct VideoDimensions {
    int width = 0;
    int height = 0;
    bool clamped = false;
};

// Broad H.264 baseline MFT / hardware limit across NVIDIA, AMD, Intel, and Windows SW H.264 MFT (<= 4096x2304).
inline constexpr int kH264MaxWidth = 4096;
inline constexpr int kH264MaxHeight = 2304;

inline constexpr const char* codecFamilyDisplayName(VideoCodecFamily family) noexcept {
    switch (family) {
        case VideoCodecFamily::AV1:  return "AV1";
        case VideoCodecFamily::HEVC: return "HEVC";
        case VideoCodecFamily::VP9:  return "VP9";
        case VideoCodecFamily::H264:
        default:                     return "H.264";
    }
}

// Parses free-form custom codec strings (e.g. "av1_nvenc", "software_h264", "hevc_amf").
inline CodecTargetSpec parseCustomCodecSpec(bool requestedHardware, std::string_view customCodec) {
    CodecTargetSpec spec{VideoCodecFamily::H264, requestedHardware};
    if (customCodec.empty()) return spec;

    std::string lower;
    lower.reserve(customCodec.size());
    for (unsigned char ch : customCodec) {
        lower.push_back(static_cast<char>(std::tolower(ch)));
    }

    if (lower.find("cpu") != std::string::npos ||
        lower.find("software") != std::string::npos ||
        lower.find("sw") != std::string::npos) {
        spec.hardware = false;
    }

    if (lower.find("av1") != std::string::npos) {
        spec.family = VideoCodecFamily::AV1;
    } else if (lower.find("hevc") != std::string::npos ||
               lower.find("h265") != std::string::npos ||
               lower.find("265") != std::string::npos) {
        spec.family = VideoCodecFamily::HEVC;
    } else if (lower.find("vp9") != std::string::npos) {
        spec.family = VideoCodecFamily::VP9;
    } else {
        spec.family = VideoCodecFamily::H264;
    }
    return spec;
}

// CodecType integer values match ReplayConfig::CodecType (0=AV1, 1=HEVC, 2=H264, 3=CUSTOM).
inline CodecTargetSpec resolveRequestedCodecSpec(
    int codecEnumValue,
    bool requestedHardware,
    std::string_view customCodec
) {
    switch (codecEnumValue) {
        case 0: return {VideoCodecFamily::AV1, requestedHardware};
        case 1: return {VideoCodecFamily::HEVC, requestedHardware};
        case 3: return parseCustomCodecSpec(requestedHardware, customCodec);
        case 2:
        default: return {VideoCodecFamily::H264, requestedHardware};
    }
}

// Scales width/height uniformly to fit within [maxWidth x maxHeight] while preserving aspect ratio
// and guaranteeing even dimensions (NV12 / Media Foundation compatibility).
inline VideoDimensions clampDimensionsToBounds(
    int width,
    int height,
    int maxWidth = kH264MaxWidth,
    int maxHeight = kH264MaxHeight
) noexcept {
    int safeW = std::max(2, width) & ~1;
    int safeH = std::max(2, height) & ~1;
    const int evenMaxW = std::max(2, maxWidth & ~1);
    const int evenMaxH = std::max(2, maxHeight & ~1);

    if (safeW <= evenMaxW && safeH <= evenMaxH) {
        return {safeW, safeH, false};
    }

    const double scaleW = static_cast<double>(evenMaxW) / static_cast<double>(safeW);
    const double scaleH = static_cast<double>(evenMaxH) / static_cast<double>(safeH);
    const double scale = std::min(scaleW, scaleH);

    auto roundEven = [](double v, int limit) noexcept -> int {
        int rounded = static_cast<int>(std::llround(v * 0.5) * 2);
        return std::clamp(rounded, 2, limit);
    };

    const int outW = roundEven(static_cast<double>(safeW) * scale, evenMaxW);
    const int outH = roundEven(static_cast<double>(safeH) * scale, evenMaxH);
    return {outW, outH, true};
}

} // namespace replay
