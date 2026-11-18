#import <AVFoundation/AVFoundation.h>
#import <AudioToolbox/AudioToolbox.h>
#import <CoreAudio/CoreAudio.h>
#import <CoreFoundation/CoreFoundation.h>
#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>
#import <OpenGL/OpenGL.h>
#import <OpenGL/gl.h>
#import <OpenGL/glext.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

#include "capture/FrameBudget.hpp"
#include "capture/GameAudioRing.hpp"
#include "config/CodecUtils.hpp"
#include "encoder/ConcatTimestampMath.hpp"
#include "encoder/VideoToolboxEncoder.hpp"
#include "platform/MacVideoCapabilities.hpp"
#include "recorder/MicMixFifo.hpp"

#include "shared_pure_tests.hpp"

namespace {

#define CHECK(expr) do { \
    if (!(expr)) { \
        std::cerr << "FAIL at line " << __LINE__ << ": " #expr "\n"; \
        std::exit(1); \
    } \
} while (0)

CVPixelBufferRef makeTestPixelBuffer(int width, int height, uint8_t topR, uint8_t topG, uint8_t topB,
                                     uint8_t botR, uint8_t botG, uint8_t botB) {
    NSDictionary* attrs = @{
        (__bridge NSString*)kCVPixelBufferIOSurfacePropertiesKey: @{},
    };
    CVPixelBufferRef pb = nullptr;
    CHECK(CVPixelBufferCreate(kCFAllocatorDefault, width, height, kCVPixelFormatType_32BGRA,
                              (__bridge CFDictionaryRef)attrs, &pb) == kCVReturnSuccess && pb != nullptr);
    CHECK(CVPixelBufferLockBaseAddress(pb, 0) == kCVReturnSuccess);
    uint8_t* base = static_cast<uint8_t*>(CVPixelBufferGetBaseAddress(pb));
    size_t stride = CVPixelBufferGetBytesPerRow(pb);
    for (int y = 0; y < height; ++y) {
        uint8_t r = (y < height / 2) ? topR : botR;
        uint8_t g = (y < height / 2) ? topG : botG;
        uint8_t b = (y < height / 2) ? topB : botB;
        uint8_t* row = base + static_cast<size_t>(y) * stride;
        for (int x = 0; x < width; ++x) {
            row[x * 4 + 0] = b;
            row[x * 4 + 1] = g;
            row[x * 4 + 2] = r;
            row[x * 4 + 3] = 255;
        }
    }
    CVPixelBufferUnlockBaseAddress(pb, 0);
    return pb;
}

void writeEncoderSegment(
    const std::filesystem::path& path,
    double videoDurationSec,
    double gameAudioDurationSec,
    double micAudioDurationSec,
    int fps = 30,
    replay::CodecType codec = replay::CodecType::H264,
    bool hardware = true,
    replay::RateControlMode rc = replay::RateControlMode::ADAPTIVE_VBR,
    int gameSampleRate = 48000
) {
    replay::VideoToolboxEncoder enc;
    const bool splitAudio = (micAudioDurationSec > 0.0);
    auto res = enc.start(
        path,
        128,
        72,
        fps,
        8,
        12,
        80,
        rc,
        splitAudio,
        codec,
        hardware,
        "",
        gameSampleRate,
        nullptr,
        true
    );
    CHECK(res.ok);

    const int numFrames = std::max(1, static_cast<int>(std::llround(videoDurationSec * fps)));
    const int64_t frameDurHns = 10000000LL / fps;
    const int totalGameSamples = static_cast<int>(std::llround(gameAudioDurationSec * gameSampleRate));
    const int totalMicSamples = splitAudio ? static_cast<int>(std::llround(micAudioDurationSec * 48000.0)) : 0;
    const int gameChunk = std::max(1, gameSampleRate / 20); // 50ms chunks
    constexpr int micChunk = 2400;                          // 50ms @ 48 kHz

    CVPixelBufferRef pb = makeTestPixelBuffer(128, 72, 220, 40, 20, 20, 40, 220);
    std::vector<float> gamePcm(static_cast<size_t>(gameChunk) * 2, 0.0f);
    std::vector<float> micPcm(static_cast<size_t>(micChunk) * 2, 0.0f);

    int videoIdx = 0;
    int gameWritten = 0;
    int micWritten = 0;

    // Interleave video frames and audio chunks in chronological presentation order matching runtime (N1/Sec 6).
    while (videoIdx < numFrames || gameWritten < totalGameSamples || micWritten < totalMicSamples) {
        const int64_t nextV = (videoIdx < numFrames)
            ? static_cast<int64_t>(videoIdx) * frameDurHns
            : INT64_MAX;
        const int64_t nextG = (gameWritten < totalGameSamples)
            ? (static_cast<int64_t>(gameWritten) * 10000000LL) / gameSampleRate
            : INT64_MAX;
        const int64_t nextM = (micWritten < totalMicSamples)
            ? (static_cast<int64_t>(micWritten) * 10000000LL) / 48000LL
            : INT64_MAX;

        if (nextV <= nextG && nextV <= nextM && videoIdx < numFrames) {
            for (int w = 0; w < 200 && enc.isVideoQueueSaturated(16, 256ull * 1024 * 1024); ++w) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            CHECK(enc.writeVideoFrame(pb, 128, 72, nextV).ok);
            videoIdx++;
        } else if (nextG <= nextM && gameWritten < totalGameSamples) {
            int n = std::min(gameChunk, totalGameSamples - gameWritten);
            for (int s = 0; s < n; ++s) {
                float v = 0.05f * std::sinf(0.02f * static_cast<float>(gameWritten + s));
                gamePcm[s * 2 + 0] = v;
                gamePcm[s * 2 + 1] = v;
            }
            CHECK(enc.writeGameAudio(gamePcm.data(), n, 2, gameSampleRate, nextG).ok);
            gameWritten += n;
        } else if (micWritten < totalMicSamples) {
            int n = std::min(micChunk, totalMicSamples - micWritten);
            for (int s = 0; s < n; ++s) {
                float v = 0.04f * std::sinf(0.03f * static_cast<float>(micWritten + s));
                micPcm[s * 2 + 0] = v;
                micPcm[s * 2 + 1] = v;
            }
            CHECK(enc.writeMicAudio(micPcm.data(), n, 2, 48000, nextM).ok);
            micWritten += n;
        }
    }
    CFRelease(pb);

    CHECK(enc.finish());
    CHECK(std::filesystem::exists(path));
    CHECK(std::filesystem::file_size(path) > 1000);
}

void verifyMp4NoOverlapAndDuration(
    const std::filesystem::path& mp4Path,
    int expectedAudioTracks,
    double minDurationSec,
    double maxDurationSec
) {
    @autoreleasepool {
        std::string u8 = mp4Path.string();
        NSURL* url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:u8.c_str()]];
        AVURLAsset* asset = [AVURLAsset URLAssetWithURL:url options:@{ AVURLAssetPreferPreciseDurationAndTimingKey: @YES }];
        CHECK(asset != nil);

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
        NSArray<AVAssetTrack*>* tracks = [asset tracks];
#pragma clang diagnostic pop

        int videoCount = 0;
        int audioCount = 0;
        for (AVAssetTrack* tr in tracks) {
            if ([tr.mediaType isEqualToString:AVMediaTypeVideo]) videoCount++;
            if ([tr.mediaType isEqualToString:AVMediaTypeAudio]) audioCount++;
        }
        CHECK(videoCount == 1);
        CHECK(audioCount == expectedAudioTracks);

        double assetDur = CMTimeGetSeconds(asset.duration);
        CHECK(assetDur >= minDurationSec && assetDur <= maxDurationSec);

        NSError* err = nil;
        AVAssetReader* reader = [AVAssetReader assetReaderWithAsset:asset error:&err];
        CHECK(reader != nil && err == nil);

        struct VerifyTrack {
            AVAssetReaderTrackOutput* out = nil;
            bool isVideo = false;
        };
        std::vector<VerifyTrack> vTracks;
        for (AVAssetTrack* tr in tracks) {
            AVAssetReaderTrackOutput* out = [AVAssetReaderTrackOutput assetReaderTrackOutputWithTrack:tr outputSettings:nil];
            out.alwaysCopiesSampleData = NO;
            [reader addOutput:out];
            vTracks.push_back({out, [tr.mediaType isEqualToString:AVMediaTypeVideo]});
        }
        CHECK([reader startReading]);

        for (size_t i = 0; i < vTracks.size(); ++i) {
            int64_t prevPtsUs = -1;
            int64_t prevDtsUs = -1;
            int64_t prevAudioEndUs = -1;
            int count = 0;
            while (CMSampleBufferRef s = [vTracks[i].out copyNextSampleBuffer]) {
                if (CMSampleBufferGetNumSamples(s) <= 0 || !CMSampleBufferGetDataBuffer(s)) {
                    CFRelease(s);
                    continue;
                }
                CMTime pts = CMSampleBufferGetOutputPresentationTimeStamp(s);
                CMTime dts = CMSampleBufferGetOutputDecodeTimeStamp(s);
                CMTime dur = CMSampleBufferGetOutputDuration(s);
                int64_t ptsUs = llround(CMTimeGetSeconds(pts) * 1e6);
                int64_t dtsUs = CMTIME_IS_VALID(dts) ? llround(CMTimeGetSeconds(dts) * 1e6) : ptsUs;
                int64_t durUs = CMTIME_IS_VALID(dur) ? llround(CMTimeGetSeconds(dur) * 1e6) : 0;

                if (vTracks[i].isVideo) {
                    if (prevDtsUs >= 0) CHECK(dtsUs >= prevDtsUs);
                    prevDtsUs = dtsUs;
                } else {
                    if (prevPtsUs >= 0) CHECK(ptsUs >= prevPtsUs);
                    // Allow <= 2ms AAC rounding slack
                    if (prevAudioEndUs >= 0) CHECK(ptsUs + 2000 >= prevAudioEndUs);
                    prevPtsUs = ptsUs;
                    if (durUs > 0) prevAudioEndUs = std::max(prevAudioEndUs, ptsUs + durUs);
                }
                count++;
                CFRelease(s);
            }
            CHECK(count > 0);
        }
        CHECK(reader.status == AVAssetReaderStatusCompleted);
    }
}

