#pragma once
#include <cstdint>
#include <optional>
namespace geode {
class SettingV3 {};
class IntSettingV3 : public SettingV3 {
public:
    std::optional<int64_t> getMinValue() const { return std::nullopt; }
    std::optional<int64_t> getMaxValue() const { return std::nullopt; }
};
class FloatSettingV3 : public SettingV3 {
public:
    std::optional<double> getMinValue() const { return std::nullopt; }
    std::optional<double> getMaxValue() const { return std::nullopt; }
};
} // namespace geode
