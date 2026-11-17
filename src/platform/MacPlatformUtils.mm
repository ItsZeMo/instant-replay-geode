#include "MacPlatformUtils.hpp"

#ifdef GEODE_IS_MACOS

#include <Geode/Geode.hpp>
#include <Geode/utils/ObjcHook.hpp>
#include <Geode/cocos/platform/mac/CCEGLView.h>
#pragma push_macro("CommentType")
#undef CommentType
#define CommentType AppleScriptCommentTypeDummy
#import <Cocoa/Cocoa.h>
#import <Foundation/Foundation.h>
#import <OpenGL/OpenGL.h>
#import <OpenGL/gl.h>
#undef CommentType
#pragma pop_macro("CommentType")
#include <objc/message.h>
#include <objc/runtime.h>
#include <libproc.h>
#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <mutex>
#include <signal.h>
#include <unistd.h>

namespace replay {

namespace {
std::atomic<void (*)()> g_onBeforeFlushCallback{nullptr};
std::atomic<bool (*)()> g_shouldCaptureCallback{nullptr};
std::atomic<bool> g_captureHookEnabled{false};
std::atomic<void*> g_origFlushBuffer{nullptr};
std::atomic<bool> g_hookInstalled{false};
std::mutex g_hookInstallMutex;
} // namespace

std::filesystem::path getMacDefaultVideosDirectory() {
    @autoreleasepool {
        NSArray<NSString*>* paths = NSSearchPathForDirectoriesInDomains(NSMoviesDirectory, NSUserDomainMask, YES);
        if (paths.count > 0 && paths[0].length > 0) {
            return std::filesystem::path(paths[0].UTF8String) / "Geometry Dash";
        }
        if (const char* home = std::getenv("HOME")) {
            return std::filesystem::path(home) / "Movies" / "Geometry Dash";
        }
        return {};
    }
}

std::filesystem::path getMacDefaultTempDirectory() {
    @autoreleasepool {
        NSString* tmp = NSTemporaryDirectory();
        if (tmp && tmp.length > 0) {
            return std::filesystem::path(tmp.UTF8String) / "GeometryDash_InstantReplay";
        }
        if (const char* tmpEnv = std::getenv("TMPDIR")) {
            return std::filesystem::path(tmpEnv) / "GeometryDash_InstantReplay";
        }
        return std::filesystem::path("/tmp/GeometryDash_InstantReplay");
    }
}

bool isMacProcessAlive(int pid) noexcept {
    if (pid <= 0) return false;
    if (::kill(static_cast<pid_t>(pid), 0) == 0) return true;
    return errno == EPERM;
}

bool isMacSameAppProcessAlive(int pid) noexcept {
    if (!isMacProcessAlive(pid)) return false;

    char selfPath[PROC_PIDPATHINFO_MAXSIZE] = {};
    char otherPath[PROC_PIDPATHINFO_MAXSIZE] = {};
    int selfLen = proc_pidpath(::getpid(), selfPath, sizeof(selfPath));
    int otherLen = proc_pidpath(static_cast<pid_t>(pid), otherPath, sizeof(otherPath));
    if (selfLen <= 0 || otherLen <= 0) {
        return false; // EPERM (another user/system daemon) or exited concurrently -> safe to clean
    }

    std::filesystem::path selfName = std::filesystem::path(selfPath).filename();
    std::filesystem::path otherName = std::filesystem::path(otherPath).filename();
    return !selfName.empty() && selfName == otherName;
}

static id getSharedEAGLView() {
    static Class eaglClass = objc_getClass("EAGLView");
    if (!eaglClass) return nil;
    SEL sharedSel = sel_registerName("sharedEGLView");
    if (![eaglClass respondsToSelector:sharedSel]) return nil;
    return reinterpret_cast<id (*)(Class, SEL)>(objc_msgSend)(eaglClass, sharedSel);
}

void getMacOpenGLDrawableSize(int& outWidth, int& outHeight) {
    outWidth = 0;
    outHeight = 0;
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    // Note (N11): GD runs its Cocos loop on CVDisplayLink rather than AppKit's main thread, so
    // [NSThread isMainThread] is normally NO here and resolution falls through to GL_VIEWPORT below.
    if ([NSThread isMainThread]) {
        @autoreleasepool {
            id eaglView = getSharedEAGLView();
            if (eaglView && [eaglView isKindOfClass:[NSView class]]) {
                NSView* nsView = static_cast<NSView*>(eaglView);
                NSRect bounds = [nsView bounds];
                NSSize sz = bounds.size;
                if ([nsView wantsBestResolutionOpenGLSurface]) {
                    sz = [nsView convertRectToBacking:bounds].size;
                }
                outWidth = static_cast<int>(sz.width + 0.5);
                outHeight = static_cast<int>(sz.height + 0.5);
            }
        }
    }
    if ((outWidth <= 0 || outHeight <= 0) && CGLGetCurrentContext() != nullptr) {
        GLint curDrawFbo = 0;
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &curDrawFbo);
        if (curDrawFbo == 0) {
            GLint vp[4] = {0, 0, 0, 0};
            glGetIntegerv(GL_VIEWPORT, vp);
            if (vp[2] >= 16 && vp[3] >= 16) {
                outWidth = vp[2];
                outHeight = vp[3];
            }
        }
    }
    if (outWidth <= 0 || outHeight <= 0) {
        // Do not fall back to CCEGLView::getFrameSize() on macOS (it reports point-halved sizes such as
        // 720x450 for a 1440x900 OpenGL drawable, N7). Leave 0x0 so FrameGrabber resolves on render thread.
        geode::log::debug(
            "Instant Replay: getMacOpenGLDrawableSize deferred (cglCtx={}, mainThread={})",
            CGLGetCurrentContext() != nullptr,
            [NSThread isMainThread] == YES);
    }
#pragma clang diagnostic pop
}

