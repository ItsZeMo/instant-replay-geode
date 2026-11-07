#include "FrameGrabber.hpp"

#ifdef GEODE_IS_WINDOWS

#include "../config/ReplayConfig.hpp"
#include "../recorder/ReplayManager.hpp"
#include "../encoder/MediaFoundationEncoder.hpp"
#include <cocos2d.h>
#include <algorithm>

using namespace cocos2d;

namespace replay {

// Steady-state pool holds just enough frames for 2x PBO + worker + encoder (~4-6 frames),
// releasing any temporary burst allocations immediately instead of hoarding ~270 MB (#7).
static constexpr size_t kMaxPooledFrames = 6;
static constexpr size_t kMaxPooledBytes = 50ull * 1024 * 1024;

bool FrameGrabber::isMediaBufferUnique(IMFMediaBuffer* buf) noexcept {
    if (!buf) return false;
    buf->AddRef();
    ULONG refs = buf->Release();
    return refs == 1;
}

FrameGrabber* FrameGrabber::get() {
    static FrameGrabber instance;
    return &instance;
}

FrameGrabber::FrameGrabber() = default;

FrameGrabber::~FrameGrabber() {
    // At static destruction time the OpenGL context is already torn down.
    cleanup(false);
}

void FrameGrabber::resetRenderThreadState() noexcept {
    m_pboPending[0] = m_pboPending[1] = false;
    m_pboIndex = 0;
    m_nextCaptureTime = {};
    m_lastSwapTime = {};
    m_lastCapturedTime = {};
    m_avgSwapIntervalNs = 0.0;
    m_lastTargetFps = 0;
}

void FrameGrabber::initialize() {
    if (m_initialized) return;

    MediaFoundationEncoder::globalInitialize();
    (void)ReplayConfig::getGpuAccelName();

    m_hasPbo = glGenBuffers && glBindBuffer && glBufferData && glMapBuffer && glUnmapBuffer;
    if (m_hasPbo) {
        glGenBuffers(2, m_pbos);
    }

    m_hasBlit = glBlitFramebuffer && glGenFramebuffers && glBindFramebuffer &&
                glGenRenderbuffers && glBindRenderbuffer && glRenderbufferStorage &&
                glFramebufferRenderbuffer && glCheckFramebufferStatus;

    resetRenderThreadState();
    m_initialized = true;
}

void FrameGrabber::clearPool() {
    std::lock_guard<std::mutex> lock(m_poolMutex);
    m_framePool.clear();
}

void FrameGrabber::setCapturing(bool active) {
    // Only touch atomics and mutex-guarded pool here; render-thread fields reset via m_captureEpoch (#12).
    m_captureEpoch.fetch_add(1, std::memory_order_acq_rel);
    m_capturing.store(active, std::memory_order_release);
    if (!active) {
        clearPool();
    }
}

void FrameGrabber::cleanup(bool glContextValid) {
    m_capturing.store(false, std::memory_order_release);
    m_captureEpoch.fetch_add(1, std::memory_order_acq_rel);
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
    resetRenderThreadState();
    m_initialized = false;

    std::lock_guard<std::mutex> lock(m_poolMutex);
    m_framePool.clear();
    m_syncScratch.clear();
    m_syncScratch.shrink_to_fit();
}

void FrameGrabber::setFrameCallback(FrameCallback callback) {
    m_callback = std::move(callback);
}

std::shared_ptr<CapturedFrame> FrameGrabber::acquireFrame(size_t requiredBytes) {
    std::shared_ptr<CapturedFrame> frame;
    {
        std::lock_guard<std::mutex> lock(m_poolMutex);
        for (size_t i = m_framePool.size(); i > 0; --i) {
            auto& candidate = m_framePool[i - 1];
            if (!candidate || !candidate->buffer) {
                m_framePool.erase(m_framePool.begin() + static_cast<std::ptrdiff_t>(i - 1));
                continue;
            }
            if (candidate->byteSize != requiredBytes) {
                // Evict stale buffers from a previous resolution so they don't linger
                if (isMediaBufferUnique(candidate->buffer.get())) {
                    m_framePool.erase(m_framePool.begin() + static_cast<std::ptrdiff_t>(i - 1));
                }
                continue;
            }
            if (isMediaBufferUnique(candidate->buffer.get())) {
                frame = std::move(candidate);
                m_framePool.erase(m_framePool.begin() + static_cast<std::ptrdiff_t>(i - 1));
                break;
            }
        }
    }
    if (!frame) {
        frame = std::make_shared<CapturedFrame>();
        if (FAILED(MFCreateMemoryBuffer(static_cast<DWORD>(requiredBytes), frame->buffer.put())) || !frame->buffer) {
            return nullptr;
        }
        frame->byteSize = requiredBytes;
    }
    return frame;
}

void FrameGrabber::recycleFrame(std::shared_ptr<CapturedFrame> frame) {
    if (!frame || !frame->buffer || frame.use_count() != 1) return;
    if (!m_capturing.load(std::memory_order_acquire)) return;
    if (!isMediaBufferUnique(frame->buffer.get())) return;

    std::lock_guard<std::mutex> lock(m_poolMutex);
    size_t maxFrames = std::clamp<size_t>(kMaxPooledBytes / std::max<size_t>(1, frame->byteSize), 4, kMaxPooledFrames);
    if (m_framePool.size() < maxFrames) {
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
    if (!m_callback || !m_capturing.load(std::memory_order_acquire)) return;

    uint64_t epoch = m_captureEpoch.load(std::memory_order_acquire);
    if (m_seenEpoch != epoch) {
        m_seenEpoch = epoch;
        resetRenderThreadState();
    }

    const auto& config = ReplayManager::get()->getSessionConfig();
    if (!ReplayManager::get()->isReplayActive()) return;

    auto now = std::chrono::steady_clock::now();

    // Track actual swap interval EMA so 1:1 VSync capture (e.g. 120 Hz -> 120 FPS, 240 Hz -> 240 FPS)
    // never drops frames when an earlier swap had a large jitter spike (#5).
    if (m_lastSwapTime != std::chrono::steady_clock::time_point{}) {
        double swapNs = static_cast<double>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(now - m_lastSwapTime).count());
        if (swapNs > 0.0 && swapNs < 250000000.0) {
            m_avgSwapIntervalNs = (m_avgSwapIntervalNs <= 0.0)
                ? swapNs
                : (m_avgSwapIntervalNs * 0.85 + swapNs * 0.15);
        }
    }
    m_lastSwapTime = now;

    int targetFps = std::max(1, config.targetFps.load());
    auto interval = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
        std::chrono::duration<double>(1.0 / static_cast<double>(targetFps))
    );
    double intervalNs = 1e9 / static_cast<double>(targetFps);
    // 3/8 interval tolerance gives wide jitter immunity while staying clear of the 1/2 sub-harmonic
    auto tolerance = (interval * 3) / 8;