void testVideoToolboxAndRemux() {
    std::cout << "[Test 1] VideoToolboxEncoder (HEVC/H264 HW & SW, RC modes, fallback cache) & Concat...\n";
    auto testDir = std::filesystem::temp_directory_path() / "InstantReplayMacHostTest";
    std::error_code ec;
    std::filesystem::remove_all(testDir, ec);
    std::filesystem::create_directories(testDir, ec);

    replay::VideoToolboxEncoder::resetCodecCache();

    // 1a. AV1 -> HEVC (or H.264) fallback & session cache check
    {
        auto av1TestPath = testDir / "av1_fallback.mp4";
        replay::VideoToolboxEncoder enc;
        auto res = enc.start(av1TestPath, 128, 72, 30, 6, 9, 80, replay::RateControlMode::ADAPTIVE_VBR,
                             false, replay::CodecType::AV1, true, "", 48000, nullptr, true);
        CHECK(res.ok);
        std::cout << "  Test 1a started with: " << res.message << "\n";
        CHECK(enc.hasActiveCodec());
        CVPixelBufferRef pb = makeTestPixelBuffer(128, 72, 255, 0, 0, 0, 0, 255);
        for (int i = 0; i < 15; ++i) {
            for (int w = 0; w < 200 && enc.isVideoQueueSaturated(16, 256ull * 1024 * 1024); ++w) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            auto wRes = enc.writeVideoFrame(pb, 128, 72, i * 333333LL);
            if (!wRes.ok) {
                std::cerr << "  frame " << i << " err (" << wRes.code << "): " << wRes.message << "\n";
            }
            CHECK(wRes.ok);
        }
        CFRelease(pb);
        std::vector<float> a(24000 * 2, 0.01f);
        CHECK(enc.writeGameAudio(a.data(), 24000, 2, 48000, 0).ok);
        std::cout << "  Test 1a finishing...\n" << std::flush;
        CHECK(enc.finish());
        std::cout << "  Test 1a verifying...\n" << std::flush;
        verifyMp4NoOverlapAndDuration(av1TestPath, 1, 0.35, 0.70);
        std::cout << "  Test 1a OK\n" << std::flush;
    }

    // 1b. Software H.264, CBR, Constant Quality, and invalid path failure
    {
        std::cout << "  Test 1b starting SW H264...\n" << std::flush;
        replay::VideoToolboxEncoder encSw;
        auto swPath = testDir / "h264_sw_cq.mp4";
        auto rSw = encSw.start(swPath, 128, 72, 30, 6, 6, 75, replay::RateControlMode::CONSTANT_QUALITY,
                               false, replay::CodecType::H264, false, "", 44100, nullptr, true);
        CHECK(rSw.ok);
        CHECK(encSw.activeHardware() == false);
        CHECK(encSw.activeCodecFamily() == replay::VideoCodecFamily::H264);
        CHECK(encSw.encodedGameSampleRate() == 44100);
        CVPixelBufferRef pb = makeTestPixelBuffer(128, 72, 0, 255, 0, 0, 0, 255);
        for (int i = 0; i < 10; ++i) {
            for (int w = 0; w < 200 && encSw.isVideoQueueSaturated(16, 256ull * 1024 * 1024); ++w) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            CHECK(encSw.writeVideoFrame(pb, 128, 72, i * 333333LL).ok);
        }
        CFRelease(pb);
        std::vector<float> a44(22050 * 2, 0.01f);
        CHECK(encSw.writeGameAudio(a44.data(), 22050, 2, 44100, 0).ok);
        CHECK(encSw.finish());
        verifyMp4NoOverlapAndDuration(swPath, 1, 0.25, 0.70);

        replay::VideoToolboxEncoder encCbr;
        auto cbrPath = testDir / "h264_cbr.mp4";
        auto rCbr = encCbr.start(cbrPath, 128, 72, 30, 5, 5, 80, replay::RateControlMode::CONSTANT_CBR,
                                 false, replay::CodecType::H264, true, "", 48000, nullptr, true);
        CHECK(rCbr.ok);
        CHECK(encCbr.finish());

        // Oversized 5K H.264 with no fallback allowed must fail cleanly
        replay::VideoToolboxEncoder encOversized;
        auto oversizedPath = testDir / "oversized_h264.mp4";
        auto rBadDims = encOversized.start(oversizedPath, 5120, 1440, 60, 20, 30, 80,
                                           replay::RateControlMode::ADAPTIVE_VBR,
                                           false, replay::CodecType::H264, true, "", 48000, nullptr, false);
        CHECK(!rBadDims.ok);
        CHECK(!rBadDims.message.empty());

        // Non-existent parent dir must fail cleanly without crashing
        replay::VideoToolboxEncoder encBadDir;
        auto rBadDir = encBadDir.start("/nonexistent_dir_zemo_test_12345/out.mp4", 128, 72, 30, 5, 8, 80,
                                         replay::RateControlMode::ADAPTIVE_VBR,
                                         false, replay::CodecType::H264, true, "", 48000, nullptr, true);
        CHECK(rBadDir.ok == false);
        std::cout << "  Test 1b OK\n" << std::flush;
    }

    // 1c. Dual-audio multi-segment encode & lossless concatenation + keyframe-snapped trim
    std::cout << "  Test 1c writing 3 dual-audio segments...\n" << std::flush;
    auto seg1 = testDir / "seg_1.mp4";
    auto seg2 = testDir / "seg_2.mp4";
    auto seg3 = testDir / "seg_3.mp4";
    writeEncoderSegment(seg1, 3.0, 3.0, 3.0, 30, replay::CodecType::HEVC, true);
    writeEncoderSegment(seg2, 1.5, 1.5, 1.5, 30, replay::CodecType::HEVC, true);
    writeEncoderSegment(seg3, 1.0, 1.0, 1.0, 30, replay::CodecType::HEVC, true);
    std::cout << "  Test 1c concatenating...\n" << std::flush;

    auto merged = testDir / "merged_full.mp4";
    auto concatRes = replay::VideoToolboxEncoder::concatenateSegments({seg1, seg2, seg3}, merged, 0.0);
    std::cout << "  Test 1c concat done ok=" << concatRes.success
              << " dur=" << concatRes.durationSeconds << "s err=" << concatRes.error << "\n" << std::flush;
    CHECK(concatRes.success);
    CHECK(concatRes.durationSeconds >= 5.2 && concatRes.durationSeconds <= 5.9);
    std::cout << "  Test 1c verifying merged...\n" << std::flush;
    verifyMp4NoOverlapAndDuration(merged, 2, 5.2, 5.9);
    std::cout << "  Test 1c trimming 1.8s...\n" << std::flush;

    auto trimmed = testDir / "merged_trimmed.mp4";
    auto trimRes = replay::VideoToolboxEncoder::concatenateSegments({seg1, seg2, seg3}, trimmed, 1.8);
    std::cout << "  Test 1c trimmed done ok=" << trimRes.success
              << " dur=" << trimRes.durationSeconds << "s err=" << trimRes.error << "\n" << std::flush;
    CHECK(trimRes.success);
    // 1s GOP keyframes in seg_1 -> trim=1.8s snaps to lastKeyBefore=1.0s -> ~4.5s output
    CHECK(trimRes.durationSeconds >= 4.1 && trimRes.durationSeconds <= 4.9);
    verifyMp4NoOverlapAndDuration(trimmed, 2, 4.1, 4.9);

    std::filesystem::remove_all(testDir, ec);
    std::cout << "  PASS Test 1\n";
}

void testOpenGLInvertedBlitAndPboToEncoder() {
    std::cout << "[Test 2] Headless OpenGL GPU inverted-Y FBO blit + PBO readback -> CVPixelBuffer...\n";
    CGLPixelFormatAttribute attrs[] = {
        kCGLPFAAccelerated,
        static_cast<CGLPixelFormatAttribute>(0)
    };
    CGLPixelFormatObj pix = nullptr;
    GLint npix = 0;
    CGLError err = CGLChoosePixelFormat(attrs, &pix, &npix);
    if (err != kCGLNoError || !pix) {
        // Retry without kCGLPFAAccelerated for headless software renderer VMs
        CGLPixelFormatAttribute swAttrs[] = { static_cast<CGLPixelFormatAttribute>(0) };
        CHECK(CGLChoosePixelFormat(swAttrs, &pix, &npix) == kCGLNoError && pix != nullptr);
    }
    CGLContextObj ctx = nullptr;
    CHECK(CGLCreateContext(pix, nullptr, &ctx) == kCGLNoError && ctx != nullptr);
    CGLDestroyPixelFormat(pix);
    CHECK(CGLSetCurrentContext(ctx) == kCGLNoError);

    constexpr int kW = 128;
    constexpr int kH = 64;
    GLuint srcFbo = 0, srcRbo = 0, dstFbo = 0, dstRbo = 0, pbo = 0;
    glGenFramebuffers(1, &srcFbo);
    glGenRenderbuffers(1, &srcRbo);
    glBindRenderbuffer(GL_RENDERBUFFER, srcRbo);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, kW, kH);
    glBindFramebuffer(GL_FRAMEBUFFER, srcFbo);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, srcRbo);
    CHECK(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE);

    // Paint OpenGL bottom half Blue and top half Red (remember OpenGL y=0 is bottom!)
    glEnable(GL_SCISSOR_TEST);
    glScissor(0, 0, kW, kH / 2);
    glClearColor(0.0f, 0.0f, 1.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glScissor(0, kH / 2, kW, kH / 2);
    glClearColor(1.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glDisable(GL_SCISSOR_TEST);

    glGenFramebuffers(1, &dstFbo);
    glGenRenderbuffers(1, &dstRbo);
    glBindRenderbuffer(GL_RENDERBUFFER, dstRbo);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, kW, kH);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, dstFbo);
    glFramebufferRenderbuffer(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, dstRbo);
    CHECK(glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE);

    // Inverted-Y GPU blit (matching FrameGrabber macOS path)
    glBindFramebuffer(GL_READ_FRAMEBUFFER, srcFbo);
    glBlitFramebuffer(0, 0, kW, kH, 0, kH, kW, 0, GL_COLOR_BUFFER_BIT, GL_NEAREST);

    glBindFramebuffer(GL_READ_FRAMEBUFFER, dstFbo);
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glPixelStorei(GL_PACK_ROW_LENGTH, 0);
    glGenBuffers(1, &pbo);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, pbo);
    glBufferData(GL_PIXEL_PACK_BUFFER, static_cast<GLsizeiptr>(kW * kH * 4), nullptr, GL_STREAM_READ);
    glReadPixels(0, 0, kW, kH, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, nullptr);

    auto* mapped = static_cast<const uint8_t*>(glMapBuffer(GL_PIXEL_PACK_BUFFER, GL_READ_ONLY));
    CHECK(mapped != nullptr);

    // Top row in top-down buffer (row 0) must be pure Red (BGRA: 0, 0, 255, 255)
    CHECK(mapped[0] == 0 && mapped[1] == 0 && mapped[2] == 255 && mapped[3] == 255);
    // Bottom row in top-down buffer (row kH-1) must be pure Blue (BGRA: 255, 0, 0, 255)
    const size_t lastRow = static_cast<size_t>(kH - 1) * kW * 4;
    CHECK(mapped[lastRow + 0] == 255 && mapped[lastRow + 1] == 0 &&
          mapped[lastRow + 2] == 0 && mapped[lastRow + 3] == 255);

    glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    glDeleteBuffers(1, &pbo);
    glDeleteFramebuffers(1, &srcFbo);
    glDeleteRenderbuffers(1, &srcRbo);
    glDeleteFramebuffers(1, &dstFbo);
    glDeleteRenderbuffers(1, &dstRbo);
    CGLSetCurrentContext(nullptr);
    CGLDestroyContext(ctx);
    std::cout << "  PASS Test 2\n";
}

