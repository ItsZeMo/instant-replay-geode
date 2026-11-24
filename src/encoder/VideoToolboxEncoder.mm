#include "VideoToolboxEncoder.hpp"

#if defined(GEODE_IS_MACOS) || defined(GEODE_IS_IOS)

#include "ConcatTimestampMath.hpp"
#include "../config/PathUtils.hpp"
#include "../platform/AppleAutoreleasePool.hpp"
#include "../platform/MacVideoCapabilities.hpp"

#pragma push_macro("CommentType")
#undef CommentType
#define CommentType AppleScriptCommentTypeDummy
#import <AVFoundation/AVFoundation.h>
#import <AudioToolbox/AudioToolbox.h>
#if TARGET_OS_IPHONE
#import <CoreAudioTypes/CoreAudioTypes.h>
#else
#import <CoreAudio/CoreAudio.h>
#endif
#import <CoreFoundation/CoreFoundation.h>
#import <CoreMedia/CoreMedia.h>
#import <VideoToolbox/VideoToolbox.h>
#undef CommentType
#pragma pop_macro("CommentType")

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <deque>
#include <limits>
#include <map>
#include <mutex>
#include <set>
#include <thread>
#include <unistd.h>
#include <vector>

namespace replay {

namespace {

constexpr int64_t kHnsPerSecond = 10000000LL;
constexpr int32_t kVideoMediaTimeScale = 1000000; // 1 us ticks (safe for >20h, exact divisor of HNS/10)
constexpr CMVideoCodecType kCodecTypeAV1 = 'av01';
constexpr double kMaxPendingAudioSeconds = 10.0; // cap deferred AAC buffers during transient writer backpressure (M2)

// VideoToolbox encoder specification. macOS can require or refuse the hardware encoder; iOS only gained
// these keys in 17.4 and only ships hardware encoders, so earlier iOS versions use the default spec.
NSDictionary* encoderSpecificationFor(bool hardware) {
#if TARGET_OS_IPHONE
    if (@available(iOS 17.4, *)) {
        return hardware
            ? @{ (__bridge NSString*)kVTVideoEncoderSpecification_RequireHardwareAcceleratedVideoEncoder: @YES }
            : @{ (__bridge NSString*)kVTVideoEncoderSpecification_EnableHardwareAcceleratedVideoEncoder: @NO };
    }
    return @{};
#else
    return hardware
        ? @{ (__bridge NSString*)kVTVideoEncoderSpecification_RequireHardwareAcceleratedVideoEncoder: @YES }
        : @{ (__bridge NSString*)kVTVideoEncoderSpecification_EnableHardwareAcceleratedVideoEncoder: @NO };
#endif
}

std::string formatNsException(NSException* ex) {
    if (!ex) return "Objective-C exception";
    const char* name = ex.name ? ex.name.UTF8String : "NSException";
    const char* reason = ex.reason ? ex.reason.UTF8String : "unknown reason";
    return std::string(name ? name : "NSException") + ": " + (reason ? reason : "unknown reason");
}

const char* familyName(VideoCodecFamily family) noexcept {
    switch (family) {
        case VideoCodecFamily::AV1:  return "AV1";
        case VideoCodecFamily::HEVC: return "HEVC";
        case VideoCodecFamily::VP9:  return "VP9";
        case VideoCodecFamily::H264:
        default:                     return "H.264";
    }
}

CMVideoCodecType cmCodecTypeForFamily(VideoCodecFamily family) noexcept {
    switch (family) {
        case VideoCodecFamily::AV1:  return kCodecTypeAV1;
        case VideoCodecFamily::HEVC: return kCMVideoCodecType_HEVC;
        case VideoCodecFamily::VP9:  return 'vp09';
        case VideoCodecFamily::H264:
        default:                     return kCMVideoCodecType_H264;
    }
}

NSString* avVideoCodecKeyForFamily(VideoCodecFamily family) {
    switch (family) {
        case VideoCodecFamily::AV1:  return @"av01";
        case VideoCodecFamily::HEVC: return AVVideoCodecTypeHEVC;
        case VideoCodecFamily::H264:
        default:                     return AVVideoCodecTypeH264;
    }
}

const char* requestedRateControlName(RateControlMode mode) noexcept {
    switch (mode) {
        case RateControlMode::ADAPTIVE_VBR: return "AdaptiveVBR";
        case RateControlMode::CONSTANT_CBR: return "CBR";
        case RateControlMode::CONSTANT_QUALITY: return "Quality";
        default: return "Unknown";
    }
}

std::string vtStatusString(OSStatus status) {
    switch (status) {
        case kVTCouldNotFindVideoEncoderErr: return "video encoder not found";
        case kVTPropertyNotSupportedErr:     return "encoder property not supported";
        case kVTParameterErr:                return "invalid encoder parameters";
        case kVTVideoEncoderNotAvailableNowErr: return "encoder temporarily unavailable";
        case kVTVideoEncoderMalfunctionErr:  return "encoder malfunction";
        case kVTAllocationFailedErr:         return "encoder allocation failed";
        default: break;
    }
    char buf[48];
    snprintf(buf, sizeof(buf), "OSStatus %d", static_cast<int>(status));
    return buf;
}

bool isPermanentVTError(OSStatus status) noexcept {
    return status == kVTCouldNotFindVideoEncoderErr ||
           status == kVTFormatDescriptionChangeNotSupportedErr ||
           status == kVTParameterErr;
}

void classifyNSError(
    NSError* err,
    int64_t& outCode,
    bool& outFatalStorage,
    bool& outPermUnsupported,
    std::string& outMsg
) {
    outCode = err ? static_cast<int64_t>(err.code) : -1;
    outFatalStorage = false;
    outPermUnsupported = false;
    outMsg.clear();

    NSError* cur = err;
    for (int depth = 0; cur && depth < 6; ++depth) {
        NSString* domain = cur.domain;
        NSInteger code = cur.code;

        if ([domain isEqualToString:NSCocoaErrorDomain]) {
            if (code == NSFileWriteOutOfSpaceError) {
                outFatalStorage = true;
                outMsg = "disk is full";
                return;
            }
            if (code == NSFileWriteNoPermissionError) {
                outFatalStorage = true;
                outMsg = "access denied";
                return;
            }
            if (code == NSFileWriteVolumeReadOnlyError) {
                outFatalStorage = true;
                outMsg = "drive is write-protected";
                return;
            }
        } else if ([domain isEqualToString:NSPOSIXErrorDomain]) {
            if (code == ENOSPC || code == EDQUOT) {
                outFatalStorage = true;
                outMsg = "disk is full";
                return;
            }
            if (code == EACCES || code == EPERM) {
                outFatalStorage = true;
                outMsg = "access denied";
                return;
            }
            if (code == EROFS) {
                outFatalStorage = true;
                outMsg = "drive is write-protected";
                return;
            }
        } else if ([domain isEqualToString:AVFoundationErrorDomain]) {
            if (code == AVErrorDiskFull) {
                outFatalStorage = true;
                outMsg = "disk is full";
                return;
            }
            if (code == AVErrorDecoderNotFound || code == AVErrorEncoderNotFound) {
                outPermUnsupported = true;
            }
        } else if ([domain isEqualToString:NSOSStatusErrorDomain]) {
            if (code == -34 /* dskFulErr */) {
                outFatalStorage = true;
                outMsg = "disk is full";
                return;
            }
            if (isPermanentVTError(static_cast<OSStatus>(code))) {
                outPermUnsupported = true;
            }
        }

        id next = cur.userInfo[NSUnderlyingErrorKey];
        cur = [next isKindOfClass:[NSError class]] ? static_cast<NSError*>(next) : nil;
    }

    if (err && err.localizedDescription.length > 0) {
        outMsg = err.localizedDescription.UTF8String;
    } else if (err) {
        char buf[80];
        snprintf(buf, sizeof(buf), "%s (%ld)", err.domain.UTF8String ? err.domain.UTF8String : "NSError",
                 static_cast<long>(err.code));
        outMsg = buf;
    } else {
        outMsg = "unknown AVFoundation error";
    }
}

bool probeVideoToolboxCodec(
    VideoCodecFamily family,
    bool hardware,
    int width,
    int height,
    int fps,
    int bitrateMbps,
    RateControlMode rateControl,
    bool& outSupportsCbr,
    int64_t& outCode,
    bool& outPermUnsupported,
    std::string& outError
) {
    outSupportsCbr = false;
    outCode = 0;
    outPermUnsupported = false;
    outError.clear();

    if (family == VideoCodecFamily::VP9) {
        outCode = kVTCouldNotFindVideoEncoderErr;
        outPermUnsupported = true;
        outError = "VP9 MP4 encoding is unsupported in VideoToolbox";
        return false;
    }
    if (family == VideoCodecFamily::H264 && (width > kH264MaxWidth || height > kH264MaxHeight)) {
        outCode = kVTParameterErr;
        outPermUnsupported = true;
        outError = "resolution exceeds H.264 4096x2304 limit";
        return false;
    }

    NSDictionary* encoderSpec = encoderSpecificationFor(hardware);

    NSDictionary* srcAttrs = @{
        (__bridge NSString*)kCVPixelBufferPixelFormatTypeKey: @(kCVPixelFormatType_32BGRA),
        (__bridge NSString*)kCVPixelBufferWidthKey: @(width),
        (__bridge NSString*)kCVPixelBufferHeightKey: @(height),
    };

    VTCompressionSessionRef session = nullptr;
    OSStatus status = VTCompressionSessionCreate(
        kCFAllocatorDefault,
        width,
        height,
        cmCodecTypeForFamily(family),
        (__bridge CFDictionaryRef)encoderSpec,
        (__bridge CFDictionaryRef)srcAttrs,
        kCFAllocatorDefault,
        nullptr,
        nullptr,
        &session
    );
    if (status != noErr || !session) {
        outCode = status;
        outPermUnsupported = isPermanentVTError(status);
        outError = "VTCompressionSessionCreate: " + vtStatusString(status);
        return false;
    }

    VTSessionSetProperty(session, kVTCompressionPropertyKey_RealTime, kCFBooleanTrue);
    VTSessionSetProperty(session, kVTCompressionPropertyKey_ExpectedFrameRate, (__bridge CFTypeRef)@(fps));

    if (rateControl == RateControlMode::CONSTANT_CBR) {
        if (@available(macOS 13.0, iOS 16.0, *)) {
            int64_t bitrateBps = static_cast<int64_t>(std::max(1, bitrateMbps)) * 1000000LL;
            OSStatus cbrStatus = VTSessionSetProperty(
                session, kVTCompressionPropertyKey_ConstantBitRate, (__bridge CFTypeRef)@(bitrateBps));
            outSupportsCbr = (cbrStatus == noErr);
        }
    }

    status = VTCompressionSessionPrepareToEncodeFrames(session);
    VTCompressionSessionInvalidate(session);
    CFRelease(session);

    if (status != noErr) {
        outCode = status;
        outPermUnsupported = isPermanentVTError(status);
        outError = "VTCompressionSessionPrepare: " + vtStatusString(status);
        return false;
    }
    return true;
}

std::string buildAudioEncoderError(OSStatus status, const char* prefix) {
    char buf[96];
    snprintf(buf, sizeof(buf), "%s (OSStatus %d)", prefix, static_cast<int>(status));
    return buf;
}

// Converts 100ns HNS timestamps to CMTime matching a target timescale.
CMTime hnsToCMTime(int64_t hns, CMTimeScale targetScale) {
    CMTime hnsTime = CMTimeMake(hns, static_cast<CMTimeScale>(kHnsPerSecond));
    if (targetScale <= 0 || targetScale == static_cast<CMTimeScale>(kHnsPerSecond)) {
        return hnsTime;
    }
    return CMTimeConvertScale(hnsTime, targetScale, kCMTimeRoundingMethod_RoundHalfAwayFromZero);
}

int64_t cmTimeToHns(CMTime t) {
    if (!CMTIME_IS_VALID(t) || CMTIME_IS_INDEFINITE(t)) return 0;
    CMTime converted = CMTimeConvertScale(t, static_cast<CMTimeScale>(kHnsPerSecond), kCMTimeRoundingMethod_RoundHalfAwayFromZero);
    return converted.value;
}

// Session-wide codec cache: avoids re-probing missing hardware codecs every 30s,
// while never pinning transient failures to a fallback or SW encoder.
std::mutex s_codecCacheMutex;
std::string s_cachedRequestKey;
std::string s_confirmedPrimaryKey;
std::string s_lastLoggedCodecKey;
std::string s_lastLoggedRcKey;
VideoCodecFamily s_cachedFamily = VideoCodecFamily::H264;
bool s_cachedHardware = true;
bool s_hasCachedCodec = false;

} // namespace

struct VideoToolboxEncoder::Impl {
    struct PendingAudioBuffer {
        CMSampleBufferRef sample = nullptr;
        double durationSec = 0.0;
    };

