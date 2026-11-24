#pragma once

#include <filesystem>
#include <string>
#include <string_view>

#ifdef GEODE_IS_WINDOWS
#include <Geode/utils/string.hpp>
#endif

namespace replay {

// Converts std::filesystem::path to UTF-8 without touching the narrow Windows ANSI codepage.
inline std::string pathToUtf8(const std::filesystem::path& path) {
#ifdef GEODE_IS_WINDOWS
    return geode::utils::string::wideToUtf8(path.wstring());
#else
    auto u8 = path.u8string();
    return std::string(reinterpret_cast<const char*>(u8.data()), u8.size());
#endif
}

// Converts a UTF-8 path string to std::filesystem::path safely on Windows, macOS, and iOS.
inline std::filesystem::path utf8ToStdPath(std::string_view utf8) {
#ifdef GEODE_IS_WINDOWS
    return std::filesystem::path(geode::utils::string::utf8ToWide(utf8));
#else
    return std::filesystem::path(utf8);
#endif
}

} // namespace replay