void testCoreAudioHardwareEnumeration() {
    std::cout << "[Test 3] CoreAudio HAL input enumeration safety check...\n";
    AudioObjectPropertyAddress devicesAddr = {
        kAudioHardwarePropertyDevices,
        kAudioObjectPropertyScopeGlobal,
        0
    };
    UInt32 dataSize = 0;
    OSStatus err = AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &devicesAddr, 0, nullptr, &dataSize);
    CHECK(err == noErr);
    if (dataSize >= sizeof(AudioDeviceID)) {
        std::vector<AudioDeviceID> devs(dataSize / sizeof(AudioDeviceID));
        CHECK(AudioObjectGetPropertyData(kAudioObjectSystemObject, &devicesAddr, 0, nullptr, &dataSize, devs.data()) == noErr);
        std::cout << "    (enumerated " << devs.size() << " CoreAudio HAL devices)\n";
    }
    std::cout << "  PASS Test 3\n";
}

void testSharedMathAndBuffers() {
    std::cout << "[Test 4] FrameBudget, GameAudioRing, MicMixFifo, CodecUtils, ConcatTimestampMath...\n";

    // FrameBudget
    {
        constexpr size_t kFrame4kBytes = 3840ull * 2160 * 4;
        CHECK(replay::effectiveFrameByteBudget(64ull * 1024 * 1024, kFrame4kBytes, 4, 256ull * 1024 * 1024) ==
              kFrame4kBytes * 4);
        CHECK(replay::canRetainPooledFrameBytes(3, kFrame4kBytes * 3, kFrame4kBytes, 6, kFrame4kBytes * 4));
        CHECK(!replay::canRetainPooledFrameBytes(4, kFrame4kBytes * 4, kFrame4kBytes, 6, kFrame4kBytes * 4));
        CHECK(!replay::canRetainPooledFrameBytes(6, 0, 100, 6, 1024));
    }

    // GameAudioRing
    {
        auto ring = std::make_unique<replay::GameAudioRing>();
        CHECK(!ring->hasPending());
        std::vector<float> pkt1(256 * 2, 0.25f);
        CHECK(ring->push(pkt1.data(), 256, 2, 1.0f, 48000, 1'000'000'000LL));
        std::vector<float> pkt2(512 * 6, 0.25f);
        CHECK(ring->push(pkt2.data(), 512, 6, 1.0f, 48000, 1'020'000'000LL));
        CHECK(ring->pendingFrames() == 768);
        std::vector<float> out;
        int outRate = 0;
        std::chrono::steady_clock::time_point outTs{};
        size_t n = ring->drain(out, outRate, outTs);
        CHECK(n == 768 && outRate == 48000);
        CHECK(std::abs(out[0] - 0.25f) < 1e-6f);
        const float expected51 = 0.25f * (1.0f + 0.7071f + 0.5f + 0.7071f);
        CHECK(std::abs(out[256 * 2] - expected51) < 1e-5f);
    }

    // MicMixFifo
    {
        replay::MicMixFifo fifo;
        std::vector<float> mic(960 * 2, 0.25f);
        fifo.appendResampled(mic.data(), 960, 48000, 7200, 48000 * 5);
        std::vector<float> game(4800 * 2, 0.10f);
        CHECK(fifo.mixInto(game.data(), 4800, 0) == 0);
        CHECK(fifo.mixInto(game.data(), 4800, 4800) == 960);
        CHECK(std::abs(game[2400 * 2] - 0.35f) < 1e-5f);
        CHECK(fifo.availFrames() == 0);
    }

    // CodecUtils
    {
        auto r = replay::clampDimensionsToBounds(5160, 2160);
        CHECK(r.clamped && r.width == 4096 && r.height == 1714);
        auto hevcVt = replay::resolveRequestedCodecSpec(3, true, "hevc_vt");
        CHECK(hevcVt.family == replay::VideoCodecFamily::HEVC && hevcVt.hardware);
        auto swH264 = replay::resolveRequestedCodecSpec(3, true, "software_h264");
        CHECK(swH264.family == replay::VideoCodecFamily::H264 && !swH264.hardware);
    }

    // ConcatTimestampMath
    {
        replay::ConcatTimestampState state(0);
        state.beginSegment();
        int64_t t0 = state.computeSampleTime(0, 0);
        CHECK(state.recordWrittenSample(1, false, t0, 200000));
        int64_t t1 = state.computeSampleTime(150000, 0);
        CHECK(!state.recordWrittenSample(1, false, t1, 200000));
        state.endSegment(166667);
        CHECK(state.outputBase == 350000);
    }

    // macOS tilde path expansion & relative path rejection
    {
        const char* home = std::getenv("HOME");
        if (home && home[0]) {
            auto tildeMovies = replay::ReplayConfig::normalizeCustomDirPath("~/Movies/Replays/");
            CHECK(!tildeMovies.empty());
            CHECK(tildeMovies == (std::filesystem::path(home) / "Movies" / "Replays"));
            CHECK(replay::ReplayConfig::normalizeCustomDirPath("relative/path").empty());
        }
    }

    std::cout << "  PASS Test 4\n";
}

void testHardwareCapabilityChecks() {
    std::cout << "[Test 5] VideoToolbox hardware compatibility & AV1 gating...\n";
    const auto& caps = replay::getMacVideoEncoderCaps();
    CHECK(caps.enumerated);
    CHECK(caps.h264Hardware || caps.h264Software);
    CHECK(caps.hevcHardware || caps.hevcSoftware);
    CHECK(!caps.supports(replay::VideoCodecFamily::VP9, true));
    CHECK(!caps.supports(replay::VideoCodecFamily::VP9, false));

    // M16 check: in CPU software mode, H.264 SW is preferred before HEVC SW.
    if (caps.h264Software) {
        CHECK(replay::ReplayConfig::bestSupportedCodec(false) == replay::CodecType::H264);
    }

    replay::ReplayConfig cfg;
    cfg.codec = replay::CodecType::AV1;
    cfg.hardwareAcceleration = true;
    const bool sanitized = cfg.sanitizeCodecSettings();
    if (!caps.av1Hardware) {
        CHECK(sanitized);
        CHECK(cfg.codec.load() != replay::CodecType::AV1);
        CHECK(replay::ReplayConfig::isPresetCodecSupported(cfg.codec.load(), true));
    }

    if (!caps.av1Hardware) {
        std::error_code ec;
        std::filesystem::path dir = std::filesystem::temp_directory_path() /
                                    ("ir_host_caps_" + std::to_string(static_cast<unsigned long long>(::getpid())));
        std::filesystem::create_directories(dir, ec);
        std::filesystem::path seg = dir / "av1_blocked.mp4";
        replay::VideoToolboxEncoder enc;
        auto res = enc.start(
            seg, 320, 180, 60, 8, 12, 80,
            replay::RateControlMode::ADAPTIVE_VBR, false,
            replay::CodecType::AV1, true, "", 48000,
            nullptr, false);
        CHECK(!res.ok);
        CHECK(!res.message.empty());
        enc.finish();

        // With fallback enabled, AV1 cleanly falls back to HEVC (or H.264) without failing.
        std::filesystem::path segFallback = dir / "av1_fallback.mp4";
        auto resFallback = enc.start(
            segFallback, 320, 180, 60, 8, 12, 80,
            replay::RateControlMode::ADAPTIVE_VBR, false,
            replay::CodecType::AV1, true, "", 48000,
            nullptr, true);
        CHECK(resFallback.ok);
        CHECK(enc.activeCodecFamily() != replay::VideoCodecFamily::AV1);
        enc.finish();
        std::filesystem::remove_all(dir, ec);
    }

    std::cout << "  PASS Test 5 (av1HW=" << caps.av1Hardware
              << ", hevcHW=" << caps.hevcHardware
              << ", h264HW=" << caps.h264Hardware << ")\n";
}

// T-M1: Middle segment has 3s shorter audio than video; remux must complete cleanly under 20s without deadlock.
void testConcatShortAudioNoDeadlock() {
    std::cout << "[Test 6 / T-M1] Concat with 3s shorter audio in middle segment (no deadlock <20s)...\n" << std::flush;
    auto testDir = std::filesystem::temp_directory_path() / "InstantReplayMacHostTest_M1";
    std::error_code ec;
    std::filesystem::remove_all(testDir, ec);
    std::filesystem::create_directories(testDir, ec);

    auto seg1 = testDir / "seg_1.mp4";
    auto seg2 = testDir / "seg_2.mp4";
    auto seg3 = testDir / "seg_3.mp4";
    auto merged = testDir / "merged_m1.mp4";

    writeEncoderSegment(seg1, 3.0, 3.0, 3.0, 30, replay::CodecType::HEVC, true);
    writeEncoderSegment(seg2, 4.5, 1.5, 1.5, 30, replay::CodecType::HEVC, true); // 3s shorter game & mic audio
    writeEncoderSegment(seg3, 2.0, 2.0, 2.0, 30, replay::CodecType::HEVC, true);

    std::atomic<bool> done{false};
    std::thread watchdog([&done]() {
        for (int i = 0; i < 200 && !done.load(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        if (!done.load()) {
            std::cerr << "FAIL: T-M1 concatenateSegments hung past 20s timeout!\n";
            std::abort();
        }
    });

    auto t0 = std::chrono::steady_clock::now();
    auto concatRes = replay::VideoToolboxEncoder::concatenateSegments({seg1, seg2, seg3}, merged, 0.0);
    done.store(true);
    watchdog.join();

    double elapsedSec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::cout << "  T-M1 finished in " << elapsedSec << "s (ok=" << concatRes.success
              << ", dur=" << concatRes.durationSeconds << "s)\n" << std::flush;
    CHECK(elapsedSec < 20.0);
    CHECK(concatRes.success);
    CHECK(concatRes.durationSeconds >= 8.8 && concatRes.durationSeconds <= 10.2);
    verifyMp4NoOverlapAndDuration(merged, 2, 8.8, 10.2);

    std::filesystem::remove_all(testDir, ec);
    std::cout << "  PASS Test 6 (T-M1)\n";
}

// T-M2: 120s of 50ms audio chunks interleaved with video (plus burst lead audio) -> zero writeGameAudio errors.
void testLongAudioBurstNoBackpressureError() {
    std::cout << "[Test 7 / T-M2] 120s of 50ms audio chunks + lead burst (no AAC readyForMoreMediaData errors)...\n" << std::flush;
    auto testDir = std::filesystem::temp_directory_path() / "InstantReplayMacHostTest_M2";
    std::error_code ec;
    std::filesystem::remove_all(testDir, ec);
    std::filesystem::create_directories(testDir, ec);

    auto outPath = testDir / "long_120s.mp4";
    replay::VideoToolboxEncoder enc;
    auto startRes = enc.start(
        outPath, 128, 72, 15, 4, 6, 80, replay::RateControlMode::ADAPTIVE_VBR,
        false, replay::CodecType::H264, true, "", 48000, nullptr, true);
    CHECK(startRes.ok);

    constexpr int kAudioChunkFrames = 2400; // 50ms @ 48 kHz
    constexpr int kTotalAudioChunks = 2400; // 120.0s
    constexpr int kTotalVideoFrames = 1800; // 120.0s @ 15 FPS
    std::vector<float> pcm(kAudioChunkFrames * 2, 0.0f);
    for (int s = 0; s < kAudioChunkFrames; ++s) {
        float v = 0.04f * std::sinf(0.05f * static_cast<float>(s));
        pcm[s * 2 + 0] = v;
        pcm[s * 2 + 1] = v;
    }

    CVPixelBufferRef pb = makeTestPixelBuffer(128, 72, 80, 180, 240, 20, 40, 90);
    int videoIdx = 0;
    int audioIdx = 0;

    // Deliberately pre-push 1.5s of audio before frame 0 to exercise deferred queue draining
    while (audioIdx < 30) {
        int64_t tsHns = static_cast<int64_t>(audioIdx) * 500000LL;
        auto aRes = enc.writeGameAudio(pcm.data(), kAudioChunkFrames, 2, 48000, tsHns);
        CHECK(aRes.ok);
        audioIdx++;
    }

    while (videoIdx < kTotalVideoFrames || audioIdx < kTotalAudioChunks) {
        int64_t nextVideoHns = static_cast<int64_t>(videoIdx) * (10000000LL / 15);
        int64_t nextAudioHns = static_cast<int64_t>(audioIdx) * 500000LL;

        if (videoIdx < kTotalVideoFrames && (audioIdx >= kTotalAudioChunks || nextVideoHns <= nextAudioHns)) {
            for (int w = 0; w < 200 && enc.isVideoQueueSaturated(16, 256ull * 1024 * 1024); ++w) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            auto vRes = enc.writeVideoFrame(pb, 128, 72, nextVideoHns);
            CHECK(vRes.ok);
            videoIdx++;
        } else if (audioIdx < kTotalAudioChunks) {
            auto aRes = enc.writeGameAudio(pcm.data(), kAudioChunkFrames, 2, 48000, nextAudioHns);
            CHECK(aRes.ok);
            audioIdx++;
        }
    }
    CFRelease(pb);

    CHECK(enc.finish());
    verifyMp4NoOverlapAndDuration(outPath, 1, 118.0, 122.0);

    std::filesystem::remove_all(testDir, ec);
    std::cout << "  PASS Test 7 (T-M2)\n";
}

// T-M8 + M18: 240 FPS with +/-1ms jitter remux preserves naturalTimeScale == 1000000, distinct PTS, and BT.709 color tags.
void test240FpsTimescaleAndBT709() {
    std::cout << "[Test 8 / T-M8] 240 FPS jitter remux timescale (1000000), distinct PTS, BT.709 tags...\n" << std::flush;
    auto testDir = std::filesystem::temp_directory_path() / "InstantReplayMacHostTest_M8";
    std::error_code ec;
    std::filesystem::remove_all(testDir, ec);
    std::filesystem::create_directories(testDir, ec);

    auto write240Seg = [&](const std::filesystem::path& path) {
        replay::VideoToolboxEncoder enc;
        auto res = enc.start(
            path, 128, 72, 240, 8, 12, 80, replay::RateControlMode::ADAPTIVE_VBR,
            false, replay::CodecType::H264, true, "", 48000, nullptr, true);
        CHECK(res.ok);

        CVPixelBufferRef pb = makeTestPixelBuffer(128, 72, 10, 220, 40, 220, 10, 40);
        CVBufferSetAttachment(pb, kCVImageBufferColorPrimariesKey, kCVImageBufferColorPrimaries_ITU_R_709_2, kCVAttachmentMode_ShouldPropagate);
        CVBufferSetAttachment(pb, kCVImageBufferTransferFunctionKey, kCVImageBufferTransferFunction_ITU_R_709_2, kCVAttachmentMode_ShouldPropagate);
        CVBufferSetAttachment(pb, kCVImageBufferYCbCrMatrixKey, kCVImageBufferYCbCrMatrix_ITU_R_709_2, kCVAttachmentMode_ShouldPropagate);

        constexpr int kFrames = 120; // 0.5s @ 240 FPS
        constexpr int64_t kNominalStepHns = 41667LL;
        int64_t prevTs = -1;
        for (int i = 0; i < kFrames; ++i) {
            int64_t jitter = (i == 0) ? 0 : ((i % 2 == 1) ? 10000LL : -10000LL); // +/- 1ms
            int64_t ts = static_cast<int64_t>(i) * kNominalStepHns + jitter;
            if (prevTs >= 0 && ts <= prevTs) ts = prevTs + 15000LL;
            prevTs = ts;
            for (int w = 0; w < 200 && enc.isVideoQueueSaturated(16, 256ull * 1024 * 1024); ++w) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            CHECK(enc.writeVideoFrame(pb, 128, 72, ts).ok);
        }
        CFRelease(pb);

        std::vector<float> a(24000 * 2, 0.01f); // 0.5s stereo
        CHECK(enc.writeGameAudio(a.data(), 24000, 2, 48000, 0).ok);
        CHECK(enc.finish());
    };

    auto seg1 = testDir / "seg240_1.mp4";
    auto seg2 = testDir / "seg240_2.mp4";
    auto merged = testDir / "merged_240.mp4";
    write240Seg(seg1);
    write240Seg(seg2);

    auto concatRes = replay::VideoToolboxEncoder::concatenateSegments({seg1, seg2}, merged, 0.0);
    CHECK(concatRes.success);

    @autoreleasepool {
        std::string u8 = geode::utils::string::pathToString(merged);
        NSURL* url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:u8.c_str()]];
        AVURLAsset* asset = [AVURLAsset URLAssetWithURL:url options:@{ AVURLAssetPreferPreciseDurationAndTimingKey: @YES }];
        CHECK(asset != nil);

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
        NSArray<AVAssetTrack*>* tracks = [asset tracks];
#pragma clang diagnostic pop
        AVAssetTrack* videoTrack = nil;
        for (AVAssetTrack* tr in tracks) {
            if ([tr.mediaType isEqualToString:AVMediaTypeVideo]) {
                videoTrack = tr;
                break;
            }
        }
        CHECK(videoTrack != nil);
        CHECK(videoTrack.naturalTimeScale == 1000000);

        NSArray* descs = videoTrack.formatDescriptions;
        CHECK(descs.count > 0);
        CMFormatDescriptionRef fmtDesc = (__bridge CMFormatDescriptionRef)descs[0];
        CFPropertyListRef primaries = CMFormatDescriptionGetExtension(fmtDesc, kCMFormatDescriptionExtension_ColorPrimaries);
        CHECK(primaries != nullptr);
        CHECK(CFEqual(primaries, kCMFormatDescriptionColorPrimaries_ITU_R_709_2));

        NSError* rErr = nil;
        AVAssetReader* reader = [AVAssetReader assetReaderWithAsset:asset error:&rErr];
        CHECK(reader != nil && rErr == nil);
        AVAssetReaderTrackOutput* out = [AVAssetReaderTrackOutput assetReaderTrackOutputWithTrack:videoTrack outputSettings:nil];
        out.alwaysCopiesSampleData = NO;
        [reader addOutput:out];
        CHECK([reader startReading]);

        std::vector<int64_t> allPtsValues;
        CMTime prevDts = kCMTimeInvalid;
        int frameCount = 0;
        while (CMSampleBufferRef s = [out copyNextSampleBuffer]) {
            if (CMSampleBufferGetNumSamples(s) > 0 && CMSampleBufferGetDataBuffer(s)) {
                CMTime pts = CMSampleBufferGetOutputPresentationTimeStamp(s);
                CMTime dts = CMSampleBufferGetOutputDecodeTimeStamp(s);
                CHECK(CMTIME_IS_VALID(pts));
                if (CMTIME_IS_VALID(dts)) {
                    if (CMTIME_IS_VALID(prevDts)) {
                        CHECK(CMTimeCompare(dts, prevDts) >= 0);
                    }
                    prevDts = dts;
                }
                // Express PTS strictly in the 1 MHz track timescale (no rounding down to 600 ticks).
                CMTime ptsScaled = CMTimeConvertScale(pts, 1000000, kCMTimeRoundingMethod_RoundHalfAwayFromZero);
                allPtsValues.push_back(ptsScaled.value);
                frameCount++;
            }
            CFRelease(s);
        }
        CHECK(frameCount == 240);
        CHECK(reader.status == AVAssetReaderStatusCompleted);

        std::sort(allPtsValues.begin(), allPtsValues.end());
        int64_t minDeltaUs = INT64_MAX;
        for (size_t i = 1; i < allPtsValues.size(); ++i) {
            int64_t delta = allPtsValues[i] - allPtsValues[i - 1];
            CHECK(delta > 0); // Strictly distinct presentation timestamps at 240 FPS + jitter
            minDeltaUs = std::min(minDeltaUs, delta);
        }
        std::cout << "  T-M8 verified: naturalTimeScale=" << videoTrack.naturalTimeScale
                  << ", frames=" << frameCount << ", minPtsDeltaUs=" << minDeltaUs << "\n";
    }

    std::filesystem::remove_all(testDir, ec);
    std::cout << "  PASS Test 8 (T-M8 + M18 BT.709)\n";
}

// T-M10: 10 x 1s segments across 44.1 kHz & 48 kHz -> verify single continuous AAC edit with <=1 sample continuity.
void test10SegmentAacContinuity() {
    std::cout << "[Test 9 / T-M10] 10 x 1s segment remux AAC continuity (44.1 kHz & 48 kHz)...\n" << std::flush;
    for (int sampleRate : {44100, 48000}) {
        auto testDir = std::filesystem::temp_directory_path() / ("InstantReplayMacHostTest_M10_" + std::to_string(sampleRate));
        std::error_code ec;
        std::filesystem::remove_all(testDir, ec);
        std::filesystem::create_directories(testDir, ec);

        std::vector<std::filesystem::path> segs;
        for (int i = 0; i < 10; ++i) {
            auto seg = testDir / ("seg_" + std::to_string(i + 1) + ".mp4");
            writeEncoderSegment(seg, 1.0, 1.0, 0.0, 30, replay::CodecType::H264, true,
                                replay::RateControlMode::ADAPTIVE_VBR, sampleRate);
            segs.push_back(seg);
        }

        auto merged = testDir / "merged_10seg.mp4";
        auto concatRes = replay::VideoToolboxEncoder::concatenateSegments(segs, merged, 0.0);
        CHECK(concatRes.success);
        CHECK(concatRes.durationSeconds >= 9.6 && concatRes.durationSeconds <= 10.5);
        verifyMp4NoOverlapAndDuration(merged, 1, 9.6, 10.5);

        @autoreleasepool {
            std::string u8 = geode::utils::string::pathToString(merged);
            NSURL* url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:u8.c_str()]];
            AVURLAsset* asset = [AVURLAsset URLAssetWithURL:url options:@{ AVURLAssetPreferPreciseDurationAndTimingKey: @YES }];
            CHECK(asset != nil);
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
            NSArray<AVAssetTrack*>* tracks = [asset tracks];
#pragma clang diagnostic pop
            AVAssetTrack* audioTrack = nil;
            for (AVAssetTrack* tr in tracks) {
                if ([tr.mediaType isEqualToString:AVMediaTypeAudio]) {
                    audioTrack = tr;
                    break;
                }
            }
            CHECK(audioTrack != nil);
            CHECK(audioTrack.segments.count == 1);
            CHECK(!audioTrack.segments[0].empty);
            CHECK(audioTrack.segments.count == 1);
            CHECK(!audioTrack.segments[0].empty);

            NSError* rErr = nil;
            AVAssetReader* reader = [AVAssetReader assetReaderWithAsset:asset error:&rErr];
            CHECK(reader != nil && rErr == nil);
            AVAssetReaderTrackOutput* out = [AVAssetReaderTrackOutput assetReaderTrackOutputWithTrack:audioTrack outputSettings:nil];
            out.alwaysCopiesSampleData = NO;
            [reader addOutput:out];
            CHECK([reader startReading]);

            // Tighter than 1 sample: 1 sample @ 44.1 kHz ~= 22.68 us (allow 25 us with integer us rounding).
            const int64_t oneSampleUs = static_cast<int64_t>(std::ceil(1000000.0 / sampleRate)) + 2;
            int64_t prevPtsUs = -1;
            int64_t prevEndUs = -1;
            int64_t totalDurUs = 0;
            int pktCount = 0;
            int sampleCountSum = 0;
            while (CMSampleBufferRef s = [out copyNextSampleBuffer]) {
                CMItemCount numSamples = CMSampleBufferGetNumSamples(s);
                if (numSamples > 0 && CMSampleBufferGetDataBuffer(s)) {
                    int64_t ptsUs = llround(CMTimeGetSeconds(CMSampleBufferGetOutputPresentationTimeStamp(s)) * 1e6);
                    int64_t durUs = llround(CMTimeGetSeconds(CMSampleBufferGetOutputDuration(s)) * 1e6);
                    if (prevPtsUs >= 0) CHECK(ptsUs > prevPtsUs);
                    if (prevEndUs >= 0) {
                        if (ptsUs + oneSampleUs < prevEndUs || ptsUs - prevEndUs > oneSampleUs) {
                            std::cerr << "  AAC discontinuity (" << sampleRate << "Hz) at pkt " << pktCount
                                      << ": ptsUs=" << ptsUs << ", prevEndUs=" << prevEndUs << "\n";
                        }
                        CHECK(ptsUs + oneSampleUs >= prevEndUs); // overlap <= 1 sample
                        CHECK(ptsUs - prevEndUs <= oneSampleUs); // gap <= 1 sample
                    }
                    prevPtsUs = ptsUs;
                    prevEndUs = std::max(prevEndUs, ptsUs + durUs);
                    totalDurUs += durUs;
                    pktCount++;
                    sampleCountSum += static_cast<int>(numSamples);
                }
                CFRelease(s);
            }
            std::cout << "  T-M10 (" << sampleRate << " Hz) AAC stats: buffers=" << pktCount
                      << ", totalSamples=" << sampleCountSum
                      << ", totalDurUs=" << totalDurUs << "\n";
            CHECK(sampleCountSum > 400);
            CHECK(totalDurUs >= 9600000LL);
            CHECK(reader.status == AVAssetReaderStatusCompleted);
        }

        std::filesystem::remove_all(testDir, ec);
    }
    std::cout << "  PASS Test 9 (T-M10)\n";
}

// Helper for T-N1 matching Appendix A: extracts decoded LPCM beep onsets and BGRA white-flash onsets via AVFoundation.
void measureMarkerOnsets(
    NSURL* url,
    int audioTrackIdx,
    std::vector<double>& aOn,
    std::vector<double>& vOn,
    bool& audioStartsWithEmptyEdit,
    NSUInteger& audioSegmentCount
) {
    AVURLAsset* asset = [AVURLAsset URLAssetWithURL:url options:@{AVURLAssetPreferPreciseDurationAndTimingKey: @YES}];
    CHECK(asset != nil);
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    NSArray<AVAssetTrack*>* audioTracks = [asset tracksWithMediaType:AVMediaTypeAudio];
    NSArray<AVAssetTrack*>* videoTracks = [asset tracksWithMediaType:AVMediaTypeVideo];
#pragma clang diagnostic pop
    CHECK(audioTrackIdx < static_cast<int>(audioTracks.count));
    CHECK(videoTracks.count > 0);
    AVAssetTrack* at = audioTracks[audioTrackIdx];
    AVAssetTrack* vt = videoTracks[0];

    audioSegmentCount = at.segments.count;
    audioStartsWithEmptyEdit = at.segments.count > 0 && at.segments[0].empty;
    CHECK(vt.segments.count == 1 && !vt.segments[0].empty);
    CHECK(CMTimeCompare(vt.segments[0].timeMapping.target.start, kCMTimeZero) == 0);

    AVAssetReader* ar = [AVAssetReader assetReaderWithAsset:asset error:nil];
    CHECK(ar != nil);
    AVAssetReaderTrackOutput* ao = [AVAssetReaderTrackOutput assetReaderTrackOutputWithTrack:at outputSettings:@{
        AVFormatIDKey: @(kAudioFormatLinearPCM),
        AVLinearPCMBitDepthKey: @32,
        AVLinearPCMIsFloatKey: @YES,
        AVLinearPCMIsNonInterleaved: @NO,
        AVLinearPCMIsBigEndianKey: @NO,
        AVNumberOfChannelsKey: @1
    }];
    [ar addOutput:ao];
    CHECK([ar startReading]);
    double lastHi = -10.0;
    while (CMSampleBufferRef sb = [ao copyNextSampleBuffer]) {
        CMFormatDescriptionRef fmt = CMSampleBufferGetFormatDescription(sb);
        const AudioStreamBasicDescription* asbd = fmt ? CMAudioFormatDescriptionGetStreamBasicDescription(fmt) : nullptr;
        CMBlockBufferRef bb = CMSampleBufferGetDataBuffer(sb);
        if (asbd && asbd->mSampleRate > 0.0 && bb) {
            double rate = asbd->mSampleRate;
            double t0 = CMTimeGetSeconds(CMSampleBufferGetPresentationTimeStamp(sb));
            std::vector<float> pcm(CMBlockBufferGetDataLength(bb) / sizeof(float));
            CMBlockBufferCopyDataBytes(bb, 0, pcm.size() * sizeof(float), pcm.data());
            for (size_t i = 0; i < pcm.size(); ++i) {
                if (std::fabs(pcm[i]) > 0.2f) {
                    double t = t0 + static_cast<double>(i) / rate;
                    if (t - lastHi > 1.0) aOn.push_back(t);
                    lastHi = t;
                }
            }
        }
        CFRelease(sb);
    }

    AVAssetReader* vr = [AVAssetReader assetReaderWithAsset:asset error:nil];
    CHECK(vr != nil);
    AVAssetReaderTrackOutput* vo = [AVAssetReaderTrackOutput assetReaderTrackOutputWithTrack:vt outputSettings:@{
        (__bridge NSString*)kCVPixelBufferPixelFormatTypeKey: @(kCVPixelFormatType_32BGRA)
    }];
    [vr addOutput:vo];
    CHECK([vr startReading]);
    bool prevHi = false;
    while (CMSampleBufferRef sb = [vo copyNextSampleBuffer]) {
        CVPixelBufferRef pb = CMSampleBufferGetImageBuffer(sb);
        if (pb) {
            CVPixelBufferLockBaseAddress(pb, kCVPixelBufferLock_ReadOnly);
            auto* base = static_cast<uint8_t*>(CVPixelBufferGetBaseAddress(pb));
            bool hi = base[(CVPixelBufferGetHeight(pb) / 2) * CVPixelBufferGetBytesPerRow(pb) + 4 * 10 + 1] > 128;
            CVPixelBufferUnlockBaseAddress(pb, kCVPixelBufferLock_ReadOnly);
            if (hi && !prevHi) {
                vOn.push_back(CMTimeGetSeconds(CMSampleBufferGetPresentationTimeStamp(sb)));
            }
            prevHi = hi;
        }
        CFRelease(sb);
    }
}

void writeMarkerSegment(
    const std::filesystem::path& path,
    int gameSampleRate,
    double markerTimeSec,
    replay::CodecType codec
) {
    constexpr int kFps = 60;
    constexpr double kSegDur = 5.0;
    constexpr int kNumFrames = 300;
    const int64_t frameDurHns = 10000000LL / kFps;
    const int markerFrameStart = static_cast<int>(std::llround(markerTimeSec * kFps));
    const int gameSamplesTotal = static_cast<int>(std::llround(kSegDur * gameSampleRate));
    constexpr int micSamplesTotal = 240000; // 5.0s @ 48 kHz

    replay::VideoToolboxEncoder enc;
    auto startRes = enc.start(
        path, 128, 72, kFps, 8, 12, 80, replay::RateControlMode::ADAPTIVE_VBR,
        true, codec, true, "", gameSampleRate, nullptr, true);
    CHECK(startRes.ok);

    CVPixelBufferRef darkPb = makeTestPixelBuffer(128, 72, 10, 10, 10, 10, 10, 10);
    CVPixelBufferRef brightPb = makeTestPixelBuffer(128, 72, 240, 240, 240, 240, 240, 240);

    const int gameChunk = std::max(1, gameSampleRate / 20);
    constexpr int micChunk = 2400;
    std::vector<float> gamePcm(static_cast<size_t>(gameChunk) * 2, 0.0f);
    std::vector<float> micPcm(static_cast<size_t>(micChunk) * 2, 0.0f);

    int videoIdx = 0;
    int gameWritten = 0;
    int micWritten = 0;

    while (videoIdx < kNumFrames || gameWritten < gameSamplesTotal || micWritten < micSamplesTotal) {
        const int64_t nextV = (videoIdx < kNumFrames) ? (static_cast<int64_t>(videoIdx) * frameDurHns) : INT64_MAX;
        const int64_t nextG = (gameWritten < gameSamplesTotal) ? ((static_cast<int64_t>(gameWritten) * 10000000LL) / gameSampleRate) : INT64_MAX;
        const int64_t nextM = (micWritten < micSamplesTotal) ? ((static_cast<int64_t>(micWritten) * 10000000LL) / 48000LL) : INT64_MAX;

        if (nextV <= nextG && nextV <= nextM && videoIdx < kNumFrames) {
            for (int w = 0; w < 200 && enc.isVideoQueueSaturated(16, 256ull * 1024 * 1024); ++w) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            CVPixelBufferRef pb = (videoIdx >= markerFrameStart && videoIdx < markerFrameStart + 3) ? brightPb : darkPb;
            CHECK(enc.writeVideoFrame(pb, 128, 72, nextV).ok);
            videoIdx++;
        } else if (nextG <= nextM && gameWritten < gameSamplesTotal) {
            int n = std::min(gameChunk, gameSamplesTotal - gameWritten);
            int mStart = static_cast<int>(std::llround(markerTimeSec * gameSampleRate));
            int mEnd = mStart + (gameSampleRate / 20); // 50ms beep
            for (int s = 0; s < n; ++s) {
                int idx = gameWritten + s;
                float v = (idx >= mStart && idx < mEnd)
                    ? (0.5f * std::sinf(2.0f * static_cast<float>(M_PI) * 1000.0f * static_cast<float>(idx) / static_cast<float>(gameSampleRate)))
                    : 0.0f;
                gamePcm[s * 2 + 0] = v;
                gamePcm[s * 2 + 1] = v;
            }
            CHECK(enc.writeGameAudio(gamePcm.data(), n, 2, gameSampleRate, nextG).ok);
            gameWritten += n;
        } else if (micWritten < micSamplesTotal) {
            int n = std::min(micChunk, micSamplesTotal - micWritten);
            int mStart = static_cast<int>(std::llround(markerTimeSec * 48000.0));
            int mEnd = mStart + 2400; // 50ms beep
            for (int s = 0; s < n; ++s) {
                int idx = micWritten + s;
                float v = (idx >= mStart && idx < mEnd)
                    ? (0.5f * std::sinf(2.0f * static_cast<float>(M_PI) * 1000.0f * static_cast<float>(idx) / 48000.0f))
                    : 0.0f;
                micPcm[s * 2 + 0] = v;
                micPcm[s * 2 + 1] = v;
            }
            CHECK(enc.writeMicAudio(micPcm.data(), n, 2, 48000, nextM).ok);
            micWritten += n;
        }
    }

    CFRelease(darkPb);
    CFRelease(brightPb);
    CHECK(enc.finish());
}

// T-N1: 4 x 5s segments with flashed visual + beep markers at 2, 7, 12, 17s for 44.1 kHz H.264 & 48 kHz HEVC.
void testAudioVideoMarkersContinuousSync() {
    std::cout << "[Test 10 / T-N1] 4 x 5s A/V marker sync (|A-V| <= 1ms, no EMPTY edit, 1 A/V segment)...\n" << std::flush;
    struct Case { int rate; replay::CodecType codec; const char* name; };
    for (const auto& c : {
             Case{44100, replay::CodecType::H264, "H264@44.1kHz"},
             Case{48000, replay::CodecType::HEVC, "HEVC@48kHz"},
         }) {
        auto testDir = std::filesystem::temp_directory_path() / ("InstantReplayMacHostTest_N1_" + std::string(c.name));
        std::error_code ec;
        std::filesystem::remove_all(testDir, ec);
        std::filesystem::create_directories(testDir, ec);

        std::vector<std::filesystem::path> segs;
        for (int i = 0; i < 4; ++i) {
            auto seg = testDir / ("seg_" + std::to_string(i + 1) + ".mp4");
            writeMarkerSegment(seg, c.rate, 2.0, c.codec);
            segs.push_back(seg);
        }

        auto merged = testDir / "merged_markers.mp4";
        auto concatRes = replay::VideoToolboxEncoder::concatenateSegments(segs, merged, 0.0);
        CHECK(concatRes.success);

        @autoreleasepool {
            std::string u8 = geode::utils::string::pathToString(merged);
            NSURL* url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:u8.c_str()]];
            for (int audioIdx : {0, 1}) {
                std::vector<double> aOn, vOn;
                bool emptyEdit = true;
                NSUInteger segCount = 0;
                measureMarkerOnsets(url, audioIdx, aOn, vOn, emptyEdit, segCount);
                CHECK(!emptyEdit);
                CHECK(segCount == 1);
                CHECK(aOn.size() == 4 && vOn.size() == 4);
                double maxAbsErrMs = 0.0;
                for (size_t i = 0; i < 4; ++i) {
                    double diffMs = (aOn[i] - vOn[i]) * 1000.0;
                    maxAbsErrMs = std::max(maxAbsErrMs, std::fabs(diffMs));
                    if (std::fabs(diffMs) > 1.0) {
                        std::cerr << "  FAIL T-N1 (" << c.name << " track " << audioIdx << ") mark " << i
                                  << ": A=" << aOn[i] << " V=" << vOn[i] << " diff=" << diffMs << "ms\n";
                    }
                    CHECK(std::fabs(diffMs) <= 1.0);
                }
                std::cout << "  T-N1 (" << c.name << " track " << audioIdx << "): max|A-V|="
                          << maxAbsErrMs << "ms, segments=" << segCount << "\n";
            }
        }
        std::filesystem::remove_all(testDir, ec);
    }
    std::cout << "  PASS Test 10 (T-N1)\n";
}

