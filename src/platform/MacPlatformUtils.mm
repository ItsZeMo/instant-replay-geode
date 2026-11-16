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
#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <signal.h>

namespace replay {

namespace {
std::atomic<void (*)()> g_onBeforeFlushCallback{nullptr};
std::atomic<bool (*)()> g_shouldCaptureCallback{nullptr};
std::atomic<bool> g_captureHookEnabled{false};
std::atomic<void*> g_origFlushBuffer{nullptr};
std::atomic<bool> g_hookInstalled{false};
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
        if (auto* view = cocos2d::CCEGLView::get()) {
            auto fs = view->getFrameSize();
            float scale = 1.0f;
            @autoreleasepool {
                id eaglView = getSharedEAGLView();
                if (eaglView && [eaglView isKindOfClass:[NSView class]] &&
                    [static_cast<NSView*>(eaglView) wantsBestResolutionOpenGLSurface]) {
                    float f = geode::utils::getDisplayFactor();
                    if (f > 1.0f) scale = f;
                }
            }
            outWidth = static_cast<int>(fs.width * scale + 0.5f);
            outHeight = static_cast<int>(fs.height * scale + 0.5f);
        }
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
                    if (self && eaglView && ctxView == eaglView) {
                        if (CGLGetCurrentContext() != [self CGLContextObj]) {
                            [self makeCurrentContext];
                        }
                        cb();
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

void installMacGLCaptureHook(void (*onBeforeFlush)(), bool (*shouldCapture)()) {
    g_onBeforeFlushCallback.store(onBeforeFlush, std::memory_order_release);
    g_shouldCaptureCallback.store(shouldCapture, std::memory_order_release);
    g_captureHookEnabled.store(true, std::memory_order_release);

    bool expected = false;
    if (!g_hookInstalled.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        return;
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
    } else {
        geode::log::error("Instant Replay: failed to swizzle -[NSOpenGLContext flushBuffer]: {}", replaceRes.unwrapErr());
    }
}

void disableMacGLCaptureHook() noexcept {
    g_captureHookEnabled.store(false, std::memory_order_release);
}

} // namespace replay

#endif // GEODE_IS_MACOS