    AVAssetWriter* writer = nil;
    AVAssetWriterInput* videoInput = nil;
    AVAssetWriterInputPixelBufferAdaptor* adaptor = nil;
    AVAssetWriterInput* gameAudioInput = nil;
    AVAssetWriterInput* micAudioInput = nil;

    CMFormatDescriptionRef gameAudioFmt = nullptr;
    CMFormatDescriptionRef micAudioFmt = nullptr;

    std::deque<PendingAudioBuffer> pendingGameAudio;
    double pendingGameDurationSec = 0.0;
    std::deque<PendingAudioBuffer> pendingMicAudio;
    double pendingMicDurationSec = 0.0;

    int64_t lastVideoTicks = -1;
    int64_t lastGameAudioTicks = -1;
    int64_t lastMicAudioTicks = -1;

    ~Impl() {
        clear();
    }

    static void clearAudioQueue(std::deque<PendingAudioBuffer>& queue, double& pendingSec) noexcept {
        for (auto& item : queue) {
            if (item.sample) {
                CFRelease(item.sample);
                item.sample = nullptr;
            }
        }
        queue.clear();
        pendingSec = 0.0;
    }

    static bool drainQueue(
        AVAssetWriter* writer,
        AVAssetWriterInput* input,
        std::deque<PendingAudioBuffer>& queue,
        double& totalDurationSec,
        EncoderOpResult& outErr
    ) {
        outErr = {true, 0, {}, false, false};
        if (!writer || !input) return true;

        while (!queue.empty()) {
            if (writer.status == AVAssetWriterStatusFailed) {
                int64_t code = -1;
                bool fatal = false;
                bool perm = false;
                std::string msg;
                classifyNSError(writer.error, code, fatal, perm, msg);
                outErr = {false, code, msg, fatal, false};
                return false;
            }
            if (writer.status != AVAssetWriterStatusWriting || !input.readyForMoreMediaData) {
                return true;
            }
            auto item = queue.front();
            BOOL appended = [input appendSampleBuffer:item.sample];
            CFRelease(item.sample);
            queue.pop_front();
            totalDurationSec = std::max(0.0, totalDurationSec - item.durationSec);
            if (!appended) {
                if (writer.status == AVAssetWriterStatusFailed) {
                    int64_t code = -1;
                    bool fatal = false;
                    bool perm = false;
                    std::string msg;
                    classifyNSError(writer.error, code, fatal, perm, msg);
                    outErr = {false, code, msg, fatal, false};
                    return false;
                }
                outErr = {false, -1, "audio appendSampleBuffer returned NO", false, false};
                return false;
            }
        }
        return true;
    }

    bool drainAllPendingAudio(EncoderOpResult& outErr) {
        outErr = {true, 0, {}, false, false};
        if (!writer) return true;
        if (!drainQueue(writer, gameAudioInput, pendingGameAudio, pendingGameDurationSec, outErr)) {
            return false;
        }
        if (micAudioInput) {
            if (!drainQueue(writer, micAudioInput, pendingMicAudio, pendingMicDurationSec, outErr)) {
                return false;
            }
        }
        return true;
    }