// T-N1b: Continuous 1 kHz sine @ 44.1 kHz across 4 segments (3 seams) -> zero clicks/holes on AVAssetReader LPCM decode.
void testContinuousSineWaveSeamContinuity() {
    std::cout << "[Test 11 / T-N1b] Continuous 1 kHz sine @ 44.1 kHz across 3 seams (no clicks/holes)...\n" << std::flush;
    auto testDir = std::filesystem::temp_directory_path() / "InstantReplayMacHostTest_N1b";
    std::error_code ec;
    std::filesystem::remove_all(testDir, ec);
    std::filesystem::create_directories(testDir, ec);

    constexpr int kRate = 44100;
    constexpr int kFps = 60;
    constexpr int kFramesPerSeg = 300;
    constexpr int64_t frameDurHns = 10000000LL / kFps;
    constexpr int kSamplesPerSeg = kRate * 5;
    constexpr int kChunk = 2205; // 50ms @ 44.1 kHz
    constexpr int kNumSegs = 4;

    int64_t globalAudioSample = 0;
    std::vector<std::filesystem::path> segs;
    CVPixelBufferRef pb = makeTestPixelBuffer(128, 72, 40, 120, 200, 40, 120, 200);
    std::vector<float> pcm(kChunk * 2);

    for (int segIdx = 0; segIdx < kNumSegs; ++segIdx) {
        auto seg = testDir / ("seg_" + std::to_string(segIdx + 1) + ".mp4");
        replay::VideoToolboxEncoder enc;
        CHECK(enc.start(seg, 128, 72, kFps, 8, 12, 80, replay::RateControlMode::ADAPTIVE_VBR,
                        false, replay::CodecType::H264, true, "", kRate, nullptr, true).ok);

        int vIdx = 0;
        int aWritten = 0;
        while (vIdx < kFramesPerSeg || aWritten < kSamplesPerSeg) {
            int64_t nextV = (vIdx < kFramesPerSeg) ? (static_cast<int64_t>(vIdx) * frameDurHns) : INT64_MAX;
            int64_t nextA = (aWritten < kSamplesPerSeg) ? ((static_cast<int64_t>(aWritten) * 10000000LL) / kRate) : INT64_MAX;
            if (nextV <= nextA && vIdx < kFramesPerSeg) {
                for (int w = 0; w < 200 && enc.isVideoQueueSaturated(16, 256ull << 20); ++w) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
                CHECK(enc.writeVideoFrame(pb, 128, 72, nextV).ok);
                vIdx++;
            } else if (aWritten < kSamplesPerSeg) {
                int n = std::min(kChunk, kSamplesPerSeg - aWritten);
                for (int s = 0; s < n; ++s) {
                    float v = 0.25f * std::sinf(2.0f * static_cast<float>(M_PI) * 1000.0f *
                                                static_cast<float>(globalAudioSample + s) / static_cast<float>(kRate));
                    pcm[s * 2 + 0] = v;
                    pcm[s * 2 + 1] = v;
                }
                CHECK(enc.writeGameAudio(pcm.data(), n, 2, kRate, nextA).ok);
                aWritten += n;
                globalAudioSample += n;
            }
        }
        CHECK(enc.finish());
        segs.push_back(seg);
    }
    CFRelease(pb);

    auto merged = testDir / "merged_sine.mp4";
    auto concatRes = replay::VideoToolboxEncoder::concatenateSegments(segs, merged, 0.0);
    CHECK(concatRes.success);

    @autoreleasepool {
        std::string u8 = geode::utils::string::pathToString(merged);
        NSURL* url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:u8.c_str()]];
        AVURLAsset* asset = [AVURLAsset URLAssetWithURL:url options:@{AVURLAssetPreferPreciseDurationAndTimingKey: @YES}];
        CHECK(asset != nil);
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
        AVAssetTrack* at = [asset tracksWithMediaType:AVMediaTypeAudio][0];
