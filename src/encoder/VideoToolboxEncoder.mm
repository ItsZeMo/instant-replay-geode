#include "VideoToolboxEncoder.hpp"

#ifdef GEODE_IS_MACOS

#include "ConcatTimestampMath.hpp"
#include "../platform/MacPlatformUtils.hpp"
#include "../platform/MacVideoCapabilities.hpp"

#pragma push_macro("CommentType")
#undef CommentType
#define CommentType AppleScriptCommentTypeDummy
#import <AVFoundation/AVFoundation.h>
#import <AudioToolbox/AudioToolbox.h>
#import <CoreAudio/CoreAudio.h>
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

    NSDictionary* encoderSpec = hardware
        ? @{ (__bridge NSString*)kVTVideoEncoderSpecification_RequireHardwareAcceleratedVideoEncoder: @YES }
        : @{ (__bridge NSString*)kVTVideoEncoderSpecification_EnableHardwareAcceleratedVideoEncoder: @NO };

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
        if (@available(macOS 13.0, *)) {
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
    AVAssetWriter* writer = nil;
    AVAssetWriterInput* videoInput = nil;
    AVAssetWriterInputPixelBufferAdaptor* adaptor = nil;
    AVAssetWriterInput* gameAudioInput = nil;
    AVAssetWriterInput* micAudioInput = nil;

    CMFormatDescriptionRef gameAudioFmt = nullptr;
    CMFormatDescriptionRef micAudioFmt = nullptr;

    int64_t lastVideoTicks = -1;
    int64_t lastGameAudioTicks = -1;
    int64_t lastMicAudioTicks = -1;

    ~Impl() {
        clear();
    }

    void clear() {
        @autoreleasepool {
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
            addAttempt(VideoCodecFamily::HEVC, enableHardware);
        }
        addAttempt(VideoCodecFamily::H264, enableHardware);
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
                [m_impl->writer cancelWriting];
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
                     ": encoder not available in VideoToolbox on this Mac";
    }

    for (const auto& attempt : attempts) {
        bool isPrimary = (attempt.family == targetFamily && attempt.hardware == enableHardware);

        int64_t failCode = 0;
        bool fatalStorage = false;
        bool permUnsupported = false;
        std::string error;
        bool started = tryStartLocked(
            outputPath, attempt.family, attempt.hardware, params,
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
                outputPath, attempt.family, attempt.hardware, params,
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

                std::string logKey = requestKey + "->" + codecLabel;
                if (!isPrimary && !hasForcedCodec && s_lastLoggedCodecKey != logKey) {
                    s_lastLoggedCodecKey = logKey;
                    geode::log::warn(
                        "Instant Replay: requested codec unavailable, fell back to {} ({}x{} @ {} FPS, effBitrate={} Mbps, effMaxBitrate={} Mbps)",
                        codecLabel, width, height, fps, params.bitrateMbps, params.maxBitrateMbps);
                } else {
                    s_lastLoggedCodecKey = logKey;
                    geode::log::info(
                        "Instant Replay: encoder started with {} ({}x{} @ {} FPS, effBitrate={} Mbps, effMaxBitrate={} Mbps)",
                        codecLabel, width, height, fps, params.bitrateMbps, params.maxBitrateMbps);
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

        std::string utf8Path = geode::utils::string::pathToString(outputPath);
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

        int64_t bitrateBps = static_cast<int64_t>(std::max(1, p.bitrateMbps)) * 1000000LL;
        int64_t maxBitrateBps = static_cast<int64_t>(std::max(p.bitrateMbps, p.maxBitrateMbps)) * 1000000LL;
        bool usingActualCbr = false;

        switch (p.rateControl) {
            case RateControlMode::CONSTANT_CBR: {
                if (@available(macOS 13.0, *)) {
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

        NSDictionary* encoderSpec = hardware
            ? @{ (__bridge NSString*)kVTVideoEncoderSpecification_RequireHardwareAcceleratedVideoEncoder: @YES }
            : @{ (__bridge NSString*)kVTVideoEncoderSpecification_EnableHardwareAcceleratedVideoEncoder: @NO };

        NSDictionary* videoSettings = @{
            AVVideoCodecKey: avVideoCodecKeyForFamily(family),
            AVVideoWidthKey: @(p.width),
            AVVideoHeightKey: @(p.height),
            AVVideoEncoderSpecificationKey: encoderSpec,
            AVVideoCompressionPropertiesKey: compProps,
        };

        if (![writer canApplyOutputSettings:videoSettings forMediaType:AVMediaTypeVideo]) {
            // Fallback for encoders rejecting AVVideoQualityKey without bitrate
            if (p.rateControl == RateControlMode::CONSTANT_QUALITY) {
                [compProps removeObjectForKey:AVVideoQualityKey];
                compProps[AVVideoAverageBitRateKey] = @(bitrateBps);
                if (![writer canApplyOutputSettings:videoSettings forMediaType:AVMediaTypeVideo]) {
                    outCode = kVTParameterErr;
                    outPermUnsupported = true;
                    outError = "AVAssetWriter rejected video output settings";
                    return false;
                }
            } else {
                outCode = kVTParameterErr;
                outPermUnsupported = true;
                outError = "AVAssetWriter rejected video output settings";
                return false;
            }
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
                ? (usingActualCbr ? "ConstantCBR" : "TightConstrainedVBR")
                : (p.rateControl == RateControlMode::CONSTANT_QUALITY ? "Quality" : "ConstrainedVBR");
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
    if (m_impl->writer.status == AVAssetWriterStatusFailed) {
        int64_t code = -1;
        bool fatal = false;
        bool perm = false;
        std::string msg;
        classifyNSError(m_impl->writer.error, code, fatal, perm, msg);
        return {false, code, msg, fatal};
    }

    int64_t ticks = std::max<int64_t>(0, timestampHns / 10); // 100ns -> 1us ticks
    if (m_impl->lastVideoTicks >= 0 && ticks <= m_impl->lastVideoTicks) {
        ticks = m_impl->lastVideoTicks + 1;
    }
    m_impl->lastVideoTicks = ticks;

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
        return {false, kVTVideoEncoderNotAvailableNowErr, "video encoder backpressure", false};
    }

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
    return {true, 0, {}, false};
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
    if (!input || !fmt) {
        return {false, -1, "audio input not initialized", false};
    }
    if (!pcmFloat || numSamples <= 0 || channels != 2) {
        return {false, kVTParameterErr, "invalid audio buffer", false};
    }
    if (m_impl->writer.status == AVAssetWriterStatusFailed) {
        int64_t code = -1;
        bool fatal = false;
        bool perm = false;
        std::string msg;
        classifyNSError(m_impl->writer.error, code, fatal, perm, msg);
        return {false, code, msg, fatal};
    }

    const int validRate = isGameAudio ? m_gameSampleRate : 48000;
    const int srcRate = (sampleRate > 0) ? sampleRate : validRate;
    const size_t byteSize = static_cast<size_t>(numSamples) * 2 * sizeof(float);

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
    lastTicks = ptsTicks;

    CMTime pts = CMTimeMake(ptsTicks, static_cast<CMTimeScale>(srcRate));
    CMSampleBufferRef sampleBuf = nullptr;
    err = CMAudioSampleBufferCreateReadyWithPacketDescriptions(
        kCFAllocatorDefault, blockBuf, fmt, numSamples, pts, nullptr, &sampleBuf);
    CFRelease(blockBuf);
    if (err != noErr || !sampleBuf) {
        return {false, err, buildAudioEncoderError(err, "CMAudioSampleBufferCreateReadyWithPacketDescriptions"), false};
    }

    for (int waitIter = 0;
         waitIter < 5 && !input.readyForMoreMediaData &&
         m_impl->writer.status == AVAssetWriterStatusWriting;
         ++waitIter) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    bool appended = false;
    if (input.readyForMoreMediaData && m_impl->writer.status == AVAssetWriterStatusWriting) {
        appended = [input appendSampleBuffer:sampleBuf];
    }
    CFRelease(sampleBuf);

    if (!appended) {
        if (m_impl->writer.status == AVAssetWriterStatusFailed) {
            int64_t code = -1;
            bool fatal = false;
            bool perm = false;
            std::string msg;
            classifyNSError(m_impl->writer.error, code, fatal, perm, msg);
            return {false, code, msg, fatal};
        }
        return {false, -1, "audio appendSampleBuffer returned NO", false};
    }
    return {true, 0, {}, false};
}

bool VideoToolboxEncoder::isVideoQueueSaturated(size_t /*maxPendingFrames*/, size_t /*maxQueuedBytes*/) const {
    MacAutoreleasePoolScope pool;
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_recording || !m_impl || !m_impl->videoInput) return false;
    return !m_impl->videoInput.readyForMoreMediaData;
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

    if (m_recording && writer.status == AVAssetWriterStatusWriting) {
        [m_impl->videoInput markAsFinished];
        [m_impl->gameAudioInput markAsFinished];
        if (m_impl->micAudioInput) {
            [m_impl->micAudioInput markAsFinished];
        }

        dispatch_semaphore_t sem = dispatch_semaphore_create(0);
        [writer finishWritingWithCompletionHandler:^{
            dispatch_semaphore_signal(sem);
        }];
        dispatch_semaphore_wait(sem, DISPATCH_TIME_FOREVER);

        ok = (writer.status == AVAssetWriterStatusCompleted);
        if (!ok && writer.error) {
            int64_t code = 0;
            bool fatal = false;
            bool perm = false;
            std::string errMsg;
            classifyNSError(writer.error, code, fatal, perm, errMsg);
            geode::log::warn("Instant Replay: segment finish failed: {}", errMsg);
        }
    } else if (!m_recording) {
        ok = true;
    } else {
        [writer cancelWriting];
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

ConcatResult concatenateImpl(
    const std::vector<std::filesystem::path>& segmentPaths,
    const std::filesystem::path& finalOutputPath,
    double trimStartSeconds
) {
    ConcatResult res;
    auto fail = [&res](std::string msg) -> ConcatResult& {
        res.success = false;
        res.error = std::move(msg);
        return res;
    };

    if (segmentPaths.empty()) return fail("no recorded segments");

    auto pathToURL = [](const std::filesystem::path& p) -> NSURL* {
        std::string utf8 = geode::utils::string::pathToString(p);
        NSString* s = [NSString stringWithUTF8String:utf8.c_str()];
        return s ? [NSURL fileURLWithPath:s] : nil;
    };

    NSURL* firstUrl = pathToURL(segmentPaths[0]);
    NSURL* outUrl = pathToURL(finalOutputPath);
    if (!firstUrl || !outUrl) return fail("invalid UTF-8 file path");

    std::error_code rmEc;
    std::filesystem::remove(finalOutputPath, rmEc);

    NSDictionary* assetOpts = @{ AVURLAssetPreferPreciseDurationAndTimingKey: @YES };
    AVURLAsset* firstAsset = [AVURLAsset URLAssetWithURL:firstUrl options:assetOpts];
    if (!firstAsset) return fail("cannot open first segment");

    NSArray<AVAssetTrack*>* firstTracks = loadAssetTracksSync(firstAsset);
    if (firstTracks.count == 0) return fail("first segment has no media tracks");

    struct TrackLayout {
        NSString* mediaType = nil;
        bool isVideo = false;
        CMFormatDescriptionRef formatDesc = nullptr;
        AVAssetWriterInput* writerInput = nil;
        bool markedFinished = false;
    };
    std::vector<TrackLayout> layout;
    int64_t videoFrameDurationHns = 0;

    NSError* writerErr = nil;
    AVAssetWriter* writer = [AVAssetWriter assetWriterWithURL:outUrl fileType:AVFileTypeMPEG4 error:&writerErr];
    if (!writer || writerErr) {
        int64_t c = 0; bool f = false; bool p = false; std::string m;
        classifyNSError(writerErr, c, f, p, m);
        return fail("cannot create output file: " + m);
    }

    for (AVAssetTrack* tr in firstTracks) {
        TrackLayout item;
        item.mediaType = tr.mediaType;
        item.isVideo = [tr.mediaType isEqualToString:AVMediaTypeVideo];
        NSArray* fmts = tr.formatDescriptions;
        if (fmts.count == 0) return fail("track has no format description");
        item.formatDesc = (__bridge CMFormatDescriptionRef)fmts[0];

        if (item.isVideo && videoFrameDurationHns == 0) {
            float fps = tr.nominalFrameRate;
            if (fps > 0.5f) {
                videoFrameDurationHns = llround(static_cast<double>(kHnsPerSecond) / static_cast<double>(fps));
            }
        }

        AVAssetWriterInput* in = [AVAssetWriterInput assetWriterInputWithMediaType:tr.mediaType
                                                                     outputSettings:nil
                                                                   sourceFormatHint:item.formatDesc];
        in.expectsMediaDataInRealTime = NO;
        if (!in || ![writer canAddInput:in]) {
            return fail("cannot configure pass-through writer input");
        }
        [writer addInput:in];
        item.writerInput = in;
        layout.push_back(std::move(item));
    }

    if (videoFrameDurationHns <= 0) {
        videoFrameDurationHns = kHnsPerSecond / 60;
    }

    // 2. Scan first segment video track for initial DTS (B-frame lead) and trim keyframe target
    int64_t cutHns = 0;
    bool hasFirstVideoDts = false;
    int64_t firstVideoDts = 0;

    {
        size_t videoTrackIdx = layout.size();
        for (size_t i = 0; i < layout.size(); ++i) {
            if (layout[i].isVideo) {
                videoTrackIdx = i;
                break;
            }
        }
        if (videoTrackIdx < layout.size()) {
            const int64_t trimHns = (trimStartSeconds > 0.05)
                ? llround(trimStartSeconds * static_cast<double>(kHnsPerSecond))
                : 0;
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

            struct KeyCandidate {
                int64_t pts = -1;
                int64_t dts = 0;
                bool hasDts = false;
            };
            KeyCandidate initialSample, lastKeyBefore, firstKeyAfter;

            while (CMSampleBufferRef sbuf = [scanOut copyNextSampleBuffer]) {
                if (CMSampleBufferGetNumSamples(sbuf) <= 0 || !CMSampleBufferGetDataBuffer(sbuf)) {
                    CFRelease(sbuf);
                    continue;
                }
                CMTime pts = CMSampleBufferGetOutputPresentationTimeStamp(sbuf);
                CMTime dts = CMSampleBufferGetOutputDecodeTimeStamp(sbuf);
                int64_t ts = cmTimeToHns(pts);
                bool sampleHasDts = CMTIME_IS_VALID(dts);
                int64_t sampleDts = sampleHasDts ? cmTimeToHns(dts) : ts;

                if (initialSample.pts < 0) {
                    initialSample = {ts, sampleDts, sampleHasDts};
                    if (trimHns <= 0) {
                        CFRelease(sbuf);
                        break;
                    }
                }

                bool isKey = sampleIsSync(sbuf);
                CFRelease(sbuf);

                if (ts <= trimHns) {
                    if (isKey) lastKeyBefore = {ts, sampleDts, sampleHasDts};
                } else {
                    if (isKey && firstKeyAfter.pts < 0) {
                        firstKeyAfter = {ts, sampleDts, sampleHasDts};
                        break;
                    }
                    if (ts - trimHns > 50000000LL) break;
                }
            }
            [scanReader cancelReading];

            if (trimHns <= 0) {
                cutHns = 0;
                hasFirstVideoDts = initialSample.hasDts;
                firstVideoDts = initialSample.dts;
            } else {
                constexpr int64_t kMaxKeyframeDriftHns = 20000000LL; // 2s
                KeyCandidate chosen = {0, initialSample.dts, initialSample.hasDts};
                if (lastKeyBefore.pts >= 0 && (trimHns - lastKeyBefore.pts) <= kMaxKeyframeDriftHns) {
                    chosen = lastKeyBefore;
                } else if (firstKeyAfter.pts >= 0 && (firstKeyAfter.pts - trimHns) <= kMaxKeyframeDriftHns) {
                    chosen = firstKeyAfter;
                } else if (lastKeyBefore.pts >= 0) {
                    chosen = lastKeyBefore;
                } else if (firstKeyAfter.pts >= 0) {
                    chosen = firstKeyAfter;
                }
                cutHns = chosen.pts;
                hasFirstVideoDts = chosen.hasDts;
                firstVideoDts = chosen.dts;
            }
        }
    }

    if (![writer startWriting]) {
        int64_t c = 0; bool f = false; bool p = false; std::string m;
        classifyNSError(writer.error, c, f, p, m);
        return fail("AVAssetWriter startWriting failed: " + m);
    }
    [writer startSessionAtSourceTime:kCMTimeZero];

    const int64_t initialPtsShift = (hasFirstVideoDts && firstVideoDts < cutHns)
        ? (cutHns - firstVideoDts)
        : 0;

    ConcatTimestampState concatState(initialPtsShift);
    size_t samplesWritten = 0;

    // 3. Remux every segment with interleaved chronological track pulling (avoids multi-track buffer deadlock)
    for (size_t segIdx = 0; segIdx < segmentPaths.size(); ++segIdx) {
        const std::string segLabel = "segment " + std::to_string(segIdx + 1) + "/" + std::to_string(segmentPaths.size());
        NSURL* segUrl = pathToURL(segmentPaths[segIdx]);
        if (!segUrl) return fail("invalid path in " + segLabel);

        AVURLAsset* segAsset = [AVURLAsset URLAssetWithURL:segUrl options:assetOpts];
        if (!segAsset) return fail("cannot open " + segLabel);

        NSError* readerErr = nil;
        AVAssetReader* reader = [AVAssetReader assetReaderWithAsset:segAsset error:&readerErr];
        if (!reader || readerErr) {
            int64_t c = 0; bool f = false; bool p = false; std::string m;
            classifyNSError(readerErr, c, f, p, m);
            return fail("cannot read " + segLabel + ": " + m);
        }

        NSArray<AVAssetTrack*>* segTracks = loadAssetTracksSync(segAsset);
        if (segTracks.count < layout.size()) {
            return fail(segLabel + " is missing an expected track");
        }

        struct ReaderSlot {
            AVAssetReaderTrackOutput* output = nil;
            AVAssetWriterInput* writerInput = nil;
            bool isVideo = false;
            uint32_t streamId = 0;
            CMSampleBufferRef peek = nullptr;
            int64_t peekPtsHns = 0;
            int64_t peekDtsHns = 0;
            int64_t peekDurHns = 0;
            bool eof = false;
        };
        std::vector<ReaderSlot> slots(layout.size());

        std::vector<bool> usedSegTrack(segTracks.count, false);
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
                return fail(segLabel + " is missing required stream");
            }
            AVAssetReaderTrackOutput* out = [AVAssetReaderTrackOutput assetReaderTrackOutputWithTrack:matched
                                                                                       outputSettings:nil];
            out.alwaysCopiesSampleData = NO;
            if (![reader canAddOutput:out]) {
                return fail("cannot attach track output in " + segLabel);
            }
            [reader addOutput:out];
            slots[li].output = out;
            slots[li].writerInput = layout[li].writerInput;
            slots[li].isVideo = layout[li].isVideo;
            slots[li].streamId = static_cast<uint32_t>(li);
        }

        if (![reader startReading]) {
            int64_t c = 0; bool f = false; bool p = false; std::string m;
            classifyNSError(reader.error, c, f, p, m);
            return fail("cannot start reading " + segLabel + ": " + m);
        }

        auto releasePeeks = [&slots]() {
            for (auto& sl : slots) {
                if (sl.peek) {
                    CFRelease(sl.peek);
                    sl.peek = nullptr;
                }
            }
        };

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
                CMTime pts = CMSampleBufferGetOutputPresentationTimeStamp(next);
                CMTime dts = CMSampleBufferGetOutputDecodeTimeStamp(next);
                CMTime dur = CMSampleBufferGetOutputDuration(next);
                sl.peek = next;
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

        for (size_t li = 0; li < slots.size(); ++li) {
            pullSlot(slots[li]);
            if (segIdx + 1 == segmentPaths.size() && slots[li].eof && !slots[li].peek && !layout[li].markedFinished) {
                [layout[li].writerInput markAsFinished];
                layout[li].markedFinished = true;
            }
        }

        while (true) {
            bool anyRemaining = false;
            size_t bestIdx = slots.size();
            for (size_t i = 0; i < slots.size(); ++i) {
                if (!slots[i].peek) continue;
                anyRemaining = true;
                if (!slots[i].writerInput.readyForMoreMediaData) continue;
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
                    releasePeeks();
                    [reader cancelReading];
                    int64_t c = 0; bool f = false; bool p = false; std::string m;
                    classifyNSError(writer.error, c, f, p, m);
                    return fail("writer failed during " + segLabel + ": " + m);
                }
                usleep(500);
                continue;
            }

            ReaderSlot& cur = slots[bestIdx];
            CMSampleBufferRef sample = cur.peek;
            int64_t ptsHns = cur.peekPtsHns;
            int64_t dtsHns = cur.peekDtsHns;
            int64_t durHns = cur.peekDurHns;
            cur.peek = nullptr;
            pullSlot(cur);

            auto maybeMarkTrackFinished = [&]() {
                if (segIdx + 1 == segmentPaths.size() && cur.eof && !cur.peek && !layout[bestIdx].markedFinished) {
                    [layout[bestIdx].writerInput markAsFinished];
                    layout[bestIdx].markedFinished = true;
                }
            };

            if (ptsHns < segCut) {
                CFRelease(sample);
                maybeMarkTrackFinished();
                continue;
            }
            if (cur.isVideo && !seenFirstVideoKey) {
                if (!sampleIsSync(sample)) {
                    CFRelease(sample);
                    maybeMarkTrackFinished();
                    continue;
                }
                seenFirstVideoKey = true;
            }

            int64_t sampleTime = concatState.computeSampleTime(ptsHns, segCut);
            int64_t newDts = sampleTime;
            if (cur.isVideo) {
                concatState.adjustVideoTimestamps(dtsHns, segCut, sampleTime, newDts, false);
            }

            CMItemCount timingCount = 0;
            OSStatus tErr = CMSampleBufferGetSampleTimingInfoArray(sample, 0, nullptr, &timingCount);
            if (tErr != noErr || timingCount <= 0) {
                releasePeeks();
                CFRelease(sample);
                [reader cancelReading];
                return fail("missing sample timing in " + segLabel);
            }

            std::vector<CMSampleTimingInfo> timings(static_cast<size_t>(timingCount));
            tErr = CMSampleBufferGetSampleTimingInfoArray(sample, timingCount, timings.data(), &timingCount);
            if (tErr != noErr) {
                releasePeeks();
                CFRelease(sample);
                [reader cancelReading];
                return fail("failed reading sample timing in " + segLabel);
            }

            const CMTime baseOrigPts = timings[0].presentationTimeStamp;
            const CMTimeScale trackScale = CMTIME_IS_VALID(baseOrigPts) && baseOrigPts.timescale > 0
                ? baseOrigPts.timescale
                : static_cast<CMTimeScale>(kHnsPerSecond);
            const CMTime baseNewPts = hnsToCMTime(sampleTime, trackScale);
            const CMTime baseNewDts = cur.isVideo ? hnsToCMTime(newDts, trackScale) : baseNewPts;

            for (size_t ti = 0; ti < timings.size(); ++ti) {
                if (ti == 0) {
                    timings[ti].presentationTimeStamp = baseNewPts;
                    if (CMTIME_IS_VALID(timings[ti].decodeTimeStamp)) {
                        timings[ti].decodeTimeStamp = baseNewDts;
                    }
                } else {
                    if (CMTIME_IS_VALID(timings[ti].presentationTimeStamp) && CMTIME_IS_VALID(baseOrigPts)) {
                        CMTime offset = CMTimeSubtract(timings[ti].presentationTimeStamp, baseOrigPts);
                        timings[ti].presentationTimeStamp = CMTimeAdd(baseNewPts, offset);
                    }
                    if (CMTIME_IS_VALID(timings[ti].decodeTimeStamp) && CMTIME_IS_VALID(baseOrigPts)) {
                        CMTime offset = CMTimeSubtract(timings[ti].decodeTimeStamp, baseOrigPts);
                        timings[ti].decodeTimeStamp = CMTimeAdd(baseNewDts, offset);
                    }
                }
            }

            CMSampleBufferRef rebased = nullptr;
            tErr = CMSampleBufferCreateCopyWithNewTiming(
                kCFAllocatorDefault, sample, timingCount, timings.data(), &rebased);
            CFRelease(sample);
            if (tErr != noErr || !rebased) {
                releasePeeks();
                [reader cancelReading];
                return fail("failed rebasing timestamp in " + segLabel);
            }

            if (writer.status != AVAssetWriterStatusWriting || ![cur.writerInput appendSampleBuffer:rebased]) {
                CFRelease(rebased);
                releasePeeks();
                [reader cancelReading];
                int64_t c = 0; bool f = false; bool p = false; std::string m;
                classifyNSError(writer.error, c, f, p, m);
                return fail("failed writing " + segLabel + ": " + m);
            }
            CFRelease(rebased);
            maybeMarkTrackFinished();

            int64_t prevAudioEnd = 0;
            if (!concatState.recordWrittenSample(cur.streamId, cur.isVideo, sampleTime, durHns, &prevAudioEnd)) {
                geode::log::warn(
                    "VideoToolboxEncoder: audio stream {} packet at {} overlaps prior end {} in {}",
                    cur.streamId, sampleTime, prevAudioEnd, segLabel);
            }
            samplesWritten++;
        }

        releasePeeks();
        if (reader.status != AVAssetReaderStatusCompleted) {
            int64_t c = 0; bool f = false; bool p = false; std::string m;
            classifyNSError(reader.error, c, f, p, m);
            [writer cancelWriting];
            return fail("failed reading " + segLabel + ": " + m);
        }

        bool anomalyWarn = false;
        int64_t excessHns = 0;
        concatState.endSegment(videoFrameDurationHns, &anomalyWarn, &excessHns);
        if (anomalyWarn) {
            geode::log::warn(
                "VideoToolboxEncoder: video end exceeded primary audio end by {:.2f} ms (>1.5 frames) in {}, falling back to max segment end",
                static_cast<double>(excessHns) / 10000.0, segLabel);
        }
    }

    if (samplesWritten == 0) {
        [writer cancelWriting];
        return fail("no media samples were written");
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
    dispatch_semaphore_wait(sem, DISPATCH_TIME_FOREVER);

    if (writer.status != AVAssetWriterStatusCompleted) {
        int64_t c = 0; bool f = false; bool p = false; std::string m;
        classifyNSError(writer.error, c, f, p, m);
        return fail("finalizing MP4 failed: " + m);
    }

    res.success = true;
    res.durationSeconds = static_cast<double>(std::max<int64_t>(0, concatState.outputBase - initialPtsShift)) /
                          static_cast<double>(kHnsPerSecond);
    return res;
}

} // namespace

ConcatResult VideoToolboxEncoder::concatenateSegments(
    const std::vector<std::filesystem::path>& segmentPaths,
    const std::filesystem::path& finalOutputPath,
    double trimStartSeconds
) {
    MacAutoreleasePoolScope pool;
    ConcatResult res;
    try {
        res = concatenateImpl(segmentPaths, finalOutputPath, trimStartSeconds);
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

#endif // GEODE_IS_MACOS
