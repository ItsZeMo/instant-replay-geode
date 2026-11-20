#include "FrameGrabber.hpp"

#ifdef GEODE_IS_IOS

#include "AspectFit.hpp"
#include "../config/ReplayConfig.hpp"
#include "../encoder/PlatformEncoder.hpp"
#include "../platform/IOSPlatformUtils.hpp"
#include "../recorder/ReplayManager.hpp"

#import <CoreVideo/CoreVideo.h>
#import <OpenGLES/EAGL.h>
#import <OpenGLES/ES2/gl.h>
#import <OpenGLES/ES2/glext.h>
#import <OpenGLES/ES3/gl.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <deque>
#include <vector>

// iOS capture backend.
//
// Geometry Dash renders with OpenGL ES 2 into a CAEAGLLayer renderbuffer. ES 2 has neither
// glBlitFramebuffer nor pixel-pack buffers, so the frame is scaled on the GPU:
//   1. the presented renderbuffer is attached to our own read FBO and copied into a source texture,
//   2. a textured quad scales it to the session output size (letterbox / pillarbox, Y flipped so rows
//      come out top-down) into the output target.
// Output targets:
//   - TextureCache (devices): a texture created from the pooled IOSurface CVPixelBuffer through
//     CVOpenGLESTextureCache, so the GPU writes straight into the buffer VideoToolbox encodes. A GPU
//     fence marks the frame; it is handed to the encoder on a later present without stalling the game.
//   - ReadPixels (fallback, e.g. the iOS Simulator, which cannot map IOSurfaces into OpenGL ES): an
//     offscreen texture read back with glReadPixels into the CVPixelBuffer. The R/B swap needed for
//     BGRA happens in the shader when the driver only reads RGBA.
// Every GL state touched is saved and restored, including state cocos2d-x caches (program, texture
// unit bindings, VAO, blend), so the game's next frame renders exactly as before.