#pragma clang diagnostic pop
        CHECK(at.segments.count == 1 && !at.segments[0].empty);

        AVAssetReader* ar = [AVAssetReader assetReaderWithAsset:asset error:nil];
        AVAssetReaderTrackOutput* ao = [AVAssetReaderTrackOutput assetReaderTrackOutputWithTrack:at outputSettings:@{
            AVFormatIDKey: @(kAudioFormatLinearPCM),
            AVLinearPCMBitDepthKey: @32,
            AVLinearPCMIsFloatKey: @YES,
            AVLinearPCMIsNonInterleaved: @NO,
            AVLinearPCMIsBigEndianKey: @NO,
            AVNumberOfChannelsKey: @1
        }];
        [ar addOutput:ao];
        CHECK([ar startReading]);

        std::vector<float> decoded;
        decoded.reserve(static_cast<size_t>(globalAudioSample) + 2048);
        while (CMSampleBufferRef sb = [ao copyNextSampleBuffer]) {
            if (CMBlockBufferRef bb = CMSampleBufferGetDataBuffer(sb)) {
                size_t bytes = CMBlockBufferGetDataLength(bb);
                size_t oldSz = decoded.size();
                decoded.resize(oldSz + bytes / sizeof(float));
                CMBlockBufferCopyDataBytes(bb, 0, bytes, decoded.data() + oldSz);
            }
            CFRelease(sb);
        }
        CHECK(ar.status == AVAssetReaderStatusCompleted);

        const int64_t expectedTotal = globalAudioSample;
        const int64_t countDiff = std::llabs(static_cast<int64_t>(decoded.size()) - expectedTotal);
        CHECK(countDiff <= 1024);

        const double omega = 2.0 * M_PI * 1000.0 / static_cast<double>(kRate);
        const double theoreticalMaxD2 = 0.25 * 4.0 * std::sin(omega / 2.0) * std::sin(omega / 2.0);
        double maxAbsD2 = 0.0;
        size_t maxIdx = 0;
        // Skip first 64 startup samples (AAC priming edge) and inspect entire 20s interior including all 3 joins.
        for (size_t i = 64; i + 64 < decoded.size(); ++i) {
            double d2 = std::fabs(static_cast<double>(decoded[i]) - 2.0 * decoded[i - 1] + decoded[i - 2]);
            if (d2 > maxAbsD2) {
                maxAbsD2 = d2;
                maxIdx = i;
            }
        }
        std::cout << "  T-N1b decodedSamples=" << decoded.size() << " (expected=" << expectedTotal
                  << "), max|D2|=" << maxAbsD2 << " at idx=" << maxIdx
                  << " (4x theoretical=" << (4.0 * theoreticalMaxD2) << ")\n";
        CHECK(maxAbsD2 <= 4.0 * theoreticalMaxD2);
    }

    std::filesystem::remove_all(testDir, ec);
    std::cout << "  PASS Test 11 (T-N1b)\n";
}

