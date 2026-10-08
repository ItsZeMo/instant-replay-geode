#pragma once

#include <Geode/Geode.hpp>
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
