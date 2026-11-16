#pragma once

#include <Geode/Geode.hpp>

#if defined(GEODE_IS_WINDOWS) || defined(GEODE_IS_MACOS)

#include "../config/ReplayConfig.hpp"
#ifdef GEODE_IS_WINDOWS
#include <mfapi.h>
#include <mfidl.h>
#elif defined(GEODE_IS_MACOS)
#pragma push_macro("CommentType")
#undef CommentType
#define CommentType AppleScriptCommentTypeDummy
#include <CoreVideo/CoreVideo.h>
#include <OpenGL/CGLTypes.h>
#undef CommentType
#pragma pop_macro("CommentType")
#endif
#include <vector>
#include <chrono>
#include <memory>
#include <mutex>
#include <atomic>
#include <functional>

namespace replay {

#ifdef GEODE_IS_MACOS
// Lightweight RAII wrapper around CVPixelBufferRef for zero-copy VideoToolbox handoff.
class PixelBufferRef {
public:
    PixelBufferRef() noexcept = default;
    ~PixelBufferRef() { reset(); }

    PixelBufferRef(const PixelBufferRef& other) noexcept : m_ptr(other.m_ptr) {
        if (m_ptr) CVPixelBufferRetain(m_ptr);
    }
    PixelBufferRef& operator=(const PixelBufferRef& other) noexcept {
        if (this != &other) {
            if (other.m_ptr) CVPixelBufferRetain(other.m_ptr);
            reset();
            m_ptr = other.m_ptr;
        }
        return *this;
    }
    PixelBufferRef(PixelBufferRef&& other) noexcept : m_ptr(other.m_ptr) {
        other.m_ptr = nullptr;
    }
    PixelBufferRef& operator=(PixelBufferRef&& other) noexcept {
        if (this != &other) {
            reset();
            m_ptr = other.m_ptr;
            other.m_ptr = nullptr;
        }
        return *this;
    }

    void reset() noexcept {
        if (m_ptr) {
            CVPixelBufferRelease(m_ptr);
            m_ptr = nullptr;
        }
    }
    CVPixelBufferRef* put() noexcept { reset(); return &m_ptr; }
    CVPixelBufferRef get() const noexcept { return m_ptr; }
    explicit operator bool() const noexcept { return m_ptr != nullptr; }

private:
    CVPixelBufferRef m_ptr = nullptr;
};
#endif

// Frames are tightly packed BGRA scaled to encoder output dimensions (always even).
// Windows delivers bottom-up OpenGL row order in IMFMediaBuffer; macOS delivers top-down CVPixelBufferRef.
struct CapturedFrame {
#ifdef GEODE_IS_WINDOWS
    ComRef<IMFMediaBuffer> buffer;
#elif defined(GEODE_IS_MACOS)
    PixelBufferRef pixelBuffer;
#endif
    size_t byteSize = 0;
    int width = 0;
    int height = 0;
    int stride = 0;
    std::chrono::steady_clock::time_point timestamp;

    bool hasBuffer() const noexcept {
#ifdef GEODE_IS_WINDOWS
        return static_cast<bool>(buffer);
#elif defined(GEODE_IS_MACOS)
        return static_cast<bool>(pixelBuffer);
#else
        return false;
#endif
    }
};

class FrameGrabber {
public:
    static FrameGrabber* get();

    // True when no external consumer (Media Foundation or VideoToolbox async pipeline) holds a ref.
    static bool isFrameBufferUnique(const CapturedFrame& frame) noexcept;
#ifdef GEODE_IS_WINDOWS
    static bool isMediaBufferUnique(IMFMediaBuffer* buf) noexcept;
#endif

    void initialize();
    void cleanup(bool glContextValid = true);

    // Thread-safe: fixes the session output resolution (0,0 = auto-detect on first frame)
    void setTargetResolution(int width, int height) noexcept;

    // Thread-safe: signals capture state change; render-thread state resets on next swap (#12).
    void setCapturing(bool active);

    // Called on the OpenGL render thread right BEFORE the back buffer is swapped
    void captureCurrentFrame();

    // Return a consumed frame buffer to the pool so the render thread does not allocate
    void recycleFrame(std::shared_ptr<CapturedFrame> frame);
    void clearPool();

    using FrameCallback = std::function<void(std::shared_ptr<CapturedFrame>)>;
    void setFrameCallback(FrameCallback callback);

private:
    FrameGrabber();
    ~FrameGrabber();

    bool ensureScaleTarget(int width, int height, std::chrono::steady_clock::time_point now);
    void ensurePbo(int index, size_t bytes);
    std::shared_ptr<CapturedFrame> acquireFrame(int width, int height, size_t requiredBytes);
    void resetRenderThreadState() noexcept;
    void resetGlResources(bool glContextValid) noexcept;

    std::atomic<bool> m_capturing{false};
    std::atomic<uint64_t> m_captureEpoch{0};
    std::atomic<int> m_sessionTargetW{0};
    std::atomic<int> m_sessionTargetH{0};
    uint64_t m_seenEpoch = 0;

    bool m_initialized = false;
    bool m_hasPbo = false;
    bool m_hasBlit = false;
#ifdef GEODE_IS_WINDOWS
    HGLRC m_glContext = nullptr;
#elif defined(GEODE_IS_MACOS)
    CGLContextObj m_glContext = nullptr;
#endif
    std::chrono::steady_clock::time_point m_blitRetryAfter{};
    int m_failedBlitW = 0;
    int m_failedBlitH = 0;
    bool m_blitFailLogged = false;

    // GPU downscale target (FBO + renderbuffer)
    unsigned int m_fbo = 0;
    unsigned int m_rbo = 0;
    int m_fboWidth = 0;
    int m_fboHeight = 0;

    // Double-buffered async readback (render thread only)
    unsigned int m_pbos[2] = {0, 0};
    size_t m_pboCapacity[2] = {0, 0};
    int m_pboWidth[2] = {0, 0};
    int m_pboHeight[2] = {0, 0};
    bool m_pboPending[2] = {false, false};
    bool m_pboTopDown[2] = {false, false};
    std::chrono::steady_clock::time_point m_pboTime[2];
    int m_pboIndex = 0;

    // Reusable IMFMediaBuffer frame pool (bounded steady-state working set, #7)
    std::mutex m_poolMutex;
    std::vector<std::shared_ptr<CapturedFrame>> m_framePool;
    std::vector<uint8_t> m_syncScratch;

    // Frame allocation stats (render thread / debug logging)
    uint64_t m_allocCount = 0;
    uint64_t m_lastLoggedAllocCount = 0;
    std::chrono::steady_clock::time_point m_lastAllocLogTime{};

    // Frame pacing state (render thread only, #5)
    std::chrono::steady_clock::time_point m_nextCaptureTime{};
    std::chrono::steady_clock::time_point m_lastSwapTime{};
    std::chrono::steady_clock::time_point m_lastCapturedTime{};
    double m_avgSwapIntervalNs = 0.0;
    int m_lastTargetFps = 0;

    FrameCallback m_callback;
};

} // namespace replay

#endif // GEODE_IS_WINDOWS || GEODE_IS_MACOS