namespace replay {

namespace {

// Frames rendered on the GPU whose fence has not been seen yet. The game thread never waits longer
// than kMaxFenceWaitNs for the oldest one; beyond this the new frame is skipped instead.
constexpr size_t kMaxGpuFramesInFlight = 3;
constexpr uint64_t kMaxFenceWaitNs = 2'000'000ull;       // 2 ms
constexpr uint64_t kTeardownFenceWaitNs = 50'000'000ull; // 50 ms

constexpr GLuint kAttribPosition = 0;
constexpr GLuint kAttribTexCoord = 1;

constexpr const char* kVertexShaderSource =
    "attribute vec2 a_position;\n"
    "attribute vec2 a_texCoord;\n"
    "varying highp vec2 v_texCoord;\n"
    "void main() {\n"
    "    v_texCoord = a_texCoord;\n"
    "    gl_Position = vec4(a_position, 0.0, 1.0);\n"
    "}\n";

// highp texture coordinates: mediump cannot address individual texels of a 2.5K-wide frame.
// u_swapRB = 1 writes BGR into the RGB channels for an RGBA readback that must land as BGRA.
constexpr const char* kFragmentShaderSource =
    "precision highp float;\n"
    "varying highp vec2 v_texCoord;\n"
    "uniform sampler2D u_texture;\n"
    "uniform float u_swapRB;\n"
    "void main() {\n"
    "    vec3 c = texture2D(u_texture, v_texCoord).rgb;\n"
    "    gl_FragColor = vec4(mix(c, c.bgr, u_swapRB), 1.0);\n"
    "}\n";

// Triangle strip covering the viewport. The source texture holds the drawable bottom-up (GL origin),
// the output must be top-down: NDC y = -1 (output row 0) samples the top of the screen (v = 1).
constexpr GLfloat kQuadVertices[] = {
    // x,     y,     u,    v
    -1.0f, -1.0f, 0.0f, 1.0f,
     1.0f, -1.0f, 1.0f, 1.0f,
    -1.0f,  1.0f, 0.0f, 0.0f,
     1.0f,  1.0f, 1.0f, 0.0f,
};

// GL_APPLE_sync / GL_OES_vertex_array_object on ES 2 contexts, core entry points on ES 3 contexts.
GLsync gpuFence(bool es3) {
    return es3 ? glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0)
               : glFenceSyncAPPLE(GL_SYNC_GPU_COMMANDS_COMPLETE_APPLE, 0);
}

GLenum gpuClientWait(bool es3, GLsync sync, uint64_t timeoutNs) {
    // Flush only when actually waiting; a zero-timeout poll must not add a flush to the game's frame.
    if (es3) {
        return glClientWaitSync(sync, timeoutNs > 0 ? GL_SYNC_FLUSH_COMMANDS_BIT : 0, timeoutNs);
    }
    return glClientWaitSyncAPPLE(sync, timeoutNs > 0 ? GL_SYNC_FLUSH_COMMANDS_BIT_APPLE : 0, timeoutNs);
}

void gpuDeleteFence(bool es3, GLsync sync) {
    if (!sync) return;
    if (es3) glDeleteSync(sync);
    else     glDeleteSyncAPPLE(sync);
}

void genVertexArray(bool es3, GLuint* out) {
    if (es3) glGenVertexArrays(1, out);
    else     glGenVertexArraysOES(1, out);
}

void bindVertexArray(bool es3, GLuint vao) {
    if (es3) glBindVertexArray(vao);
    else     glBindVertexArrayOES(vao);
}

void deleteVertexArray(bool es3, GLuint vao) {
    if (!vao) return;
    if (es3) glDeleteVertexArrays(1, &vao);
    else     glDeleteVertexArraysOES(1, &vao);
}

// GL_READ/DRAW_FRAMEBUFFER(_APPLE) share their values between GL_APPLE_framebuffer_multisample and ES 3.
constexpr GLenum kReadFramebuffer = 0x8CA8;
constexpr GLenum kDrawFramebuffer = 0x8CA9;
constexpr GLenum kReadFramebufferBinding = 0x8CAA;
constexpr GLenum kDrawFramebufferBinding = 0x8CA6;
constexpr GLenum kVertexArrayBinding = 0x85B5; // GL_VERTEX_ARRAY_BINDING(_OES)

// Everything the capture pass changes, restored before Geometry Dash presents.
struct SavedGlState {
    GLint readFbo = 0;
    GLint drawFbo = 0;
    GLint renderbuffer = 0;
    GLint program = 0;
    GLint activeTexture = GL_TEXTURE0;
    GLint texture0 = 0;
    GLint arrayBuffer = 0;
    GLint vertexArray = 0;
    GLint packAlignment = 4;
    GLint viewport[4] = {0, 0, 0, 0};
    GLfloat clearColor[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    GLboolean colorMask[4] = {GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE};
    GLboolean blend = GL_FALSE;
    GLboolean scissor = GL_FALSE;
    GLboolean depth = GL_FALSE;
    GLboolean stencil = GL_FALSE;
    GLboolean cull = GL_FALSE;
    GLboolean dither = GL_FALSE;

    void save() {
        glGetIntegerv(kReadFramebufferBinding, &readFbo);
        glGetIntegerv(kDrawFramebufferBinding, &drawFbo);
        glGetIntegerv(GL_RENDERBUFFER_BINDING, &renderbuffer);
        glGetIntegerv(GL_CURRENT_PROGRAM, &program);
        glGetIntegerv(GL_ACTIVE_TEXTURE, &activeTexture);
        glActiveTexture(GL_TEXTURE0);
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &texture0);
        glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &arrayBuffer);
        glGetIntegerv(kVertexArrayBinding, &vertexArray);
        glGetIntegerv(GL_PACK_ALIGNMENT, &packAlignment);
        glGetIntegerv(GL_VIEWPORT, viewport);
        glGetFloatv(GL_COLOR_CLEAR_VALUE, clearColor);
        glGetBooleanv(GL_COLOR_WRITEMASK, colorMask);
        blend = glIsEnabled(GL_BLEND);
        scissor = glIsEnabled(GL_SCISSOR_TEST);
        depth = glIsEnabled(GL_DEPTH_TEST);
        stencil = glIsEnabled(GL_STENCIL_TEST);
        cull = glIsEnabled(GL_CULL_FACE);
        dither = glIsEnabled(GL_DITHER);
    }

