#include "FrameGrabber.hpp"

#ifdef GEODE_IS_WINDOWS

#include "../config/ReplayConfig.hpp"
#include <cocos2d.h>
#include <algorithm>

using namespace cocos2d;

namespace replay {

static constexpr size_t kMaxPooledFrames = 8;

FrameGrabber* FrameGrabber::get() {
    static FrameGrabber instance;
    return &instance;
}

FrameGrabber::FrameGrabber() = default;

FrameGrabber::~FrameGrabber() {
    // At static destruction time the OpenGL context is already torn down.
    cleanup(false);
}

void FrameGrabber::initialize() {
    if (m_initialized) return;

    m_hasPbo = glGenBuffers && glBindBuffer && glBufferData && glMapBuffer && glUnmapBuffer;
    if (m_hasPbo) {
        glGenBuffers(2, m_pbos);
    }

    m_hasBlit = glBlitFramebuffer && glGenFramebuffers && glBindFramebuffer &&
                glGenRenderbuffers && glBindRenderbuffer && glRenderbufferStorage &&
                glFramebufferRenderbuffer && glCheckFramebufferStatus;

    m_pboIndex = 0;
    m_initialized = true;
}

void FrameGrabber::cleanup(bool glContextValid) {
    if (glContextValid && m_initialized) {
        if (m_pbos[0] != 0 && glDeleteBuffers) {
            glDeleteBuffers(2, m_pbos);
        }
        if (m_fbo && glDeleteFramebuffers) {
            glDeleteFramebuffers(1, &m_fbo);
        }
        if (m_rbo && glDeleteRenderbuffers) {
            glDeleteRenderbuffers(1, &m_rbo);
        }
    }
    m_pbos[0] = m_pbos[1] = 0;
    m_fbo = 0;
    m_rbo = 0;
    m_fboWidth = m_fboHeight = 0;
    m_pboCapacity[0] = m_pboCapacity[1] = 0;
    m_pboPending[0] = m_pboPending[1] = false;
    m_initialized = false;

    std::lock_guard<std::mutex> lock(m_poolMutex);
    m_framePool.clear();
    m_syncScratch.clear();
}

void FrameGrabber::setFrameCallback(FrameCallback callback) {
    m_callback = std::move(callback);
}

std::shared_ptr<CapturedFrame> FrameGrabber::acquireFrame(size_t requiredBytes) {
    std::shared_ptr<CapturedFrame> frame;
    {
        std::lock_guard<std::mutex> lock(m_poolMutex);
        if (!m_framePool.empty()) {
            frame = std::move(m_framePool.back());
            m_framePool.pop_back();
        }
    }
    if (!frame) {
        frame = std::make_shared<CapturedFrame>();
    }
    if (frame->data.size() != requiredBytes) {
        frame->data.resize(requiredBytes);
    }
    return frame;
}

void FrameGrabber::recycleFrame(std::shared_ptr<CapturedFrame> frame) {
    if (!frame || frame.use_count() != 1) return;
    std::lock_guard<std::mutex> lock(m_poolMutex);
    if (m_framePool.size() < kMaxPooledFrames) {
        m_framePool.push_back(std::move(frame));
    }
}

bool FrameGrabber::ensureScaleTarget(int width, int height) {
    if (m_fbo && m_fboWidth == width && m_fboHeight == height) return true;

    GLint prevRbo = 0, prevDraw = 0;
    glGetIntegerv(GL_RENDERBUFFER_BINDING, &prevRbo);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &prevDraw);

    if (!m_fbo) glGenFramebuffers(1, &m_fbo);
    if (!m_rbo) glGenRenderbuffers(1, &m_rbo);

    glBindRenderbuffer(GL_RENDERBUFFER, m_rbo);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, width, height);

    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_fbo);
    glFramebufferRenderbuffer(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, m_rbo);
    bool ok = glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;

    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, prevDraw);
    glBindRenderbuffer(GL_RENDERBUFFER, prevRbo);

    if (!ok) {
        m_hasBlit = false; // fall back to CPU scaling permanently
        return false;
    }
    m_fboWidth = width;
    m_fboHeight = height;
    return true;
}

