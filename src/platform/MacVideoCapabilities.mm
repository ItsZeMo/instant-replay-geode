#include "MacVideoCapabilities.hpp"

#if defined(__APPLE__) && !TARGET_OS_IPHONE

#pragma push_macro("CommentType")
#undef CommentType
#define CommentType AppleScriptCommentTypeDummy
#import <CoreFoundation/CoreFoundation.h>
#import <CoreMedia/CoreMedia.h>
#import <Foundation/Foundation.h>
#import <VideoToolbox/VideoToolbox.h>
#undef CommentType
#pragma pop_macro("CommentType")

namespace replay {

namespace {

constexpr CMVideoCodecType kMacCodecTypeAV1 = 'av01';

MacVideoEncoderCaps probeMacVideoEncoderCaps() {
    MacVideoEncoderCaps caps{};

    CFArrayRef rawList = nullptr;
    OSStatus err = VTCopyVideoEncoderList(nullptr, &rawList);
    if (err != noErr || !rawList) {
        // Conservative fallback if VideoToolbox list fails unexpectedly.
        caps.enumerated = false;
        caps.hevcHardware = true;
        caps.hevcSoftware = true;
        caps.h264Hardware = true;
        caps.h264Software = true;
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
        const bool isHw = ([hwNum isKindOfClass:[NSNumber class]] && hwNum.boolValue);

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
    return caps;
}

} // namespace

const MacVideoEncoderCaps& getMacVideoEncoderCaps() {
    static const MacVideoEncoderCaps s_caps = probeMacVideoEncoderCaps();
    return s_caps;
}

} // namespace replay

#endif // __APPLE__ && !TARGET_OS_IPHONE
