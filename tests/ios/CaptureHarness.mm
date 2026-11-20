// iOS Simulator integration test for the iOS capture + encode path (no Geometry Dash needed).
//
// A cocos2d-x shaped view (class EAGLView with +sharedEGLView, -context, -surfaceSize, an OpenGL ES 2
// context and a CAEAGLLayer renderbuffer) renders a known pattern every frame and presents it with
// -[EAGLContext presentRenderbuffer:]. Capture runs through the production code: the
// installIOSGLCaptureHook swizzle, FrameGrabberIOS (CVOpenGLESTextureCache render + GPU fences) and
// VideoToolboxEncoder (AVAssetWriter + AAC), and two segments are merged with concatenateSegments.
// The MP4s are then decoded to check orientation, colours, letterboxing, motion, duration and that
// every GL state the capture pass touches is restored before the game presents.
//
// Build and run: tests/ios/run_capture_harness.sh

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <unistd.h>

#import <AVFoundation/AVFoundation.h>
#import <CoreVideo/CoreVideo.h>
#import <OpenGLES/EAGL.h>
#import <OpenGLES/EAGLDrawable.h>
#import <OpenGLES/ES2/gl.h>
#import <OpenGLES/ES2/glext.h>
#import <QuartzCore/QuartzCore.h>
#import <UIKit/UIKit.h>
#import <VideoToolbox/VideoToolbox.h>

#include "capture/AspectFit.hpp"
#include "capture/FrameGrabber.hpp"
#include "encoder/VideoToolboxEncoder.hpp"
#include "platform/IOSPlatformUtils.hpp"
#include "recorder/ReplayManager.hpp"

using namespace replay;

// FrameGrabberIOS only reads the session config and the "active" flag from ReplayManager.
namespace replay {
ReplayManager::ReplayManager() { m_running.store(true); }
ReplayManager::~ReplayManager() = default;
ReplayManager* ReplayManager::get() {
    static ReplayManager instance;
    return &instance;
}
} // namespace replay

namespace {

std::atomic<int> g_failures{0};

void logLine(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);
    printf("\n");
    fflush(stdout);
}

#define CHECK(cond, ...)                         \
    do {                                         \
        if (!(cond)) {                           \
            g_failures.fetch_add(1);             \
            printf("  FAIL: ");                  \
            printf(__VA_ARGS__);                 \
            printf("\n");                        \
            fflush(stdout);                      \
        }                                        \
    } while (0)

// ---------------------------------------------------------------------------------------------
// Encoder sink: FrameGrabber callback (main thread) -> worker thread -> VideoToolboxEncoder.
// ---------------------------------------------------------------------------------------------
struct EncoderSink {
    std::mutex mutex;
    std::condition_variable cv;
    std::deque<std::shared_ptr<CapturedFrame>> queue;
    std::thread worker;
    bool quit = false;

    // Segment state (worker thread)
    std::unique_ptr<VideoToolboxEncoder> encoder;
    std::filesystem::path path;
    CodecType codec = CodecType::HEVC;
    std::chrono::steady_clock::time_point segmentStart{};
    int64_t lastTs = -1;
    int64_t audioSamples = 0;
    bool segmentOpen = false;
    bool segmentRequested = false;
    bool finishRequested = false;
    bool finishResult = false;
    std::atomic<int> framesWritten{0};
    std::atomic<int> framesReceived{0};
    std::atomic<int> writeErrors{0};
    int width = 0;
    int height = 0;

    void begin(const std::filesystem::path& p, CodecType c) {
        std::lock_guard<std::mutex> lock(mutex);
        path = p;
        codec = c;
        segmentRequested = true;
        finishRequested = false;
        framesWritten.store(0);
        framesReceived.store(0);
    }