    void clear() {
        @autoreleasepool {
            clearAudioQueue(pendingGameAudio, pendingGameDurationSec);
            clearAudioQueue(pendingMicAudio, pendingMicDurationSec);
            if (gameAudioFmt) {
                CFRelease(gameAudioFmt);
                gameAudioFmt = nullptr;
            }
            if (micAudioFmt) {
                CFRelease(micAudioFmt);
                micAudioFmt = nullptr;
            }
            micAudioInput = nil;
            gameAudioInput = nil;
            adaptor = nil;
            videoInput = nil;
            writer = nil;
            lastVideoTicks = -1;
            lastGameAudioTicks = -1;
            lastMicAudioTicks = -1;
        }
    }
};

void VideoToolboxEncoder::globalInitialize() {
    // VideoToolbox / AVFoundation require no explicit global init on macOS.
}

void VideoToolboxEncoder::resetCodecCache() {
    std::lock_guard<std::mutex> cacheLock(s_codecCacheMutex);
    s_cachedRequestKey.clear();
    s_confirmedPrimaryKey.clear();
    s_lastLoggedCodecKey.clear();
    s_lastLoggedRcKey.clear();
    s_hasCachedCodec = false;
}

VideoToolboxEncoder::VideoToolboxEncoder() {
    globalInitialize();
}

VideoToolboxEncoder::~VideoToolboxEncoder() {
    finish();
}

std::string VideoToolboxEncoder::activeCodecName() const {
    if (!m_hasActiveCodec) return "encoder";
    return std::string(familyName(m_activeCodecFamily)) + (m_activeHardware ? " (HW)" : " (SW)");
}

EncoderOpResult VideoToolboxEncoder::start(
    const std::filesystem::path& outputPath,
    int width,
    int height,
    int fps,
    int bitrateMbps,
    int maxBitrateMbps,
    int qualityLevel,
    RateControlMode rateControl,
    bool splitAudio,
    CodecType codec,
    bool hardwareAcceleration,
    const std::string& customCodec,
    int gameSampleRate,
    const std::function<void()>& onBeforeFallback,
    bool allowFallback,
    bool hasForcedCodec,
    VideoCodecFamily forcedFamily,
    bool forcedHardware
) {
    MacAutoreleasePoolScope pool;
    std::lock_guard<std::mutex> lock(m_mutex);
    finishLocked();

    fps = std::max(1, fps);
    int aacGameSampleRate = (gameSampleRate == 44100) ? 44100 : 48000;

    m_width = width;
    m_height = height;
    m_fps = fps;
    m_gameSampleRate = aacGameSampleRate;
    m_splitAudio = splitAudio;
    m_formatKey.clear();
    m_hasActiveCodec = false;
    m_activeCodecFamily = VideoCodecFamily::H264;
    m_activeHardware = hardwareAcceleration;
    m_activeBitrateMbps = 0;
    m_activeMaxBitrateMbps = 0;

    const CodecTargetSpec resolvedSpec = resolveRequestedCodecSpec(
        static_cast<int>(codec), hardwareAcceleration, customCodec);
    VideoCodecFamily targetFamily = resolvedSpec.family;
    bool enableHardware = resolvedSpec.hardware;

    std::string cname = customCodec;
    std::transform(cname.begin(), cname.end(), cname.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    if (hasForcedCodec) {
        targetFamily = forcedFamily;
        enableHardware = forcedHardware;
    }

    char reqKeyBuf[200];
    snprintf(reqKeyBuf, sizeof(reqKeyBuf), "%d|%d|%s|%dx%d@%d",
             static_cast<int>(codec), enableHardware ? 1 : 0, cname.c_str(), width, height, fps);
    std::string requestKey = reqKeyBuf;

    const auto& vtCaps = getMacVideoEncoderCaps();
    const bool targetSupportedByList = !vtCaps.enumerated || vtCaps.supports(targetFamily, enableHardware);

    struct Attempt {
        VideoCodecFamily family;
        bool hardware;
    };
    std::vector<Attempt> attempts;
    auto addAttempt = [&attempts, &vtCaps](VideoCodecFamily fam, bool hw) {
        if (vtCaps.enumerated && !vtCaps.supports(fam, hw)) {
            return;
        }
        for (const auto& a : attempts) {
            if (a.family == fam && a.hardware == hw) return;
        }
        attempts.push_back({fam, hw});
    };

    if (!hasForcedCodec) {
        std::lock_guard<std::mutex> cacheLock(s_codecCacheMutex);
        if (s_hasCachedCodec && s_cachedRequestKey == requestKey && s_cachedHardware == enableHardware) {
            addAttempt(s_cachedFamily, s_cachedHardware);
        }
    }

    addAttempt(targetFamily, enableHardware);
    if (allowFallback && !hasForcedCodec) {
        if (targetFamily == VideoCodecFamily::AV1) {
            if (enableHardware) {
                addAttempt(VideoCodecFamily::HEVC, true);
                addAttempt(VideoCodecFamily::H264, true);
            } else {
                addAttempt(VideoCodecFamily::H264, false);
                addAttempt(VideoCodecFamily::HEVC, false);
            }
        } else {
            addAttempt(VideoCodecFamily::H264, enableHardware);
        }
        if (enableHardware) {
            addAttempt(VideoCodecFamily::H264, false);
        }
    }

    StartParams params;
    params.width = width;
    params.height = height;
    params.fps = fps;
    params.bitrateMbps = bitrateMbps;
    params.maxBitrateMbps = std::max(maxBitrateMbps, bitrateMbps);
    params.qualityLevel = qualityLevel;
    params.rateControl = rateControl;
    params.splitAudio = splitAudio;
    params.gameSampleRate = aacGameSampleRate;

    auto cleanupFailedAttempt = [this, &outputPath]() {
        if (m_impl) {
            if (m_impl->writer) {
                @try {
                    [m_impl->writer cancelWriting];
                } @catch (NSException*) {}
            }
            m_impl.reset();
        }
        std::error_code ec;
        std::filesystem::remove(outputPath, ec);
    };

    bool primaryFailedPermanently = !targetSupportedByList;
    bool syncedBeforeFallback = false;
    int64_t lastCode = targetSupportedByList ? -1 : kVTCouldNotFindVideoEncoderErr;
    bool lastFatalStorage = false;
    std::string lastErrMsg;
    if (!targetSupportedByList) {
        lastErrMsg = std::string(familyName(targetFamily)) +
                     (enableHardware ? " (HW)" : " (SW)") +
                     ": encoder not available in VideoToolbox on this " +
#if TARGET_OS_IPHONE
                     "device";
#else
                     "Mac";
#endif
    }

    for (const auto& attempt : attempts) {
        bool isPrimary = (attempt.family == targetFamily && attempt.hardware == enableHardware);

        // Each attempt encodes at its own codec family's session bitrate when ReplayManager provided the
        // per-family table, like the Media Foundation cascade (W1 parity, K3).
        StartParams attemptParams = params;
        if (m_familyBitrates.valid) {
            const EffectiveBitrates familyRates = m_familyBitrates.forFamily(attempt.family);
            if (familyRates.meanMbps > 0) {
                attemptParams.bitrateMbps = familyRates.meanMbps;
                attemptParams.maxBitrateMbps = std::max(familyRates.maxMbps, familyRates.meanMbps);
            }
        }

        int64_t failCode = 0;
        bool fatalStorage = false;
        bool permUnsupported = false;
        std::string error;
        bool started = tryStartLocked(
            outputPath, attempt.family, attempt.hardware, attemptParams,
            failCode, fatalStorage, permUnsupported, error);

        if (!started && isPrimary && !permUnsupported && !fatalStorage &&
            onBeforeFallback && !syncedBeforeFallback) {
            syncedBeforeFallback = true;
            cleanupFailedAttempt();
            onBeforeFallback();
            failCode = 0;
            fatalStorage = false;
            permUnsupported = false;
            error.clear();
            started = tryStartLocked(
                outputPath, attempt.family, attempt.hardware, attemptParams,
                failCode, fatalStorage, permUnsupported, error);
        }

        if (started) {
            std::string codecLabel = std::string(familyName(attempt.family)) + (attempt.hardware ? " (HW)" : " (SW)");
            char key[180];
            snprintf(key, sizeof(key), "%s|%s|%dx%d|%d|a%d|s%d",
                     familyName(attempt.family), attempt.hardware ? "hw" : "sw",
                     width, height, fps, params.gameSampleRate, splitAudio ? 1 : 0);
            m_formatKey = key;
            m_hasActiveCodec = true;
            m_activeCodecFamily = attempt.family;
            m_activeHardware = attempt.hardware;
            m_activeBitrateMbps = attemptParams.bitrateMbps;
            m_activeMaxBitrateMbps = attemptParams.maxBitrateMbps;
            m_recording = true;

            {
                std::lock_guard<std::mutex> cacheLock(s_codecCacheMutex);
                if (!hasForcedCodec) {
                    if (isPrimary) {
                        s_cachedRequestKey = requestKey;
                        s_confirmedPrimaryKey = requestKey;
                        s_cachedFamily = attempt.family;
                        s_cachedHardware = attempt.hardware;
                        s_hasCachedCodec = true;
                    } else if (attempt.hardware == enableHardware &&
                               primaryFailedPermanently &&
                               s_confirmedPrimaryKey != requestKey) {
                        s_cachedRequestKey = requestKey;
                        s_cachedFamily = attempt.family;
                        s_cachedHardware = attempt.hardware;
                        s_hasCachedCodec = true;
                    }
                }

                std::string logKey = requestKey + "->" + codecLabel + "|" +
                                     std::to_string(attemptParams.bitrateMbps) + "|" +
                                     std::to_string(attemptParams.maxBitrateMbps);
                if (s_lastLoggedCodecKey != logKey) {
                    s_lastLoggedCodecKey = logKey;
                    if (!isPrimary && !hasForcedCodec) {
                        geode::log::warn(
                            "Instant Replay: requested codec unavailable, fell back to {} ({}x{} @ {} FPS, effBitrate={} Mbps, effMaxBitrate={} Mbps)",
                            codecLabel, width, height, fps, attemptParams.bitrateMbps, attemptParams.maxBitrateMbps);
                    } else {
                        geode::log::info(
                            "Instant Replay: encoder started with {} ({}x{} @ {} FPS, effBitrate={} Mbps, effMaxBitrate={} Mbps)",
                            codecLabel, width, height, fps, attemptParams.bitrateMbps, attemptParams.maxBitrateMbps);
                    }
                } else {
                    geode::log::debug(
                        "Instant Replay: encoder started with {} ({}x{} @ {} FPS, effBitrate={} Mbps, effMaxBitrate={} Mbps)",
                        codecLabel, width, height, fps, attemptParams.bitrateMbps, attemptParams.maxBitrateMbps);
                }
            }
            return {true, 0, codecLabel, false};
        }

        std::string attemptLabel = std::string(familyName(attempt.family)) + (attempt.hardware ? " (HW)" : " (SW)");
        lastCode = failCode;
        lastFatalStorage = fatalStorage;
        lastErrMsg = attemptLabel + ": " + error;

        if (isPrimary && permUnsupported) {
            primaryFailedPermanently = true;
        }
        geode::log::warn("Instant Replay: {} failed: {}", attemptLabel, error);
        cleanupFailedAttempt();

        if (fatalStorage) {
            break;
        }
    }

    bool fellBackToOversizedH264 = false;
    if (targetFamily != VideoCodecFamily::H264 && (width > kH264MaxWidth || height > kH264MaxHeight)) {
        for (const auto& attempt : attempts) {
            if (attempt.family == VideoCodecFamily::H264) {
                fellBackToOversizedH264 = true;
                break;
            }
        }
    }

    if (fellBackToOversizedH264) {
        lastErrMsg = "AV1/HEVC unavailable and " + std::to_string(width) + "x" + std::to_string(height) +
                     " exceeds H.264 " + std::to_string(kH264MaxWidth) + "x" + std::to_string(kH264MaxHeight) +
                     " limit (select HEVC/AV1 or a lower resolution preset)";
    } else if (lastErrMsg.empty()) {
        lastErrMsg = "no usable video encoder found";
    }
    if (allowFallback) {
        geode::log::error("Instant Replay: encoder start failed ({})", lastErrMsg);
    }
    return {false, lastCode, lastErrMsg, lastFatalStorage};
}

bool VideoToolboxEncoder::tryStartLocked(
    const std::filesystem::path& outputPath,
    VideoCodecFamily family,
    bool hardware,
    const StartParams& p,
    int64_t& outCode,
    bool& outFatalStorage,
    bool& outPermUnsupported,
    std::string& outError
) {
    @autoreleasepool {
        @try {
        outCode = 0;
        outFatalStorage = false;
        outPermUnsupported = false;
        outError.clear();

        if (p.width <= 0 || p.height <= 0 || (p.width & 1) != 0 || (p.height & 1) != 0) {
            outCode = kVTParameterErr;
            outPermUnsupported = true;
            outError = "video dimensions must be positive even integers";
            return false;
        }

        // 1. Probe VideoToolbox capability before creating AVAssetWriter (enforces HW/SW & CBR probe)
        bool supportsCbr = false;
        if (!probeVideoToolboxCodec(
                family, hardware, p.width, p.height, p.fps, p.bitrateMbps, p.rateControl,
                supportsCbr, outCode, outPermUnsupported, outError)) {
            return false;
        }

        std::error_code rmEc;
        std::filesystem::remove(outputPath, rmEc);

        std::string utf8Path = replay::pathToUtf8(outputPath);
        NSString* nsPath = [NSString stringWithUTF8String:utf8Path.c_str()];
        if (!nsPath) {
            outCode = EINVAL;
            outError = "invalid UTF-8 output path";
            return false;
        }
        NSURL* outUrl = [NSURL fileURLWithPath:nsPath];

        NSError* nsErr = nil;
        AVAssetWriter* writer = [AVAssetWriter assetWriterWithURL:outUrl fileType:AVFileTypeMPEG4 error:&nsErr];
        if (!writer || nsErr) {
            classifyNSError(nsErr, outCode, outFatalStorage, outPermUnsupported, outError);
            return false;
        }

        auto impl = std::make_unique<Impl>();
        impl->writer = writer;

        // 2. Build VideoToolbox compression settings
        NSMutableDictionary* compProps = [NSMutableDictionary dictionary];
        compProps[AVVideoExpectedSourceFrameRateKey] = @(p.fps);
        compProps[AVVideoMaxKeyFrameIntervalKey] = @(p.fps);
        compProps[AVVideoMaxKeyFrameIntervalDurationKey] = @(1.0);
        if (family == VideoCodecFamily::H264) {
            compProps[AVVideoProfileLevelKey] = AVVideoProfileLevelH264HighAutoLevel;
        }
#if TARGET_OS_IPHONE
        else if (family == VideoCodecFamily::HEVC) {
            // 8-bit Main: the encoder's default can be Main10, which many editors and players cannot open.
            compProps[AVVideoProfileLevelKey] = (__bridge NSString*)kVTProfileLevel_HEVC_Main_AutoLevel;
        }
#endif

        int64_t bitrateBps = static_cast<int64_t>(std::max(1, p.bitrateMbps)) * 1000000LL;
        int64_t maxBitrateBps = static_cast<int64_t>(std::max(p.bitrateMbps, p.maxBitrateMbps)) * 1000000LL;
        bool usingActualCbr = false;
        bool peakCapDropped = false;

        switch (p.rateControl) {
            case RateControlMode::CONSTANT_CBR: {
                if (@available(macOS 13.0, iOS 16.0, *)) {
                    if (supportsCbr) {
                        compProps[(__bridge NSString*)kVTCompressionPropertyKey_ConstantBitRate] = @(bitrateBps);
                        usingActualCbr = true;
                    }
                }
                if (!usingActualCbr) {
                    compProps[AVVideoAverageBitRateKey] = @(bitrateBps);
                    compProps[(__bridge NSString*)kVTCompressionPropertyKey_DataRateLimits] =
                        @[@(std::max<int64_t>(1, bitrateBps / 8)), @(1.0)];
                }
                break;
            }
            case RateControlMode::CONSTANT_QUALITY: {
                double qNorm = static_cast<double>(std::clamp(p.qualityLevel, 1, 100)) / 100.0;
                compProps[AVVideoQualityKey] = @(qNorm);
                break;
            }
            case RateControlMode::ADAPTIVE_VBR:
            default: {
                compProps[AVVideoAverageBitRateKey] = @(bitrateBps);
                compProps[(__bridge NSString*)kVTCompressionPropertyKey_DataRateLimits] =
                    @[@(std::max<int64_t>(1, maxBitrateBps / 8)), @(1.0)];
                break;
            }
        }

        NSMutableDictionary* videoSettings = [@{
            AVVideoCodecKey: avVideoCodecKeyForFamily(family),
            AVVideoWidthKey: @(p.width),
            AVVideoHeightKey: @(p.height),
            AVVideoCompressionPropertiesKey: compProps,
            AVVideoColorPropertiesKey: @{
                AVVideoColorPrimariesKey: AVVideoColorPrimaries_ITU_R_709_2,
                AVVideoTransferFunctionKey: AVVideoTransferFunction_ITU_R_709_2,
                AVVideoYCbCrMatrixKey: AVVideoYCbCrMatrix_ITU_R_709_2,
            },
        } mutableCopy];
#if !TARGET_OS_IPHONE
        // iOS AVAssetWriter has no encoder-specification key and always encodes in hardware.
        videoSettings[AVVideoEncoderSpecificationKey] = encoderSpecificationFor(hardware);
#endif

        // Settings an encoder may reject are relaxed in order before giving up on the codec (e.g. HEVC in the
        // iOS Simulator refuses the peak-rate cap): 1. the peak-rate cap, 2. CQ without a bitrate,
        // 3. the profile / level.
        auto canApply = [&]() -> bool {
            return [writer canApplyOutputSettings:videoSettings forMediaType:AVMediaTypeVideo] == YES;
        };
        NSString* const peakCapKey = (__bridge NSString*)kVTCompressionPropertyKey_DataRateLimits;
        if (!canApply() && compProps[peakCapKey] != nil) {
            [compProps removeObjectForKey:peakCapKey];
            peakCapDropped = true;
            geode::log::info("Instant Replay: {} encoder rejected the peak bitrate cap (DataRateLimits), retrying without it",
                             familyName(family));
        }
        if (!canApply() && p.rateControl == RateControlMode::CONSTANT_QUALITY && compProps[AVVideoQualityKey] != nil) {
            // Fallback for encoders rejecting AVVideoQualityKey without bitrate
            [compProps removeObjectForKey:AVVideoQualityKey];
            compProps[AVVideoAverageBitRateKey] = @(bitrateBps);
        }
        if (!canApply() && compProps[AVVideoProfileLevelKey] != nil) {
            [compProps removeObjectForKey:AVVideoProfileLevelKey];
            geode::log::info("Instant Replay: {} encoder rejected the profile / level, retrying without it", familyName(family));
        }
        if (!canApply()) {
            outCode = kVTParameterErr;
            outPermUnsupported = true;
            outError = "AVAssetWriter rejected video output settings";
            return false;
        }

        AVAssetWriterInput* videoInput = [AVAssetWriterInput assetWriterInputWithMediaType:AVMediaTypeVideo
                                                                           outputSettings:videoSettings];
        videoInput.expectsMediaDataInRealTime = YES;
        videoInput.mediaTimeScale = kVideoMediaTimeScale;
        if (![writer canAddInput:videoInput]) {
            outCode = -1;
            outError = "AVAssetWriter cannot add video input";
            return false;
        }
        [writer addInput:videoInput];
        impl->videoInput = videoInput;

        NSDictionary* adaptorAttrs = @{
            (__bridge NSString*)kCVPixelBufferPixelFormatTypeKey: @(kCVPixelFormatType_32BGRA),
            (__bridge NSString*)kCVPixelBufferWidthKey: @(p.width),
            (__bridge NSString*)kCVPixelBufferHeightKey: @(p.height),
        };
        impl->adaptor = [AVAssetWriterInputPixelBufferAdaptor
            assetWriterInputPixelBufferAdaptorWithAssetWriterInput:videoInput
                                       sourcePixelBufferAttributes:adaptorAttrs];

        // Log effective rate control once per codec/mode transition
        {
            const char* actualRc = (p.rateControl == RateControlMode::CONSTANT_CBR)
                ? (usingActualCbr ? "ConstantCBR" : (peakCapDropped ? "AverageBitrate" : "TightConstrainedVBR"))
                : (p.rateControl == RateControlMode::CONSTANT_QUALITY
                       ? "Quality"
                       : (peakCapDropped ? "AverageBitrate" : "ConstrainedVBR"));
            char rcLogBuf[128];
            snprintf(rcLogBuf, sizeof(rcLogBuf), "%s|%s|%d|%d->%s",
                     familyName(family), hardware ? "hw" : "sw",
                     static_cast<int>(p.rateControl), p.qualityLevel, actualRc);
            std::string rcLogKey(rcLogBuf);

            std::lock_guard<std::mutex> cacheLock(s_codecCacheMutex);
            if (s_lastLoggedRcKey != rcLogKey) {
                s_lastLoggedRcKey = rcLogKey;
                geode::log::info(
                    "Instant Replay: VideoToolbox RC requested={}, applied={}, target={} Mbps, max={} Mbps, q={}",
                    requestedRateControlName(p.rateControl), actualRc, p.bitrateMbps, p.maxBitrateMbps, p.qualityLevel);
            }
        }

        // 3. Add AAC audio inputs (Game track + optional split Mic track)
        auto addAudioTrack = [&](int sampleRate, int audioBitrate, AVAssetWriterInput* __strong& outInput,
                                 CMFormatDescriptionRef& outFmt, const char* label) -> bool {
            int validRate = (sampleRate == 44100) ? 44100 : 48000;
            AudioChannelLayout acl{};
            acl.mChannelLayoutTag = kAudioChannelLayoutTag_Stereo;
            NSData* aclData = [NSData dataWithBytes:&acl length:sizeof(acl)];

            NSDictionary* audioSettings = @{
                AVFormatIDKey: @(kAudioFormatMPEG4AAC),
                AVSampleRateKey: @(validRate),
                AVNumberOfChannelsKey: @(2),
                AVEncoderBitRateKey: @(audioBitrate),
                AVChannelLayoutKey: aclData,
            };
            if (![writer canApplyOutputSettings:audioSettings forMediaType:AVMediaTypeAudio]) {
                outCode = -1;
                outError = std::string("AVAssetWriter rejected audio settings for ") + label;
                return false;
            }
            AVAssetWriterInput* audioIn = [AVAssetWriterInput assetWriterInputWithMediaType:AVMediaTypeAudio
                                                                              outputSettings:audioSettings];
            audioIn.expectsMediaDataInRealTime = YES;
            if (![writer canAddInput:audioIn]) {
                outCode = -1;
                outError = std::string("AVAssetWriter cannot add audio input for ") + label;
                return false;
            }
            [writer addInput:audioIn];
            outInput = audioIn;

            AudioStreamBasicDescription asbd{};
            asbd.mSampleRate = static_cast<Float64>(validRate);
            asbd.mFormatID = kAudioFormatLinearPCM;
            asbd.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
            asbd.mBitsPerChannel = 32;
            asbd.mChannelsPerFrame = 2;
            asbd.mBytesPerFrame = 8;
            asbd.mFramesPerPacket = 1;
            asbd.mBytesPerPacket = 8;
            OSStatus fmtErr = CMAudioFormatDescriptionCreate(
                kCFAllocatorDefault, &asbd, sizeof(acl), &acl, 0, nullptr, nullptr, &outFmt);
            if (fmtErr != noErr || !outFmt) {
                outCode = fmtErr;
                outError = buildAudioEncoderError(fmtErr, "CMAudioFormatDescriptionCreate");
                return false;
            }
            return true;
        };

        if (!addAudioTrack(p.gameSampleRate, 192000, impl->gameAudioInput, impl->gameAudioFmt, "game audio")) {
            return false;
        }
        if (p.splitAudio) {
            if (!addAudioTrack(48000, 128000, impl->micAudioInput, impl->micAudioFmt, "mic audio")) {
                return false;
            }
        }

        if (![writer startWriting]) {
            classifyNSError(writer.error, outCode, outFatalStorage, outPermUnsupported, outError);
            if (outError.empty()) outError = "AVAssetWriter startWriting failed";
            return false;
        }
        [writer startSessionAtSourceTime:kCMTimeZero];
        if (writer.status == AVAssetWriterStatusFailed) {
            classifyNSError(writer.error, outCode, outFatalStorage, outPermUnsupported, outError);
            if (outError.empty()) outError = "AVAssetWriter startSessionAtSourceTime failed";
            return false;
        }

        m_impl = std::move(impl);
        return true;
        } @catch (NSException* ex) {
            outCode = -1;
            outPermUnsupported = true;
            outError = formatNsException(ex);
            m_impl.reset();
            return false;
        }
    }
}

EncoderOpResult VideoToolboxEncoder::writeVideoFrame(
    CVPixelBufferRef pixelBuffer,
    int width,
    int height,
    int64_t timestampHns
) {
    MacAutoreleasePoolScope pool;
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_recording || !m_impl || !m_impl->writer || !m_impl->adaptor || !pixelBuffer) {
        return {false, -1, "encoder not recording", false};
    }
    if (width != m_width || height != m_height) {
        return {false, kVTParameterErr, "frame resolution mismatch", false};
    }

    @try {
        if (m_impl->writer.status == AVAssetWriterStatusFailed) {
            int64_t code = -1;
            bool fatal = false;
            bool perm = false;
            std::string msg;
            classifyNSError(m_impl->writer.error, code, fatal, perm, msg);
            return {false, code, msg, fatal};
        }

        // Give VideoToolbox a tiny window if backpressure hits right at a transition
        for (int waitIter = 0;
             waitIter < 5 && !m_impl->videoInput.readyForMoreMediaData &&
             m_impl->writer.status == AVAssetWriterStatusWriting;
             ++waitIter) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        if (!m_impl->videoInput.readyForMoreMediaData) {
            if (m_impl->writer.status == AVAssetWriterStatusFailed) {
                int64_t code = -1;
                bool fatal = false;
                bool perm = false;
                std::string msg;
                classifyNSError(m_impl->writer.error, code, fatal, perm, msg);
                return {false, code, msg, fatal};
            }
            return {false, kVTVideoEncoderNotAvailableNowErr, "video encoder backpressure", false, true};
        }

        int64_t ticks = std::max<int64_t>(0, timestampHns / 10); // 100ns -> 1us ticks
        if (m_impl->lastVideoTicks >= 0 && ticks <= m_impl->lastVideoTicks) {
            ticks = m_impl->lastVideoTicks + 1;
        }
        m_impl->lastVideoTicks = ticks;

        CMTime pts = CMTimeMake(ticks, kVideoMediaTimeScale);
        if (![m_impl->adaptor appendPixelBuffer:pixelBuffer withPresentationTime:pts]) {
            if (m_impl->writer.status == AVAssetWriterStatusFailed) {
                int64_t code = -1;
                bool fatal = false;
                bool perm = false;
                std::string msg;
                classifyNSError(m_impl->writer.error, code, fatal, perm, msg);
                return {false, code, msg, fatal};
            }
            return {false, -1, "appendPixelBuffer returned NO", false};
        }

        // Flush any queued audio buffers that unblocked as video advanced the writer timeline (M2).
        EncoderOpResult audioDrainErr;
        if (!m_impl->drainAllPendingAudio(audioDrainErr)) {
            return audioDrainErr;
        }
        return {true, 0, {}, false};
    } @catch (NSException* ex) {
        return {false, -1, formatNsException(ex), false};
    }
}

EncoderOpResult VideoToolboxEncoder::writeGameAudio(
    const float* pcmFloat,
    int numSamples,
    int channels,
    int sampleRate,
    int64_t timestampHns
) {
    return writeAudioLocked(true, pcmFloat, numSamples, channels, sampleRate, timestampHns);
}

EncoderOpResult VideoToolboxEncoder::writeMicAudio(
    const float* pcmFloat,
    int numSamples,
    int channels,
    int sampleRate,
    int64_t timestampHns
) {
    return writeAudioLocked(false, pcmFloat, numSamples, channels, sampleRate, timestampHns);
}

EncoderOpResult VideoToolboxEncoder::writeAudioLocked(
    bool isGameAudio,
    const float* pcmFloat,
    int numSamples,
    int channels,
    int sampleRate,
    int64_t timestampHns
) {
    MacAutoreleasePoolScope pool;
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_recording || !m_impl || !m_impl->writer || (!isGameAudio && !m_splitAudio)) {
        return {false, -1, "encoder not recording", false};
    }
    AVAssetWriterInput* input = isGameAudio ? m_impl->gameAudioInput : m_impl->micAudioInput;
    CMFormatDescriptionRef fmt = isGameAudio ? m_impl->gameAudioFmt : m_impl->micAudioFmt;
    int64_t& lastTicks = isGameAudio ? m_impl->lastGameAudioTicks : m_impl->lastMicAudioTicks;
    auto& queue = isGameAudio ? m_impl->pendingGameAudio : m_impl->pendingMicAudio;
    double& pendingSec = isGameAudio ? m_impl->pendingGameDurationSec : m_impl->pendingMicDurationSec;
    if (!input || !fmt) {
        return {false, -1, "audio input not initialized", false};
    }
    if (!pcmFloat || numSamples <= 0 || channels != 2) {
        return {false, kVTParameterErr, "invalid audio buffer", false};
    }

