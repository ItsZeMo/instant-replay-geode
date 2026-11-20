#pragma once

#include <Geode/platform/cplatform.h>

#if defined(GEODE_IS_MACOS) || defined(GEODE_IS_IOS)

extern "C" void* objc_autoreleasePoolPush(void);
extern "C" void objc_autoreleasePoolPop(void*);

namespace replay {

// RAII autorelease pool for C++ worker/background threads on macOS and iOS: AVFoundation and
// VideoToolbox autorelease objects that would otherwise pile up on threads without a run loop.
class AppleAutoreleasePoolScope {
public:
    AppleAutoreleasePoolScope() noexcept : m_pool(objc_autoreleasePoolPush()) {}
    ~AppleAutoreleasePoolScope() noexcept {
        if (m_pool) {
            objc_autoreleasePoolPop(m_pool);
            m_pool = nullptr;
        }
    }
    AppleAutoreleasePoolScope(const AppleAutoreleasePoolScope&) = delete;
    AppleAutoreleasePoolScope& operator=(const AppleAutoreleasePoolScope&) = delete;

private:
    void* m_pool = nullptr;
};

// Name used throughout the macOS port; the pool is identical on iOS.
using MacAutoreleasePoolScope = AppleAutoreleasePoolScope;

} // namespace replay

#endif // GEODE_IS_MACOS || GEODE_IS_IOS