// T-N3: isPresetCodecSupported(HEVC, false) must match actual AVAssetWriter start() without fallback.
void testHevcSoftwareWritableConsistency() {
    std::cout << "[Test 12 / T-N3] HEVC SW writable capability matches AVAssetWriter start()...\n";
    replay::VideoToolboxEncoder::resetCodecCache();
    const auto& caps = replay::getMacVideoEncoderCaps();
    const bool presetSupported = replay::ReplayConfig::isPresetCodecSupported(replay::CodecType::HEVC, false);
    CHECK(presetSupported == (caps.hevcSoftware && caps.hevcSoftwareWritable));

    auto testDir = std::filesystem::temp_directory_path() / "InstantReplayMacHostTest_N3";
    std::error_code ec;
    std::filesystem::remove_all(testDir, ec);
    std::filesystem::create_directories(testDir, ec);

    {
        replay::VideoToolboxEncoder enc;
        auto seg = testDir / "hevc_sw_strict.mp4";
        auto res = enc.start(
            seg, 320, 180, 60, 8, 12, 80, replay::RateControlMode::ADAPTIVE_VBR,
            false, replay::CodecType::HEVC, false, "", 48000, nullptr, false);
        enc.finish();
        CHECK(res.ok == presetSupported);
    }

    if (!presetSupported) {
        replay::ReplayConfig cfg;
        cfg.codec = replay::CodecType::HEVC;
        cfg.hardwareAcceleration = false;
        CHECK(cfg.sanitizeCodecSettings());
        CHECK(cfg.codec.load() == replay::CodecType::H264);

        replay::VideoToolboxEncoder encFb;
        auto segFb = testDir / "hevc_sw_fb.mp4";
        auto resFb = encFb.start(
            segFb, 320, 180, 60, 8, 12, 80, replay::RateControlMode::ADAPTIVE_VBR,
            false, replay::CodecType::HEVC, false, "", 48000, nullptr, true);
        CHECK(resFb.ok);
        CHECK(encFb.activeCodecFamily() == replay::VideoCodecFamily::H264);
        CHECK(!encFb.activeHardware());
        encFb.finish();
    }

    std::filesystem::remove_all(testDir, ec);
    std::cout << "  PASS Test 12 (T-N3: hevcSW_vt=" << caps.hevcSoftware
              << ", hevcSW_writable=" << caps.hevcSoftwareWritable
              << ", presetSupported=" << presetSupported << ")\n";
}

