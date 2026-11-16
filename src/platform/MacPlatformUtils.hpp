#pragma once

#include <Geode/platform/cplatform.h>

#ifdef GEODE_IS_MACOS

#include <filesystem>

extern "C" void* objc_autoreleasePoolPush(void);
extern "C" void objc_autoreleasePoolPop(void*);

namespace replay {

// Returns ~/Movies/Geometry Dash via Cocoa NSFileManager / NSSearchPathForDirectoriesInDomains.
std::filesystem::path getMacDefaultVideosDirectory();

// Returns NSTemporaryDirectory() / "GeometryDash_InstantReplay".
std::filesystem::path getMacDefaultTempDirectory();

// True if a POSIX process with `pid` currently exists.
bool isMacProcessAlive(int pid) noexcept;

// Queries the actual pixel dimensions of Geometry Dash's NSOpenGLView backing surface
// (accounting for Retina wantsBestResolutionOpenGLSurface). Must be called on the render thread.
void getMacOpenGLDrawableSize(int& outWidth, int& outHeight);

// Installs an Objective-C method replacement on -[NSOpenGLContext flushBuffer] (running right before
// presentation without patching AppKit shared-cache code pages) that invokes `onBeforeFlush()` when
// `shouldCapture` is null or true and the context belongs to Geometry Dash's main EAGLView.
void installMacGLCaptureHook(void (*onBeforeFlush)(), bool (*shouldCapture)() = nullptr);

// Disables the flushBuffer callback cleanly during shutdown.
void disableMacGLCaptureHook() noexcept;

// RAII autorelease pool wrapper for C++ worker/background threads on macOS.
class MacAutoreleasePoolScope {
public:
    MacAutoreleasePoolScope() noexcept : m_pool(objc_autoreleasePoolPush()) {}
    ~MacAutoreleasePoolScope() noexcept {
        if (m_pool) {
            objc_autoreleasePoolPop(m_pool);
            m_pool = nullptr;
        }
    }
    MacAutoreleasePoolScope(const MacAutoreleasePoolScope&) = delete;
    MacAutoreleasePoolScope& operator=(const MacAutoreleasePoolScope&) = delete;

private:
    void* m_pool = nullptr;
};

} // namespace replay

#endif // GEODE_IS_MACOS
