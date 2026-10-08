#include "CodecGuidePopup.hpp"
#include <cocos2d.h>

using namespace cocos2d;
using namespace geode::prelude;

namespace replay {

CodecGuidePopup* CodecGuidePopup::create() {
    auto ret = new CodecGuidePopup();
    if (ret && ret->init(450.0f, 290.0f)) {
        ret->autorelease();
        return ret;
    }
    CC_SAFE_DELETE(ret);
    return nullptr;
}

bool CodecGuidePopup::init(float width, float height) {
    if (!Popup::init(width, height, "GJ_square01.png")) {
        return false;
    }
    this->setup();
    return true;
}

void CodecGuidePopup::setup() {
    this->setTitle("Codec & Hardware Guide");
    if (m_title) {
        m_title->setScale(0.75f);
        m_title->setPositionY(m_title->getPositionY() + 3.0f);
    }

    auto winSize = m_mainLayer->getContentSize();
    float centerX = winSize.width / 2.0f;
    float startY = winSize.height - 48.0f;

    auto subTitle = CCLabelBMFont::create("Recommended video codec settings for your graphics card:", "chatFont.fnt");
    subTitle->setScale(0.48f);
    subTitle->setColor(ccc3(120, 220, 255));
    subTitle->setPosition(ccp(centerX, startY));
    m_mainLayer->addChild(subTitle);

    struct GuideEntry {
        const char* title;
        ccColor3B titleColor;
        const char* desc1;
        const char* desc2;
    };

    GuideEntry entries[] = {
        {
            "AV1 (Best Quality & Smallest Size):",
            ccc3(100, 230, 255),
            "- NVIDIA RTX 40-Series (Ada), AMD RX 7000 (RDNA 3), Intel Arc",
            "- Next-gen compression: cleanest image with lowest file size."
        },
        {
            "HEVC / H.265 (Recommended for Most Players):",
            ccc3(255, 215, 80),
            "- NVIDIA GTX 16 / RTX 20 / RTX 30, AMD RX 5000 / 6000, Intel Core",
            "- Outstanding quality and super-fast hardware encoding with zero lag."
        },
        {
            "H.264 (Maximum Compatibility):",
            ccc3(255, 140, 100),
            "- Supported on all older GPUs, web players, Discord, and editors.",
            "- Larger files. Hardware limits: does NOT support 4K at 240 FPS."
        }
    };

    float curY = startY - 24.0f;
    for (const auto& item : entries) {
        auto header = CCLabelBMFont::create(item.title, "goldFont.fnt");
        header->setScale(0.48f);
        header->setColor(item.titleColor);
        header->setAnchorPoint(ccp(0.0f, 0.5f));
        header->setPosition(ccp(24.0f, curY));
        m_mainLayer->addChild(header);
        curY -= 17.0f;

        auto d1 = CCLabelBMFont::create(item.desc1, "chatFont.fnt");
        d1->setScale(0.43f);
        d1->setAnchorPoint(ccp(0.0f, 0.5f));
        d1->setPosition(ccp(32.0f, curY));
        m_mainLayer->addChild(d1);
        curY -= 15.0f;

        auto d2 = CCLabelBMFont::create(item.desc2, "chatFont.fnt");
        d2->setScale(0.43f);
        d2->setColor(ccc3(210, 225, 240));
        d2->setAnchorPoint(ccp(0.0f, 0.5f));
        d2->setPosition(ccp(32.0f, curY));
        m_mainLayer->addChild(d2);
        curY -= 20.0f;
    }

    auto btnMenu = CCMenu::create();
    btnMenu->setPosition(ccp(centerX, 26.0f));
    m_mainLayer->addChild(btnMenu);

    auto okSpr = ButtonSprite::create("Got it!", "goldFont.fnt", "GJ_button_01.png", 0.75f);
    auto okBtn = CCMenuItemExt::createSpriteExtra(okSpr, [this](auto*) {
        this->onClose(nullptr);
    });
    okBtn->setScale(0.70f);
    okBtn->m_baseScale = 0.70f;
    btnMenu->addChild(okBtn);
}

} // namespace replay