    static void setCap(GLenum cap, GLboolean on) {
        if (on) glEnable(cap);
        else    glDisable(cap);
    }

    void restore(bool es3) const {
        bindVertexArray(es3, static_cast<GLuint>(vertexArray));
        glBindBuffer(GL_ARRAY_BUFFER, static_cast<GLuint>(arrayBuffer));
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(texture0));
        glActiveTexture(static_cast<GLenum>(activeTexture));
        glUseProgram(static_cast<GLuint>(program));
        glBindFramebuffer(kReadFramebuffer, static_cast<GLuint>(readFbo));
        glBindFramebuffer(kDrawFramebuffer, static_cast<GLuint>(drawFbo));
        // presentRenderbuffer: presents whatever is bound to GL_RENDERBUFFER, so this must come back exactly.
        glBindRenderbuffer(GL_RENDERBUFFER, static_cast<GLuint>(renderbuffer));
        glPixelStorei(GL_PACK_ALIGNMENT, packAlignment);
        glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
        glClearColor(clearColor[0], clearColor[1], clearColor[2], clearColor[3]);
        glColorMask(colorMask[0], colorMask[1], colorMask[2], colorMask[3]);
        setCap(GL_BLEND, blend);
        setCap(GL_SCISSOR_TEST, scissor);
        setCap(GL_DEPTH_TEST, depth);
        setCap(GL_STENCIL_TEST, stencil);
        setCap(GL_CULL_FACE, cull);
        setCap(GL_DITHER, dither);
    }
};

// Tells the player once per launch that video capture is off (the replay would otherwise just be empty).
void notifyCaptureDisabled() {
    static std::atomic<bool> s_notified{false};
    if (s_notified.exchange(true)) return;
    geode::queueInMainThread([]() {
        geode::Notification::create("Instant Replay: video capture is not supported on this device (see Geode log)",
                                    geode::NotificationIcon::Error, 5.0f)->show();
    });
}

GLuint compileShader(GLenum type, const char* source) {
    GLuint shader = glCreateShader(type);
    if (!shader) return 0;
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);
    GLint ok = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512] = {};
        glGetShaderInfoLog(shader, sizeof(log) - 1, nullptr, log);
        geode::log::error("FrameGrabber(iOS): shader compile failed: {}", log);
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

} // namespace

struct FrameGrabber::IOSGLState {
    enum class Mode { TextureCache, ReadPixels };

    struct InFlightFrame {
        std::shared_ptr<CapturedFrame> frame;
        CVOpenGLESTextureRef texture = nullptr;
        GLsync fence = nullptr;
    };

    bool es3 = false;
    Mode mode = Mode::TextureCache;
    CVOpenGLESTextureCacheRef textureCache = nullptr;

    bool glReady = false; // program / VAO / FBOs created
    bool glSetupFailed = false;
    GLuint program = 0;
    GLint textureUniform = -1;
    GLint swapUniform = -1;
    GLuint vertexBuffer = 0;
    GLuint vertexArray = 0;
    GLuint readFbo = 0;
    GLuint writeFbo = 0;
    GLuint sourceTexture = 0;
    int sourceTextureW = 0;
    int sourceTextureH = 0;

    GLuint attachedRenderbuffer = 0;
    int attachedW = 0;
    int attachedH = 0;
    bool readFboComplete = false;
    Mode writeCheckedMode = Mode::TextureCache;
    int writeCheckedW = 0;
    int writeCheckedH = 0;

    // ReadPixels fallback
    GLuint scaledTexture = 0;
    int scaledW = 0;
    int scaledH = 0;
    bool readFormatQueried = false;
    bool readBgra = false;
    std::vector<uint8_t> readScratch;

    std::deque<InFlightFrame> inFlight;