// T-N4: 10,000 consecutive 1024-sample AAC packets at 44.1 kHz & 48 kHz produce 0 false overlap warnings.
void testAacRoundingOverlapTolerance() {
    std::cout << "[Test 13 / T-N4] ConcatTimestampState 10,000 AAC packet rounding tolerance...\n";
    for (int rate : {44100, 48000}) {
        replay::ConcatTimestampState state(0);
        state.beginSegment();
        const int64_t tol = replay::ConcatTimestampState::audioOverlapToleranceHns(rate);
        const int64_t fixedDur = static_cast<int64_t>(std::llround(1024.0 * 10'000'000.0 / rate));
        int overlaps = 0;
        for (int pkt = 0; pkt < 10000; ++pkt) {
            const int64_t ptsHns = static_cast<int64_t>(
                std::llround(static_cast<double>(pkt) * 1024.0 * 10'000'000.0 / rate));
            if (!state.recordWrittenSample(1, false, ptsHns, fixedDur, nullptr, tol)) {
                ++overlaps;
            }
        }
        CHECK(overlaps == 0);
        // Real > 1 sample overlap still triggers warning cleanly.
        const int64_t nextEnd = state.lastAudioEndByWriterStream[1];
        CHECK(!state.recordWrittenSample(1, false, nextEnd - tol - 100, fixedDur, nullptr, tol));
    }
    std::cout << "  PASS Test 13 (T-N4)\n";
}

// Measures A/V marker offsets of `merged` on every audio track; returns the worst |A-V| in ms.
double maxMarkerOffsetMs(const std::filesystem::path& merged, size_t expectedMarkers, const char* label) {
    double worst = 0.0;
    @autoreleasepool {
        std::string u8 = geode::utils::string::pathToString(merged);
        NSURL* url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:u8.c_str()]];
        for (int audioIdx : {0, 1}) {
            std::vector<double> aOn, vOn;
            bool emptyEdit = true;
            NSUInteger segCount = 0;
            measureMarkerOnsets(url, audioIdx, aOn, vOn, emptyEdit, segCount);
            CHECK(!emptyEdit);
            CHECK(segCount == 1);
            if (aOn.size() != expectedMarkers || vOn.size() != expectedMarkers) {
                std::cerr << "  FAIL " << label << " track " << audioIdx << ": markers A=" << aOn.size()
                          << " V=" << vOn.size() << " expected=" << expectedMarkers << "\n";
            }
            CHECK(aOn.size() == expectedMarkers && vOn.size() == expectedMarkers);
            for (size_t i = 0; i < expectedMarkers; ++i) {
                worst = std::max(worst, std::fabs(aOn[i] - vOn[i]) * 1000.0);
            }
        }
    }
    return worst;
}

// T-trim (K7): the export trims the oldest segment at a keyframe near trimStartSeconds; audio is cut at
// the same instant, so markers stay in sync after the trim on both 44.1 kHz H.264 and 48 kHz HEVC.
void testTrimmedExportSync() {
    std::cout << "[Test 14 / T-trim] Trimmed export keeps A/V markers in sync (|A-V| <= 1ms)...\n" << std::flush;
    struct Case { int rate; replay::CodecType codec; const char* name; };
    for (const auto& c : {
             Case{44100, replay::CodecType::H264, "H264@44.1kHz"},
             Case{48000, replay::CodecType::HEVC, "HEVC@48kHz"},
         }) {
        auto testDir = std::filesystem::temp_directory_path() / ("InstantReplayMacHostTest_Trim_" + std::string(c.name));
        std::error_code ec;
        std::filesystem::remove_all(testDir, ec);
        std::filesystem::create_directories(testDir, ec);

        std::vector<std::filesystem::path> segs;
        for (int i = 0; i < 4; ++i) {
            auto seg = testDir / ("seg_" + std::to_string(i + 1) + ".mp4");
            writeMarkerSegment(seg, c.rate, 2.0, c.codec); // markers at 2, 7, 12, 17 s of the source timeline
            segs.push_back(seg);
        }
        for (double trim : {1.5, 3.3, 4.7}) {
            auto merged = testDir / ("merged_trim_" + std::to_string(static_cast<int>(trim * 10)) + ".mp4");
            auto res = replay::VideoToolboxEncoder::concatenateSegments(segs, merged, trim);
            CHECK(res.success);
            // The cut lands on a keyframe at or before the requested trim (<= 1 s GOP), so it never drops
            // more than asked and keeps at most one extra second.
            CHECK(res.durationSeconds >= 20.0 - trim - 0.02);
            CHECK(res.durationSeconds <= 20.0 - trim + 1.05);
            const double cutSec = 20.0 - res.durationSeconds;
            const size_t expected = (cutSec <= 2.0) ? 4 : 3; // a cut past 2 s removes the first marker
            const double worst = maxMarkerOffsetMs(merged, expected, c.name);
            std::cout << "  T-trim (" << c.name << ", trim " << trim << " s): dur=" << res.durationSeconds
                      << "s max|A-V|=" << worst << "ms\n";
            CHECK(worst <= 1.0);
        }
        std::filesystem::remove_all(testDir, ec);
    }
    std::cout << "  PASS Test 14 (T-trim)\n";
}

