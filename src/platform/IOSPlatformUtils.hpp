#pragma once

#include <Geode/platform/cplatform.h>

#ifdef GEODE_IS_IOS

#include "AppleAutoreleasePool.hpp"
#include <filesystem>
#include <memory>

namespace replay {

// <app Documents>/Instant Replay: shows up in the Files app next to Geode's "game" and "save" folders.
std::filesystem::path getIOSDefaultVideosDirectory();

// NSTemporaryDirectory() / "GeometryDash_InstantReplay" (purged by iOS while the game is not running).
std::filesystem::path getIOSDefaultTempDirectory();

// Backing pixel size of Geometry Dash's EAGLView (CAEAGLLayer renderbuffer, Retina scale applied).
// Cocos runs on the UIKit main thread on iOS, so this is only called there. 0x0 when unknown.
void getIOSGLDrawableSize(int& outWidth, int& outHeight);

// Installs an Objective-C method replacement on -[EAGLContext presentRenderbuffer:] (no code patching,
// so it also works in Geode's JIT-less mode). `onBeforePresent` runs right before Geometry Dash's frame is
// presented, with GD's EAGLContext current and the presented renderbuffer bound to GL_RENDERBUFFER, after
// every other mod has drawn its overlays. Returns true on success (or if already installed).
bool installIOSGLCaptureHook(void (*onBeforePresent)(), bool (*shouldCapture)() = nullptr);
bool isIOSGLCaptureHookInstalled() noexcept;
void disableIOSGLCaptureHook() noexcept;

// Unretained pointer of the current thread's EAGLContext (nullptr if none) and its API version (2 or 3).
void* getIOSCurrentGLContext() noexcept;
int getIOSCurrentGLContextAPI() noexcept;

// Registers UIApplication lifecycle observers (callbacks run on the main thread):
// `onSuspend` on UIApplicationWillResignActive (home gesture, app switcher, Control Center, calls) and
// `onResume` on UIApplicationDidBecomeActive. VideoToolbox's hardware encoder is not available to
// background apps, so recording closes its segment before the app can be suspended.
void installIOSAppLifecycleObservers(void (*onSuspend)(), void (*onResume)());
bool isIOSAppActive() noexcept;

// Asks iOS for extra background execution time while a segment is finalized or a replay is merged.
// Safe to create from any thread; ends the task when destroyed or when iOS expires it.
class IOSBackgroundTaskScope {
public:
    explicit IOSBackgroundTaskScope(const char* name) noexcept;
    ~IOSBackgroundTaskScope();
    IOSBackgroundTaskScope(const IOSBackgroundTaskScope&) = delete;
    IOSBackgroundTaskScope& operator=(const IOSBackgroundTaskScope&) = delete;

    struct State;

private:
    std::shared_ptr<State> m_state;
};

// Info.plist lookup that also checks the bundle of the running executable: launchers that run GD inside
// their own app (LiveContainer based) are what iOS privacy checks see, not GD's own Info.plist.
bool iosHostHasInfoKey(const char* key) noexcept;

// Presents the system share sheet (AirDrop, Save to Files, Photos when allowed, messengers) for `file`.
// Must be called on the main thread. Returns false when no view controller is available.
bool presentIOSShareSheet(const std::filesystem::path& file);

// "iPad" or "iPhone" for UI strings.
const char* iosDeviceKindName() noexcept;

} // namespace replay

#endif // GEODE_IS_IOS
