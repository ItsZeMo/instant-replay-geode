#pragma once

#include <Geode/Geode.hpp>

#if defined(GEODE_IS_WINDOWS) || defined(GEODE_IS_MACOS) || defined(GEODE_IS_IOS)

#include <Geode/ui/Popup.hpp>

namespace replay {

class CodecGuidePopup : public geode::Popup {
public:
    static CodecGuidePopup* create();
    void show() override;

protected:
    bool init(float width, float height);
    void setup();
};

} // namespace replay

#endif // GEODE_IS_WINDOWS || GEODE_IS_MACOS || GEODE_IS_IOS