    @try {
        if (m_impl->writer.status == AVAssetWriterStatusFailed) {
            int64_t code = -1;
            bool fatal = false;
            bool perm = false;
            std::string msg;
            classifyNSError(m_impl->writer.error, code, fatal, perm, msg);
            return {false, code, msg, fatal};
        }

        // First flush any previously deferred packets on this track (M2).
        EncoderOpResult preDrainErr;
        if (!Impl::drainQueue(m_impl->writer, input, queue, pendingSec, preDrainErr)) {
            return preDrainErr;
        }

        const int validRate = isGameAudio ? m_gameSampleRate : 48000;
        const int srcRate = (sampleRate > 0) ? sampleRate : validRate;
        const size_t byteSize = static_cast<size_t>(numSamples) * 2 * sizeof(float);
        const double chunkDurationSec = static_cast<double>(numSamples) / static_cast<double>(srcRate);

        if (pendingSec + chunkDurationSec > kMaxPendingAudioSeconds) {
            for (int waitIter = 0;
                 waitIter < 250 &&
                 (pendingSec + chunkDurationSec > kMaxPendingAudioSeconds) &&
                 m_impl->writer.status == AVAssetWriterStatusWriting;
                 ++waitIter) {
                if (input.readyForMoreMediaData) {
                    if (!Impl::drainQueue(m_impl->writer, input, queue, pendingSec, preDrainErr)) {
                        return preDrainErr;
                    }
                    if (pendingSec + chunkDurationSec <= kMaxPendingAudioSeconds) {
                        break;
                    }
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            if (pendingSec + chunkDurationSec > kMaxPendingAudioSeconds) {
                if (m_impl->writer.status == AVAssetWriterStatusFailed) {
                    int64_t code = -1;
                    bool fatal = false;
                    bool perm = false;
                    std::string msg;
                    classifyNSError(m_impl->writer.error, code, fatal, perm, msg);
                    return {false, code, msg, fatal};
                }
                return {false, kVTVideoEncoderNotAvailableNowErr,
                        "audio encoder queue saturated (>10s backpressure)", false, true};
            }
        }

        // Build CMBlockBuffer owning a copy of clamped interleaved Float32 samples
        std::vector<float> clamped(static_cast<size_t>(numSamples) * 2);
        for (size_t i = 0; i < clamped.size(); ++i) {
            clamped[i] = std::clamp(pcmFloat[i], -1.0f, 1.0f);
        }

        CMBlockBufferRef blockBuf = nullptr;
        OSStatus err = CMBlockBufferCreateWithMemoryBlock(
            kCFAllocatorDefault, nullptr, byteSize, kCFAllocatorDefault, nullptr, 0, byteSize, 0, &blockBuf);
        if (err != noErr || !blockBuf) {
            return {false, err, buildAudioEncoderError(err, "CMBlockBufferCreateWithMemoryBlock"), false};
        }
        err = CMBlockBufferAssureBlockMemory(blockBuf);
        if (err == noErr) {
            err = CMBlockBufferReplaceDataBytes(clamped.data(), blockBuf, 0, byteSize);
        }
        if (err != noErr) {
            CFRelease(blockBuf);
            return {false, err, buildAudioEncoderError(err, "CMBlockBufferReplaceDataBytes"), false};
        }

        int64_t ptsTicks = llround(static_cast<double>(std::max<int64_t>(0, timestampHns)) *
                                   static_cast<double>(srcRate) / static_cast<double>(kHnsPerSecond));
        if (lastTicks >= 0 && ptsTicks <= lastTicks) {
            ptsTicks = lastTicks + 1;
        }

        CMTime pts = CMTimeMake(ptsTicks, static_cast<CMTimeScale>(srcRate));
        CMSampleBufferRef sampleBuf = nullptr;
        err = CMAudioSampleBufferCreateReadyWithPacketDescriptions(
            kCFAllocatorDefault, blockBuf, fmt, numSamples, pts, nullptr, &sampleBuf);
        CFRelease(blockBuf);
        if (err != noErr || !sampleBuf) {
            return {false, err, buildAudioEncoderError(err, "CMAudioSampleBufferCreateReadyWithPacketDescriptions"), false};
        }

        lastTicks = ptsTicks;
        queue.push_back({sampleBuf, chunkDurationSec});
        pendingSec += chunkDurationSec;

        EncoderOpResult postDrainErr;
        if (!Impl::drainQueue(m_impl->writer, input, queue, pendingSec, postDrainErr)) {
            return postDrainErr;
        }
        return {true, 0, {}, false};
    } @catch (NSException* ex) {
        return {false, -1, formatNsException(ex), false};
    }
}

bool VideoToolboxEncoder::isVideoQueueSaturated(size_t /*maxPendingFrames*/, size_t /*maxQueuedBytes*/) const {
    MacAutoreleasePoolScope pool;
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_recording || !m_impl || !m_impl->videoInput) return false;
    @try {
        return !m_impl->videoInput.readyForMoreMediaData;
    } @catch (NSException*) {
        return true;
    }
}

bool VideoToolboxEncoder::finish() {
    MacAutoreleasePoolScope pool;
    std::lock_guard<std::mutex> lock(m_mutex);
    return finishLocked();
}

bool VideoToolboxEncoder::finishLocked() {
    if (!m_impl || !m_impl->writer) {
        m_recording = false;
        m_impl.reset();
        return true;
    }

    AVAssetWriter* writer = m_impl->writer;
    bool ok = false;

    @try {
        if (m_recording && writer.status == AVAssetWriterStatusWriting) {
            // Mark video finished first so AVAssetWriter immediately unblocks any pending AAC audio samples (M2).
            [m_impl->videoInput markAsFinished];

            auto drainDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            while ((!m_impl->pendingGameAudio.empty() || !m_impl->pendingMicAudio.empty()) &&
                   writer.status == AVAssetWriterStatusWriting) {
                EncoderOpResult drainErr;
                if (!m_impl->drainAllPendingAudio(drainErr)) {
                    geode::log::warn("Instant Replay: audio flush during finish failed: {}", drainErr.message);
                    break;
                }
                if (m_impl->pendingGameAudio.empty() && m_impl->pendingMicAudio.empty()) {
                    break;
                }
                if (std::chrono::steady_clock::now() >= drainDeadline) {
                    geode::log::warn(
                        "Instant Replay: timed out flushing {} game / {} mic buffered audio chunks at segment end",
                        m_impl->pendingGameAudio.size(), m_impl->pendingMicAudio.size());
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }

            if (writer.status == AVAssetWriterStatusWriting) {
                [m_impl->gameAudioInput markAsFinished];
                if (m_impl->micAudioInput) {
                    [m_impl->micAudioInput markAsFinished];
                }

                dispatch_semaphore_t sem = dispatch_semaphore_create(0);
                [writer finishWritingWithCompletionHandler:^{
                    dispatch_semaphore_signal(sem);
                }];
                dispatch_time_t finishDeadline = dispatch_time(DISPATCH_TIME_NOW, 60LL * NSEC_PER_SEC);
                if (dispatch_semaphore_wait(sem, finishDeadline) != 0) {
                    geode::log::warn("Instant Replay: segment finishWriting timed out after 60s, cancelling");
                    [writer cancelWriting];
                    ok = false;
                } else {
                    ok = (writer.status == AVAssetWriterStatusCompleted);
                    if (!ok && writer.error) {
                        int64_t code = 0;
                        bool fatal = false;
                        bool perm = false;
                        std::string errMsg;
                        classifyNSError(writer.error, code, fatal, perm, errMsg);
                        geode::log::warn("Instant Replay: segment finish failed: {}", errMsg);
                    }
                }
            } else {
                if (writer.error) {
                    int64_t code = 0;
                    bool fatal = false;
                    bool perm = false;
                    std::string errMsg;
                    classifyNSError(writer.error, code, fatal, perm, errMsg);
                    geode::log::warn("Instant Replay: segment writer failed during flush: {}", errMsg);
                }
                [writer cancelWriting];
                ok = false;
            }
        } else if (!m_recording) {
            ok = true;
        } else {
            [writer cancelWriting];
            ok = false;
        }
    } @catch (NSException* ex) {
        geode::log::warn("Instant Replay: exception during finishLocked: {}", formatNsException(ex));
        @try {
            if (writer.status == AVAssetWriterStatusWriting) {
                [writer cancelWriting];
            }
        } @catch (NSException*) {}
        ok = false;
    }

    m_recording = false;
    m_impl.reset();
    return ok;
}

// ---------------------------------------------------------------------------
// Segment merge (remux, no re-encode)
// ---------------------------------------------------------------------------

namespace {

NSArray<AVAssetTrack*>* loadAssetTracksSync(AVAsset* asset) {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    return [asset tracks];
#pragma clang diagnostic pop
}

bool sampleIsSync(CMSampleBufferRef sample) {
    CFArrayRef attachments = CMSampleBufferGetSampleAttachmentsArray(sample, false);
    if (!attachments || CFArrayGetCount(attachments) == 0) {
        return true;
    }
    auto dict = static_cast<CFDictionaryRef>(CFArrayGetValueAtIndex(attachments, 0));
    if (!dict) return true;
    CFBooleanRef notSync = nullptr;
    if (CFDictionaryGetValueIfPresent(dict, kCMSampleAttachmentKey_NotSync, reinterpret_cast<const void**>(&notSync)) &&
        notSync && CFBooleanGetValue(notSync)) {
        return false;
    }
    return true;
}

bool appendPcmChunkToAudioInput(
    AVAssetWriter* writer,
    AVAssetWriterInput* input,
    CMFormatDescriptionRef pcmFmt,
    const float* pcmInterleavedStereo,
    int numFrames,
    int sampleRate,
    int64_t ptsSampleIndex,
    std::string& outFailMsg
) {
    if (!writer || !input || !pcmFmt || !pcmInterleavedStereo || numFrames <= 0 || sampleRate <= 0) {
        outFailMsg = "invalid PCM chunk parameters";
        return false;
    }

    const size_t byteSize = static_cast<size_t>(numFrames) * 2 * sizeof(float);
    CMBlockBufferRef blockBuf = nullptr;
    OSStatus err = CMBlockBufferCreateWithMemoryBlock(
        kCFAllocatorDefault, nullptr, byteSize, kCFAllocatorDefault, nullptr, 0, byteSize, 0, &blockBuf);
    if (err != noErr || !blockBuf) {
        outFailMsg = buildAudioEncoderError(err, "CMBlockBufferCreateWithMemoryBlock");
        return false;
    }
    err = CMBlockBufferAssureBlockMemory(blockBuf);
    if (err == noErr) {
        err = CMBlockBufferReplaceDataBytes(pcmInterleavedStereo, blockBuf, 0, byteSize);
    }
    if (err != noErr) {
        CFRelease(blockBuf);
        outFailMsg = buildAudioEncoderError(err, "CMBlockBufferReplaceDataBytes");
        return false;
    }

    CMTime pts = CMTimeMake(ptsSampleIndex, static_cast<CMTimeScale>(sampleRate));
    CMSampleBufferRef sampleBuf = nullptr;
    err = CMAudioSampleBufferCreateReadyWithPacketDescriptions(
        kCFAllocatorDefault, blockBuf, pcmFmt, numFrames, pts, nullptr, &sampleBuf);
    CFRelease(blockBuf);
    if (err != noErr || !sampleBuf) {
        outFailMsg = buildAudioEncoderError(err, "CMAudioSampleBufferCreateReadyWithPacketDescriptions");
        return false;
    }

    bool ok = (writer.status == AVAssetWriterStatusWriting && [input appendSampleBuffer:sampleBuf]);
    CFRelease(sampleBuf);
    if (!ok) {
        int64_t c = 0; bool f = false; bool p = false; std::string m;
        classifyNSError(writer.error, c, f, p, m);
        outFailMsg = m.empty() ? "failed writing re-encoded AAC sample" : m;
        return false;
    }
    return true;
}

ConcatResult concatenateImpl(
    const std::vector<std::filesystem::path>& segmentPaths,
    const std::filesystem::path& finalOutputPath,
    double trimStartSeconds,
    std::atomic<int>* progressPermille
) {
    ConcatResult res;
    auto fail = [&res](std::string msg) -> ConcatResult& {
        res.success = false;
        res.error = std::move(msg);
        return res;
    };

    // Monotonic export progress (K4): never moves backwards, whoever else writes the counter.
    auto reportProgress = [progressPermille](int permille) {
        if (!progressPermille) return;
        permille = std::clamp(permille, 0, 1000);
        int cur = progressPermille->load(std::memory_order_relaxed);
        while (permille > cur &&
               !progressPermille->compare_exchange_weak(cur, permille, std::memory_order_relaxed)) {
        }
    };
    reportProgress(0);

    if (segmentPaths.empty()) return fail("no recorded segments");

    @try {
        struct RemuxGuard {
            AVAssetWriter* writer = nil;
            AVAssetReader* reader = nil;
            bool writerStarted = false;
            bool completed = false;
            ~RemuxGuard() {
                if (!completed) {
                    @try {
                        if (reader) {
                            [reader cancelReading];
                            reader = nil;
                        }
                        if (writer && writerStarted) {
                            [writer cancelWriting];
                        }
                    } @catch (NSException*) {}
                }
            }
        };

        auto pathToURL = [](const std::filesystem::path& p) -> NSURL* {
            std::string utf8 = replay::pathToUtf8(p);
            NSString* s = [NSString stringWithUTF8String:utf8.c_str()];
            return s ? [NSURL fileURLWithPath:s] : nil;
        };

        NSURL* firstUrl = pathToURL(segmentPaths[0]);
        NSURL* outUrl = pathToURL(finalOutputPath);
        if (!firstUrl || !outUrl) return fail("invalid UTF-8 file path");

        std::error_code rmEc;
        std::filesystem::remove(finalOutputPath, rmEc);

        NSDictionary* assetOpts = @{ AVURLAssetPreferPreciseDurationAndTimingKey: @YES };

        struct TrackLayout {
            NSString* mediaType = nil;
            bool isVideo = false;
            AVAssetWriterInput* writerInput = nil;
            bool markedFinished = false;
            // Audio transcoding state (N1: decode LPCM -> single continuous AAC stream with 1 edit).
            int sampleRate = 48000;
            int64_t totalOutputSamples = 0;
            int64_t skipLeadingSamples = 0;
            int64_t pendingPadSamples = 0;
            NSDictionary* pcmReadSettings = nil;
            CMFormatDescriptionRef pcmFmt = nullptr;
        };
        struct LayoutGuard {
            std::vector<TrackLayout> tracks;
            ~LayoutGuard() {
                for (auto& tr : tracks) {
                    if (tr.pcmFmt) {
                        CFRelease(tr.pcmFmt);
                        tr.pcmFmt = nullptr;
                    }
                }
            }
        } layoutGuard;
        std::vector<TrackLayout>& layout = layoutGuard.tracks;
        int64_t videoFrameDurationHns = 0;

        NSError* writerErr = nil;
        AVAssetWriter* writer = [AVAssetWriter assetWriterWithURL:outUrl fileType:AVFileTypeMPEG4 error:&writerErr];
        if (!writer || writerErr) {
            int64_t c = 0; bool f = false; bool p = false; std::string m;
            classifyNSError(writerErr, c, f, p, m);
            return fail("cannot create output file: " + m);
        }

        RemuxGuard remuxGuard;
        remuxGuard.writer = writer;

        // 1 & 2. Inspect first segment inside an inner autorelease pool (M9) for track layout
        // and trim keyframe target. AVAssetWriter natively accepts negative B-frame decode lead
        // on segment 0 (creating an internal video elst without shifting movie start_time > 0), so
        // audio & video both begin cleanly at 0.000s with zero empty audio edits (N1).
        int64_t cutHns = 0;

        {
            @autoreleasepool {
                AVURLAsset* firstAsset = [AVURLAsset URLAssetWithURL:firstUrl options:assetOpts];
                if (!firstAsset) return fail("cannot open first segment");

                NSArray<AVAssetTrack*>* firstTracks = loadAssetTracksSync(firstAsset);
                if (firstTracks.count == 0) return fail("first segment has no media tracks");

                int audioTrackOrd = 0;
                for (AVAssetTrack* tr in firstTracks) {
                    TrackLayout item;
                    item.mediaType = tr.mediaType;
                    item.isVideo = [tr.mediaType isEqualToString:AVMediaTypeVideo];
                    NSArray* fmts = tr.formatDescriptions;
                    if (fmts.count == 0) return fail("track has no format description");
                    CMFormatDescriptionRef srcFmt = (__bridge CMFormatDescriptionRef)fmts[0];

                    if (item.isVideo) {
                        if (videoFrameDurationHns == 0) {
                            float fps = tr.nominalFrameRate;
                            if (fps > 0.5f) {
                                videoFrameDurationHns = llround(static_cast<double>(kHnsPerSecond) / static_cast<double>(fps));
                            }
                        }

                        AVAssetWriterInput* in = [AVAssetWriterInput assetWriterInputWithMediaType:AVMediaTypeVideo
                                                                                     outputSettings:nil
                                                                                   sourceFormatHint:srcFmt];
                        if (!in) {
                            return fail("cannot configure pass-through video writer input");
                        }
                        // Preserve 1us video timescale so 120/240 FPS frame timestamps never collapse into 600-tick grid (M8).
                        in.mediaTimeScale = (tr.naturalTimeScale > 0) ? tr.naturalTimeScale : kVideoMediaTimeScale;
                        // Real-time flag disables cross-track interleaver lockups across segment boundaries (M1).
                        in.expectsMediaDataInRealTime = YES;
                        if (![writer canAddInput:in]) {
                            return fail("cannot configure pass-through video writer input");
                        }
                        [writer addInput:in];
                        item.writerInput = in;
                    } else if ([tr.mediaType isEqualToString:AVMediaTypeAudio]) {
                        int sampleRate = 48000;
                        if (const AudioStreamBasicDescription* srcAsbd = CMAudioFormatDescriptionGetStreamBasicDescription(srcFmt)) {
                            if (srcAsbd->mSampleRate > 1000.0) {
                                int sr = static_cast<int>(llround(srcAsbd->mSampleRate));
                                sampleRate = (sr == 44100) ? 44100 : 48000;
                            }
                        }
                        const int audioBitrate = (audioTrackOrd == 0) ? 192000 : 128000;
                        audioTrackOrd++;

                        AudioChannelLayout acl{};
                        acl.mChannelLayoutTag = kAudioChannelLayoutTag_Stereo;
                        NSData* aclData = [NSData dataWithBytes:&acl length:sizeof(acl)];

                        NSDictionary* aacOutSettings = @{
                            AVFormatIDKey: @(kAudioFormatMPEG4AAC),
                            AVSampleRateKey: @(sampleRate),
                            AVNumberOfChannelsKey: @(2),
                            AVEncoderBitRateKey: @(audioBitrate),
                            AVChannelLayoutKey: aclData,
                        };
                        if (![writer canApplyOutputSettings:aacOutSettings forMediaType:AVMediaTypeAudio]) {
                            return fail("AVAssetWriter rejected export AAC settings");
                        }
                        AVAssetWriterInput* in = [AVAssetWriterInput assetWriterInputWithMediaType:AVMediaTypeAudio
                                                                                     outputSettings:aacOutSettings];
                        if (!in) {
                            return fail("cannot create AAC writer input");
                        }
                        in.expectsMediaDataInRealTime = YES;
                        if (![writer canAddInput:in]) {
                            return fail("cannot add AAC writer input");
                        }
                        [writer addInput:in];

                        AudioStreamBasicDescription pcmAsbd{};
                        pcmAsbd.mSampleRate = static_cast<Float64>(sampleRate);
                        pcmAsbd.mFormatID = kAudioFormatLinearPCM;
                        pcmAsbd.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
                        pcmAsbd.mBitsPerChannel = 32;
                        pcmAsbd.mChannelsPerFrame = 2;
                        pcmAsbd.mBytesPerFrame = 8;
                        pcmAsbd.mFramesPerPacket = 1;
                        pcmAsbd.mBytesPerPacket = 8;
                        CMFormatDescriptionRef pcmFmt = nullptr;
                        OSStatus fmtErr = CMAudioFormatDescriptionCreate(
                            kCFAllocatorDefault, &pcmAsbd, sizeof(acl), &acl, 0, nullptr, nullptr, &pcmFmt);
                        if (fmtErr != noErr || !pcmFmt) {
                            return fail(buildAudioEncoderError(fmtErr, "CMAudioFormatDescriptionCreate"));
                        }

                        item.sampleRate = sampleRate;
                        item.pcmFmt = pcmFmt;
                        item.pcmReadSettings = @{
                            AVFormatIDKey: @(kAudioFormatLinearPCM),
                            AVSampleRateKey: @(sampleRate),
                            AVNumberOfChannelsKey: @(2),
                            AVLinearPCMBitDepthKey: @32,
                            AVLinearPCMIsFloatKey: @YES,
                            AVLinearPCMIsBigEndianKey: @NO,
                            AVLinearPCMIsNonInterleaved: @NO,
                        };
                        item.writerInput = in;
                    } else {
                        AVAssetWriterInput* in = [AVAssetWriterInput assetWriterInputWithMediaType:tr.mediaType
                                                                                     outputSettings:nil
                                                                                   sourceFormatHint:srcFmt];
                        if (!in) {
                            return fail("cannot configure pass-through writer input");
                        }
                        in.expectsMediaDataInRealTime = YES;
                        if (![writer canAddInput:in]) {
                            return fail("cannot configure pass-through writer input");
                        }
                        [writer addInput:in];
                        item.writerInput = in;
                    }
                    layout.push_back(std::move(item));
                }

                if (videoFrameDurationHns <= 0) {
                    videoFrameDurationHns = kHnsPerSecond / 60;
                }

                size_t videoTrackIdx = layout.size();
                for (size_t i = 0; i < layout.size(); ++i) {
                    if (layout[i].isVideo) {
                        videoTrackIdx = i;
                        break;
                    }
                }
                if (videoTrackIdx < layout.size() && trimStartSeconds > 0.05) {
                    const int64_t trimHns = llround(trimStartSeconds * static_cast<double>(kHnsPerSecond));
                    NSError* scanErr = nil;
                    AVAssetReader* scanReader = [AVAssetReader assetReaderWithAsset:firstAsset error:&scanErr];
                    if (!scanReader || scanErr) {
                        int64_t c = 0; bool f = false; bool p = false; std::string m;
                        classifyNSError(scanErr, c, f, p, m);
                        return fail("cannot scan first segment: " + m);
                    }
                    AVAssetReaderTrackOutput* scanOut =
                        [AVAssetReaderTrackOutput assetReaderTrackOutputWithTrack:firstTracks[videoTrackIdx]
                                                                   outputSettings:nil];
                    scanOut.alwaysCopiesSampleData = NO;
                    [scanReader addOutput:scanOut];
                    [scanReader startReading];

                    int64_t lastKeyBeforePts = -1;
                    int64_t firstKeyAfterPts = -1;

                    while (CMSampleBufferRef sbuf = [scanOut copyNextSampleBuffer]) {
                        if (CMSampleBufferGetNumSamples(sbuf) <= 0 || !CMSampleBufferGetDataBuffer(sbuf)) {
                            CFRelease(sbuf);
                            continue;
                        }
                        CMTime pts = CMSampleBufferGetOutputPresentationTimeStamp(sbuf);
                        int64_t ts = cmTimeToHns(pts);
                        bool isKey = sampleIsSync(sbuf);
                        CFRelease(sbuf);

                        if (ts <= trimHns) {
                            if (isKey) lastKeyBeforePts = ts;
                        } else {
                            if (isKey && firstKeyAfterPts < 0) {
                                firstKeyAfterPts = ts;
                                break;
                            }
                            if (ts - trimHns > 50000000LL) break;
                        }
                    }
                    [scanReader cancelReading];

                    constexpr int64_t kMaxKeyframeDriftHns = 20000000LL; // 2s
                    if (lastKeyBeforePts >= 0 && (trimHns - lastKeyBeforePts) <= kMaxKeyframeDriftHns) {
                        cutHns = lastKeyBeforePts;
                    } else if (firstKeyAfterPts >= 0 && (firstKeyAfterPts - trimHns) <= kMaxKeyframeDriftHns) {
                        cutHns = firstKeyAfterPts;
                    } else if (lastKeyBeforePts >= 0) {
                        cutHns = lastKeyBeforePts;
                    } else if (firstKeyAfterPts >= 0) {
                        cutHns = firstKeyAfterPts;
                    } else {
                        cutHns = 0;
                    }
                }
            }
        }

        if (![writer startWriting]) {
            int64_t c = 0; bool f = false; bool p = false; std::string m;
            classifyNSError(writer.error, c, f, p, m);
            return fail("AVAssetWriter startWriting failed: " + m);
        }
        remuxGuard.writerStarted = true;
        [writer startSessionAtSourceTime:kCMTimeZero];

        ConcatTimestampState concatState(0);
        size_t samplesWritten = 0;
        size_t audioOverlapCount = 0;
        int64_t maxAudioOverlapHns = 0;
        uint32_t maxAudioOverlapStream = 0;

        auto recordSampleWithOverlapTracking = [&](uint32_t streamId, bool isVideo, int64_t startHns, int64_t durHns, int sampleRate) {
            int64_t prevAudioEnd = 0;
            const int64_t tol = isVideo ? 0 : ConcatTimestampState::audioOverlapToleranceHns(sampleRate);
            if (!concatState.recordWrittenSample(streamId, isVideo, startHns, durHns, &prevAudioEnd, tol)) {
                ++audioOverlapCount;
                const int64_t ov = std::max<int64_t>(1, prevAudioEnd - startHns);
                if (ov > maxAudioOverlapHns) {
                    maxAudioOverlapHns = ov;
                    maxAudioOverlapStream = streamId;
                }
            }
        };

        // 3. Remux every segment inside its own autoreleasepool (M9) so file descriptors & AVAsset objects drop each iteration.
        for (size_t segIdx = 0; segIdx < segmentPaths.size(); ++segIdx) {
            const std::string segLabel = "segment " + std::to_string(segIdx + 1) + "/" + std::to_string(segmentPaths.size());
            std::string segFailMsg;

            @autoreleasepool {
                NSURL* segUrl = pathToURL(segmentPaths[segIdx]);
                if (!segUrl) {
                    segFailMsg = "invalid path in " + segLabel;
                } else {
                    AVURLAsset* segAsset = [AVURLAsset URLAssetWithURL:segUrl options:assetOpts];
                    if (!segAsset) {
                        segFailMsg = "cannot open " + segLabel;
                    } else {
                        NSError* readerErr = nil;
                        AVAssetReader* reader = [AVAssetReader assetReaderWithAsset:segAsset error:&readerErr];
                        if (!reader || readerErr) {
                            int64_t c = 0; bool f = false; bool p = false; std::string m;
                            classifyNSError(readerErr, c, f, p, m);
                            segFailMsg = "cannot read " + segLabel + ": " + m;
                        } else {
                            remuxGuard.reader = reader;

                            NSArray<AVAssetTrack*>* segTracks = loadAssetTracksSync(segAsset);
                            if (segTracks.count < layout.size()) {
                                segFailMsg = segLabel + " is missing an expected track";
                            } else {
                                struct ReaderSlot {
                                    AVAssetReaderTrackOutput* output = nil;
                                    AVAssetWriterInput* writerInput = nil;
                                    bool isVideo = false;
                                    uint32_t streamId = 0;
                                    CMSampleBufferRef peek = nullptr;
                                    CMTime peekOrigOutPts = kCMTimeInvalid;
                                    int64_t peekPtsHns = 0;
                                    int64_t peekDtsHns = 0;
                                    int64_t peekDurHns = 0;
                                    bool eof = false;
                                };
                                struct PeekGuard {
                                    std::vector<ReaderSlot>& slots;
                                    ~PeekGuard() {
                                        for (auto& sl : slots) {
                                            if (sl.peek) {
                                                CFRelease(sl.peek);
                                                sl.peek = nullptr;
                                            }
                                        }
                                    }
                                };

                                std::vector<ReaderSlot> slots(layout.size());
                                PeekGuard peekGuard{slots};

                                std::vector<bool> usedSegTrack(segTracks.count, false);
                                bool trackSetupOk = true;
                                for (size_t li = 0; li < layout.size(); ++li) {
                                    AVAssetTrack* matched = nil;
                                    for (NSUInteger ti = 0; ti < segTracks.count; ++ti) {
                                        if (!usedSegTrack[ti] && [segTracks[ti].mediaType isEqualToString:layout[li].mediaType]) {
                                            matched = segTracks[ti];
                                            usedSegTrack[ti] = true;
                                            break;
                                        }
                                    }
                                    if (!matched) {
                                        segFailMsg = segLabel + " is missing required stream";
                                        trackSetupOk = false;
                                        break;
                                    }
                                    AVAssetReaderTrackOutput* out =
                                        [AVAssetReaderTrackOutput assetReaderTrackOutputWithTrack:matched
                                                                                   outputSettings:layout[li].pcmReadSettings];
                                    out.alwaysCopiesSampleData = NO;
                                    if (![reader canAddOutput:out]) {
                                        segFailMsg = "cannot attach track output in " + segLabel;
                                        trackSetupOk = false;
                                        break;
                                    }
                                    [reader addOutput:out];
                                    slots[li].output = out;
                                    slots[li].writerInput = layout[li].writerInput;
                                    slots[li].isVideo = layout[li].isVideo;
                                    slots[li].streamId = static_cast<uint32_t>(li);
                                }

                                if (trackSetupOk && ![reader startReading]) {
                                    int64_t c = 0; bool f = false; bool p = false; std::string m;
                                    classifyNSError(reader.error, c, f, p, m);
                                    segFailMsg = "cannot start reading " + segLabel + ": " + m;
                                    trackSetupOk = false;
                                }

                                if (trackSetupOk) {
                                    auto pullSlot = [&](ReaderSlot& sl) {
                                        if (sl.peek || sl.eof) return;
                                        while (true) {
                                            CMSampleBufferRef next = [sl.output copyNextSampleBuffer];
                                            if (!next) {
                                                sl.eof = true;
                                                return;
                                            }
                                            if (CMSampleBufferGetNumSamples(next) <= 0 || !CMSampleBufferGetDataBuffer(next)) {
                                                CFRelease(next);
                                                continue;
                                            }
                                            CMTime pts = sl.isVideo
                                                ? CMSampleBufferGetOutputPresentationTimeStamp(next)
                                                : CMSampleBufferGetPresentationTimeStamp(next);
                                            CMTime dts = sl.isVideo
                                                ? CMSampleBufferGetOutputDecodeTimeStamp(next)
                                                : kCMTimeInvalid;
                                            CMTime dur = sl.isVideo
                                                ? CMSampleBufferGetOutputDuration(next)
                                                : CMSampleBufferGetDuration(next);
                                            sl.peek = next;
                                            sl.peekOrigOutPts = pts;
                                            sl.peekPtsHns = cmTimeToHns(pts);
                                            sl.peekDtsHns = CMTIME_IS_VALID(dts) ? cmTimeToHns(dts) : sl.peekPtsHns;
                                            int64_t durHns = CMTIME_IS_VALID(dur) ? cmTimeToHns(dur) : 0;
                                            if (sl.isVideo && durHns <= 0) durHns = videoFrameDurationHns;
                                            sl.peekDurHns = durHns;
                                            return;
                                        }
                                    };

                                    const int64_t segCut = (segIdx == 0) ? cutHns : 0;
                                    bool seenFirstVideoKey = (segCut <= 0);
                                    concatState.beginSegment();

                                    // Progress inside this segment from the local timestamp just written (K4).
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
                                    const int64_t segDurHns = cmTimeToHns(segAsset.duration);
#pragma clang diagnostic pop
                                    const double segCount = static_cast<double>(segmentPaths.size());
                                    auto noteProgress = [&](int64_t localHns) {
                                        double frac = 0.0;
                                        if (segDurHns > segCut) {
                                            frac = std::clamp(
                                                static_cast<double>(localHns - segCut) / static_cast<double>(segDurHns - segCut),
                                                0.0, 1.0);
                                        }
                                        reportProgress(static_cast<int>(990.0 * (static_cast<double>(segIdx) + frac) / segCount));
                                    };

                                    // Align audio sample counters with outputBase when a prior segment had an anomaly gap
                                    // or when a secondary stream ran slightly longer than primary audio (N1).
                                    std::vector<float> audioPadScratch;
                                    std::vector<float> audioPcmScratch;
                                    for (size_t li = 0; li < layout.size(); ++li) {
                                        if (layout[li].isVideo || !layout[li].pcmFmt) continue;
                                        const int sr = layout[li].sampleRate;
                                        const int64_t expectedStartSamples = llround(
                                            static_cast<double>(concatState.outputBase) * static_cast<double>(sr) /
                                            static_cast<double>(kHnsPerSecond));
                                        if (segIdx == 0) {
                                            layout[li].totalOutputSamples = expectedStartSamples;
                                            layout[li].skipLeadingSamples = 0;
                                            layout[li].pendingPadSamples = 0;
                                        } else {
                                            const int64_t diffSamples = expectedStartSamples - layout[li].totalOutputSamples;
                                            const int64_t tolSamples = std::max<int64_t>(4, sr / 500); // ~2ms rounding guard
                                            layout[li].skipLeadingSamples = (diffSamples < -tolSamples) ? (-diffSamples) : 0;
                                            layout[li].pendingPadSamples = (diffSamples > tolSamples) ? diffSamples : 0;
                                        }
                                    }

                                    for (size_t li = 0; li < slots.size(); ++li) {
                                        pullSlot(slots[li]);
                                        if (segIdx + 1 == segmentPaths.size() && slots[li].eof && !slots[li].peek &&
                                            layout[li].pendingPadSamples <= 0 && !layout[li].markedFinished) {
                                            [layout[li].writerInput markAsFinished];
                                            layout[li].markedFinished = true;
                                        }
                                    }

                                    auto lastProgress = std::chrono::steady_clock::now();
                                    while (segFailMsg.empty()) {
                                        bool anyRemaining = false;
                                        size_t bestIdx = slots.size();
                                        for (size_t i = 0; i < slots.size(); ++i) {
                                            const bool hasPendingPad = (!layout[i].isVideo && layout[i].pendingPadSamples > 0);
                                            if (!slots[i].peek && !hasPendingPad) continue;
                                            anyRemaining = true;
                                            if (!slots[i].writerInput.readyForMoreMediaData) continue;
                                            if (hasPendingPad) {
                                                // Drain any inter-segment silence padding before decoded PCM samples for this track.
                                                bestIdx = i;
                                                break;
                                            }
                                            if (bestIdx == slots.size()) {
                                                bestIdx = i;
                                            } else {
                                                const auto& a = slots[i];
                                                const auto& b = slots[bestIdx];
                                                if (a.peekDtsHns < b.peekDtsHns ||
                                                    (a.peekDtsHns == b.peekDtsHns && a.peekPtsHns < b.peekPtsHns)) {
                                                    bestIdx = i;
                                                }
                                            }
                                        }
                                        if (!anyRemaining) break;
                                        if (bestIdx == slots.size()) {
                                            if (writer.status != AVAssetWriterStatusWriting) {
                                                int64_t c = 0; bool f = false; bool p = false; std::string m;
                                                classifyNSError(writer.error, c, f, p, m);
                                                segFailMsg = "writer failed during " + segLabel + ": " + m;
                                                break;
                                            }
                                            if (std::chrono::steady_clock::now() - lastProgress >= std::chrono::seconds(10)) {
                                                segFailMsg = "remux stalled in " + segLabel + ": writer inputs not ready for 10s";
                                                break;
                                            }
                                            usleep(500);
                                            continue;
                                        }

                                        TrackLayout& curLayout = layout[bestIdx];
                                        ReaderSlot& cur = slots[bestIdx];

                                        auto maybeMarkTrackFinished = [&]() {
                                            if (segIdx + 1 == segmentPaths.size() && cur.eof && !cur.peek &&
                                                curLayout.pendingPadSamples <= 0 && !curLayout.markedFinished) {
                                                [curLayout.writerInput markAsFinished];
                                                curLayout.markedFinished = true;
                                            }
                                        };

                                        // Emit pending gap-fill silence (e.g. shorter audio in prior segment, T-M1) before PCM reads.
                                        if (!cur.isVideo && curLayout.pendingPadSamples > 0) {
                                            const int sr = curLayout.sampleRate;
                                            const int chunkFrames = static_cast<int>(
                                                std::min<int64_t>(curLayout.pendingPadSamples, std::max(480, sr / 10)));
                                            audioPadScratch.assign(static_cast<size_t>(chunkFrames) * 2, 0.0f);
                                            const int64_t startSample = curLayout.totalOutputSamples;
                                            if (!appendPcmChunkToAudioInput(
                                                    writer, cur.writerInput, curLayout.pcmFmt,
                                                    audioPadScratch.data(), chunkFrames, sr, startSample, segFailMsg)) {
                                                break;
                                            }
                                            curLayout.totalOutputSamples += chunkFrames;
                                            curLayout.pendingPadSamples -= chunkFrames;
                                            const int64_t sHns = llround(static_cast<double>(startSample) * kHnsPerSecond / sr);
                                            const int64_t eHns = llround(static_cast<double>(curLayout.totalOutputSamples) * kHnsPerSecond / sr);
                                            recordSampleWithOverlapTracking(cur.streamId, false, sHns, std::max<int64_t>(1, eHns - sHns), sr);
                                            lastProgress = std::chrono::steady_clock::now();
                                            maybeMarkTrackFinished();
                                            samplesWritten++;
                                            continue;
                                        }

                                        CMSampleBufferRef sample = cur.peek;
                                        CMTime origOutPts = cur.peekOrigOutPts;
                                        int64_t ptsHns = cur.peekPtsHns;
                                        int64_t dtsHns = cur.peekDtsHns;
                                        int64_t durHns = cur.peekDurHns;
                                        cur.peek = nullptr;
                                        struct SampleRefGuard {
                                            CMSampleBufferRef ref = nullptr;
                                            ~SampleRefGuard() { if (ref) CFRelease(ref); }
                                        } sampleGuard{sample};
                                        pullSlot(cur);

                                        if (!cur.isVideo && curLayout.pcmFmt) {
                                            // Audio track (N1): sample is already LPCM Float32 stereo decoded by AVAssetReader
                                            // with all edit-list priming and remainder trims resolved cleanly. Slice exact
                                            // segCut / skipLeadingSamples and re-encode as one uninterrupted AAC track.
                                            CMItemCount rawCount = CMSampleBufferGetNumSamples(sample);
                                            CMBlockBufferRef dataBuf = CMSampleBufferGetDataBuffer(sample);
                                            if (rawCount <= 0 || !dataBuf) {
                                                maybeMarkTrackFinished();
                                                lastProgress = std::chrono::steady_clock::now();
                                                continue;
                                            }

                                            const int sr = curLayout.sampleRate;
                                            const size_t availBytes = CMBlockBufferGetDataLength(dataBuf);
                                            const size_t totalFrames = std::min<size_t>(
                                                static_cast<size_t>(rawCount), availBytes / (2 * sizeof(float)));
                                            if (totalFrames == 0) {
                                                maybeMarkTrackFinished();
                                                lastProgress = std::chrono::steady_clock::now();
                                                continue;
                                            }

                                            int64_t trimFrames = 0;
                                            if (segCut > 0) {
                                                const int64_t cutSamples = llround(
                                                    static_cast<double>(segCut) * static_cast<double>(sr) /
                                                    static_cast<double>(kHnsPerSecond));
                                                const int64_t bufStartSamples = llround(
                                                    CMTimeGetSeconds(origOutPts) * static_cast<double>(sr));
                                                if (bufStartSamples < cutSamples) {
                                                    trimFrames = cutSamples - bufStartSamples;
                                                }
                                            }
                                            if (curLayout.skipLeadingSamples > 0) {
                                                const int64_t postCutAvail = std::max<int64_t>(0, static_cast<int64_t>(totalFrames) - trimFrames);
                                                const int64_t extraSkip = std::min(curLayout.skipLeadingSamples, postCutAvail);
                                                trimFrames += extraSkip;
                                                curLayout.skipLeadingSamples -= extraSkip;
                                            }
                                            if (trimFrames >= static_cast<int64_t>(totalFrames)) {
                                                maybeMarkTrackFinished();
                                                lastProgress = std::chrono::steady_clock::now();
                                                continue;
                                            }

                                            const size_t skipCount = static_cast<size_t>(std::max<int64_t>(0, trimFrames));
                                            const size_t keptFrames = totalFrames - skipCount;
                                            audioPcmScratch.resize(totalFrames * 2);
                                            OSStatus copyErr = CMBlockBufferCopyDataBytes(
                                                dataBuf, 0, totalFrames * 2 * sizeof(float), audioPcmScratch.data());
                                            if (copyErr != noErr) {
                                                segFailMsg = buildAudioEncoderError(copyErr, "CMBlockBufferCopyDataBytes");
                                                break;
                                            }

                                            const int64_t startSample = curLayout.totalOutputSamples;
                                            const float* srcPtr = audioPcmScratch.data() + (skipCount * 2);
                                            if (!appendPcmChunkToAudioInput(
                                                    writer, cur.writerInput, curLayout.pcmFmt,
                                                    srcPtr, static_cast<int>(keptFrames), sr, startSample, segFailMsg)) {
                                                break;
                                            }

                                            curLayout.totalOutputSamples += static_cast<int64_t>(keptFrames);
                                            const int64_t sHns = llround(static_cast<double>(startSample) * kHnsPerSecond / sr);
                                            const int64_t eHns = llround(static_cast<double>(curLayout.totalOutputSamples) * kHnsPerSecond / sr);
                                            recordSampleWithOverlapTracking(cur.streamId, false, sHns, std::max<int64_t>(1, eHns - sHns), sr);
                                            noteProgress(ptsHns);

                                            lastProgress = std::chrono::steady_clock::now();
                                            maybeMarkTrackFinished();
                                            samplesWritten++;
                                            continue;
                                        }

                                        if (ptsHns < segCut) {
                                            maybeMarkTrackFinished();
                                            lastProgress = std::chrono::steady_clock::now();
                                            continue;
                                        }
                                        if (cur.isVideo && !seenFirstVideoKey) {
                                            if (!sampleIsSync(sample)) {
                                                maybeMarkTrackFinished();
                                                lastProgress = std::chrono::steady_clock::now();
                                                continue;
                                            }
                                            seenFirstVideoKey = true;
                                        }

                                        int64_t sampleTime = concatState.computeSampleTime(ptsHns, segCut);
                                        int64_t newDts = sampleTime;
                                        if (cur.isVideo) {
                                            concatState.adjustVideoTimestamps(dtsHns, segCut, sampleTime, newDts, false, true);
                                        }

                                        CMItemCount timingCount = 0;
                                        OSStatus tErr = CMSampleBufferGetSampleTimingInfoArray(sample, 0, nullptr, &timingCount);
                                        if (tErr != noErr || timingCount <= 0) {
                                            segFailMsg = "missing sample timing in " + segLabel;
                                            break;
                                        }

                                        std::vector<CMSampleTimingInfo> timings(static_cast<size_t>(timingCount));
                                        tErr = CMSampleBufferGetSampleTimingInfoArray(sample, timingCount, timings.data(), &timingCount);
                                        if (tErr != noErr) {
                                            segFailMsg = "failed reading sample timing in " + segLabel;
                                            break;
                                        }

                                        // Rebase video passthrough sample timing directly onto the unified movie timeline
                                        // (targetOutPts / targetOutDts). Rebasing from raw track timings[0] strips any
                                        // source segment B-frame edit-list lead so AVAssetWriter does not emit an initial
                                        // empty edit entry on H.264 video while keeping DTS monotonic and negative lead intact.
                                        // Audio streams are transcoded above from LPCM into a single continuous AAC stream (N1).
                                        const CMTimeScale trackScale =
                                            (CMTIME_IS_VALID(timings[0].presentationTimeStamp) && timings[0].presentationTimeStamp.timescale > 0)
                                                ? timings[0].presentationTimeStamp.timescale
                                                : ((CMTIME_IS_VALID(origOutPts) && origOutPts.timescale > 0)
                                                       ? origOutPts.timescale
                                                       : static_cast<CMTimeScale>(kHnsPerSecond));

                                        const CMTime targetOutPts = hnsToCMTime(sampleTime, trackScale);
                                        const CMTime rawPtsRef = CMTIME_IS_VALID(timings[0].presentationTimeStamp)
                                            ? CMTimeConvertScale(timings[0].presentationTimeStamp, trackScale, kCMTimeRoundingMethod_RoundHalfAwayFromZero)
                                            : targetOutPts;
                                        const CMTime ptsDelta = CMTimeSubtract(targetOutPts, rawPtsRef);

                                        CMTime dtsDelta = ptsDelta;
                                        if (cur.isVideo && CMTIME_IS_VALID(timings[0].decodeTimeStamp)) {
                                            const CMTimeScale dtsScale =
                                                (timings[0].decodeTimeStamp.timescale > 0)
                                                    ? timings[0].decodeTimeStamp.timescale
                                                    : trackScale;
                                            const CMTime targetOutDts = hnsToCMTime(newDts, dtsScale);
                                            const CMTime rawDtsRef = CMTimeConvertScale(
                                                timings[0].decodeTimeStamp, dtsScale, kCMTimeRoundingMethod_RoundHalfAwayFromZero);
                                            dtsDelta = CMTimeSubtract(targetOutDts, rawDtsRef);
                                        }

                                        for (size_t ti = 0; ti < timings.size(); ++ti) {
                                            if (CMTIME_IS_VALID(timings[ti].presentationTimeStamp)) {
                                                CMTimeScale ts = (timings[ti].presentationTimeStamp.timescale > 0)
                                                    ? timings[ti].presentationTimeStamp.timescale
                                                    : trackScale;
                                                CMTime d = (ts == trackScale)
                                                    ? ptsDelta
                                                    : CMTimeConvertScale(ptsDelta, ts, kCMTimeRoundingMethod_RoundHalfAwayFromZero);
                                                timings[ti].presentationTimeStamp = CMTimeAdd(timings[ti].presentationTimeStamp, d);
                                            }
                                            if (CMTIME_IS_VALID(timings[ti].decodeTimeStamp)) {
                                                CMTime refDelta = cur.isVideo ? dtsDelta : ptsDelta;
                                                CMTimeScale ts = (timings[ti].decodeTimeStamp.timescale > 0)
                                                    ? timings[ti].decodeTimeStamp.timescale
                                                    : trackScale;
                                                CMTime d = (ts == refDelta.timescale)
                                                    ? refDelta
                                                    : CMTimeConvertScale(refDelta, ts, kCMTimeRoundingMethod_RoundHalfAwayFromZero);
                                                timings[ti].decodeTimeStamp = CMTimeAdd(timings[ti].decodeTimeStamp, d);
                                            }
                                        }

                                        CMSampleBufferRef rebased = nullptr;
                                        tErr = CMSampleBufferCreateCopyWithNewTiming(
                                            kCFAllocatorDefault, sample, timingCount, timings.data(), &rebased);
                                        CFRelease(sampleGuard.ref);
                                        sampleGuard.ref = nullptr;
                                        if (tErr != noErr || !rebased) {
                                            segFailMsg = "failed rebasing timestamp in " + segLabel;
                                            break;
                                        }

                                        if (writer.status != AVAssetWriterStatusWriting || ![cur.writerInput appendSampleBuffer:rebased]) {
                                            CFRelease(rebased);
                                            int64_t c = 0; bool f = false; bool p = false; std::string m;
                                            classifyNSError(writer.error, c, f, p, m);
                                            segFailMsg = "failed writing " + segLabel + ": " + m;
                                            break;
                                        }
                                        CFRelease(rebased);
                                        lastProgress = std::chrono::steady_clock::now();
                                        noteProgress(ptsHns);
                                        maybeMarkTrackFinished();

                                        recordSampleWithOverlapTracking(
                                            cur.streamId, cur.isVideo, sampleTime, durHns,
                                            curLayout.sampleRate > 0 ? curLayout.sampleRate : 44100);
                                        samplesWritten++;
                                    }

                                    if (segFailMsg.empty()) {
                                        if (reader.status != AVAssetReaderStatusCompleted) {
                                            int64_t c = 0; bool f = false; bool p = false; std::string m;
                                            classifyNSError(reader.error, c, f, p, m);
                                            segFailMsg = "failed reading " + segLabel + ": " + m;
                                        } else {
                                            bool anomalyWarn = false;
                                            int64_t excessHns = 0;
                                            concatState.endSegment(videoFrameDurationHns, &anomalyWarn, &excessHns);
                                            reportProgress(static_cast<int>(
                                                990.0 * static_cast<double>(segIdx + 1) / static_cast<double>(segmentPaths.size())));
                                            if (anomalyWarn) {
                                                geode::log::warn(
                                                    "VideoToolboxEncoder: video end exceeded primary audio end by {:.2f} ms (>1.5 frames) in {}, falling back to max segment end",
                                                    static_cast<double>(excessHns) / 10000.0, segLabel);
                                            }
                                            remuxGuard.reader = nil;
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }

            if (!segFailMsg.empty()) {
                return fail(segFailMsg);
            }
        }

        if (samplesWritten == 0) {
            return fail("no media samples were written");
        }
        if (audioOverlapCount > 0) {
            geode::log::warn(
                "VideoToolboxEncoder: audio overlap across export (count={}, maxOverlap={:.3f} ms on stream {})",
                audioOverlapCount,
                static_cast<double>(maxAudioOverlapHns) / 10000.0,
                maxAudioOverlapStream);
        }

        for (auto& tr : layout) {
            if (!tr.markedFinished) {
                [tr.writerInput markAsFinished];
                tr.markedFinished = true;
            }
        }

        dispatch_semaphore_t sem = dispatch_semaphore_create(0);
        [writer finishWritingWithCompletionHandler:^{
            dispatch_semaphore_signal(sem);
        }];
        dispatch_time_t finishDeadline = dispatch_time(DISPATCH_TIME_NOW, 60LL * NSEC_PER_SEC);
        if (dispatch_semaphore_wait(sem, finishDeadline) != 0) {
            return fail("timed out waiting 60s for AVAssetWriter finishWriting");
        }

        if (writer.status != AVAssetWriterStatusCompleted) {
            int64_t c = 0; bool f = false; bool p = false; std::string m;
            classifyNSError(writer.error, c, f, p, m);
            return fail("finalizing MP4 failed: " + m);
        }

        remuxGuard.completed = true;
        reportProgress(1000);
        res.success = true;
        res.durationSeconds = static_cast<double>(std::max<int64_t>(0, concatState.outputBase)) /
                              static_cast<double>(kHnsPerSecond);
        return res;
    } @catch (NSException* ex) {
        return fail(formatNsException(ex));
    }
}

} // namespace

ConcatResult VideoToolboxEncoder::concatenateSegments(
    const std::vector<std::filesystem::path>& segmentPaths,
    const std::filesystem::path& finalOutputPath,
    double trimStartSeconds,
    std::atomic<int>* progressPermille
) {
    MacAutoreleasePoolScope pool;
    ConcatResult res;
    try {
        res = concatenateImpl(segmentPaths, finalOutputPath, trimStartSeconds, progressPermille);
    } catch (const std::exception& e) {
        res.success = false;
        res.error = std::string("exception: ") + e.what();
    }
    if (!res.success) {
        std::error_code ec;
        std::filesystem::remove(finalOutputPath, ec);
    }
    return res;
}

} // namespace replay

#endif // GEODE_IS_MACOS || GEODE_IS_IOS