    bool loggedFboFailure = false;
    uint64_t gpuBusyDrops = 0;
    uint64_t lastLoggedGpuBusyDrops = 0;
    std::chrono::steady_clock::time_point lastBusyLog{};
};

void FrameGrabber::initialize() {
    if (m_initialized) return;

    PlatformEncoder::globalInitialize();
    (void)ReplayConfig::getGpuAccelName();

    m_glContext = getIOSCurrentGLContext();
    if (!m_glContext) {
        return; // retried on the next present
    }

    auto* state = new IOSGLState();
    state->es3 = getIOSCurrentGLContextAPI() >= 3;
    EAGLContext* ctx = (__bridge EAGLContext*)m_glContext;
    CVReturn ret = CVOpenGLESTextureCacheCreate(kCFAllocatorDefault, nullptr, ctx, nullptr, &state->textureCache);
    if (ret != kCVReturnSuccess || !state->textureCache) {
        geode::log::warn("FrameGrabber(iOS): CVOpenGLESTextureCacheCreate failed ({}), using glReadPixels capture",
                         static_cast<int>(ret));
        state->textureCache = nullptr;
        state->mode = IOSGLState::Mode::ReadPixels;
    }
    geode::log::info("FrameGrabber(iOS): capturing from OpenGL ES {} context {}",
                     state->es3 ? 3 : 2, static_cast<const void*>(m_glContext));
    m_ios = state;
    resetRenderThreadState();
    m_initialized = true;
}

void FrameGrabber::iosReleaseGlState(bool glContextValid) noexcept {
    IOSGLState* state = m_ios;
    m_ios = nullptr;
    if (!state) return;

    // GL calls are only legal with our context current, and never from a background app (iOS kills
    // processes that touch the GPU while backgrounded).
    bool canUseGl = glContextValid && m_glContext && getIOSCurrentGLContext() == m_glContext && isIOSAppActive();
    if (!canUseGl && glContextValid) {
        geode::log::debug("FrameGrabber(iOS): skipping GL cleanup (context not current or app inactive)");
    }

    for (auto& f : state->inFlight) {
        if (canUseGl) {
            (void)gpuClientWait(state->es3, f.fence, kTeardownFenceWaitNs);
            gpuDeleteFence(state->es3, f.fence);
        }
        if (f.texture) CFRelease(f.texture);
        f.frame.reset();
    }
    state->inFlight.clear();

    if (canUseGl) {
        if (state->program) glDeleteProgram(state->program);
        if (state->vertexBuffer) glDeleteBuffers(1, &state->vertexBuffer);
        deleteVertexArray(state->es3, state->vertexArray);
        if (state->readFbo) glDeleteFramebuffers(1, &state->readFbo);
        if (state->writeFbo) glDeleteFramebuffers(1, &state->writeFbo);
        if (state->sourceTexture) glDeleteTextures(1, &state->sourceTexture);
        if (state->scaledTexture) glDeleteTextures(1, &state->scaledTexture);
    }
    if (state->textureCache) {
        CVOpenGLESTextureCacheFlush(state->textureCache, 0);
        CFRelease(state->textureCache);
    }
    delete state;
}

void FrameGrabber::resetGlResources(bool glContextValid) noexcept {
    iosReleaseGlState(glContextValid && m_initialized);
    m_glContext = nullptr;
    resetRenderThreadState();
    m_initialized = false;
}

void FrameGrabber::iosDeliverCompletedFrames(bool waitForAll) {
    IOSGLState* state = m_ios;
    if (!state || state->inFlight.empty()) return;

    std::vector<std::shared_ptr<CapturedFrame>> completed;
    while (!state->inFlight.empty()) {
        auto& entry = state->inFlight.front();
        GLenum res = gpuClientWait(state->es3, entry.fence, waitForAll ? kTeardownFenceWaitNs : 0);
        if (res == GL_TIMEOUT_EXPIRED) {
            break; // frames complete in submission order
        }
        gpuDeleteFence(state->es3, entry.fence);
        if (entry.texture) CFRelease(entry.texture);
        if (res == GL_WAIT_FAILED) {
            recycleFrame(std::move(entry.frame));
        } else {
            completed.push_back(std::move(entry.frame));
        }
        state->inFlight.pop_front();
    }
    if (completed.empty()) return;

    // Drop the texture cache's references before the encoder sees the buffers, so the pool's
    // retain-count check recognizes them as free again once VideoToolbox is done (#7).
    if (state->textureCache) {
        CVOpenGLESTextureCacheFlush(state->textureCache, 0);
    }
    for (auto& frame : completed) {
        if (m_callback && m_capturing.load(std::memory_order_acquire)) {
            m_callback(std::move(frame));
        } else {
            recycleFrame(std::move(frame));
        }
    }
}