    void push(std::shared_ptr<CapturedFrame> frame) {
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (!segmentRequested && !segmentOpen) return;
            queue.push_back(std::move(frame));
        }
        framesReceived.fetch_add(1);
        cv.notify_one();
    }

    bool finish() {
        std::unique_lock<std::mutex> lock(mutex);
        finishRequested = true;
        cv.notify_one();
        cv.wait(lock, [this] { return !finishRequested; });
        return finishResult;
    }

    void writeSilenceUntil(int64_t tsHns) {
        constexpr int kRate = 48000;
        const int64_t targetSamples = (tsHns * kRate) / 10000000LL;
        while (audioSamples < targetSamples) {
            const int chunk = static_cast<int>(std::min<int64_t>(1024, targetSamples - audioSamples));
            std::vector<float> silence(static_cast<size_t>(chunk) * 2, 0.0f);
            const int64_t chunkTs = (audioSamples * 10000000LL) / kRate;
            if (!encoder->writeGameAudio(silence.data(), chunk, 2, kRate, chunkTs)) {
                writeErrors.fetch_add(1);
                break;
            }
            audioSamples += chunk;
        }
    }

    void run() {
        while (true) {
            std::shared_ptr<CapturedFrame> frame;
            bool doFinish = false;
            {
                std::unique_lock<std::mutex> lock(mutex);
                cv.wait(lock, [this] { return quit || !queue.empty() || finishRequested; });
                if (quit) return;
                if (!queue.empty()) {
                    frame = std::move(queue.front());
                    queue.pop_front();
                } else if (finishRequested) {
                    doFinish = true;
                }
            }

            if (frame) {
                @autoreleasepool {
                    if (!segmentOpen) {
                        encoder = std::make_unique<VideoToolboxEncoder>();
                        width = frame->width;
                        height = frame->height;
                        auto res = encoder->start(path, width, height, 60, 12, 20, 80, RateControlMode::ADAPTIVE_VBR,
                                                  false, codec, true, "", 48000);
                        CHECK(res.ok, "encoder start failed for %s (%dx%d): %s",
                              path.filename().c_str(), width, height, res.detailString().c_str());
                        segmentStart = frame->timestamp;
                        lastTs = -1;
                        audioSamples = 0;
                        segmentOpen = true;
                        std::lock_guard<std::mutex> lock(mutex);
                        segmentRequested = false;
                    }
                    int64_t ts = std::chrono::duration_cast<std::chrono::nanoseconds>(frame->timestamp - segmentStart).count() / 100;
                    if (ts <= lastTs) ts = lastTs + 1000;
                    auto res = encoder->writeVideoFrame(frame->pixelBuffer.get(), frame->width, frame->height, ts);
                    if (res.ok) {
                        framesWritten.fetch_add(1);
                        lastTs = ts;
                        // Like ReplayManager's audio horizon: audio never runs ahead of the written video.
                        writeSilenceUntil(ts);
                    } else if (!res.retryable) {
                        writeErrors.fetch_add(1);
                    }
                }
                FrameGrabber::get()->recycleFrame(std::move(frame));
            }

            if (doFinish) {
                bool ok = false;
                @autoreleasepool {
                    if (segmentOpen && encoder) {
                        writeSilenceUntil(lastTs + 166666); // close the audio track just past the last frame
                        ok = encoder->finish();
                        encoder.reset();
                    }
                }
                std::lock_guard<std::mutex> lock(mutex);
                segmentOpen = false;
                finishRequested = false;
                finishResult = ok;
                cv.notify_all();
            }
        }
    }
};

EncoderSink g_sink;

// ---------------------------------------------------------------------------------------------
// Video checks (AVAssetReader decode to BGRA)
// ---------------------------------------------------------------------------------------------
struct Rgb {
    int r = 0, g = 0, b = 0;
};

Rgb pixelAt(CVPixelBufferRef pb, int x, int y) {
    const uint8_t* base = static_cast<const uint8_t*>(CVPixelBufferGetBaseAddress(pb));
    const size_t stride = CVPixelBufferGetBytesPerRow(pb);
    const uint8_t* p = base + static_cast<size_t>(y) * stride + static_cast<size_t>(x) * 4;
    return {p[2], p[1], p[0]};
}

bool isColor(Rgb c, int r, int g, int b) {
    auto near = [](int v, int target) { return target > 127 ? v >= 170 : v <= 85; };
    return near(c.r, r) && near(c.g, g) && near(c.b, b);
}

struct VideoReport {
    bool opened = false;
    int frames = 0;
    double videoSeconds = 0.0;
    double audioSeconds = 0.0;
    int width = 0;
    int height = 0;
    int colorChecksOk = 0;
    int colorChecksBad = 0;
    int barChecksOk = 0;
    int barChecksBad = 0;
    std::vector<double> barCenters;
};

