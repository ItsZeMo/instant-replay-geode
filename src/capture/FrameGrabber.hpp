#pragma once

#include <Geode/Geode.hpp>
#include <vector>
#include <chrono>
#include <memory>
#include <functional>

namespace replay {

// Frames are delivered BOTTOM-UP (native OpenGL row order), tightly packed BGRA,
// already scaled to the encoder output size (width/height are always even).
struct CapturedFrame {
    std::vector<uint8_t> data;
    int width = 0;
    int height = 0;
    int stride = 0;
    std::chrono::steady_clock::time_point timestamp;
};

class FrameGrabber {
public:
    static FrameGrabber* get();

    void initialize();
    void cleanup();

    // Called on the OpenGL render thread right BEFORE the back buffer is swapped
    void captureCurrentFrame();

    using FrameCallback = std::function<void(std::shared_ptr<CapturedFrame>)>;
    void setFrameCallback(FrameCallback callback);

private:
    FrameGrabber();
    ~FrameGrabber();

    bool ensureScaleTarget(int width, int height);
    void ensurePbo(int index, size_t bytes);

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

    std::chrono::steady_clock::time_point m_nextCaptureTime{};
    int m_lastTargetFps = 0;
    FrameCallback m_callback;
};

} // namespace replay
