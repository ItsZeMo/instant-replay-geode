#pragma once

#include <Geode/Geode.hpp>

#ifdef GEODE_IS_WINDOWS

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

#endif // GEODE_IS_WINDOWS
