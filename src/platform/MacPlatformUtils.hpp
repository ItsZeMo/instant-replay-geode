#pragma once

#include <Geode/platform/cplatform.h>

#ifdef GEODE_IS_MACOS

#include <filesystem>

#include "AppleAutoreleasePool.hpp"

namespace replay {

// Returns ~/Movies/Geometry Dash via Cocoa NSFileManager / NSSearchPathForDirectoriesInDomains.
std::filesystem::path getMacDefaultVideosDirectory();

// Returns NSTemporaryDirectory() / "GeometryDash_InstantReplay".
std::filesystem::path getMacDefaultTempDirectory();

// True if a POSIX process with `pid` currently exists and matches this application binary
// (guards against PID reuse of orphaned pid_<PID> temp directories after a crash).
bool isMacProcessAlive(int pid) noexcept;
bool isMacSameAppProcessAlive(int pid) noexcept;

// Queries the actual pixel dimensions of Geometry Dash's NSOpenGLView backing surface
// (accounting for Retina wantsBestResolutionOpenGLSurface). Must be called on the render thread.
void getMacOpenGLDrawableSize(int& outWidth, int& outHeight);

// Installs an Objective-C method replacement on -[NSOpenGLContext flushBuffer] (running right before
// presentation without patching AppKit shared-cache code pages) that invokes `onBeforeFlush()` when
// `shouldCapture` is null or true and the context belongs to Geometry Dash's main EAGLView.
// Returns true on success (or if already installed).
bool installMacGLCaptureHook(void (*onBeforeFlush)(), bool (*shouldCapture)() = nullptr);
bool isMacGLCaptureHookInstalled() noexcept;

// Disables the flushBuffer callback cleanly during shutdown.
void disableMacGLCaptureHook() noexcept;

} // namespace replay

#endif // GEODE_IS_MACOS