// T-K2: the export writes "<name>.mp4.part" (AVAssetWriter with an explicit MPEG-4 file type accepts the
// extension) and the renamed file is a complete, playable MP4; a failed export leaves neither file.
void testPartialExportPathK2() {
    std::cout << "[Test 16 / T-K2] Export into .mp4.part, rename to .mp4, failure leaves no file...\n" << std::flush;
    auto testDir = std::filesystem::temp_directory_path() / "InstantReplayMacHostTest_K2";
    std::error_code ec;
    std::filesystem::remove_all(testDir, ec);
    std::filesystem::create_directories(testDir, ec);

    std::vector<std::filesystem::path> segs;
    for (int i = 0; i < 2; ++i) {
        auto seg = testDir / ("seg_" + std::to_string(i + 1) + ".mp4");
        writeEncoderSegment(seg, 1.0, 1.0, 1.0, 30, replay::CodecType::H264, true);
        segs.push_back(seg);
    }

    auto finalPath = testDir / "replay_k2.mp4";
    auto partPath = testDir / "replay_k2.mp4.part";
    auto res = replay::VideoToolboxEncoder::concatenateSegments(segs, partPath, 0.0);
    CHECK(res.success);
    CHECK(std::filesystem::exists(partPath));
    CHECK(!std::filesystem::exists(finalPath));
    std::filesystem::rename(partPath, finalPath, ec);
    CHECK(!ec);
    CHECK(!std::filesystem::exists(partPath));
    @autoreleasepool {
        std::string u8 = geode::utils::string::pathToString(finalPath);
        AVURLAsset* asset = [AVURLAsset URLAssetWithURL:[NSURL fileURLWithPath:[NSString stringWithUTF8String:u8.c_str()]]
                                                options:@{AVURLAssetPreferPreciseDurationAndTimingKey: @YES}];
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
        CHECK(asset.playable);
        CHECK([asset tracksWithMediaType:AVMediaTypeVideo].count == 1);
        CHECK([asset tracksWithMediaType:AVMediaTypeAudio].count == 2);
        CHECK(std::fabs(CMTimeGetSeconds(asset.duration) - 2.0) < 0.1);
#pragma clang diagnostic pop
    }

    // A failed export (unreadable segment) removes its partial output.
    auto badSeg = testDir / "missing_seg.mp4";
    auto badPart = testDir / "replay_bad.mp4.part";
    auto bad = replay::VideoToolboxEncoder::concatenateSegments({segs[0], badSeg}, badPart, 0.0);
    CHECK(!bad.success);
    CHECK(!std::filesystem::exists(badPart));

    std::filesystem::remove_all(testDir, ec);
    std::cout << "  PASS Test 16 (T-K2)\n";
}

// Writes one ~30 s segment the way the recorder does in game: jittered 60 FPS video with dropped
// frames, irregular FMOD-like game chunks and CoreAudio-like mic chunks, audio padded to the boundary.
// The marker flash and both beeps start at the same presentation time (the flashed frame's PTS).
void writeProductionLikeSegment(
    const std::filesystem::path& path, int gameRate, double segDurSec, double markerAtSec, unsigned seed
) {
    constexpr int kW = 128, kH = 72, kFps = 60;
    replay::VideoToolboxEncoder enc;
    auto startRes = enc.start(path, kW, kH, kFps, 8, 12, 80, replay::RateControlMode::ADAPTIVE_VBR,
                              true, replay::CodecType::HEVC, true, "", gameRate, nullptr, true);
    CHECK(startRes.ok);

    std::vector<int64_t> frameHns;
    unsigned rng = seed * 2654435761u + 12345u;
    for (int i = 0;; ++i) {
        rng = rng * 1664525u + 1013904223u;
        const double jitterMs = (static_cast<double>(rng >> 8) / static_cast<double>(1u << 24) - 0.5) * 4.0;
        const double t = (static_cast<double>(i) / kFps) + jitterMs / 1000.0;
        if (t >= segDurSec - 0.0001) break;
        if (i % 97 == 50) continue; // dropped frame
        const int64_t hns = std::max<int64_t>(0, std::llround(t * 1e7));
        if (!frameHns.empty() && hns <= frameHns.back()) continue;
        frameHns.push_back(hns);
    }
    size_t markerFrame = 0;
    while (markerFrame < frameHns.size() && frameHns[markerFrame] < std::llround(markerAtSec * 1e7)) ++markerFrame;
    CHECK(markerFrame < frameHns.size());
    const double beepAtSec = static_cast<double>(frameHns[markerFrame]) / 1e7;

    const int64_t gameTotal = std::llround(segDurSec * gameRate);
    const int64_t micTotal = std::llround(segDurSec * 48000.0);
    const int gameChunks[] = {1024, 512, 2048, 1024, 1536, 1024};
    const int micChunks[] = {960, 960, 480, 1440, 960};

    CVPixelBufferRef dark = makeTestPixelBuffer(kW, kH, 10, 10, 10, 10, 10, 10);
    CVPixelBufferRef bright = makeTestPixelBuffer(kW, kH, 240, 240, 240, 240, 240, 240);
    std::vector<float> buf;
    size_t vi = 0;
    int64_t g = 0, m = 0;
    int gc = 0, mc = 0;
    auto beep = [&](int64_t idx, int rate) -> float {
        const int64_t s = std::llround(beepAtSec * rate);
        if (idx < s || idx >= s + rate / 20) return 0.0f;
        return 0.5f * std::sinf(2.0f * static_cast<float>(M_PI) * 1000.0f * static_cast<float>(idx) / static_cast<float>(rate));
    };
    while (vi < frameHns.size() || g < gameTotal || m < micTotal) {
        const int64_t nv = vi < frameHns.size() ? frameHns[vi] : INT64_MAX;
        const int64_t ng = g < gameTotal ? (g * 10000000LL) / gameRate : INT64_MAX;
        const int64_t nm = m < micTotal ? (m * 10000000LL) / 48000 : INT64_MAX;
        if (nv <= ng && nv <= nm) {
            for (int w = 0; w < 200 && enc.isVideoQueueSaturated(16, 256ull << 20); ++w) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            const bool hi = (vi >= markerFrame && vi < markerFrame + 3);
            CHECK(enc.writeVideoFrame(hi ? bright : dark, kW, kH, nv).ok);
            ++vi;
        } else if (ng <= nm) {
            const int n = static_cast<int>(std::min<int64_t>(gameChunks[gc++ % 6], gameTotal - g));
            buf.assign(static_cast<size_t>(n) * 2, 0.0f);
            for (int s = 0; s < n; ++s) buf[s * 2] = buf[s * 2 + 1] = beep(g + s, gameRate);
            CHECK(enc.writeGameAudio(buf.data(), n, 2, gameRate, ng).ok);
            g += n;
        } else {
            const int n = static_cast<int>(std::min<int64_t>(micChunks[mc++ % 5], micTotal - m));
            buf.assign(static_cast<size_t>(n) * 2, 0.0f);
            for (int s = 0; s < n; ++s) buf[s * 2] = buf[s * 2 + 1] = beep(m + s, 48000);
            CHECK(enc.writeMicAudio(buf.data(), n, 2, 48000, nm).ok);
            m += n;
        }
    }
    CFRelease(dark);
    CFRelease(bright);
    CHECK(enc.finish());
}

// T-long (K7, opt-in with --long, ~70 s on M1): a 10-minute replay (20 x ~30 s segments like
// duration=600) keeps every marker within 1 ms; nothing drifts across 19 seams.
void testLongProductionLikeExport() {
    std::cout << "[Test 15 / T-long] 10-minute production-like export (20 x ~30 s, jitter, drops)...\n" << std::flush;
    auto testDir = std::filesystem::temp_directory_path() / "InstantReplayMacHostTest_Long";
    std::error_code ec;
    std::filesystem::remove_all(testDir, ec);
    std::filesystem::create_directories(testDir, ec);

    std::vector<std::filesystem::path> segs;
    for (int i = 0; i < 20; ++i) {
        const double segDur = 30.0 + ((i % 3) - 1) * 0.0137; // 29.986 / 30.000 / 30.014 s like real rotations
        auto seg = testDir / ("seg_" + std::to_string(i + 1) + ".mp4");
        writeProductionLikeSegment(seg, 44100, segDur, 2.0 + 0.37 * (i % 5), static_cast<unsigned>(i + 1));
        segs.push_back(seg);
    }
    for (double trim : {0.0, 12.4}) {
        auto merged = testDir / (trim > 0.0 ? "merged_long_trim.mp4" : "merged_long.mp4");
        const auto t0 = std::chrono::steady_clock::now();
        auto res = replay::VideoToolboxEncoder::concatenateSegments(segs, merged, trim);
        const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        CHECK(res.success);
        const double worst = maxMarkerOffsetMs(merged, trim > 2.0 ? 19 : 20, "T-long");
        std::cout << "  T-long (trim " << trim << " s): dur=" << res.durationSeconds << "s merge=" << secs
                  << "s max|A-V|=" << worst << "ms\n";
        CHECK(worst <= 1.0);
    }
    std::filesystem::remove_all(testDir, ec);
    std::cout << "  PASS Test 15 (T-long)\n";
}

} // namespace

int main(int argc, char** argv) {
    bool runLong = false;
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--long") runLong = true;
    }
    @autoreleasepool {
        replay_host_tests::runSharedPureTests();
        testVideoToolboxAndRemux();
        testOpenGLInvertedBlitAndPboToEncoder();
        testCoreAudioHardwareEnumeration();
        testSharedMathAndBuffers();
        testHardwareCapabilityChecks();
        testConcatShortAudioNoDeadlock();
        testLongAudioBurstNoBackpressureError();
        test240FpsTimescaleAndBT709();
        test10SegmentAacContinuity();
        testAudioVideoMarkersContinuousSync();
        testContinuousSineWaveSeamContinuity();
        testHevcSoftwareWritableConsistency();
        testAacRoundingOverlapTolerance();
        testTrimmedExportSync();
        testPartialExportPathK2();
        if (runLong) {
            testLongProductionLikeExport();
        } else {
            std::cout << "SKIP Test 15 (T-long): pass --long to run the 10-minute export check\n";
        }
    }
    std::cout << "ALL MACOS HOST TESTS PASSED!\n";
    return 0;
}
