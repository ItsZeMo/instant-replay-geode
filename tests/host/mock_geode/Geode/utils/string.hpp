#pragma once
#include <filesystem>
#include <string>

namespace geode::utils::string {
inline std::string pathToString(const std::filesystem::path& p) {
    auto u8 = p.u8string();
    return std::string(reinterpret_cast<const char*>(u8.data()), u8.size());
}
} // namespace geode::utils::string
