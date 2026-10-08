#include "ReplayNotification.hpp"
#include <cocos2d.h>
#include <cocos-ext.h>
#include <filesystem>

using namespace cocos2d;
using namespace cocos2d::extension;

namespace replay {

void ReplayNotification::show(const std::string& filePath, double durationSeconds) {
    auto scene = CCDirector::sharedDirector()->getRunningScene();
    if (!scene) return;

    auto notif = ReplayNotification::create(filePath, durationSeconds);
    if (notif) {
        scene->addChild(notif, 1000);
    }
}

ReplayNotification* ReplayNotification::create(const std::string& filePath, double durationSeconds) {
    auto ret = new ReplayNotification();
    if (ret && ret->init(filePath, durationSeconds)) {
        ret->autorelease();
        return ret;
    }
    CC_SAFE_DELETE(ret);
    return nullptr;
}

bool ReplayNotification::init(const std::string& filePath, double durationSeconds) {
    if (!CCNode::init()) return false;

    auto winSize = CCDirector::sharedDirector()->getWinSize();

    // Background banner
    float width = 340.0f;
    float height = 52.0f;

    auto bg = CCScale9Sprite::create("square02_001.png");
    bg->setContentSize(CCSizeMake(width, height));
    bg->setColor(ccc3(18, 22, 28));
    bg->setOpacity(230);
    this->addChild(bg);

    // Green glow / completion icon
    auto icon = CCSprite::createWithSpriteFrameName("GJ_completesIcon_001.png");
    if (icon) {
        icon->setScale(0.8f);
        icon->setPosition(ccp(-width / 2 + 30.0f, 0));
        this->addChild(icon);
    }

    // Main Title
    auto titleLabel = CCLabelBMFont::create("Instant Replay Saved!", "goldFont.fnt");
    titleLabel->setScale(0.6f);
    titleLabel->setAnchorPoint(ccp(0.0f, 0.5f));
    titleLabel->setPosition(ccp(-width / 2 + 60.0f, 10.0f));
    this->addChild(titleLabel);

    // Subtitle with filename and duration
    auto slash = filePath.find_last_of("\\/");
    std::string filename = (slash == std::string::npos) ? filePath : filePath.substr(slash + 1);
    int m = static_cast<int>(durationSeconds) / 60;
    int s = static_cast<int>(durationSeconds) % 60;
    char subBuf[128];
    snprintf(subBuf, sizeof(subBuf), "%s (%d:%02d)", filename.c_str(), m, s);

    auto subLabel = CCLabelBMFont::create(subBuf, "chatFont.fnt");
    subLabel->setScale(0.5f);
    subLabel->setColor(ccc3(200, 220, 240));
    subLabel->setAnchorPoint(ccp(0.0f, 0.5f));
    subLabel->setPosition(ccp(-width / 2 + 60.0f, -10.0f));
    this->addChild(subLabel);

    // Position initially offscreen
    this->setPosition(ccp(winSize.width / 2, winSize.height + 60.0f));
    animateIn();

    return true;
}

void ReplayNotification::animateIn() {
    auto winSize = CCDirector::sharedDirector()->getWinSize();
    auto moveTo = CCEaseExponentialOut::create(CCMoveTo::create(0.4f, ccp(winSize.width / 2, winSize.height - 40.0f)));
    auto delay = CCDelayTime::create(3.2f);
    auto callOut = CCCallFunc::create(this, callfunc_selector(ReplayNotification::animateOut));
    this->runAction(CCSequence::create(moveTo, delay, callOut, nullptr));
}

void ReplayNotification::animateOut() {
    auto winSize = CCDirector::sharedDirector()->getWinSize();
    auto moveUp = CCEaseExponentialIn::create(CCMoveTo::create(0.35f, ccp(winSize.width / 2, winSize.height + 70.0f)));
    auto remove = CCRemoveSelf::create();
    this->runAction(CCSequence::create(moveUp, remove, nullptr));
}

} // namespace replay