// expectContent: rectangle (top-down rows) the game image must fill; pixels outside must be black.
VideoReport analyzeVideo(const std::filesystem::path& file, int srcW, int srcH) {
    VideoReport rep;
    @autoreleasepool {
        AVURLAsset* asset = [AVURLAsset URLAssetWithURL:[NSURL fileURLWithPath:@(file.c_str())] options:nil];
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
        AVAssetTrack* videoTrack = [asset tracksWithMediaType:AVMediaTypeVideo].firstObject;
        AVAssetTrack* audioTrack = [asset tracksWithMediaType:AVMediaTypeAudio].firstObject;
#pragma clang diagnostic pop
        if (!videoTrack) return rep;
        rep.opened = true;
        rep.width = static_cast<int>(videoTrack.naturalSize.width);
        rep.height = static_cast<int>(videoTrack.naturalSize.height);
        rep.videoSeconds = CMTimeGetSeconds(videoTrack.timeRange.duration);
        if (audioTrack) rep.audioSeconds = CMTimeGetSeconds(audioTrack.timeRange.duration);

        NSError* err = nil;
        AVAssetReader* reader = [AVAssetReader assetReaderWithAsset:asset error:&err];
        AVAssetReaderTrackOutput* out = [AVAssetReaderTrackOutput
            assetReaderTrackOutputWithTrack:videoTrack
                             outputSettings:@{(id)kCVPixelBufferPixelFormatTypeKey: @(kCVPixelFormatType_32BGRA)}];
        out.alwaysCopiesSampleData = NO;
        [reader addOutput:out];
        if (![reader startReading]) return rep;

        const DestRect rect = computeAspectFitRect(srcW, srcH, rep.width, rep.height);
        const int cw = rect.x1 - rect.x0;
        const int ch = rect.y1 - rect.y0;
        while (CMSampleBufferRef sb = [out copyNextSampleBuffer]) {
            CVImageBufferRef pb = CMSampleBufferGetImageBuffer(sb);
            if (pb) {
                ++rep.frames;
                if (rep.frames % 5 == 3) {
                    CVPixelBufferLockBaseAddress(pb, kCVPixelBufferLock_ReadOnly);
                    // Quadrant centres of the drawn pattern (top-down): TL red, TR green, BL blue, BR white.
                    struct Probe { double fx, fy; int r, g, b; const char* name; } probes[] = {
                        {0.25, 0.25, 255, 0, 0, "top-left red"},
                        {0.75, 0.25, 0, 255, 0, "top-right green"},
                        {0.25, 0.75, 0, 0, 255, "bottom-left blue"},
                        {0.75, 0.75, 255, 255, 255, "bottom-right white"},
                    };
                    for (const auto& pr : probes) {
                        int x = rect.x0 + static_cast<int>(pr.fx * cw);
                        int y = rect.y0 + static_cast<int>(pr.fy * ch);
                        Rgb c = pixelAt(pb, x, y);
                        if (isColor(c, pr.r, pr.g, pr.b)) {
                            ++rep.colorChecksOk;
                        } else {
                            ++rep.colorChecksBad;
                            logLine("    frame %d: %s at (%d,%d) is (%d,%d,%d)", rep.frames, pr.name, x, y, c.r, c.g, c.b);
                        }
                    }
                    if (rect.hasBars) {
                        // Letterbox / pillarbox bars must be black.
                        Rgb barA = rect.y0 > 2 ? pixelAt(pb, rep.width / 2, rect.y0 / 2) : pixelAt(pb, rect.x0 / 2, rep.height / 2);
                        Rgb barB = rect.y0 > 2 ? pixelAt(pb, rep.width / 2, rep.height - 1 - rect.y0 / 2)
                                               : pixelAt(pb, rep.width - 1 - rect.x0 / 2, rep.height / 2);
                        if (isColor(barA, 0, 0, 0) && isColor(barB, 0, 0, 0)) {
                            ++rep.barChecksOk;
                        } else {
                            ++rep.barChecksBad;
                            logLine("    frame %d: bars not black (%d,%d,%d) / (%d,%d,%d)", rep.frames,
                                    barA.r, barA.g, barA.b, barB.r, barB.g, barB.b);
                        }
                    }
                    // Yellow motion band across the vertical centre: its centre must move between frames.
                    const int yMid = rect.y0 + ch / 2;
                    double sumX = 0.0;
                    int count = 0;
                    for (int x = rect.x0; x < rect.x1; x += 2) {
                        Rgb c = pixelAt(pb, x, yMid);
                        if (c.r >= 170 && c.g >= 170 && c.b <= 110) {
                            sumX += x;
                            ++count;
                        }
                    }
                    if (count > 0) rep.barCenters.push_back(sumX / count);
                    CVPixelBufferUnlockBaseAddress(pb, kCVPixelBufferLock_ReadOnly);
                }
            }
            CFRelease(sb);
        }
    }
    return rep;
}

} // namespace

// ---------------------------------------------------------------------------------------------
// cocos2d-x shaped EAGL view (resolved by IOSPlatformUtils through objc_getClass("EAGLView")).
// ---------------------------------------------------------------------------------------------
@interface EAGLView : UIView
+ (id)sharedEGLView;
- (EAGLContext*)context;
- (CGSize)surfaceSize;
@end

static EAGLView* g_sharedView = nil;

@implementation EAGLView {
    EAGLContext* _context;
    GLuint _fbo;
    GLuint _rb;
    GLint _w;
    GLint _h;
}

+ (Class)layerClass {
    return [CAEAGLLayer class];
}

+ (id)sharedEGLView {
    return g_sharedView;
}

- (EAGLContext*)context {
    return _context;
}

- (CGSize)surfaceSize {
    return CGSizeMake(_w, _h);
}

- (instancetype)initWithFrame:(CGRect)frame {
    if ((self = [super initWithFrame:frame])) {
        self.contentScaleFactor = UIScreen.mainScreen.scale;
        CAEAGLLayer* layer = (CAEAGLLayer*)self.layer;
        layer.opaque = YES;
        layer.drawableProperties = @{
            kEAGLDrawablePropertyRetainedBacking: @NO,
            kEAGLDrawablePropertyColorFormat: kEAGLColorFormatRGBA8,
        };
        _context = [[EAGLContext alloc] initWithAPI:kEAGLRenderingAPIOpenGLES2];
        [EAGLContext setCurrentContext:_context];
        glGenFramebuffers(1, &_fbo);
        glGenRenderbuffers(1, &_rb);
        glBindFramebuffer(GL_FRAMEBUFFER, _fbo);
        glBindRenderbuffer(GL_RENDERBUFFER, _rb);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, _rb);
        g_sharedView = self;
    }
    return self;
}