static void nsOpenGLContextFlushBufferHook(NSOpenGLContext* self, SEL sel) {
    if (g_captureHookEnabled.load(std::memory_order_acquire)) {
        auto shouldCapture = g_shouldCaptureCallback.load(std::memory_order_acquire);
        if (!shouldCapture || shouldCapture()) {
            if (auto cb = g_onBeforeFlushCallback.load(std::memory_order_acquire)) {
                @autoreleasepool {
                    id eaglView = getSharedEAGLView();
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
                    id ctxView = self ? [self view] : nil;
                    CGLContextObj targetCtx = self ? [self CGLContextObj] : nullptr;
                    if (self && eaglView && ctxView == eaglView && targetCtx) {
                        CGLContextObj prevCtx = CGLGetCurrentContext();
                        CGLLockContext(targetCtx);
                        bool switched = (prevCtx != targetCtx);
                        if (switched) {
                            static std::atomic<bool> s_loggedSwitch{false};
                            if (!s_loggedSwitch.exchange(true, std::memory_order_relaxed)) {
                                geode::log::debug(
                                    "Instant Replay: switching CGL context {} -> {} during flushBuffer capture",
                                    static_cast<const void*>(prevCtx), static_cast<const void*>(targetCtx));
                            }
                            CGLSetCurrentContext(targetCtx);
                        }
                        cb();
                        if (switched) {
                            CGLSetCurrentContext(prevCtx);
                        }
                        CGLUnlockContext(targetCtx);
                    }
#pragma clang diagnostic pop
                }
            }
        }
    }
    if (void* orig = g_origFlushBuffer.load(std::memory_order_acquire)) {
        reinterpret_cast<void (*)(NSOpenGLContext*, SEL)>(orig)(self, sel);
    }
}

bool installMacGLCaptureHook(void (*onBeforeFlush)(), bool (*shouldCapture)()) {
    std::lock_guard<std::mutex> lock(g_hookInstallMutex);
    g_onBeforeFlushCallback.store(onBeforeFlush, std::memory_order_release);
    g_shouldCaptureCallback.store(shouldCapture, std::memory_order_release);
    g_captureHookEnabled.store(true, std::memory_order_release);

    if (g_hookInstalled.load(std::memory_order_acquire)) {
        return true;
    }

    // Seed original IMP before replacing the ObjC method table entry so any concurrent
    // CVDisplayLink flushBuffer call immediately after method_setImplementation sees a non-null IMP.
    // Using replaceObjcMethod (method_setImplementation) avoids TulipHook inline-patching AppKit's
    // dyld_shared_cache page, which strips VM_PROT_EXECUTE and crashes with SIGBUS on arm64.
    if (auto curImpRes = geode::hook::getObjcMethodImp("NSOpenGLContext", "flushBuffer")) {
        void* curImp = curImpRes.unwrap();
        if (curImp && curImp != reinterpret_cast<void*>(&nsOpenGLContextFlushBufferHook)) {
            g_origFlushBuffer.store(curImp, std::memory_order_release);
        }
    }

    if (auto replaceRes = geode::hook::replaceObjcMethod(
            "NSOpenGLContext",
            "flushBuffer",
            reinterpret_cast<void*>(&nsOpenGLContextFlushBufferHook))) {
        void* prevImp = replaceRes.unwrap();
        if (prevImp && prevImp != reinterpret_cast<void*>(&nsOpenGLContextFlushBufferHook)) {
            g_origFlushBuffer.store(prevImp, std::memory_order_release);
        }
        g_hookInstalled.store(true, std::memory_order_release);
        return true;
    } else {
        geode::log::error("Instant Replay: failed to swizzle -[NSOpenGLContext flushBuffer]: {}", replaceRes.unwrapErr());
        g_hookInstalled.store(false, std::memory_order_release);
        return false;
    }
}

bool isMacGLCaptureHookInstalled() noexcept {
    return g_hookInstalled.load(std::memory_order_acquire);
}

void disableMacGLCaptureHook() noexcept {
    g_captureHookEnabled.store(false, std::memory_order_release);
}

} // namespace replay

#endif // GEODE_IS_MACOS
