#include "MacVideoCapabilities.hpp"

#if defined(__APPLE__)

#pragma push_macro("CommentType")
#undef CommentType
#define CommentType AppleScriptCommentTypeDummy
#import <AVFoundation/AVFoundation.h>
#import <CoreFoundation/CoreFoundation.h>
#import <CoreMedia/CoreMedia.h>
#import <Foundation/Foundation.h>
#import <VideoToolbox/VideoToolbox.h>
#undef CommentType
#pragma pop_macro("CommentType")

#include <mutex>
#include <tuple>
#include <unistd.h>
#include <vector>

namespace replay {

namespace {

constexpr CMVideoCodecType kMacCodecTypeAV1 = 'av01';

// Mirrors VideoToolboxEncoder's encoder specification: iOS < 17.4 has no hardware/software keys and only
// hardware encoders, so it uses the default specification for hardware and reports software as missing.
NSDictionary* encoderSpecificationFor(bool hardware) {
#if TARGET_OS_IPHONE
    if (@available(iOS 17.4, *)) {
        return hardware
            ? @{ (__bridge NSString*)kVTVideoEncoderSpecification_RequireHardwareAcceleratedVideoEncoder: @YES }
            : @{ (__bridge NSString*)kVTVideoEncoderSpecification_EnableHardwareAcceleratedVideoEncoder: @NO };
    }
    return hardware ? @{} : nil;
#else
    return hardware
        ? @{ (__bridge NSString*)kVTVideoEncoderSpecification_RequireHardwareAcceleratedVideoEncoder: @YES }
        : @{ (__bridge NSString*)kVTVideoEncoderSpecification_EnableHardwareAcceleratedVideoEncoder: @NO };
#endif
}

// Mirrors VideoToolboxEncoder::tryStartLocked output settings to verify whether AVAssetWriter
// actually accepts software encoding for the given codec (e.g. VTCopyVideoEncoderList lists
// hevc.vcp on M1, yet AVAssetWriter rejects HEVC with hardware acceleration disabled, N3).
bool probeAssetWriterVideoSettings(VideoCodecFamily family, bool hardware, int width, int height) {
    @autoreleasepool {
        @try {
            NSString* codecKey = nil;
            switch (family) {
                case VideoCodecFamily::AV1:  codecKey = @"av01"; break;
                case VideoCodecFamily::HEVC: codecKey = AVVideoCodecTypeHEVC; break;
                case VideoCodecFamily::H264: codecKey = AVVideoCodecTypeH264; break;
                case VideoCodecFamily::VP9:
                default:                     return false;
            }

            NSString* probePath = [NSTemporaryDirectory() stringByAppendingPathComponent:
                [NSString stringWithFormat:@"instant_replay_cap_probe_%d.mp4", getpid()]];
            NSURL* probeUrl = [NSURL fileURLWithPath:probePath];
            NSError* err = nil;
            AVAssetWriter* writer = [AVAssetWriter assetWriterWithURL:probeUrl fileType:AVFileTypeMPEG4 error:&err];
            if (!writer || err) {
                return false;
            }

            NSMutableDictionary* compProps = [NSMutableDictionary dictionary];
            compProps[AVVideoExpectedSourceFrameRateKey] = @60;
            compProps[AVVideoMaxKeyFrameIntervalKey] = @60;
            compProps[AVVideoMaxKeyFrameIntervalDurationKey] = @(1.0);
            if (family == VideoCodecFamily::H264) {
                compProps[AVVideoProfileLevelKey] = AVVideoProfileLevelH264HighAutoLevel;
            }
            compProps[AVVideoAverageBitRateKey] = @8000000;
            compProps[(__bridge NSString*)kVTCompressionPropertyKey_DataRateLimits] = @[@1000000, @(1.0)];

            NSDictionary* encoderSpec = encoderSpecificationFor(hardware);
            if (!encoderSpec) {
                return false;
            }

            NSMutableDictionary* videoSettings = [@{
                AVVideoCodecKey: codecKey,
                AVVideoWidthKey: @(width),
                AVVideoHeightKey: @(height),
                AVVideoCompressionPropertiesKey: compProps,
                AVVideoColorPropertiesKey: @{
                    AVVideoColorPrimariesKey: AVVideoColorPrimaries_ITU_R_709_2,
                    AVVideoTransferFunctionKey: AVVideoTransferFunction_ITU_R_709_2,
                    AVVideoYCbCrMatrixKey: AVVideoYCbCrMatrix_ITU_R_709_2,
                },
            } mutableCopy];
#if TARGET_OS_IPHONE
            // AVAssetWriter has no encoder-specification key on iOS: it always uses the hardware encoder.
            if (!hardware) {
                return false;
            }
#else
            videoSettings[AVVideoEncoderSpecificationKey] = encoderSpec;
#endif

            return [writer canApplyOutputSettings:videoSettings forMediaType:AVMediaTypeVideo] == YES;
        } @catch (NSException*) {
            return false;
        }
    }
}

MacVideoEncoderCaps probeMacVideoEncoderCaps() {
    MacVideoEncoderCaps caps{};

    CFArrayRef rawList = nullptr;
    OSStatus err = VTCopyVideoEncoderList(nullptr, &rawList);
    if (err != noErr || !rawList) {
        // Conservative fallback if VideoToolbox list fails unexpectedly.
        caps.enumerated = false;
        caps.hevcHardware = true;
        caps.hevcSoftware = true;
        caps.hevcSoftwareWritable = probeAssetWriterVideoSettings(VideoCodecFamily::HEVC, false, 1280, 720);
        caps.h264Hardware = true;
        caps.h264Software = true;
        caps.h264SoftwareWritable = probeAssetWriterVideoSettings(VideoCodecFamily::H264, false, 1280, 720);
        return caps;
    }

    caps.enumerated = true;
    NSArray* entries = (__bridge NSArray*)rawList;
    for (id item in entries) {
        if (![item isKindOfClass:[NSDictionary class]]) continue;
        NSDictionary* dict = static_cast<NSDictionary*>(item);

        NSNumber* codecNum = dict[(__bridge NSString*)kVTVideoEncoderList_CodecType];
        if (![codecNum isKindOfClass:[NSNumber class]]) continue;
        const CMVideoCodecType codecType = static_cast<CMVideoCodecType>(codecNum.unsignedIntValue);

        NSNumber* hwNum = dict[(__bridge NSString*)kVTVideoEncoderList_IsHardwareAccelerated];
#if TARGET_OS_IPHONE
        // iOS AVAssetWriter cannot choose between encoders: it always uses the system encoder for the codec
        // (the media engine on devices), so every listed encoder counts as the "hardware" one.
        (void)hwNum;
        const bool isHw = true;
#else
        const bool isHw = ([hwNum isKindOfClass:[NSNumber class]] && hwNum.boolValue);
#endif

        if (codecType == kMacCodecTypeAV1) {
            if (isHw) caps.av1Hardware = true;
            else      caps.av1Software = true;
        } else if (codecType == kCMVideoCodecType_HEVC) {
            if (isHw) caps.hevcHardware = true;
            else      caps.hevcSoftware = true;
        } else if (codecType == kCMVideoCodecType_H264) {
            if (isHw) caps.h264Hardware = true;
            else      caps.h264Software = true;
        }
    }

    CFRelease(rawList);

    if (caps.av1Software) {
        caps.av1SoftwareWritable = probeAssetWriterVideoSettings(VideoCodecFamily::AV1, false, 1280, 720);
    }
    if (caps.hevcSoftware) {
        caps.hevcSoftwareWritable = probeAssetWriterVideoSettings(VideoCodecFamily::HEVC, false, 1280, 720);
    }
    if (caps.h264Software) {
        caps.h264SoftwareWritable = probeAssetWriterVideoSettings(VideoCodecFamily::H264, false, 1280, 720);
    }
    return caps;
}

} // namespace

const MacVideoEncoderCaps& getMacVideoEncoderCaps() {
    static const MacVideoEncoderCaps s_caps = probeMacVideoEncoderCaps();
    return s_caps;
}

bool isMacVideoEncodeSizeSupported(VideoCodecFamily family, bool hardware, int width, int height) {
    if (width < 16 || height < 16) return false;
    if (family == VideoCodecFamily::VP9) return false;
    if (family == VideoCodecFamily::H264 && (width > kH264MaxWidth || height > kH264MaxHeight)) {
        return false;
    }
    if (!getMacVideoEncoderCaps().supports(family, hardware)) {
        return false;
    }
    if (!hardware && !probeAssetWriterVideoSettings(family, false, width, height)) {
        return false;
    }

    using Key = std::tuple<VideoCodecFamily, bool, int, int>;
    static std::mutex s_cacheMutex;
    static std::vector<std::pair<Key, bool>> s_sizeCache;

    Key key{family, hardware, width, height};
    {
        std::lock_guard<std::mutex> lock(s_cacheMutex);
        for (const auto& entry : s_sizeCache) {
            if (entry.first == key) {
                return entry.second;
            }
        }
    }

    bool supported = false;
    @try {
        @autoreleasepool {
            CMVideoCodecType codecType = kCMVideoCodecType_H264;
            if (family == VideoCodecFamily::AV1) {
                codecType = kMacCodecTypeAV1;
            } else if (family == VideoCodecFamily::HEVC) {
                codecType = kCMVideoCodecType_HEVC;
            }

            NSDictionary* encoderSpec = encoderSpecificationFor(hardware);
            if (!encoderSpec) {
                return false;
            }

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
                codecType,
                (__bridge CFDictionaryRef)encoderSpec,
                (__bridge CFDictionaryRef)srcAttrs,
                kCFAllocatorDefault,
                nullptr,
                nullptr,
                &session);
            if (status == noErr && session) {
                VTSessionSetProperty(session, kVTCompressionPropertyKey_RealTime, kCFBooleanTrue);
                status = VTCompressionSessionPrepareToEncodeFrames(session);
                VTCompressionSessionInvalidate(session);
                CFRelease(session);
                supported = (status == noErr);
            }
        }
    } @catch (NSException*) {
        supported = false;
    }

    {
        std::lock_guard<std::mutex> lock(s_cacheMutex);
        if (s_sizeCache.size() >= 32) {
            s_sizeCache.erase(s_sizeCache.begin());
        }
        s_sizeCache.emplace_back(key, supported);
    }
    return supported;
}

} // namespace replay

#endif // __APPLE__