void FrameGrabber::captureCurrentFrame() {
    if (!m_callback || !m_capturing.load(std::memory_order_acquire)) return;

    std::unique_lock<std::mutex> glLock(m_glMutex);
    if (!m_callback || !m_capturing.load(std::memory_order_acquire)) return;

    void* curCtx = getIOSCurrentGLContext();
    if (!curCtx) return;

    if (!m_initialized) {
        initialize();
    } else if (curCtx != m_glContext) {
        geode::log::warn("FrameGrabber(iOS): EAGLContext changed ({} -> {}), recreating GL resources",
                         static_cast<const void*>(m_glContext), curCtx);
        resetGlResources(false);
        initialize();
    }

    const uint64_t epoch = m_captureEpoch.load(std::memory_order_acquire);
    if (m_seenEpoch != epoch) {
        m_seenEpoch = epoch;
        resetRenderThreadState();
        // Frames still on the GPU belong to the previous session: free them without delivering.
        if (m_ios) {
            for (auto& f : m_ios->inFlight) {
                gpuDeleteFence(m_ios->es3, f.fence);
                if (f.texture) CFRelease(f.texture);
            }
            m_ios->inFlight.clear();
            if (m_ios->textureCache) CVOpenGLESTextureCacheFlush(m_ios->textureCache, 0);
        }
    }

    IOSGLState* state = m_ios;
    if (!state || state->glSetupFailed) return;

    // Hand finished frames to the encoder first (non-blocking poll).
    iosDeliverCompletedFrames(false);

    const auto& config = ReplayManager::get()->getSessionConfig();
    if (!ReplayManager::get()->isReplayActive()) return;

    const auto now = std::chrono::steady_clock::now();
    if (!admitSwapForCapture(now, config.targetFps.load())) return;

    // Source = the renderbuffer presentRenderbuffer: is about to show.
    GLint presentedRb = 0;
    glGetIntegerv(GL_RENDERBUFFER_BINDING, &presentedRb);
    if (presentedRb == 0) return;
    GLint srcW = 0, srcH = 0;
    glGetRenderbufferParameteriv(GL_RENDERBUFFER, GL_RENDERBUFFER_WIDTH, &srcW);
    glGetRenderbufferParameteriv(GL_RENDERBUFFER, GL_RENDERBUFFER_HEIGHT, &srcH);
    if (srcW < 16 || srcH < 16) return;
    ReplayConfig::s_nativeFrameWidth.store(srcW, std::memory_order_relaxed);
    ReplayConfig::s_nativeFrameHeight.store(srcH, std::memory_order_relaxed);

    int dstW = 0, dstH = 0;
    resolveSessionTargetSize(config, srcW, srcH, dstW, dstH);
    if (dstW < 16 || dstH < 16) return;

    // GPU backpressure: wait a moment for the oldest frame, otherwise skip this one.
    if (state->inFlight.size() >= kMaxGpuFramesInFlight) {
        auto& oldest = state->inFlight.front();
        (void)gpuClientWait(state->es3, oldest.fence, kMaxFenceWaitNs);
        iosDeliverCompletedFrames(false);
        if (state->inFlight.size() >= kMaxGpuFramesInFlight) {
            ++state->gpuBusyDrops;
            if (now - state->lastBusyLog >= std::chrono::seconds(5) && state->gpuBusyDrops != state->lastLoggedGpuBusyDrops) {
                geode::log::debug("FrameGrabber(iOS): {} frame(s) skipped while the GPU was busy",
                                  state->gpuBusyDrops - state->lastLoggedGpuBusyDrops);
                state->lastLoggedGpuBusyDrops = state->gpuBusyDrops;
                state->lastBusyLog = now;
            }
            return;
        }
    }

    const size_t dstBytes = static_cast<size_t>(dstW) * static_cast<size_t>(dstH) * 4;
    auto frame = acquireFrame(dstW, dstH, dstBytes);
    if (!frame || !frame->hasBuffer()) return;

    const bool es3 = state->es3;
    SavedGlState saved;
    saved.save(); // leaves GL_TEXTURE0 active

    bool ok = true;
    if (!state->glReady) {
        // One-time setup (inside the save/restore so GD's bindings are untouched).
        GLuint vs = compileShader(GL_VERTEX_SHADER, kVertexShaderSource);
        GLuint fs = compileShader(GL_FRAGMENT_SHADER, kFragmentShaderSource);
        if (vs && fs) {
            state->program = glCreateProgram();
            glAttachShader(state->program, vs);
            glAttachShader(state->program, fs);
            glBindAttribLocation(state->program, kAttribPosition, "a_position");
            glBindAttribLocation(state->program, kAttribTexCoord, "a_texCoord");
            glLinkProgram(state->program);
            GLint linked = GL_FALSE;
            glGetProgramiv(state->program, GL_LINK_STATUS, &linked);
            if (!linked) {
                char log[512] = {};
                glGetProgramInfoLog(state->program, sizeof(log) - 1, nullptr, log);
                geode::log::error("FrameGrabber(iOS): program link failed: {}", log);
                ok = false;
            }
        } else {
            ok = false;
        }
        if (vs) glDeleteShader(vs);
        if (fs) glDeleteShader(fs);

        if (ok) {
            state->textureUniform = glGetUniformLocation(state->program, "u_texture");
            state->swapUniform = glGetUniformLocation(state->program, "u_swapRB");
            glUseProgram(state->program);
            glUniform1i(state->textureUniform, 0);
            glUniform1f(state->swapUniform, 0.0f);

            genVertexArray(es3, &state->vertexArray);
            glGenBuffers(1, &state->vertexBuffer);
            glGenFramebuffers(1, &state->readFbo);
            glGenFramebuffers(1, &state->writeFbo);
            glGenTextures(1, &state->sourceTexture);
            ok = state->vertexArray && state->vertexBuffer && state->readFbo && state->writeFbo && state->sourceTexture;
        }
        if (ok) {
            // Our own VAO: GD's default vertex-array state (pointers / enabled attributes) stays untouched.
            bindVertexArray(es3, state->vertexArray);
            glBindBuffer(GL_ARRAY_BUFFER, state->vertexBuffer);
            glBufferData(GL_ARRAY_BUFFER, sizeof(kQuadVertices), kQuadVertices, GL_STATIC_DRAW);
            glEnableVertexAttribArray(kAttribPosition);
            glVertexAttribPointer(kAttribPosition, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), reinterpret_cast<const void*>(0));
            glEnableVertexAttribArray(kAttribTexCoord);
            glVertexAttribPointer(kAttribTexCoord, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat),
                                  reinterpret_cast<const void*>(2 * sizeof(GLfloat)));
            bindVertexArray(es3, 0);

            glBindTexture(GL_TEXTURE_2D, state->sourceTexture);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            state->glReady = true;
        } else {
            state->glSetupFailed = true;
            geode::log::error("FrameGrabber(iOS): GL setup failed, video capture disabled for this launch");
            notifyCaptureDisabled();
        }
    }

    // Output target: the pooled CVPixelBuffer itself (texture cache), or the readback texture.
    CVOpenGLESTextureRef dstTexture = nullptr;
    if (ok && state->mode == IOSGLState::Mode::TextureCache) {
        CVReturn cvRet = CVOpenGLESTextureCacheCreateTextureFromImage(
            kCFAllocatorDefault, state->textureCache, frame->pixelBuffer.get(), nullptr,
            GL_TEXTURE_2D, GL_RGBA, dstW, dstH, GL_BGRA_EXT, GL_UNSIGNED_BYTE, 0, &dstTexture);
        if (cvRet != kCVReturnSuccess || !dstTexture) {
            dstTexture = nullptr;
            state->mode = IOSGLState::Mode::ReadPixels;
            geode::log::warn("FrameGrabber(iOS): cannot render into CVPixelBuffers on this device "
                             "(CVOpenGLESTextureCache error {}), switching to glReadPixels capture",
                             static_cast<int>(cvRet));
        }
    }
    const bool readBack = (state->mode == IOSGLState::Mode::ReadPixels);
    GLenum dstTarget = GL_TEXTURE_2D;
    GLuint dstName = 0;
    if (ok) {
        if (readBack) {
            if (!state->scaledTexture) glGenTextures(1, &state->scaledTexture);
            glBindTexture(GL_TEXTURE_2D, state->scaledTexture);
            if (state->scaledW != dstW || state->scaledH != dstH) {
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, dstW, dstH, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
                state->scaledW = dstW;
                state->scaledH = dstH;
            }
            dstName = state->scaledTexture;
            ok = (dstName != 0);
        } else {
            dstTarget = CVOpenGLESTextureGetTarget(dstTexture);
            dstName = CVOpenGLESTextureGetName(dstTexture);
        }
    }

    if (ok) {
        // 1. Copy the presented drawable into the source texture through our read FBO.
        glBindFramebuffer(GL_FRAMEBUFFER, state->readFbo);
        if (state->attachedRenderbuffer != static_cast<GLuint>(presentedRb) ||
            state->attachedW != srcW || state->attachedH != srcH) {
            glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, static_cast<GLuint>(presentedRb));
            state->readFboComplete = (glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE);
            state->attachedRenderbuffer = static_cast<GLuint>(presentedRb);
            state->attachedW = srcW;
            state->attachedH = srcH;
            if (!state->readFboComplete && !state->loggedFboFailure) {
                state->loggedFboFailure = true;
                geode::log::error("FrameGrabber(iOS): drawable renderbuffer {} ({}x{}) cannot be read", presentedRb, srcW, srcH);
            }
        }
        ok = state->readFboComplete;
    }

    if (ok) {
        glBindTexture(GL_TEXTURE_2D, state->sourceTexture);
        if (state->sourceTextureW != srcW || state->sourceTextureH != srcH) {
            // GL_RGB is copyable from both RGBA8 and RGB565 drawables.
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, srcW, srcH, 0, GL_RGB, GL_UNSIGNED_BYTE, nullptr);
            state->sourceTextureW = srcW;
            state->sourceTextureH = srcH;
        }
        glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, srcW, srcH);

        // 2. Scale + flip into the output target.
        glBindFramebuffer(GL_FRAMEBUFFER, state->writeFbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, dstTarget, dstName, 0);
        if (state->writeCheckedMode != state->mode || state->writeCheckedW != dstW || state->writeCheckedH != dstH) {
            const GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
            if (status != GL_FRAMEBUFFER_COMPLETE) {
                ok = false;
                if (!readBack) {
                    // The driver cannot render into this IOSurface: read back from our own texture instead.
                    geode::log::warn("FrameGrabber(iOS): cannot render into CVPixelBuffer textures (FBO status 0x{:X}), "
                                     "switching to glReadPixels capture", static_cast<unsigned>(status));
                    state->mode = IOSGLState::Mode::ReadPixels;
                } else {
                    geode::log::error("FrameGrabber(iOS): output FBO {}x{} incomplete (0x{:X}), video capture disabled",
                                      dstW, dstH, static_cast<unsigned>(status));
                    state->glSetupFailed = true;
                    notifyCaptureDisabled();
                }
            } else {
                state->writeCheckedMode = state->mode;
                state->writeCheckedW = dstW;
                state->writeCheckedH = dstH;
            }
        }
        if (ok && readBack && !state->readFormatQueried) {
            // ES 2 always reads GL_RGBA; Apple drivers usually also offer BGRA for the bound framebuffer.
            GLint fmt = 0, type = 0;
            glGetIntegerv(GL_IMPLEMENTATION_COLOR_READ_FORMAT, &fmt);
            glGetIntegerv(GL_IMPLEMENTATION_COLOR_READ_TYPE, &type);
            state->readBgra = (fmt == GL_BGRA_EXT && type == GL_UNSIGNED_BYTE);
            state->readFormatQueried = true;
            geode::log::info("FrameGrabber(iOS): glReadPixels capture reads {}", state->readBgra ? "BGRA" : "RGBA (R/B swapped in shader)");
        }

        if (ok) {
            glDisable(GL_BLEND);
            glDisable(GL_SCISSOR_TEST);
            glDisable(GL_DEPTH_TEST);
            glDisable(GL_STENCIL_TEST);
            glDisable(GL_CULL_FACE);
            glDisable(GL_DITHER);
            glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

            // Clearing also avoids a tile load of the target's stale contents on the TBDR GPU.
            glViewport(0, 0, dstW, dstH);
            glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT);

            const DestRect rect = computeAspectFitRect(srcW, srcH, dstW, dstH);
            glViewport(rect.x0, rect.y0, rect.x1 - rect.x0, rect.y1 - rect.y0);
            glUseProgram(state->program);
            glUniform1f(state->swapUniform, (readBack && !state->readBgra) ? 1.0f : 0.0f);
            bindVertexArray(es3, state->vertexArray);
            glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
            bindVertexArray(es3, 0);
        }

        if (ok && readBack) {
            // 3b. Synchronous readback into the pooled buffer (rows are already top-down and BGRA).
            CVPixelBufferRef pb = frame->pixelBuffer.get();
            if (CVPixelBufferLockBaseAddress(pb, 0) == kCVReturnSuccess) {
                auto* dst = static_cast<uint8_t*>(CVPixelBufferGetBaseAddress(pb));
                const size_t dstStride = CVPixelBufferGetBytesPerRow(pb);
                const size_t rowBytes = static_cast<size_t>(dstW) * 4;
                const GLenum readFormat = state->readBgra ? GL_BGRA_EXT : GL_RGBA;
                glPixelStorei(GL_PACK_ALIGNMENT, 4);
                if (dst && dstStride == rowBytes) {
                    glReadPixels(0, 0, dstW, dstH, readFormat, GL_UNSIGNED_BYTE, dst);
                } else if (dst && dstStride > rowBytes) {
                    state->readScratch.resize(rowBytes * static_cast<size_t>(dstH));
                    glReadPixels(0, 0, dstW, dstH, readFormat, GL_UNSIGNED_BYTE, state->readScratch.data());
                    for (int y = 0; y < dstH; ++y) {
                        std::memcpy(dst + static_cast<size_t>(y) * dstStride,
                                    state->readScratch.data() + static_cast<size_t>(y) * rowBytes, rowBytes);
                    }
                } else {
                    ok = false;
                }
                CVPixelBufferUnlockBaseAddress(pb, 0);
            } else {
                ok = false;
            }
        } else if (!readBack) {
            // Detach so the FBO never keeps the pooled IOSurface alive after its texture is released.
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, dstTarget, 0, 0);
        }
    }

    GLsync fence = (ok && !readBack) ? gpuFence(es3) : nullptr;
    saved.restore(es3);

    if (!ok || (!readBack && !fence)) {
        if (dstTexture) {
            CFRelease(dstTexture);
            CVOpenGLESTextureCacheFlush(state->textureCache, 0);
        }
        recycleFrame(std::move(frame));
        return;
    }

    frame->width = dstW;
    frame->height = dstH;
    frame->stride = static_cast<int>(CVPixelBufferGetBytesPerRow(frame->pixelBuffer.get()));
    frame->timestamp = now;
    if (readBack) {
        m_callback(std::move(frame));
    } else {
        state->inFlight.push_back({std::move(frame), dstTexture, fence});
    }
}

} // namespace replay

#endif // GEODE_IS_IOS
