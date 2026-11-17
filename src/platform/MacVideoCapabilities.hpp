#pragma once

#include <TargetConditionals.h>

#if defined(__APPLE__) && !TARGET_OS_IPHONE

#include "../config/CodecUtils.hpp"

namespace replay {

struct MacVideoEncoderCaps {
    bool enumerated = false;
    bool av1Hardware = false;
    bool av1Software = false;
    bool av1SoftwareWritable = false;
    bool hevcHardware = false;
    bool hevcSoftware = false;
    bool hevcSoftwareWritable = false;
    bool h264Hardware = false;
    bool h264Software = false;
    bool h264SoftwareWritable = false;

    bool hasHardware(VideoCodecFamily family) const noexcept {
        switch (family) {
            case VideoCodecFamily::AV1:  return av1Hardware;
            case VideoCodecFamily::HEVC: return hevcHardware;
            case VideoCodecFamily::H264: return h264Hardware;
            case VideoCodecFamily::VP9:
            default:                     return false;
        }
    }

    bool hasSoftware(VideoCodecFamily family) const noexcept {
        switch (family) {
            case VideoCodecFamily::AV1:  return av1Software && av1SoftwareWritable;
            case VideoCodecFamily::HEVC: return hevcSoftware && hevcSoftwareWritable;
            case VideoCodecFamily::H264: return h264Software && h264SoftwareWritable;
            case VideoCodecFamily::VP9:
            default:                     return false;
        }
    }

    bool supports(VideoCodecFamily family, bool hardware) const noexcept {
        return hardware ? hasHardware(family) : hasSoftware(family);
    }

    bool hasAny(VideoCodecFamily family) const noexcept {
        return hasHardware(family) || hasSoftware(family);
    }
};

// Queries macOS VideoToolbox once (thread-safe) via VTCopyVideoEncoderList to detect
// real hardware and software encoder availability on the host Mac (e.g. M1 lacks AV1 HW).
const MacVideoEncoderCaps& getMacVideoEncoderCaps();

// Cached VideoToolbox session capability check for oversize Retina resolutions (>4096x2304, M4).
bool isMacVideoEncodeSizeSupported(VideoCodecFamily family, bool hardware, int width, int height);

} // namespace replay

#endif // __APPLE__ && !TARGET_OS_IPHONE