- (void)layoutSubviews {
    [super layoutSubviews];
    [EAGLContext setCurrentContext:_context];
    glBindRenderbuffer(GL_RENDERBUFFER, _rb);
    [_context renderbufferStorage:GL_RENDERBUFFER fromDrawable:(CAEAGLLayer*)self.layer];
    glGetRenderbufferParameteriv(GL_RENDERBUFFER, GL_RENDERBUFFER_WIDTH, &_w);
    glGetRenderbufferParameteriv(GL_RENDERBUFFER, GL_RENDERBUFFER_HEIGHT, &_h);
    glBindFramebuffer(GL_FRAMEBUFFER, _fbo);
}

- (GLuint)framebuffer {
    return _fbo;
}

- (GLuint)renderbuffer {
    return _rb;
}

- (GLint)pixelWidth {
    return _w;
}

- (GLint)pixelHeight {
    return _h;
}
@end

// ---------------------------------------------------------------------------------------------
// Test driver
// ---------------------------------------------------------------------------------------------
@interface HarnessController : UIViewController
@end

@implementation HarnessController {
    CADisplayLink* _link;
    int _frame;
    int _phase;
    int _phaseFrames;
    GLuint _cocosProgram;
    GLuint _cocosVao;
    GLuint _cocosBuffer;
    GLuint _cocosTex0;
    GLuint _cocosTex1;
    int _stateMismatches;
    int _srcW;
    int _srcH;
    std::filesystem::path _dir;
}

- (BOOL)prefersStatusBarHidden {
    return YES;
}

- (UIInterfaceOrientationMask)supportedInterfaceOrientations {
    return UIInterfaceOrientationMaskLandscape;
}

- (void)loadView {
    self.view = [[EAGLView alloc] initWithFrame:UIScreen.mainScreen.bounds];
}

- (void)viewDidAppear:(BOOL)animated {
    [super viewDidAppear:animated];
    _dir = std::filesystem::path(NSTemporaryDirectory().UTF8String) / "ir_harness";
    std::error_code ec;
    std::filesystem::remove_all(_dir, ec);
    std::filesystem::create_directories(_dir, ec);

    if (getenv("IR_HARNESS_SHARE_ONLY")) {
        // Quick check of presentIOSShareSheet alone (no capture phases).
        const std::filesystem::path file = _dir / "share_probe.mp4";
        FILE* fp = fopen(file.c_str(), "wb");
        if (fp) { fputs("probe", fp); fclose(fp); }
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC), dispatch_get_main_queue(), ^{
            const bool presented = presentIOSShareSheet(file);
            UIViewController* immediate = self.presentedViewController;
            logLine("[share-only] presented=%d immediate=%s root=%s", presented,
                    immediate ? NSStringFromClass([immediate class]).UTF8String : "(none)",
                    NSStringFromClass([self.view.window.rootViewController class]).UTF8String);
            [self waitForShareSheet:presented attempt:0 dir:_dir];
        });
        return;
    }

    EAGLView* view = (EAGLView*)self.view;
    [EAGLContext setCurrentContext:view.context];
    [self createCocosLikeState];
    [self probeTextureCacheWithContext:view.context];
    [self probeWriterSettings];

    bool hookOk = installIOSGLCaptureHook(
        []() { FrameGrabber::get()->captureCurrentFrame(); },
        []() { return FrameGrabber::get()->isCapturing(); });
    logLine("[setup] presentRenderbuffer: hook installed=%d (reported=%d), context API=%d",
            hookOk, isIOSGLCaptureHookInstalled(), getIOSCurrentGLContextAPI());
    CHECK(hookOk && isIOSGLCaptureHookInstalled(), "capture hook did not install");
    CHECK(getIOSCurrentGLContextAPI() == 2, "expected an OpenGL ES 2 context like cocos2d-x");

    FrameGrabber::get()->setFrameCallback([](std::shared_ptr<CapturedFrame> frame) {
        g_sink.push(std::move(frame));
    });
    g_sink.worker = std::thread([] { g_sink.run(); });

    _link = [CADisplayLink displayLinkWithTarget:self selector:@selector(tick:)];
    [_link addToRunLoop:[NSRunLoop mainRunLoop] forMode:NSRunLoopCommonModes];
}

