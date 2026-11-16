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
std::atomic<bool> g_captureHookEnabled{false};
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
#pragma clang diagnostic pop
    if (outWidth <= 0 || outHeight <= 0) {
        if (auto* view = cocos2d::CCEGLView::get()) {
            auto fs = view->getFrameSize();
            float factor = geode::utils::getDisplayFactor();
            if (factor <= 0.0f) factor = 1.0f;
            outWidth = static_cast<int>(fs.width * factor + 0.5f);
            outHeight = static_cast<int>(fs.height * factor + 0.5f);
        }
    }
    if (outWidth <= 0 || outHeight <= 0) {
        GLint vp[4] = {0, 0, 0, 0};
        glGetIntegerv(GL_VIEWPORT, vp);
        outWidth = vp[2];
        outHeight = vp[3];
    }
}

static void nsOpenGLContextFlushBufferHook(NSOpenGLContext* self, SEL sel) {
    if (g_captureHookEnabled.load(std::memory_order_acquire)) {
        if (auto cb = g_onBeforeFlushCallback.load(std::memory_order_acquire)) {
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
    reinterpret_cast<void (*)(id, SEL)>(objc_msgSend)(self, sel);
}

void installMacGLCaptureHook(void (*onBeforeFlush)()) {
    g_onBeforeFlushCallback.store(onBeforeFlush, std::memory_order_release);
    g_captureHookEnabled.store(true, std::memory_order_release);

    static bool s_hookInstalled = false;
    if (s_hookInstalled) return;
    s_hookInstalled = true;

    if (auto hookRes = geode::ObjcHook::create("NSOpenGLContext", "flushBuffer", &nsOpenGLContextFlushBufferHook)) {
        auto hook = std::move(hookRes).unwrap();
        hook->setPriority(100000);
        (void)geode::Mod::get()->claimHook(std::move(hook));
    } else {
        geode::log::error("Instant Replay: failed to hook -[NSOpenGLContext flushBuffer]: {}", hookRes.unwrapErr());
    }
}

void disableMacGLCaptureHook() noexcept {
    g_captureHookEnabled.store(false, std::memory_order_release);
}

} // namespace replay

#endif // GEODE_IS_MACOS
