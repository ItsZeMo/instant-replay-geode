#pragma once

#include <Geode/Geode.hpp>

#if defined(GEODE_IS_WINDOWS) || defined(GEODE_IS_MACOS)

#include <string>

namespace replay {

class ReplayNotification : public cocos2d::CCNode {
public:
    static void show(const std::string& filePath, double durationSeconds);

private:
    static ReplayNotification* create(const std::string& filePath, double durationSeconds);
    bool init(const std::string& filePath, double durationSeconds);
    void animateIn();
    void animateOut();
};

} // namespace replay

#endif // GEODE_IS_WINDOWS || GEODE_IS_MACOS