// Diagnostics: which CVPixelBuffer configurations CVOpenGLESTextureCache accepts on this device.
- (void)probeTextureCacheWithContext:(EAGLContext*)ctx {
    CVOpenGLESTextureCacheRef cache = nullptr;
    CVReturn r = CVOpenGLESTextureCacheCreate(kCFAllocatorDefault, nullptr, ctx, nullptr, &cache);
    logLine("[probe] texture cache create: %d", r);
    if (!cache) return;
    struct Variant { const char* name; bool iosurface; bool glesCompat; bool cacheCompat; int w; int h; } variants[] = {
        {"iosurface+gles 256", true, true, true, 256, 256},
        {"iosurface+gles 2622x1206", true, true, true, 2622, 1206},
        {"iosurface only 256", true, false, false, 256, 256},
        {"gles only 256", false, true, false, 256, 256},
        {"plain 256", false, false, false, 256, 256},
    };
    for (const auto& v : variants) {
        NSMutableDictionary* attrs = [NSMutableDictionary dictionary];
        if (v.iosurface) attrs[(id)kCVPixelBufferIOSurfacePropertiesKey] = @{};
        if (v.glesCompat) attrs[(id)kCVPixelBufferOpenGLESCompatibilityKey] = @YES;
        if (v.cacheCompat) attrs[(id)kCVPixelBufferOpenGLESTextureCacheCompatibilityKey] = @YES;
        CVPixelBufferRef pb = nullptr;
        CVReturn cr = CVPixelBufferCreate(kCFAllocatorDefault, v.w, v.h, kCVPixelFormatType_32BGRA,
                                          (__bridge CFDictionaryRef)attrs, &pb);
        CVOpenGLESTextureRef tex = nullptr;
        CVReturn tr = pb ? CVOpenGLESTextureCacheCreateTextureFromImage(kCFAllocatorDefault, cache, pb, nullptr, GL_TEXTURE_2D,
                                                                        GL_RGBA, v.w, v.h, GL_BGRA_EXT, GL_UNSIGNED_BYTE, 0, &tex)
                         : -1;
        logLine("[probe] %-26s create=%d texture=%d iosurface=%d bpr=%zu", v.name, cr, tr,
                pb ? (CVPixelBufferGetIOSurface(pb) != nullptr) : 0, pb ? CVPixelBufferGetBytesPerRow(pb) : 0);
        if (tex) CFRelease(tex);
        if (pb) CVPixelBufferRelease(pb);
    }
    CFRelease(cache);
}

// Diagnostics: which AVAssetWriter video settings this device accepts (HEVC support differs per device).
- (void)probeWriterSettings {
    NSString* path = [NSTemporaryDirectory() stringByAppendingPathComponent:@"ir_probe.mp4"];
    [[NSFileManager defaultManager] removeItemAtPath:path error:nil];
    AVAssetWriter* writer = [AVAssetWriter assetWriterWithURL:[NSURL fileURLWithPath:path] fileType:AVFileTypeMPEG4 error:nil];
    for (NSString* codec in @[AVVideoCodecTypeHEVC, AVVideoCodecTypeH264]) {
        NSMutableDictionary* comp = [NSMutableDictionary dictionary];
        NSMutableDictionary* settings = [@{AVVideoCodecKey: codec, AVVideoWidthKey: @1920, AVVideoHeightKey: @1080} mutableCopy];
        auto probe = [&](const char* step) {
            settings[AVVideoCompressionPropertiesKey] = comp.count ? [comp copy] : nil;
            BOOL ok = [writer canApplyOutputSettings:settings forMediaType:AVMediaTypeVideo];
            logLine("[probe] %s %-28s canApply=%d", codec.UTF8String, step, ok);
        };
        probe("minimal");
        comp[AVVideoAverageBitRateKey] = @12000000;
        probe("+bitrate");
        comp[AVVideoExpectedSourceFrameRateKey] = @60;
        comp[AVVideoMaxKeyFrameIntervalKey] = @60;
        comp[AVVideoMaxKeyFrameIntervalDurationKey] = @1.0;
        probe("+fps/keyframes");
        comp[(__bridge NSString*)kVTCompressionPropertyKey_DataRateLimits] = @[@2500000, @1.0];
        probe("+DataRateLimits");
        settings[AVVideoColorPropertiesKey] = @{
            AVVideoColorPrimariesKey: AVVideoColorPrimaries_ITU_R_709_2,
            AVVideoTransferFunctionKey: AVVideoTransferFunction_ITU_R_709_2,
            AVVideoYCbCrMatrixKey: AVVideoYCbCrMatrix_ITU_R_709_2,
        };
        probe("+BT.709");
    }
}

- (void)createCocosLikeState {
    // A valid program, VAO, VBO and two textures, so the test can bind "game" state before presenting.
    const char* vs = "attribute vec4 p; void main(){ gl_Position = p; }";
    const char* fs = "precision mediump float; void main(){ gl_FragColor = vec4(1.0); }";
    GLuint v = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(v, 1, &vs, nullptr);
    glCompileShader(v);
    GLuint f = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(f, 1, &fs, nullptr);
    glCompileShader(f);
    _cocosProgram = glCreateProgram();
    glAttachShader(_cocosProgram, v);
    glAttachShader(_cocosProgram, f);
    glLinkProgram(_cocosProgram);
    glGenVertexArraysOES(1, &_cocosVao);
    glGenBuffers(1, &_cocosBuffer);
    glGenTextures(1, &_cocosTex0);
    glGenTextures(1, &_cocosTex1);
    uint32_t px = 0xFFFFFFFF;
    for (GLuint t : {_cocosTex0, _cocosTex1}) {
        glBindTexture(GL_TEXTURE_2D, t);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, &px);
    }
}

