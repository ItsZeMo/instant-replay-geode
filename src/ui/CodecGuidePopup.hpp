#pragma once

#include <Geode/Geode.hpp>

#ifdef GEODE_IS_WINDOWS

#include <Geode/ui/Popup.hpp>

namespace replay {

class CodecGuidePopup : public geode::Popup {
public:
    static CodecGuidePopup* create();

protected:
    bool init(float width, float height);
    void setup();
};

} // namespace replay

#endif // GEODE_IS_WINDOWS
