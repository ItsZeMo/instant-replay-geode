#pragma once
// Host-test stand-in for Geode's platform detection: macOS host tests and the iOS Simulator harness.
#include <TargetConditionals.h>
#if TARGET_OS_IPHONE
#ifndef GEODE_IS_IOS
#define GEODE_IS_IOS
#endif
#ifndef GEODE_PLATFORM_NAME
#define GEODE_PLATFORM_NAME "iOS"
#endif
#else
#ifndef GEODE_IS_MACOS
#define GEODE_IS_MACOS
#endif
#ifndef GEODE_PLATFORM_NAME
#define GEODE_PLATFORM_NAME "MacOS"
#endif
#endif