void FrameGrabber::ensurePbo(int index, size_t bytes) {
    if (m_pboCapacity[index] >= bytes) return;
    glBindBuffer(GL_PIXEL_PACK_BUFFER, m_pbos[index]);
    glBufferData(GL_PIXEL_PACK_BUFFER, static_cast<GLsizeiptr>(bytes), nullptr, GL_STREAM_READ);
    m_pboCapacity[index] = bytes;
}

// Nearest-neighbour BGRA resize (only used when FBO blit is unavailable)
static void resizeNearest(const uint8_t* src, int sw, int sh, uint8_t* dst, int dw, int dh) {
    const uint32_t* s = reinterpret_cast<const uint32_t*>(src);
    uint32_t* d = reinterpret_cast<uint32_t*>(dst);
    for (int y = 0; y < dh; ++y) {
        int sy = std::min(sh - 1, static_cast<int>((static_cast<int64_t>(y) * sh) / dh));
        const uint32_t* row = s + static_cast<size_t>(sy) * sw;
        uint32_t* out = d + static_cast<size_t>(y) * dw;
        for (int x = 0; x < dw; ++x) {
            int sx = std::min(sw - 1, static_cast<int>((static_cast<int64_t>(x) * sw) / dw));
            out[x] = row[sx];
        }
    }
}