- (void)drawPatternWithView:(EAGLView*)view {
    const GLint w = view.pixelWidth;
    const GLint h = view.pixelHeight;
    glBindFramebuffer(GL_FRAMEBUFFER, view.framebuffer);
    glViewport(0, 0, w, h);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glDisable(GL_BLEND);
    glEnable(GL_SCISSOR_TEST);
    auto fill = [](GLint x, GLint y, GLint fw, GLint fh, float r, float g, float b) {
        glScissor(x, y, fw, fh);
        glClearColor(r, g, b, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
    };
    // GL origin is bottom-left: the top half of the screen is y >= h / 2.
    fill(0, h / 2, w / 2, h - h / 2, 1, 0, 0);         // top-left red
    fill(w / 2, h / 2, w - w / 2, h - h / 2, 0, 1, 0); // top-right green
    fill(0, 0, w / 2, h / 2, 0, 0, 1);                 // bottom-left blue
    fill(w / 2, 0, w - w / 2, h / 2, 1, 1, 1);         // bottom-right white
    const GLint barW = std::max(8, w / 24);
    const GLint barX = (_frame * 9) % std::max(1, w - barW);
    fill(barX, h / 2 - h / 20, barW, h / 10, 1, 1, 0); // moving yellow band
    glDisable(GL_SCISSOR_TEST);
}

- (void)applyCocosLikeStateWithView:(EAGLView*)view {
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, _cocosTex0);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, _cocosTex1);
    glUseProgram(_cocosProgram);
    glBindVertexArrayOES(_cocosVao);
    glBindBuffer(GL_ARRAY_BUFFER, _cocosBuffer);
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glEnable(GL_SCISSOR_TEST);
    glScissor(3, 5, 7, 11);
    glViewport(1, 2, view.pixelWidth - 3, view.pixelHeight - 4);
    glClearColor(0.25f, 0.5f, 0.75f, 1.0f);
    glColorMask(GL_TRUE, GL_FALSE, GL_TRUE, GL_TRUE);
    glBindFramebuffer(GL_FRAMEBUFFER, view.framebuffer);
    glBindRenderbuffer(GL_RENDERBUFFER, view.renderbuffer);
}

- (int)countStateMismatchesWithView:(EAGLView*)view {
    int bad = 0;
    auto expectInt = [&bad](GLenum pname, GLint expected, const char* name) {
        GLint v = -1;
        glGetIntegerv(pname, &v);
        if (v != expected) {
            ++bad;
            logLine("    state %s = %d, expected %d", name, v, expected);
        }
    };
    expectInt(GL_ACTIVE_TEXTURE, GL_TEXTURE1, "GL_ACTIVE_TEXTURE");
    expectInt(GL_TEXTURE_BINDING_2D, static_cast<GLint>(_cocosTex1), "texture unit 1");
    glActiveTexture(GL_TEXTURE0);
    expectInt(GL_TEXTURE_BINDING_2D, static_cast<GLint>(_cocosTex0), "texture unit 0");
    glActiveTexture(GL_TEXTURE1);
    expectInt(GL_CURRENT_PROGRAM, static_cast<GLint>(_cocosProgram), "GL_CURRENT_PROGRAM");
    expectInt(GL_VERTEX_ARRAY_BINDING_OES, static_cast<GLint>(_cocosVao), "VAO");
    expectInt(GL_ARRAY_BUFFER_BINDING, static_cast<GLint>(_cocosBuffer), "GL_ARRAY_BUFFER");
    expectInt(GL_FRAMEBUFFER_BINDING, static_cast<GLint>(view.framebuffer), "GL_FRAMEBUFFER");
    expectInt(GL_RENDERBUFFER_BINDING, static_cast<GLint>(view.renderbuffer), "GL_RENDERBUFFER");
    expectInt(GL_BLEND_SRC_RGB, GL_ONE, "blend src");
    GLint vp[4] = {};
    glGetIntegerv(GL_VIEWPORT, vp);
    if (vp[0] != 1 || vp[1] != 2 || vp[2] != view.pixelWidth - 3 || vp[3] != view.pixelHeight - 4) {
        ++bad;
        logLine("    viewport changed to %d,%d %dx%d", vp[0], vp[1], vp[2], vp[3]);
    }
    GLint sc[4] = {};
    glGetIntegerv(GL_SCISSOR_BOX, sc);
    if (sc[0] != 3 || sc[1] != 5 || sc[2] != 7 || sc[3] != 11) {
        ++bad;
        logLine("    scissor box changed");
    }
    if (!glIsEnabled(GL_BLEND)) { ++bad; logLine("    blend disabled"); }
    if (!glIsEnabled(GL_SCISSOR_TEST)) { ++bad; logLine("    scissor test disabled"); }
    GLfloat cc[4] = {};
    glGetFloatv(GL_COLOR_CLEAR_VALUE, cc);
    if (std::fabs(cc[0] - 0.25f) > 1e-3f || std::fabs(cc[2] - 0.75f) > 1e-3f) { ++bad; logLine("    clear color changed"); }
    GLboolean mask[4] = {};
    glGetBooleanv(GL_COLOR_WRITEMASK, mask);
    if (mask[1] != GL_FALSE || mask[0] != GL_TRUE) { ++bad; logLine("    color mask changed"); }
    if (glGetError() != GL_NO_ERROR) { ++bad; logLine("    GL error after capture"); }
    return bad;
}

