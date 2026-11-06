#pragma once

#include <Geode/Geode.hpp>

#ifdef GEODE_IS_WINDOWS

#include "../config/ReplayConfig.hpp"
#include <mfapi.h>
#include <mfidl.h>
#include <vector>
#include <chrono>
#include <memory>
#include <mutex>
#include <atomic>
#include <functional>

namespace replay {

// Frames are delivered BOTTOM-UP (native OpenGL row order), tightly packed BGRA,
// already scaled to the encoder output size (width/height are always even).
// Pixels are stored directly in a reusable IMFMediaBuffer to avoid a second copy in the encoder.
struct CapturedFrame {
    ComRef<IMFMediaBuffer> buffer;
    size_t byteSize = 0;
    int width = 0;
    int height = 0;
    int stride = 0;
    std::chrono::steady_clock::time_point timestamp;
};

class FrameGrabber {
public:
    static FrameGrabber* get();

    void initialize();
    void cleanup(bool glContextValid = true);

    // Enables/disables capture and discards any stale pending PBO readback (#6)
    void setCapturing(bool active);

    // Called on the OpenGL render thread right BEFORE the back buffer is swapped
    void captureCurrentFrame();

    // Return a consumed frame buffer to the pool so the render thread does not allocate
    void recycleFrame(std::shared_ptr<CapturedFrame> frame);

    using FrameCallback = std::function<void(std::shared_ptr<CapturedFrame>)>;
    void setFrameCallback(FrameCallback callback);

private:
    FrameGrabber();
    ~FrameGrabber();

    bool ensureScaleTarget(int width, int height);
    void ensurePbo(int index, size_t bytes);
    std::shared_ptr<CapturedFrame> acquireFrame(size_t requiredBytes);

    std::atomic<bool> m_capturing{false};
    bool m_initialized = false;
    bool m_hasPbo = false;
    bool m_hasBlit = false;

    // GPU downscale target (FBO + renderbuffer)
    unsigned int m_fbo = 0;
    unsigned int m_rbo = 0;
    int m_fboWidth = 0;
    int m_fboHeight = 0;

    // Double-buffered async readback
    unsigned int m_pbos[2] = {0, 0};
    size_t m_pboCapacity[2] = {0, 0};
    int m_pboWidth[2] = {0, 0};
    int m_pboHeight[2] = {0, 0};
    bool m_pboPending[2] = {false, false};
    std::chrono::steady_clock::time_point m_pboTime[2];
    int m_pboIndex = 0;

    // Reusable IMFMediaBuffer frame pool (eliminates per-frame 8-33 MB allocation & double copy)
    std::mutex m_poolMutex;
    std::vector<std::shared_ptr<CapturedFrame>> m_framePool;
    std::vector<uint8_t> m_syncScratch;

    std::chrono::steady_clock::time_point m_nextCaptureTime{};
    int m_lastTargetFps = 0;
    FrameCallback m_callback;
};

} // namespace replay

#endif // GEODE_IS_WINDOWS
