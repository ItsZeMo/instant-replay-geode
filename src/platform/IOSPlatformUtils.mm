#include "IOSPlatformUtils.hpp"

#ifdef GEODE_IS_IOS

#include <Geode/Geode.hpp>
#include <Geode/utils/ObjcHook.hpp>

#import <Foundation/Foundation.h>
#import <OpenGLES/EAGL.h>
#import <OpenGLES/ES2/gl.h>
#import <UIKit/UIKit.h>

#include <mach-o/dyld.h>
#include <objc/message.h>
#include <objc/runtime.h>

#include <atomic>
#include <cstdlib>
#include <mutex>
#include <string>
#include <vector>

namespace replay {

namespace {
std::atomic<void (*)()> g_onBeforePresentCallback{nullptr};
std::atomic<bool (*)()> g_shouldCaptureCallback{nullptr};
std::atomic<bool> g_captureHookEnabled{false};
std::atomic<void*> g_origPresentRenderbuffer{nullptr};
std::atomic<bool> g_hookInstalled{false};
std::mutex g_hookInstallMutex;

std::atomic<void (*)()> g_onSuspendCallback{nullptr};
std::atomic<void (*)()> g_onResumeCallback{nullptr};
std::atomic<bool> g_lifecycleInstalled{false};

// GD's cocos2d-x EAGLView (UIView backed by a CAEAGLLayer). Resolved at runtime: the class lives in the
// game binary, not in a framework the mod links against.
id getSharedEAGLView() {
    static Class eaglClass = objc_getClass("EAGLView");
    if (!eaglClass) return nil;
    SEL sharedSel = sel_registerName("sharedEGLView");
    if (![eaglClass respondsToSelector:sharedSel]) return nil;
    return reinterpret_cast<id (*)(Class, SEL)>(objc_msgSend)(eaglClass, sharedSel);
}

EAGLContext* getGameEAGLContext() {
    id view = getSharedEAGLView();
    if (!view) return nil;
    SEL ctxSel = sel_registerName("context");
    if (![view respondsToSelector:ctxSel]) return nil;
    id ctx = reinterpret_cast<id (*)(id, SEL)>(objc_msgSend)(view, ctxSel);
    return [ctx isKindOfClass:[EAGLContext class]] ? static_cast<EAGLContext*>(ctx) : nil;
}

UIViewController* topViewController() {
    UIWindow* window = nil;
    for (UIScene* scene in [UIApplication sharedApplication].connectedScenes) {
        if (![scene isKindOfClass:[UIWindowScene class]]) continue;
        for (UIWindow* win in static_cast<UIWindowScene*>(scene).windows) {
            if (win.isKeyWindow) {
                window = win;
                break;
            }
            if (!window && win.rootViewController) {
                window = win;
            }
        }
        if (window.isKeyWindow) break;
    }
    UIViewController* vc = window.rootViewController;
    while (vc.presentedViewController && !vc.presentedViewController.isBeingDismissed) {
        vc = vc.presentedViewController;
    }
    return vc;
}

// Bundle directory of the running executable (the launcher app when GD runs inside one).
NSString* executableBundleDirectory() {
    uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    if (size == 0) return nil;
    std::vector<char> buf(static_cast<size_t>(size) + 1, '\0');
    if (_NSGetExecutablePath(buf.data(), &size) != 0) return nil;
    NSString* exe = [[NSString stringWithUTF8String:buf.data()] stringByResolvingSymlinksInPath];
    return exe.length ? [exe stringByDeletingLastPathComponent] : nil;
}

BOOL eaglPresentRenderbufferHook(EAGLContext* self, SEL sel, NSUInteger target) {
    if (g_captureHookEnabled.load(std::memory_order_acquire) && self && target == GL_RENDERBUFFER) {
        auto shouldCapture = g_shouldCaptureCallback.load(std::memory_order_acquire);
        auto cb = g_onBeforePresentCallback.load(std::memory_order_acquire);
        if (cb && (!shouldCapture || shouldCapture())) {
            @autoreleasepool {
                // GL calls need GD's context current on this thread; other contexts (overlay mods, system
                // views) present through the same method and are ignored.
                EAGLContext* gameCtx = getGameEAGLContext();
                if ([EAGLContext currentContext] == self && (gameCtx == nil || gameCtx == self)) {
                    if (gameCtx == nil) {
                        static std::atomic<bool> s_loggedNoView{false};
                        if (!s_loggedNoView.exchange(true, std::memory_order_relaxed)) {
                            geode::log::warn("Instant Replay: EAGLView not resolved, capturing the presenting context {}",
                                             static_cast<const void*>((__bridge void*)self));
                        }
                    }
                    cb();
                }
            }
        }
    }
    if (void* orig = g_origPresentRenderbuffer.load(std::memory_order_acquire)) {
        return reinterpret_cast<BOOL (*)(EAGLContext*, SEL, NSUInteger)>(orig)(self, sel, target);
    }
    return NO;
}

} // namespace

std::filesystem::path getIOSDefaultVideosDirectory() {
    @autoreleasepool {
        NSArray<NSString*>* paths = NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES);
        if (paths.count > 0 && paths[0].length > 0) {
            return std::filesystem::path(paths[0].UTF8String) / "Instant Replay";
        }
        if (const char* home = std::getenv("HOME")) {
            return std::filesystem::path(home) / "Documents" / "Instant Replay";
        }
        return {};
    }
}