- (void)startPhase:(int)phase {
    _phase = phase;
    _phaseFrames = 0;
    EAGLView* view = (EAGLView*)self.view;
    switch (phase) {
        case 1: {
            // Native-size capture, two HEVC segments (rotation), as recording does by default.
            _srcW = view.pixelWidth;
            _srcH = view.pixelHeight;
            int dw = 0, dh = 0;
            getIOSGLDrawableSize(dw, dh);
            logLine("[phase 1] drawable %dx%d (getIOSGLDrawableSize %dx%d), HEVC segment 1", _srcW, _srcH, dw, dh);
            CHECK(dw == _srcW && dh == _srcH, "getIOSGLDrawableSize mismatch");
            g_sink.begin(_dir / "seg_1.mp4", CodecType::HEVC);
            FrameGrabber::get()->setTargetResolution(0, 0);
            FrameGrabber::get()->setCapturing(true);
            break;
        }
        case 2:
            logLine("[phase 2] HEVC segment 2");
            g_sink.begin(_dir / "seg_2.mp4", CodecType::HEVC);
            break;
        case 3:
            // 16:9 output from a wider phone drawable: letterbox bars, H.264.
            logLine("[phase 3] 1280x720 H.264 segment (letterbox)");
            FrameGrabber::get()->setCapturing(false);
            FrameGrabber::get()->setTargetResolution(1280, 720);
            g_sink.begin(_dir / "seg_3.mp4", CodecType::H264);
            FrameGrabber::get()->setCapturing(true);
            break;
        default:
            break;
    }
}

- (void)tick:(CADisplayLink*)link {
    EAGLView* view = (EAGLView*)self.view;
    [EAGLContext setCurrentContext:view.context];
    if (view.pixelWidth < 16) return;

    ++_frame;
    if (_frame == 15) [self startPhase:1];

    [self drawPatternWithView:view];
    [self applyCocosLikeStateWithView:view];
    [view.context presentRenderbuffer:GL_RENDERBUFFER];
    _stateMismatches += [self countStateMismatchesWithView:view];

    if (_phase == 0) return;
    ++_phaseFrames;
    // The Simulator renders OpenGL ES in software, so keep the phases short.
    const int target = (_phase == 3) ? 30 : 45;
    if (_phaseFrames < target) return;

    // End of phase: close the segment on a background queue so the display link keeps running.
    const int finishedPhase = _phase;
    _phase = 0;
    [link setPaused:YES];
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        // Let frames still on the GPU reach the encoder before closing.
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        const bool ok = g_sink.finish(); // writes every queued frame before closing
        const int written = g_sink.framesWritten.load();
        logLine("  segment %d closed: finalized=%d, frames received=%d, written=%d, write errors=%d",
                finishedPhase, ok, g_sink.framesReceived.load(), written, g_sink.writeErrors.load());
        CHECK(ok, "segment %d did not finalize", finishedPhase);
        // Throughput is not under test: the Simulator encodes in software and drops frames while it starts.
        CHECK(written >= 3, "segment %d: only %d of ~%d frames written", finishedPhase, written, target);
        if (written < target / 5) {
            logLine("  note: Simulator encoder kept up with only %d of %d frames", written, target);
        }
        dispatch_async(dispatch_get_main_queue(), ^{
            if (finishedPhase < 3) {
                [self startPhase:finishedPhase + 1];
                [link setPaused:NO];
            } else {
                FrameGrabber::get()->setCapturing(false);
                [link invalidate];
                [self validate];
            }
        });
    });
}

- (void)waitForShareSheet:(bool)presented attempt:(int)attempt dir:(std::filesystem::path)dir {
    UIViewController* shown = self.presentedViewController;
    const bool onScreen = [shown isKindOfClass:[UIActivityViewController class]];
    if (!onScreen && attempt < 30) {
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC / 2), dispatch_get_main_queue(), ^{
            [self waitForShareSheet:presented attempt:attempt + 1 dir:dir];
        });
        return;
    }
    logLine("[share] presented=%d, controller=%s after %.1f s", presented,
            shown ? NSStringFromClass([shown class]).UTF8String : "(none)", attempt * 0.5);
    CHECK(onScreen, "share sheet is not on screen");
    const int failures = g_failures.load();
    logLine(failures == 0 ? "ALL iOS CAPTURE HARNESS TESTS PASSED" : "iOS CAPTURE HARNESS: %d FAILURE(S)", failures);
    logLine("[output] %s", dir.c_str());
    // _exit: skip static destructors (the encoder worker thread is still parked).
    _exit(failures == 0 ? 0 : 1);
}

