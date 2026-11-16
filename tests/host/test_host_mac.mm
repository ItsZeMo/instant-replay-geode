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
    replay::RateControlMode rc = replay::RateControlMode::ADAPTIVE_VBR
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
        48000,
        nullptr,
        true
    );
    CHECK(res.ok);

    const int numFrames = std::max(1, static_cast<int>(std::llround(videoDurationSec * fps)));
    const int64_t frameDurHns = 10000000LL / fps;
    CVPixelBufferRef pb = makeTestPixelBuffer(128, 72, 220, 40, 20, 20, 40, 220);

    for (int i = 0; i < numFrames; ++i) {
        for (int w = 0; w < 200 && enc.isVideoQueueSaturated(16, 256ull * 1024 * 1024); ++w) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        auto wRes = enc.writeVideoFrame(pb, 128, 72, static_cast<int64_t>(i) * frameDurHns);
        CHECK(wRes.ok);
    }
    CFRelease(pb);

    auto writePcm = [&](bool isGame, double durSec) {
        constexpr int chunk = 480;
        const int totalSamples = static_cast<int>(std::llround(durSec * 48000.0));
        std::vector<float> pcm(chunk * 2, 0.0f);
        int written = 0;
        while (written < totalSamples) {
            int n = std::min(chunk, totalSamples - written);
            int64_t tsHns = (static_cast<int64_t>(written) * 10000000LL) / 48000LL;
            for (int s = 0; s < n; ++s) {
                float v = 0.05f * std::sinf(0.02f * static_cast<float>(written + s));
                pcm[s * 2 + 0] = v;
                pcm[s * 2 + 1] = v;
            }
            auto aRes = isGame
                ? enc.writeGameAudio(pcm.data(), n, 2, 48000, tsHns)
                : enc.writeMicAudio(pcm.data(), n, 2, 48000, tsHns);
            CHECK(aRes.ok);
            written += n;
        }
    };

    writePcm(true, gameAudioDurationSec);
    if (splitAudio) {
        writePcm(false, micAudioDurationSec);
    }

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

} // namespace

int main() {
    @autoreleasepool {
        testVideoToolboxAndRemux();
        testOpenGLInvertedBlitAndPboToEncoder();
        testCoreAudioHardwareEnumeration();
        testSharedMathAndBuffers();
        testHardwareCapabilityChecks();
    }
    std::cout << "ALL MACOS HOST TESTS PASSED!\n";
    return 0;
}