std::filesystem::path getIOSDefaultTempDirectory() {
    @autoreleasepool {
        NSString* tmp = NSTemporaryDirectory();
        if (tmp && tmp.length > 0) {
            return std::filesystem::path(tmp.UTF8String) / "GeometryDash_InstantReplay";
        }
        if (const char* tmpEnv = std::getenv("TMPDIR")) {
            return std::filesystem::path(tmpEnv) / "GeometryDash_InstantReplay";
        }
        return {};
    }
}

void getIOSGLDrawableSize(int& outWidth, int& outHeight) {
    outWidth = 0;
    outHeight = 0;
    if (![NSThread isMainThread]) return;
    @autoreleasepool {
        id view = getSharedEAGLView();
        if (!view) return;
        SEL surfaceSel = sel_registerName("surfaceSize");
        if ([view respondsToSelector:surfaceSel]) {
            // arm64 returns CGSize in registers, so plain objc_msgSend is correct here.
            CGSize sz = reinterpret_cast<CGSize (*)(id, SEL)>(objc_msgSend)(view, surfaceSel);
            outWidth = static_cast<int>(sz.width + 0.5);
            outHeight = static_cast<int>(sz.height + 0.5);
        }
        if ((outWidth < 16 || outHeight < 16) && [view isKindOfClass:[UIView class]]) {
            UIView* uiView = static_cast<UIView*>(view);
            CGFloat scale = uiView.contentScaleFactor > 0 ? uiView.contentScaleFactor : 1.0;
            outWidth = static_cast<int>(uiView.bounds.size.width * scale + 0.5);
            outHeight = static_cast<int>(uiView.bounds.size.height * scale + 0.5);
        }
        if (outWidth < 16 || outHeight < 16) {
            outWidth = 0;
            outHeight = 0;
        }
    }
}

bool installIOSGLCaptureHook(void (*onBeforePresent)(), bool (*shouldCapture)()) {
    std::lock_guard<std::mutex> lock(g_hookInstallMutex);
    g_onBeforePresentCallback.store(onBeforePresent, std::memory_order_release);
    g_shouldCaptureCallback.store(shouldCapture, std::memory_order_release);
    g_captureHookEnabled.store(true, std::memory_order_release);

    if (g_hookInstalled.load(std::memory_order_acquire)) {
        return true;
    }

    // Seed the original IMP before swapping the method so a present racing the swap never sees null.
    if (auto curImpRes = geode::hook::getObjcMethodImp("EAGLContext", "presentRenderbuffer:")) {
        void* curImp = curImpRes.unwrap();
        if (curImp && curImp != reinterpret_cast<void*>(&eaglPresentRenderbufferHook)) {
            g_origPresentRenderbuffer.store(curImp, std::memory_order_release);
        }
    }

    auto replaceRes = geode::hook::replaceObjcMethod(
        "EAGLContext", "presentRenderbuffer:", reinterpret_cast<void*>(&eaglPresentRenderbufferHook));
    if (!replaceRes) {
        geode::log::error("Instant Replay: failed to swizzle -[EAGLContext presentRenderbuffer:]: {}", replaceRes.unwrapErr());
        g_hookInstalled.store(false, std::memory_order_release);
        return false;
    }
    void* prevImp = replaceRes.unwrap();
    if (prevImp && prevImp != reinterpret_cast<void*>(&eaglPresentRenderbufferHook)) {
        g_origPresentRenderbuffer.store(prevImp, std::memory_order_release);
    }
    if (!g_origPresentRenderbuffer.load(std::memory_order_acquire)) {
        geode::log::error("Instant Replay: -[EAGLContext presentRenderbuffer:] has no original implementation");
    }
    g_hookInstalled.store(true, std::memory_order_release);
    return true;
}

bool isIOSGLCaptureHookInstalled() noexcept {
    return g_hookInstalled.load(std::memory_order_acquire);
}

void disableIOSGLCaptureHook() noexcept {
    g_captureHookEnabled.store(false, std::memory_order_release);
}

void* getIOSCurrentGLContext() noexcept {
    return (__bridge void*)[EAGLContext currentContext];
}

int getIOSCurrentGLContextAPI() noexcept {
    EAGLContext* ctx = [EAGLContext currentContext];
    if (!ctx) return 0;
    return ctx.API >= kEAGLRenderingAPIOpenGLES3 ? 3 : 2;
}

