#pragma once

#ifdef _WIN32

#include "../config/CodecUtils.hpp"

namespace replay {

// Media Foundation video encoder availability on this PC (W4). Probed once in the background with
// MFTEnumEx; until the probe finishes `probed` is false and callers must assume support (the encoder
// fallback cascade still guards the real start).
struct WinVideoEncoderCaps {
    bool probed = false;
    bool av1Hardware = false;
    bool av1Software = false;
    bool hevcHardware = false;
    bool hevcSoftware = false;
    bool h264Hardware = false;
    bool h264Software = false;

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
            case VideoCodecFamily::AV1:  return av1Software;
            case VideoCodecFamily::HEVC: return hevcSoftware;
            case VideoCodecFamily::H264: return h264Software;
            case VideoCodecFamily::VP9:
            default:                     return false;
        }
    }

    bool supports(VideoCodecFamily family, bool hardware) const noexcept {
        return hardware ? hasHardware(family) : hasSoftware(family);
    }
};

// Starts the one-time background probe (idempotent, safe from any thread).
void startWinVideoEncoderProbe();

// Thread-safe snapshot of the probe result.
WinVideoEncoderCaps getWinVideoEncoderCaps() noexcept;

} // namespace replay

#endif // _WIN32
