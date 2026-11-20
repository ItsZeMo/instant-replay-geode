#pragma once

#include "platform/cplatform.h"
#include "loader/SettingV3.hpp"
#include "utils/string.hpp"
#include <atomic>
#include <cstdio>
#include <sstream>
#include <type_traits>
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

namespace detail {
template <class T>
inline std::string toText(const T& value) {
    using D = std::decay_t<T>;
    if constexpr (std::is_same_v<D, std::string> || std::is_same_v<D, std::string_view>) {
        return std::string(value);
    } else if constexpr (std::is_same_v<D, const char*> || std::is_same_v<D, char*>) {
        return value ? std::string(value) : std::string("(null)");
    } else if constexpr (std::is_same_v<D, bool>) {
        return value ? "true" : "false";
    } else if constexpr (std::is_enum_v<D>) {
        return std::to_string(static_cast<long long>(value));
    } else if constexpr (std::is_arithmetic_v<D>) {
        std::ostringstream out;
        out << value;
        return out.str();
    } else if constexpr (std::is_pointer_v<D>) {
        std::ostringstream out;
        out << static_cast<const void*>(value);
        return out.str();
    } else if constexpr (std::is_same_v<D, std::filesystem::path>) {
        return value.string();
    } else {
        return "?";
    }
}

// Prints "[level] message" with every {...} placeholder replaced by the next argument when the test
// build defines IR_MOCK_LOG_PRINT (the iOS Simulator harness); silent otherwise (macOS host tests).
template <class... Args>
inline void emit([[maybe_unused]] const char* level, [[maybe_unused]] std::string_view fmt, [[maybe_unused]] Args&&... args) {
#ifdef IR_MOCK_LOG_PRINT
    const std::string values[] = {toText(args)..., std::string()};
    std::string out;
    size_t next = 0;
    for (size_t i = 0; i < fmt.size(); ++i) {
        if (fmt[i] == '{') {
            size_t close = fmt.find('}', i);
            if (close != std::string_view::npos) {
                out += (next < sizeof...(Args)) ? values[next] : std::string("{}");
                ++next;
                i = close;
                continue;
            }
        }
        out += fmt[i];
    }
    std::printf("[%s] %s\n", level, out.c_str());
    std::fflush(stdout);
#endif
}
} // namespace detail

template <class... Args>
inline void debug(std::string_view fmt, Args&&... args) {
    detail::emit("debug", fmt, std::forward<Args>(args)...);
}
template <class... Args>
inline void info(std::string_view fmt, Args&&... args) {
    detail::emit("info", fmt, std::forward<Args>(args)...);
}
template <class... Args>
inline void warn(std::string_view fmt, Args&&... args) {
    g_warnCount.fetch_add(1, std::memory_order_relaxed);
    detail::emit("warn", fmt, std::forward<Args>(args)...);
}
template <class... Args>
inline void error(std::string_view fmt, Args&&... args) {
    g_errorCount.fetch_add(1, std::memory_order_relaxed);
    detail::emit("error", fmt, std::forward<Args>(args)...);
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

// UI stand-ins: the iOS harness prints notifications instead of showing them.
enum class NotificationIcon { None, Loading, Success, Warning, Error, Info };
class Notification {
public:
    static Notification* create(std::string const& text, NotificationIcon = NotificationIcon::None, float = 1.0f) {
        static Notification s_instance;
        s_instance.m_text = text;
        return &s_instance;
    }
    void show() { log::detail::emit("notification", m_text); }

private:
    std::string m_text;
};

template <class F>
inline void queueInMainThread(F&& func) {
    func();
}

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