void installIOSAppLifecycleObservers(void (*onSuspend)(), void (*onResume)()) {
    g_onSuspendCallback.store(onSuspend, std::memory_order_release);
    g_onResumeCallback.store(onResume, std::memory_order_release);
    if (g_lifecycleInstalled.exchange(true)) return;

    @autoreleasepool {
        NSNotificationCenter* nc = [NSNotificationCenter defaultCenter];
        NSOperationQueue* mainQueue = [NSOperationQueue mainQueue];
        void (^suspend)(NSNotification*) = ^(NSNotification*) {
            if (auto cb = g_onSuspendCallback.load(std::memory_order_acquire)) cb();
        };
        void (^resume)(NSNotification*) = ^(NSNotification*) {
            if (auto cb = g_onResumeCallback.load(std::memory_order_acquire)) cb();
        };
        // The observer tokens live for the whole process (mods are never unloaded on iOS).
        static id s_resignToken = [nc addObserverForName:UIApplicationWillResignActiveNotification
                                                  object:nil queue:mainQueue usingBlock:suspend];
        static id s_backgroundToken = [nc addObserverForName:UIApplicationDidEnterBackgroundNotification
                                                      object:nil queue:mainQueue usingBlock:suspend];
        static id s_activeToken = [nc addObserverForName:UIApplicationDidBecomeActiveNotification
                                                  object:nil queue:mainQueue usingBlock:resume];
        (void)s_resignToken;
        (void)s_backgroundToken;
        (void)s_activeToken;
    }
}

bool isIOSAppActive() noexcept {
    if (![NSThread isMainThread]) return true;
    return [UIApplication sharedApplication].applicationState == UIApplicationStateActive;
}

struct IOSBackgroundTaskScope::State {
    std::atomic<UIBackgroundTaskIdentifier> taskId{UIBackgroundTaskInvalid};
    std::string name;

    void end() noexcept {
        UIBackgroundTaskIdentifier tid = taskId.exchange(UIBackgroundTaskInvalid);
        if (tid != UIBackgroundTaskInvalid) {
            [[UIApplication sharedApplication] endBackgroundTask:tid];
        }
    }
};

IOSBackgroundTaskScope::IOSBackgroundTaskScope(const char* name) noexcept {
    @autoreleasepool {
        UIApplication* app = [UIApplication sharedApplication];
        if (!app) return;
        auto state = std::make_shared<State>();
        state->name = name ? name : "InstantReplay";
        // beginBackgroundTask / endBackgroundTask may be called from any thread. The block keeps the
        // state alive until iOS either expires the task or we end it.
        UIBackgroundTaskIdentifier tid = [app beginBackgroundTaskWithName:@(state->name.c_str()) expirationHandler:^{
            geode::log::warn("Instant Replay: iOS background time for '{}' expired", state->name);
            state->end();
        }];
        if (tid == UIBackgroundTaskInvalid) return;
        state->taskId.store(tid);
        m_state = std::move(state);
    }
}

IOSBackgroundTaskScope::~IOSBackgroundTaskScope() {
    if (m_state) {
        m_state->end();
    }
}

bool iosHostHasInfoKey(const char* key) noexcept {
    if (!key || !key[0]) return false;
    @autoreleasepool {
        NSString* k = @(key);
        NSString* dir = executableBundleDirectory();
        if (dir.length) {
            NSDictionary* info = [NSDictionary dictionaryWithContentsOfFile:[dir stringByAppendingPathComponent:@"Info.plist"]];
            if (info) {
                return info[k] != nil;
            }
        }
        // Executable bundle unreadable: fall back to the main bundle (the game itself when not containerized).
        return [[NSBundle mainBundle] objectForInfoDictionaryKey:k] != nil;
    }
}

bool presentIOSShareSheet(const std::filesystem::path& file) {
    if (![NSThread isMainThread]) return false;
    @autoreleasepool {
        UIViewController* vc = topViewController();
        if (!vc) return false;

        NSURL* url = [NSURL fileURLWithPath:@(file.c_str())];
        UIActivityViewController* sheet = [[UIActivityViewController alloc] initWithActivityItems:@[url]
                                                                            applicationActivities:nil];
        // "Save Video" writes to Photos through this process: without a Photos usage description iOS
        // terminates the app, so hide it unless the host app declares one (Save to Files still works).
        if (!iosHostHasInfoKey("NSPhotoLibraryAddUsageDescription") && !iosHostHasInfoKey("NSPhotoLibraryUsageDescription")) {
            sheet.excludedActivityTypes = @[UIActivityTypeSaveToCameraRoll];
        }
        if (UIPopoverPresentationController* popover = sheet.popoverPresentationController) {
            // iPad presents the sheet as a popover, which needs an anchor.
            popover.sourceView = vc.view;
            CGRect b = vc.view.bounds;
            popover.sourceRect = CGRectMake(CGRectGetMidX(b), CGRectGetMidY(b), 1.0, 1.0);
            popover.permittedArrowDirections = 0;
        }
        geode::log::debug("Instant Replay: presenting the share sheet from {}", NSStringFromClass([vc class]).UTF8String);
        [vc presentViewController:sheet animated:YES completion:nil];
        return true;
    }
}

const char* iosDeviceKindName() noexcept {
    static const char* s_kind = []() -> const char* {
        @autoreleasepool {
            return [UIDevice currentDevice].userInterfaceIdiom == UIUserInterfaceIdiomPad ? "iPad" : "iPhone";
        }
    }();
    return s_kind;
}

} // namespace replay

#endif // GEODE_IS_IOS