- (void)validate {
    const int mismatches = _stateMismatches;
    const int srcW = _srcW;
    const int srcH = _srcH;
    const std::filesystem::path dir = _dir;
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        logLine("[state] GL state mismatches after presentRenderbuffer: %d", mismatches);
        CHECK(mismatches == 0, "capture changed game GL state %d time(s)", mismatches);

        // Merge the two native segments like a save does.
        std::atomic<int> progress{0};
        auto merged = VideoToolboxEncoder::concatenateSegments({dir / "seg_1.mp4", dir / "seg_2.mp4"}, dir / "replay.mp4", 0.0, &progress);
        logLine("[concat] success=%d duration=%.2fs progress=%d error='%s'", merged.success, merged.durationSeconds,
                progress.load(), merged.error.c_str());
        CHECK(merged.success, "concatenateSegments failed: %s", merged.error.c_str());
        CHECK(progress.load() == 1000, "export progress ended at %d", progress.load());

        struct Case { const char* name; std::filesystem::path file; int minFrames; double minSeconds; } cases[] = {
            {"seg_1 (HEVC native)", dir / "seg_1.mp4", 3, 0.03},
            {"replay (merged HEVC)", dir / "replay.mp4", 6, 0.1},
            {"seg_3 (H.264 1280x720 letterbox)", dir / "seg_3.mp4", 3, 0.03},
        };
        VideoReport letterbox;
        for (const auto& c : cases) {
            VideoReport rep = analyzeVideo(c.file, srcW, srcH);
            if (c.file.filename() == "seg_3.mp4") letterbox = rep;
            logLine("[check] %s: %dx%d, %d frames, video %.2fs, audio %.2fs, colours %d ok / %d bad, bars %d ok / %d bad, motion samples %zu",
                    c.name, rep.width, rep.height, rep.frames, rep.videoSeconds, rep.audioSeconds,
                    rep.colorChecksOk, rep.colorChecksBad, rep.barChecksOk, rep.barChecksBad, rep.barCenters.size());
            CHECK(rep.opened, "%s: no video track", c.name);
            CHECK(rep.frames >= c.minFrames, "%s: %d frames (< %d)", c.name, rep.frames, c.minFrames);
            CHECK(rep.videoSeconds >= c.minSeconds, "%s: video %.2fs (< %.2fs)", c.name, rep.videoSeconds, c.minSeconds);
            CHECK(rep.audioSeconds > 0.0, "%s: no audio", c.name);
            // Audio ends right after the last frame's timestamp while the video track also counts that frame's
            // duration, so the tracks may differ by about one frame interval (large at the Simulator's frame rate).
            const double frameInterval = rep.frames > 0 ? rep.videoSeconds / rep.frames : 0.0;
            const double syncTolerance = std::max(0.25, 1.5 * frameInterval);
            CHECK(std::fabs(rep.audioSeconds - rep.videoSeconds) < syncTolerance,
                  "%s: audio %.2fs vs video %.2fs out of sync (tolerance %.2fs)",
                  c.name, rep.audioSeconds, rep.videoSeconds, syncTolerance);
            CHECK(rep.colorChecksOk > 0 && rep.colorChecksBad == 0, "%s: orientation / colour probes failed", c.name);
            CHECK(rep.barChecksBad == 0, "%s: letterbox bars not black", c.name);
            bool moved = false;
            for (size_t i = 1; i < rep.barCenters.size(); ++i) {
                if (std::fabs(rep.barCenters[i] - rep.barCenters[0]) > 4.0) moved = true;
            }
            if (rep.barCenters.size() >= 2) {
                CHECK(moved, "%s: motion band did not move between frames", c.name);
            } else {
                logLine("  note: %s has too few frames to check motion", c.name);
            }
        }
        CHECK(letterbox.width == 1280 && letterbox.height == 720, "seg_3 size %dx%d", letterbox.width, letterbox.height);
        CHECK(letterbox.barChecksOk > 0, "seg_3: letterbox bars were not checked");

        // The share sheet must present (iPad needs a popover anchor, or UIKit throws). UIActivityViewController
        // loads the file's metadata first and appears asynchronously, so poll for it.
        dispatch_async(dispatch_get_main_queue(), ^{
            const bool presented = presentIOSShareSheet(dir / "replay.mp4");
            CHECK(presented, "presentIOSShareSheet returned false");
            [self waitForShareSheet:presented attempt:0 dir:dir];
        });
    });
}
@end

// Apps built with recent SDKs must adopt the UIScene lifecycle (iOS 27 traps at launch otherwise).
@interface HarnessSceneDelegate : UIResponder <UIWindowSceneDelegate>
@property(strong, nonatomic) UIWindow* window;
@end

@implementation HarnessSceneDelegate
- (void)scene:(UIScene*)scene willConnectToSession:(UISceneSession*)session options:(UISceneConnectionOptions*)options {
    self.window = [[UIWindow alloc] initWithWindowScene:(UIWindowScene*)scene];
    self.window.rootViewController = [HarnessController new];
    [self.window makeKeyAndVisible];
}
@end

@interface HarnessAppDelegate : UIResponder <UIApplicationDelegate>
@end

@implementation HarnessAppDelegate
- (BOOL)application:(UIApplication*)application didFinishLaunchingWithOptions:(NSDictionary*)launchOptions {
    // Safety net: never hang the test run.
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 300 * NSEC_PER_SEC), dispatch_get_main_queue(), ^{
        logLine("TIMEOUT: harness did not finish in 300 s");
        _exit(2);
    });
    return YES;
}
@end

int main(int argc, char* argv[]) {
    @autoreleasepool {
        logLine("Instant Replay iOS capture harness");
        return UIApplicationMain(argc, argv, nil, NSStringFromClass([HarnessAppDelegate class]));
    }
}
