#pragma once

#include <Geode/Geode.hpp>
#include <algorithm>

namespace replay {

// Geometry Dash's window is always 320 points tall and 320 x aspect wide, so 4:3 iPads (~427 pt) and
// 5:4 / 4:3 monitors are narrower than the dashboard. Call right after FLAlertLayer::show(): it re-runs
// the open animation towards a scale at which the whole popup fits (no-op when it already fits).
inline void fitPopupToWindow(FLAlertLayer* popup, float margin = 12.0f) {
    if (!popup || !popup->m_mainLayer) return;
    const auto win = cocos2d::CCDirector::sharedDirector()->getWinSize();
    const auto size = popup->m_mainLayer->getContentSize();
    if (size.width <= 0.0f || size.height <= 0.0f) return;
    const float fit = std::min({1.0f, (win.width - margin) / size.width, (win.height - margin) / size.height});
    if (fit >= 0.999f) return;
    auto* layer = popup->m_mainLayer;
    if (layer->numberOfRunningActions() > 0) {
        layer->stopAllActions();
        layer->runAction(cocos2d::CCEaseElasticOut::create(cocos2d::CCScaleTo::create(0.5f, fit), 0.6f));
    } else {
        layer->setScale(fit);
    }
}

} // namespace replay
