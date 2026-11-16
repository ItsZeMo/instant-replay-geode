#pragma once

#include "platform/cplatform.h"
#include "loader/SettingV3.hpp"
#include "utils/string.hpp"
#include <atomic>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>

namespace cocos2d {
struct CCSize { float width = 0.0f; float height = 0.0f; };
class CCEGLView {
public:
    static CCEGLView* get() noexcept { return nullptr; }
    CCSize getFrameSize() const noexcept { return {}; }
};
} // namespace cocos2d

namespace geode {

namespace cast {
template <class To, class From>
inline std::shared_ptr<To> typeinfo_pointer_cast(const std::shared_ptr<From>&) noexcept {
    return nullptr;
}
} // namespace cast

namespace log {
inline std::atomic<int> g_warnCount{0};
inline std::atomic<int> g_errorCount{0};

template <class... Args>
inline void debug(std::string_view, Args&&...) {}
template <class... Args>
inline void info(std::string_view, Args&&...) {}
template <class... Args>
inline void warn(std::string_view, Args&&...) {
    g_warnCount.fetch_add(1, std::memory_order_relaxed);
}
template <class... Args>
inline void error(std::string_view, Args&&...) {
    g_errorCount.fetch_add(1, std::memory_order_relaxed);
}
} // namespace log

class Mod {
public:
    static Mod* get() noexcept { return nullptr; }
    template <class T>
    T getSettingValue(std::string_view) const { return T{}; }
    template <class T>
    void setSettingValue(std::string_view, const T&) {}
    template <class T>
    T getSavedValue(std::string_view, T def = T{}) const { return def; }
    template <class T>
    void setSavedValue(std::string_view, const T&) {}
    bool saveData() { return true; }
    std::shared_ptr<SettingV3> getSetting(std::string_view) const { return nullptr; }
};

namespace dirs {
inline std::filesystem::path getTempDir() {
    return std::filesystem::temp_directory_path();
}
inline std::filesystem::path getSaveDir() {
    return std::filesystem::temp_directory_path() / "InstantReplayHostTestSave";
}
inline std::filesystem::path getGameDir() {
    return std::filesystem::temp_directory_path();
}
} // namespace dirs

} // namespace geode
