#pragma once

#include <cstdint>

namespace replay {

enum class MicCaptureStatus : uint8_t {
    Idle = 0,
    Active,
    PermissionDenied,
    BlockedNoUsageDescription,
    UsingFallbackDevice,
    DigitalSilenceDetected,
    NoInputDevice
};

// macOS microphone privacy (TCC) grant as reported by +[AVCaptureDevice authorizationStatusForMediaType:].
// Kept platform-neutral so the policy below is host-testable (K1).
enum class MicPrivacyAuthorization : uint8_t {
    NotDetermined = 0,
    Restricted,
    Denied,
    Authorized
};

// Only an explicit privacy denial ends microphone capture for the launch. Every other status, including
// the silence statuses below, keeps the capture thread and its recovery logic running (K1).
constexpr bool micStatusStopsCapture(MicCaptureStatus status) noexcept {
    return status == MicCaptureStatus::PermissionDenied;
}

// A denied or restricted grant is final for this launch: capture does not start at all.
constexpr bool micAuthorizationBlocksCapture(MicPrivacyAuthorization auth) noexcept {
    return auth == MicPrivacyAuthorization::Denied || auth == MicPrivacyAuthorization::Restricted;
}

// Status to report after >= 3 s of pure digital silence since the capture queue started (K1).
// Geometry Dash's bundle has no NSMicrophoneUsageDescription, so macOS never prompts for a launch that is
// not attributed to Steam: with the grant still undetermined the silence is most likely that TCC block.
// With a granted permission (e.g. Steam asked for it) the input is muted or silent instead. Both statuses
// are soft: capture keeps running and returns to Active as soon as real audio arrives.
constexpr MicCaptureStatus micStatusForDigitalSilence(
    MicPrivacyAuthorization auth, bool bundleHasUsageDescription
) noexcept {
    if (auth == MicPrivacyAuthorization::NotDetermined && !bundleHasUsageDescription) {
        return MicCaptureStatus::BlockedNoUsageDescription;
    }
    return MicCaptureStatus::DigitalSilenceDetected;
}

// Soft silence statuses that real audio clears.
constexpr bool isMicSilenceStatus(MicCaptureStatus status) noexcept {
    return status == MicCaptureStatus::BlockedNoUsageDescription ||
           status == MicCaptureStatus::DigitalSilenceDetected;
}

} // namespace replay