    if (m_lastTargetFps != targetFps || m_nextCaptureTime == std::chrono::steady_clock::time_point{}) {
        m_lastTargetFps = targetFps;
        m_nextCaptureTime = now;
    }

    if (now + tolerance < m_nextCaptureTime) {
        bool isOneToOneOrSlower = (m_avgSwapIntervalNs >= intervalNs * 0.92) &&
                                  (m_lastCapturedTime == std::chrono::steady_clock::time_point{} ||
                                   (now - m_lastCapturedTime) >= (interval / 2));
        if (!isOneToOneOrSlower) {
            return;
        }
        // Re-center phase onto real VSync cadence after an unusually late initial/prior frame
        m_nextCaptureTime = now;
    }

    m_lastCapturedTime = now;
    m_nextCaptureTime += interval;
    // Allow up to 2 intervals of temporary render lag so catch-up frames are still captured;
    // never snap forward to `now + interval` on jitter (#5).
    if (m_nextCaptureTime + 2 * interval <= now) {
        m_nextCaptureTime = now;
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
        if (srcH > 0) {
            ReplayConfig::s_nativeFrameHeight.store(srcH, std::memory_order_relaxed);
        }
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
        if (!frame || !frame->buffer) return;
        BYTE* dst = nullptr;
        if (FAILED(frame->buffer->Lock(&dst, nullptr, nullptr)) || !dst) {
            recycleFrame(std::move(frame));
            return;
        }
        if (w == dstW && h == dstH) {
            memcpy(dst, pixels, dstBytes);
        } else {
            resizeNearest(pixels, w, h, dst, dstW, dstH);
        }
        frame->buffer->Unlock();
        frame->buffer->SetCurrentLength(static_cast<DWORD>(dstBytes));
        frame->width = dstW;
        frame->height = dstH;
        frame->stride = dstW * 4;
        frame->timestamp = ts;
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