void FrameGrabber::captureCurrentFrame() {
    if (!m_callback) return;

    const auto& config = ReplayConfig::get();
    if (!config.enabled.load()) return;

    // Cache GPU vendor string while we are guaranteed to be on the GL render thread
    (void)ReplayConfig::getGpuAccelName();

    // Throttle capture to target FPS while preserving fractional frame-interval remainders
    int targetFps = std::max(1, config.targetFps.load());
    auto interval = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
        std::chrono::duration<double>(1.0 / static_cast<double>(targetFps))
    );
    auto now = std::chrono::steady_clock::now();
    if (m_lastTargetFps != targetFps) {
        m_lastTargetFps = targetFps;
        m_nextCaptureTime = now;
    }
    if (now < m_nextCaptureTime) {
        return;
    }
    m_nextCaptureTime += interval;
    if (m_nextCaptureTime <= now) {
        m_nextCaptureTime = now + interval;
    }

    if (!m_initialized) {
        initialize();
    }

    // Real size of the window back buffer (GL_VIEWPORT may be a sub-region)
    int srcW = 0, srcH = 0;
    if (auto view = CCEGLView::get()) {
        auto fs = view->getFrameSize();
        srcW = static_cast<int>(fs.width);
        srcH = static_cast<int>(fs.height);
    }
    if (srcW <= 0 || srcH <= 0) {
        GLint viewport[4] = {0, 0, 0, 0};
        glGetIntegerv(GL_VIEWPORT, viewport);
        srcW = viewport[2];
        srcH = viewport[3];
    }
    if (srcW < 16 || srcH < 16) return;

    auto [dstW, dstH] = config.getResolutionDimensions(srcW, srcH);

    GLint prevRead = 0, prevDraw = 0, prevPack = 4;
    GLint prevReadBuffer = GL_BACK, prevDrawBuffer = GL_BACK;
    if (glBindFramebuffer) {
        glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &prevRead);
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &prevDraw);
    }
    glGetIntegerv(GL_READ_BUFFER, &prevReadBuffer);
    glGetIntegerv(GL_DRAW_BUFFER, &prevDrawBuffer);
    glGetIntegerv(GL_PACK_ALIGNMENT, &prevPack);
    glPixelStorei(GL_PACK_ALIGNMENT, 4);

    // 1. Scale the back buffer to the encoder size on the GPU
    int readW = srcW & ~1;
    int readH = srcH & ~1;
    if (m_hasBlit && ensureScaleTarget(dstW, dstH)) {
        GLboolean scissor = glIsEnabled(GL_SCISSOR_TEST);
        if (scissor) glDisable(GL_SCISSOR_TEST);

        glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
        glReadBuffer(GL_BACK);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_fbo);
        glDrawBuffer(GL_COLOR_ATTACHMENT0);
        GLenum filter = (srcW == dstW && srcH == dstH) ? GL_NEAREST : GL_LINEAR;
        glBlitFramebuffer(0, 0, srcW, srcH, 0, 0, dstW, dstH, GL_COLOR_BUFFER_BIT, filter);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, m_fbo);
        glReadBuffer(GL_COLOR_ATTACHMENT0);

        if (scissor) glEnable(GL_SCISSOR_TEST);
        readW = dstW;
        readH = dstH;
    } else if (glBindFramebuffer) {
        glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
        glReadBuffer(GL_BACK);
    }

    size_t readBytes = static_cast<size_t>(readW) * readH * 4;
    size_t dstBytes = static_cast<size_t>(dstW) * dstH * 4;

    auto deliver = [&](const uint8_t* pixels, int w, int h, std::chrono::steady_clock::time_point ts) {
        auto frame = acquireFrame(dstBytes);
        frame->width = dstW;
        frame->height = dstH;
        frame->stride = dstW * 4;
        frame->timestamp = ts;
        if (w == dstW && h == dstH) {
            memcpy(frame->data.data(), pixels, dstBytes);
        } else {
            resizeNearest(pixels, w, h, frame->data.data(), dstW, dstH);
        }
        m_callback(std::move(frame));
    };

    if (m_hasPbo) {
        // 2a. Async readback: start transfer for this frame, consume previous one
        int cur = m_pboIndex;
        int prev = 1 - cur;

        ensurePbo(cur, readBytes);
        glBindBuffer(GL_PIXEL_PACK_BUFFER, m_pbos[cur]);
        glReadPixels(0, 0, readW, readH, GL_BGRA, GL_UNSIGNED_BYTE, nullptr);
        m_pboWidth[cur] = readW;
        m_pboHeight[cur] = readH;
        m_pboTime[cur] = now;
        m_pboPending[cur] = true;

        if (glBindFramebuffer) {
            glBindFramebuffer(GL_READ_FRAMEBUFFER, prevRead);
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER, prevDraw);
        }
        glReadBuffer(prevReadBuffer);
        glDrawBuffer(prevDrawBuffer);

        if (m_pboPending[prev]) {
            glBindBuffer(GL_PIXEL_PACK_BUFFER, m_pbos[prev]);
            auto src = static_cast<const uint8_t*>(glMapBuffer(GL_PIXEL_PACK_BUFFER, GL_READ_ONLY));
            if (src) {
                // Previous frame may have been captured at a different size;
                // deliver() rescales so the encoder always gets dstW x dstH.
                deliver(src, m_pboWidth[prev], m_pboHeight[prev], m_pboTime[prev]);
                glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
            }
            m_pboPending[prev] = false;
        }
        glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
        m_pboIndex = prev;
    } else {
        // 2b. Synchronous fallback (reuses m_syncScratch buffer)
        if (m_syncScratch.size() < readBytes) {
            m_syncScratch.resize(readBytes);
        }
        glReadPixels(0, 0, readW, readH, GL_BGRA, GL_UNSIGNED_BYTE, m_syncScratch.data());
        if (glBindFramebuffer) {
            glBindFramebuffer(GL_READ_FRAMEBUFFER, prevRead);
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER, prevDraw);
        }
        glReadBuffer(prevReadBuffer);
        glDrawBuffer(prevDrawBuffer);
        deliver(m_syncScratch.data(), readW, readH, now);
    }

    glPixelStorei(GL_PACK_ALIGNMENT, prevPack);
}

} // namespace replay

#endif // GEODE_IS_WINDOWS
