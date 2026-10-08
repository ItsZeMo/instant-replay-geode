#include "ReplayPopup.hpp"
#include "CodecGuidePopup.hpp"
#include <cocos2d.h>
#include <Geode/utils/file.hpp>
#include <Geode/utils/async.hpp>

using namespace cocos2d;
using namespace geode::prelude;

namespace replay {

ReplayPopup* ReplayPopup::create() {
    auto ret = new ReplayPopup();
    if (ret && ret->init(470.0f, 295.0f)) {
        ret->autorelease();
        return ret;
    }
    CC_SAFE_DELETE(ret);
    return nullptr;
}

void ReplayPopup::toggle() {
    if (s_instance) {
        s_instance->onClose(nullptr);
        return;
    }
    if (auto scene = CCDirector::sharedDirector()->getRunningScene()) {
        if (auto existing = scene->getChildByID("replay-popup")) {
            if (auto p = typeinfo_cast<ReplayPopup*>(existing)) {
                p->onClose(nullptr);
                return;
            } else {
                existing->removeFromParentAndCleanup(true);
                return;
            }
        }
    }
    ReplayPopup::create()->show();
}

ReplayPopup::~ReplayPopup() {
    if (s_instance == this) {
        s_instance = nullptr;
    }
}

void ReplayPopup::onClose(CCObject* sender) {
    if (s_instance == this) {
        s_instance = nullptr;
    }
    Popup::onClose(sender);
}

bool ReplayPopup::init(float width, float height) {
    if (!Popup::init(width, height, "GJ_square01.png")) {
        return false;
    }
    this->setup();
    return true;
}

namespace {
void setBtnScale(CCMenuItemSpriteExtra* btn, float scale) {
    if (!btn) return;
    btn->setScale(scale);
    btn->m_baseScale = scale;
}

void setupMenuPrio(CCMenu* menu) {
    if (!menu) return;
    menu->setTouchPriority(-505);
}

void setupInputPrio(TextInput* input) {
    if (!input) return;
    if (auto inner = input->getInputNode()) {
        inner->setTouchPriority(-505);
    }
}

void fixAllTouchPriorities(CCNode* node) {
    if (!node) return;
    if (auto menu = typeinfo_cast<CCMenu*>(node)) {
        menu->setTouchPriority(-505);
    } else if (auto input = typeinfo_cast<TextInput*>(node)) {
        if (auto inner = input->getInputNode()) {
            inner->setTouchPriority(-505);
        }
    } else if (auto slider = typeinfo_cast<Slider*>(node)) {
        if (slider->m_touchLogic) {
            slider->m_touchLogic->setTouchPriority(-505);
        }
    }
    for (auto child : CCArrayExt<CCNode*>(node->getChildren())) {
        fixAllTouchPriorities(child);
    }
}
}

void ReplayPopup::setup() {
    s_instance = this;
    this->setID("replay-popup");

    this->setTitle("Instant Replay");
    if (m_title) {
        m_title->setScale(0.85f);
        m_title->setPositionY(m_title->getPositionY() + 4.0f);
    }

    auto winSize = m_mainLayer->getContentSize();
    float centerX = winSize.width / 2.0f;

    // 1. Navigation Tab Bar (4 tabs)
    auto tabMenu = CCMenu::create();
    tabMenu->setPosition(ccp(centerX, winSize.height - 46.0f));
    setupMenuPrio(tabMenu);
    m_mainLayer->addChild(tabMenu);

    const char* tabNames[] = {"Dashboard", "Video Codec", "Bitrate (VBR)", "Audio & Storage"};
    for (int i = 0; i < 4; ++i) {
        auto spr = ButtonSprite::create(tabNames[i], "goldFont.fnt", (i == 0) ? "GJ_button_01.png" : "GJ_button_04.png", 0.70f);
        auto btn = CCMenuItemExt::createSpriteExtra(spr, [this, i](auto*) {
            this->switchTab(i);
        });
        setBtnScale(btn, 0.58f);
        btn->setTag(i);
        tabMenu->addChild(btn);
        m_tabButtons[i] = btn;
    }
    tabMenu->alignItemsHorizontallyWithPadding(5.0f);

    // Global persistent warning banner across all tabs
    m_lockWarningBanner = CCLabelBMFont::create("(!) Recording settings can only be changed when recording is disabled", "goldFont.fnt");
    m_lockWarningBanner->setScale(0.32f);
    m_lockWarningBanner->setColor(ccc3(255, 195, 45));
    m_lockWarningBanner->setPosition(ccp(centerX, winSize.height - 62.0f));
    m_mainLayer->addChild(m_lockWarningBanner, 50);

    // Create 4 Page Nodes
    for (int i = 0; i < 4; ++i) {
        m_pages[i] = CCNode::create();
        m_pages[i]->setContentSize(winSize);
        m_pages[i]->setPosition(CCPointZero);
        m_mainLayer->addChild(m_pages[i]);
    }

    // =========================================================================
    // PAGE 0: DASHBOARD
    // =========================================================================
    {
        auto page = m_pages[0];

        // Status Card
        m_statusDot = CCSprite::createWithSpriteFrameName("GJ_completesIcon_001.png");
        m_statusDot->setScale(0.55f);
        m_statusDot->setPosition(ccp(centerX - 130.0f, winSize.height - 82.0f));
        page->addChild(m_statusDot);

        m_statusLabel = CCLabelBMFont::create("Replay Active...", "chatFont.fnt");
        m_statusLabel->setScale(0.7f);
        m_statusLabel->setAnchorPoint(ccp(0.0f, 0.5f));
        m_statusLabel->setPosition(ccp(centerX - 110.0f, winSize.height - 82.0f));
        page->addChild(m_statusLabel);

        m_codecBadgeLabel = CCLabelBMFont::create("AV1 NVENC | Native | 35 Mbps (Adaptive VBR)", "chatFont.fnt");
        m_codecBadgeLabel->setScale(0.5f);
        m_codecBadgeLabel->setColor(ccc3(180, 210, 240));
        m_codecBadgeLabel->setAnchorPoint(ccp(0.5f, 0.5f));
        m_codecBadgeLabel->setPosition(ccp(centerX, winSize.height - 104.0f));
        page->addChild(m_codecBadgeLabel);

        m_diskSpaceWarnLabel = CCLabelBMFont::create("", "goldFont.fnt");
        m_diskSpaceWarnLabel->setScale(0.38f);
        m_diskSpaceWarnLabel->setColor(ccc3(255, 75, 75));
        m_diskSpaceWarnLabel->setPosition(ccp(centerX, winSize.height - 120.0f));
        m_diskSpaceWarnLabel->setVisible(false);
        page->addChild(m_diskSpaceWarnLabel);

        // Action Buttons Menu
        auto actionMenu = CCMenu::create();
        actionMenu->setPosition(ccp(centerX, winSize.height - 144.0f));
        setupMenuPrio(actionMenu);
        page->addChild(actionMenu);

        auto saveSpr = ButtonSprite::create("Save Replay (F6)", "goldFont.fnt", "GJ_button_01.png", 0.75f);
        auto saveBtn = CCMenuItemExt::createSpriteExtra(saveSpr, [this](auto*) {
            ReplayManager::get()->saveReplay();
            this->updateStatusDisplay(0.0f);
        });
        setBtnScale(saveBtn, 0.68f);
        actionMenu->addChild(saveBtn);

        auto toggleSpr = ButtonSprite::create("Toggle Replay", "goldFont.fnt", "GJ_button_04.png", 0.75f);
        m_toggleBtn = CCMenuItemExt::createSpriteExtra(toggleSpr, [this](auto*) {
            ReplayManager::get()->toggleReplay();
            this->refreshUI();
            this->updateStatusDisplay(0.0f);
        });
        setBtnScale(m_toggleBtn, 0.68f);
        actionMenu->addChild(m_toggleBtn);

        auto folderSpr = ButtonSprite::create("Open Folder", "goldFont.fnt", "GJ_button_05.png", 0.75f);
        auto folderBtn = CCMenuItemExt::createSpriteExtra(folderSpr, [](auto*) {
            ReplayManager::get()->openRecordingsFolder();
        });
        setBtnScale(folderBtn, 0.68f);
        actionMenu->addChild(folderBtn);

        actionMenu->alignItemsHorizontallyWithPadding(6.0f);

        // Duration Presets Row
        auto durTitle = CCLabelBMFont::create("Duration:", "goldFont.fnt");
        durTitle->setScale(0.52f);
        durTitle->setAnchorPoint(ccp(0.0f, 0.5f));
        durTitle->setPosition(ccp(25.0f, winSize.height - 180.0f));
        page->addChild(durTitle);

        m_durationMenu = CCMenu::create();
        m_durationMenu->setPosition(ccp(centerX + 35.0f, winSize.height - 180.0f));
        setupMenuPrio(m_durationMenu);
        page->addChild(m_durationMenu);

        struct DurOpt { const char* label; int seconds; };
        DurOpt durOpts[] = { {"30s", 30}, {"1m", 60}, {"2m", 120}, {"5m", 300}, {"10m", 600} };
        for (const auto& opt : durOpts) {
            auto spr = ButtonSprite::create(opt.label, "goldFont.fnt", "GJ_button_04.png", 0.75f);
            auto btn = CCMenuItemExt::createSpriteExtra(spr, [this, sec = opt.seconds](auto*) {
                if (ReplayManager::get()->isReplayActive()) {
                    Notification::create("Recording settings can only be changed when recording is OFF!", NotificationIcon::Warning, 1.2f)->show();
                    return;
                }
                ReplayConfig::get().durationSeconds = sec;
                ReplayConfig::get().saveToSettings();
                if (m_durationInput) {
                    m_durationInput->setString(std::to_string(sec));
                }
                this->refreshUI();
                this->updateStatusDisplay(0.0f);
            });
            setBtnScale(btn, 0.62f);
            btn->setTag(opt.seconds);
            m_durationMenu->addChild(btn);
        }
        m_durationMenu->alignItemsHorizontallyWithPadding(5.0f);

        // Custom Duration Manual Input Row
        auto customDurTitle = CCLabelBMFont::create("Custom (sec):", "goldFont.fnt");
        customDurTitle->setScale(0.48f);
        customDurTitle->setAnchorPoint(ccp(0.0f, 0.5f));
        customDurTitle->setPosition(ccp(25.0f, winSize.height - 215.0f));
        page->addChild(customDurTitle);

        m_durationInput = TextInput::create(75.0f, "sec", "chatFont.fnt");
        m_durationInput->setCommonFilter(CommonFilter::Uint);
        m_durationInput->setString(std::to_string(ReplayConfig::get().durationSeconds));
        m_durationInput->setPosition(ccp(160.0f, winSize.height - 215.0f));
        setupInputPrio(m_durationInput);
        m_durationInput->setCallback([this](std::string const& str) {
            if (ReplayManager::get()->isReplayActive()) {
                Notification::create("Recording settings can only be changed when recording is OFF!", NotificationIcon::Warning, 1.2f)->show();
                m_durationInput->setString(std::to_string(ReplayConfig::get().durationSeconds));
                return;
            }
            if (!str.empty()) {
                try {
                    int sec = std::stoi(str);
                    sec = std::clamp(sec, 5, 3600);
                    ReplayConfig::get().durationSeconds = sec;
                    ReplayConfig::get().saveToSettings();
                    this->refreshUI();
                    this->updateStatusDisplay(0.0f);
                } catch (...) {}
            }
        });
        page->addChild(m_durationInput);

        auto rangeHint = CCLabelBMFont::create("(5 - 3600s)", "chatFont.fnt");
        rangeHint->setScale(0.42f);
        rangeHint->setColor(ccc3(140, 200, 240));
        rangeHint->setAnchorPoint(ccp(0.0f, 0.5f));
        rangeHint->setPosition(ccp(205.0f, winSize.height - 215.0f));
        page->addChild(rangeHint);

        // Adobe Premiere Pro style Estimated File Size badge
        m_estSizeLabel = CCLabelBMFont::create("", "goldFont.fnt");
        m_estSizeLabel->setScale(0.50f);
        m_estSizeLabel->setColor(ccc3(255, 215, 80));
        m_estSizeLabel->setAnchorPoint(ccp(1.0f, 0.5f));
        m_estSizeLabel->setPosition(ccp(winSize.width - 25.0f, winSize.height - 215.0f));
        page->addChild(m_estSizeLabel);

        // Save Folder Quick Row
        auto folderTitle = CCLabelBMFont::create("Save Folder:", "goldFont.fnt");
        folderTitle->setScale(0.48f);
        folderTitle->setAnchorPoint(ccp(0.0f, 0.5f));
        folderTitle->setPosition(ccp(25.0f, winSize.height - 250.0f));
        page->addChild(folderTitle);

        m_folderPathLabel = CCLabelBMFont::create("", "chatFont.fnt");
        m_folderPathLabel->setScale(0.42f);
        m_folderPathLabel->setColor(ccc3(180, 225, 255));
        m_folderPathLabel->setAnchorPoint(ccp(0.0f, 0.5f));
        m_folderPathLabel->setPosition(ccp(110.0f, winSize.height - 250.0f));
        page->addChild(m_folderPathLabel);

        auto folderQuickMenu = CCMenu::create();
        folderQuickMenu->setPosition(ccp(winSize.width - 80.0f, winSize.height - 250.0f));
        setupMenuPrio(folderQuickMenu);
        page->addChild(folderQuickMenu);

        auto browseSpr = ButtonSprite::create("Browse", "goldFont.fnt", "GJ_button_04.png", 0.75f);
        auto browseBtn = CCMenuItemExt::createSpriteExtra(browseSpr, [this](auto*) {
            if (ReplayManager::get()->isReplayActive()) {
                Notification::create("Recording settings can only be changed when recording is OFF!", NotificationIcon::Warning, 1.2f)->show();
                return;
            }
            this->onBrowseFolder();
        });
        setBtnScale(browseBtn, 0.62f);
        folderQuickMenu->addChild(browseBtn);
        m_quickBrowseBtn = browseBtn;
    }

    // =========================================================================
    // PAGE 1: VIDEO CODEC & CUSTOM CODEC
    // =========================================================================
    {
        auto page = m_pages[1];

        // Codec Presets Row
        // Codec Selection Row (AV1, HEVC, H.264)
        auto codecTitle = CCLabelBMFont::create("Codec:", "goldFont.fnt");
        codecTitle->setScale(0.52f);
        codecTitle->setAnchorPoint(ccp(0.0f, 0.5f));
        codecTitle->setPosition(ccp(20.0f, winSize.height - 76.0f));
        page->addChild(codecTitle);

        m_codecMenu = CCMenu::create();
        m_codecMenu->setPosition(ccp(175.0f, winSize.height - 76.0f));
        setupMenuPrio(m_codecMenu);
        page->addChild(m_codecMenu);

        struct CodecOpt { const char* label; int tag; };
        CodecOpt codecOpts[] = {
            {"AV1", static_cast<int>(CodecType::AV1)},
            {"HEVC", static_cast<int>(CodecType::HEVC)},
            {"H.264", static_cast<int>(CodecType::H264)}
        };
        for (const auto& opt : codecOpts) {
            auto spr = ButtonSprite::create(opt.label, "goldFont.fnt", "GJ_button_04.png", 0.75f);
            auto btn = CCMenuItemExt::createSpriteExtra(spr, [this, tag = opt.tag](auto*) {
                if (ReplayManager::get()->isReplayActive()) {
                    Notification::create("Recording settings can only be changed when recording is OFF!", NotificationIcon::Warning, 1.2f)->show();
                    return;
                }
                auto& cfg = ReplayConfig::get();
                cfg.codec = static_cast<CodecType>(tag);
                cfg.customCodec = "";
                cfg.sanitizeCodecSettings();
                cfg.saveToSettings();
                if (m_customCodecInput) {
                    m_customCodecInput->setString("");
                }
                this->refreshUI();
                this->updateStatusDisplay(0.0f);
            });
            setBtnScale(btn, 0.62f);
            btn->setTag(opt.tag);
            m_codecMenu->addChild(btn);
        }
        m_codecMenu->alignItemsHorizontallyWithPadding(8.0f);

        // Hardware Acceleration Checkbox (Auto GPU: NVENC / AMF / QSV)
        auto hwMenu = CCMenu::create();
        hwMenu->setPosition(CCPointZero);
        setupMenuPrio(hwMenu);
        page->addChild(hwMenu);

        float hwX = 35.0f;
        float hwY = winSize.height - 105.0f;

        m_hwAccelToggle = CCMenuItemExt::createTogglerWithStandardSprites(0.65f, [this](auto* toggler) {
            if (ReplayManager::get()->isReplayActive()) {
                Notification::create("Recording settings can only be changed when recording is OFF!", NotificationIcon::Warning, 1.2f)->show();
                toggler->toggle(ReplayConfig::get().hardwareAcceleration);
                return;
            }
            auto& cfg = ReplayConfig::get();
            cfg.hardwareAcceleration = !cfg.hardwareAcceleration;
            cfg.saveToSettings();
            toggler->toggle(cfg.hardwareAcceleration);
            this->refreshUI();
            this->updateStatusDisplay(0.0f);
        });
        m_hwAccelToggle->toggle(ReplayConfig::get().hardwareAcceleration);
        m_hwAccelToggle->setPosition(ccp(hwX, hwY));
        hwMenu->addChild(m_hwAccelToggle);

        std::string accelText = "Hardware Acceleration (Auto GPU: " + ReplayConfig::getGpuAccelName() + ")";
        m_hwAccelLabel = CCLabelBMFont::create(accelText.c_str(), "chatFont.fnt");
        m_hwAccelLabel->setScale(0.48f);
        m_hwAccelLabel->setAnchorPoint(ccp(0.0f, 0.5f));
        m_hwAccelLabel->setPosition(ccp(hwX + 22.0f, hwY));
        page->addChild(m_hwAccelLabel);

        // Custom Codec Manual Input Row + Guide Button
        auto customCodecTitle = CCLabelBMFont::create("Custom Codec:", "goldFont.fnt");
        customCodecTitle->setScale(0.48f);
        customCodecTitle->setAnchorPoint(ccp(0.0f, 0.5f));
        customCodecTitle->setPosition(ccp(20.0f, winSize.height - 135.0f));
        page->addChild(customCodecTitle);

        m_customCodecInput = TextInput::create(150.0f, "e.g. av1_nvenc", "chatFont.fnt");
        m_customCodecInput->setCommonFilter(CommonFilter::Any);
        m_customCodecInput->setString(ReplayConfig::get().customCodec);
        m_customCodecInput->setPosition(ccp(centerX - 10.0f, winSize.height - 135.0f));
        setupInputPrio(m_customCodecInput);
        m_customCodecInput->setCallback([this](std::string const& str) {
            if (ReplayManager::get()->isReplayActive()) {
                Notification::create("Recording settings can only be changed when recording is OFF!", NotificationIcon::Warning, 1.2f)->show();
                m_customCodecInput->setString(ReplayConfig::get().customCodec);
                return;
            }
            auto& c = ReplayConfig::get();
            c.customCodec = str;
            if (!str.empty()) {
                c.codec = CodecType::CUSTOM;
            }
            c.saveToSettings();
            this->refreshUI();
            this->updateStatusDisplay(0.0f);
        });
        page->addChild(m_customCodecInput);

        // Guide / Info Button
        auto guideMenu = CCMenu::create();
        guideMenu->setPosition(ccp(winSize.width - 65.0f, winSize.height - 135.0f));
        setupMenuPrio(guideMenu);
        page->addChild(guideMenu);

        auto guideSpr = ButtonSprite::create("? Guide", "goldFont.fnt", "GJ_button_02.png", 0.75f);
        auto guideBtn = CCMenuItemExt::createSpriteExtra(guideSpr, [](auto*) {
            CodecGuidePopup::create()->show();
        });
        setBtnScale(guideBtn, 0.62f);
        guideMenu->addChild(guideBtn);

        // Resolution Row
        auto resTitle = CCLabelBMFont::create("Resolution:", "goldFont.fnt");
        resTitle->setScale(0.52f);
        resTitle->setAnchorPoint(ccp(0.0f, 0.5f));
        resTitle->setPosition(ccp(20.0f, winSize.height - 162.0f));
        page->addChild(resTitle);

        m_resMenu = CCMenu::create();
        m_resMenu->setPosition(ccp(centerX + 35.0f, winSize.height - 162.0f));
        setupMenuPrio(m_resMenu);
        page->addChild(m_resMenu);

        struct ResOpt { const char* label; int tag; };
        ResOpt resOpts[] = {
            {"Native", 0},
            {"4K", 1},
            {"1440p", 2},
            {"1080p", 3},
            {"720p", 4}
        };
        for (const auto& opt : resOpts) {
            auto spr = ButtonSprite::create(opt.label, "goldFont.fnt", "GJ_button_04.png", 0.75f);
            auto btn = CCMenuItemExt::createSpriteExtra(spr, [this, tag = opt.tag](auto*) {
                if (ReplayManager::get()->isReplayActive()) {
                    Notification::create("Recording settings can only be changed when recording is OFF!", NotificationIcon::Warning, 1.2f)->show();
                    return;
                }
                auto newRes = static_cast<ResolutionType>(tag);
                if (ReplayConfig::get().codec == CodecType::H264 && ReplayConfig::isH264Conflict(newRes, ReplayConfig::get().targetFps)) {
                    ReplayConfig::get().targetFps = 120;
                }
                ReplayConfig::get().resolution = newRes;
                ReplayConfig::get().saveToSettings();
                this->refreshUI();
                this->updateStatusDisplay(0.0f);
            });
            setBtnScale(btn, 0.60f);
            btn->setTag(opt.tag);
            m_resMenu->addChild(btn);
        }
        m_resMenu->alignItemsHorizontallyWithPadding(4.0f);

        // Target FPS Row
        auto fpsTitle = CCLabelBMFont::create("Target FPS:", "goldFont.fnt");
        fpsTitle->setScale(0.52f);
        fpsTitle->setAnchorPoint(ccp(0.0f, 0.5f));
        fpsTitle->setPosition(ccp(20.0f, winSize.height - 200.0f));
        page->addChild(fpsTitle);

        m_fpsMenu = CCMenu::create();
        m_fpsMenu->setPosition(ccp(centerX + 35.0f, winSize.height - 200.0f));
        setupMenuPrio(m_fpsMenu);
        page->addChild(m_fpsMenu);

        int fpsOpts[] = { 60, 120, 240 };
        for (int fps : fpsOpts) {
            char buf[16];
            snprintf(buf, sizeof(buf), "%d FPS", fps);
            auto spr = ButtonSprite::create(buf, "goldFont.fnt", "GJ_button_04.png", 0.75f);
            auto btn = CCMenuItemExt::createSpriteExtra(spr, [this, fps](auto*) {
                if (ReplayManager::get()->isReplayActive()) {
                    Notification::create("Recording settings can only be changed when recording is OFF!", NotificationIcon::Warning, 1.2f)->show();
                    return;
                }
                if (ReplayConfig::get().codec == CodecType::H264 && ReplayConfig::isH264Conflict(ReplayConfig::get().resolution, fps)) {
                    ReplayConfig::get().resolution = ResolutionType::QHD_1440P;
                }
                ReplayConfig::get().targetFps = fps;
                ReplayConfig::get().saveToSettings();
                this->refreshUI();
                this->updateStatusDisplay(0.0f);
            });
            setBtnScale(btn, 0.62f);
            btn->setTag(fps);
            m_fpsMenu->addChild(btn);
        }
        m_fpsMenu->alignItemsHorizontallyWithPadding(8.0f);

        // Live estimated file size indicator on Resolution & Codec page
        m_estSizeResLabel = CCLabelBMFont::create("", "goldFont.fnt");
        m_estSizeResLabel->setScale(0.50f);
        m_estSizeResLabel->setColor(ccc3(255, 215, 80));
        m_estSizeResLabel->setPosition(ccp(centerX, winSize.height - 232.0f));
        page->addChild(m_estSizeResLabel);

        m_codecConflictLabel = CCLabelBMFont::create("Note: H.264 hardware encoders do not support 4K @ 240 FPS", "goldFont.fnt");
        m_codecConflictLabel->setScale(0.38f);
        m_codecConflictLabel->setColor(ccc3(255, 120, 80));
        m_codecConflictLabel->setPosition(ccp(centerX, winSize.height - 248.0f));
        m_codecConflictLabel->setVisible(false);
        page->addChild(m_codecConflictLabel);

        m_pageNoticeLabels[1] = CCLabelBMFont::create("Select your preferred codec, GPU acceleration, resolution & FPS.", "chatFont.fnt");
        m_pageNoticeLabels[1]->setScale(0.36f);
        m_pageNoticeLabels[1]->setColor(ccc3(130, 240, 190));
        m_pageNoticeLabels[1]->setPosition(ccp(centerX, winSize.height - 264.0f));
        page->addChild(m_pageNoticeLabels[1]);
    }

    // =========================================================================
    // PAGE 2: BITRATE & QUALITY (ADAPTIVE BITRATE)
    // =========================================================================
    {
        auto page = m_pages[2];

        // Rate Control Mode
        auto rcTitle = CCLabelBMFont::create("Rate Control:", "goldFont.fnt");
        rcTitle->setScale(0.52f);
        rcTitle->setAnchorPoint(ccp(0.0f, 0.5f));
        rcTitle->setPosition(ccp(25.0f, winSize.height - 82.0f));
        page->addChild(rcTitle);

        m_rateControlMenu = CCMenu::create();
        m_rateControlMenu->setPosition(ccp(centerX + 40.0f, winSize.height - 82.0f));
        setupMenuPrio(m_rateControlMenu);
        page->addChild(m_rateControlMenu);

        struct RCOpt { const char* label; int tag; };
        RCOpt rcOpts[] = {
            {"Adaptive (VBR)", 0},
            {"Constant (CBR)", 1}
        };
        for (const auto& opt : rcOpts) {
            auto spr = ButtonSprite::create(opt.label, "goldFont.fnt", "GJ_button_04.png", 0.75f);
            auto btn = CCMenuItemExt::createSpriteExtra(spr, [this, tag = opt.tag](auto*) {
                if (ReplayManager::get()->isReplayActive()) {
                    Notification::create("Recording settings can only be changed when recording is OFF!", NotificationIcon::Warning, 1.2f)->show();
                    return;
                }
                ReplayConfig::get().rateControl = static_cast<RateControlMode>(tag);
                ReplayConfig::get().saveToSettings();
                this->refreshUI();
                this->updateStatusDisplay(0.0f);
            });
            setBtnScale(btn, 0.62f);
            btn->setTag(opt.tag);
            m_rateControlMenu->addChild(btn);
        }
        m_rateControlMenu->alignItemsHorizontallyWithPadding(10.0f);

        // Target Bitrate Row
        auto bitTitle = CCLabelBMFont::create("Target Bitrate:", "goldFont.fnt");
        bitTitle->setScale(0.50f);
        bitTitle->setAnchorPoint(ccp(0.0f, 0.5f));
        bitTitle->setPosition(ccp(25.0f, winSize.height - 120.0f));
        page->addChild(bitTitle);

        m_bitrateMenu = CCMenu::create();
        m_bitrateMenu->setPosition(ccp(centerX + 40.0f, winSize.height - 120.0f));
        setupMenuPrio(m_bitrateMenu);
        page->addChild(m_bitrateMenu);

        int bitrates[] = { 15, 25, 35, 50, 80 };
        for (int b : bitrates) {
            char buf[16];
            snprintf(buf, sizeof(buf), "%dM", b);
            auto spr = ButtonSprite::create(buf, "goldFont.fnt", "GJ_button_04.png", 0.75f);
            auto btn = CCMenuItemExt::createSpriteExtra(spr, [this, b](auto*) {
                if (ReplayManager::get()->isReplayActive()) {
                    Notification::create("Recording settings can only be changed when recording is OFF!", NotificationIcon::Warning, 1.2f)->show();
                    return;
                }
                ReplayConfig::get().bitrateMbps = b;
                if (ReplayConfig::get().maxBitrateMbps < b) {
                    ReplayConfig::get().maxBitrateMbps = b;
                }
                ReplayConfig::get().saveToSettings();
                if (m_customBitrateInput) {
                    m_customBitrateInput->setString(std::to_string(b));
                }
                this->refreshUI();
                this->updateStatusDisplay(0.0f);
            });
            setBtnScale(btn, 0.62f);
            btn->setTag(b);
            m_bitrateMenu->addChild(btn);
        }
        m_bitrateMenu->alignItemsHorizontallyWithPadding(6.0f);

        // Custom Bitrate Manual Input Row
        auto customBitTitle = CCLabelBMFont::create("Custom Mbps:", "goldFont.fnt");
        customBitTitle->setScale(0.48f);
        customBitTitle->setAnchorPoint(ccp(0.0f, 0.5f));
        customBitTitle->setPosition(ccp(25.0f, winSize.height - 156.0f));
        page->addChild(customBitTitle);

        m_customBitrateInput = TextInput::create(75.0f, "Mbps", "chatFont.fnt");
        m_customBitrateInput->setCommonFilter(CommonFilter::Uint);
        m_customBitrateInput->setString(std::to_string(ReplayConfig::get().bitrateMbps));
        m_customBitrateInput->setPosition(ccp(160.0f, winSize.height - 156.0f));
        setupInputPrio(m_customBitrateInput);
        m_customBitrateInput->setCallback([this](std::string const& str) {
            if (ReplayManager::get()->isReplayActive()) {
                Notification::create("Recording settings can only be changed when recording is OFF!", NotificationIcon::Warning, 1.2f)->show();
                m_customBitrateInput->setString(std::to_string(ReplayConfig::get().bitrateMbps));
                return;
            }
            if (!str.empty()) {
                try {
                    int b = std::stoi(str);
                    b = std::clamp(b, 5, 250);
                    ReplayConfig::get().bitrateMbps = b;
                    if (ReplayConfig::get().maxBitrateMbps < b) {
                        ReplayConfig::get().maxBitrateMbps = b;
                    }
                    ReplayConfig::get().saveToSettings();
                    this->refreshUI();
                    this->updateStatusDisplay(0.0f);
                } catch (...) {}
            }
        });
        page->addChild(m_customBitrateInput);

        auto bitHint = CCLabelBMFont::create("(5 - 250 Mbps)", "chatFont.fnt");
        bitHint->setScale(0.42f);
        bitHint->setColor(ccc3(140, 200, 240));
        bitHint->setAnchorPoint(ccp(0.0f, 0.5f));
        bitHint->setPosition(ccp(205.0f, winSize.height - 156.0f));
        page->addChild(bitHint);

        // Peak / Max Bitrate (Adaptive VBR only)
        m_peakBitrateNode = CCNode::create();
        m_peakBitrateNode->setContentSize(winSize);
        m_peakBitrateNode->setPosition(CCPointZero);
        page->addChild(m_peakBitrateNode);

        auto maxBitTitle = CCLabelBMFont::create("Peak Bitrate (VBR):", "goldFont.fnt");
        maxBitTitle->setScale(0.50f);
        maxBitTitle->setAnchorPoint(ccp(0.0f, 0.5f));
        maxBitTitle->setPosition(ccp(25.0f, winSize.height - 192.0f));
        m_peakBitrateNode->addChild(maxBitTitle);

        m_maxBitrateMenu = CCMenu::create();
        m_maxBitrateMenu->setPosition(ccp(centerX + 40.0f, winSize.height - 192.0f));
        setupMenuPrio(m_maxBitrateMenu);
        m_peakBitrateNode->addChild(m_maxBitrateMenu);

        int maxBits[] = { 35, 50, 60, 80, 120 };
        for (int mb : maxBits) {
            char buf[16];
            snprintf(buf, sizeof(buf), "%dM", mb);
            auto spr = ButtonSprite::create(buf, "goldFont.fnt", "GJ_button_04.png", 0.75f);
            auto btn = CCMenuItemExt::createSpriteExtra(spr, [this, mb](auto*) {
                if (ReplayManager::get()->isReplayActive()) {
                    Notification::create("Recording settings can only be changed when recording is OFF!", NotificationIcon::Warning, 1.2f)->show();
                    return;
                }
                ReplayConfig::get().maxBitrateMbps = mb;
                ReplayConfig::get().saveToSettings();
                this->refreshUI();
                this->updateStatusDisplay(0.0f);
            });
            setBtnScale(btn, 0.62f);
            btn->setTag(mb);
            m_maxBitrateMenu->addChild(btn);
        }
        m_maxBitrateMenu->alignItemsHorizontallyWithPadding(6.0f);

        // Adobe Premiere Pro style Estimated File Size on Bitrate page
        m_estSizeBitrateLabel = CCLabelBMFont::create("", "goldFont.fnt");
        m_estSizeBitrateLabel->setScale(0.50f);
        m_estSizeBitrateLabel->setColor(ccc3(255, 215, 80));
        m_estSizeBitrateLabel->setPosition(ccp(centerX, winSize.height - 226.0f));
        page->addChild(m_estSizeBitrateLabel);

        m_pageNoticeLabels[2] = CCLabelBMFont::create("Adaptive VBR dynamically allocates bitrate: saves space in menus, peaks in fast gameplay.", "chatFont.fnt");
        m_pageNoticeLabels[2]->setScale(0.38f);
        m_pageNoticeLabels[2]->setColor(ccc3(220, 220, 160));
        m_pageNoticeLabels[2]->setPosition(ccp(centerX, winSize.height - 252.0f));
        page->addChild(m_pageNoticeLabels[2]);
    }

    // =========================================================================
    // PAGE 3: AUDIO & STORAGE
    // =========================================================================
    {
        auto page = m_pages[3];

        auto audioMenu = CCMenu::create();
        audioMenu->setPosition(CCPointZero);
        setupMenuPrio(audioMenu);
        page->addChild(audioMenu);

        // Separate Audio Tracks
        float checkX = 42.0f;
        float row1Y = winSize.height - 82.0f;
        float row2Y = winSize.height - 110.0f;

        m_splitAudioToggle = CCMenuItemExt::createTogglerWithStandardSprites(0.68f, [this](auto* toggler) {
            if (ReplayManager::get()->isReplayActive()) {
                Notification::create("Recording settings can only be changed when recording is OFF!", NotificationIcon::Warning, 1.2f)->show();
                toggler->toggle(ReplayConfig::get().splitAudioTracks);
                return;
            }
            auto& cfg = ReplayConfig::get();
            cfg.splitAudioTracks = !cfg.splitAudioTracks;
            cfg.saveToSettings();
            toggler->toggle(cfg.splitAudioTracks);
            this->updateStatusDisplay(0.0f);
        });
        m_splitAudioToggle->toggle(ReplayConfig::get().splitAudioTracks);
        m_splitAudioToggle->setPosition(ccp(checkX, row1Y));
        audioMenu->addChild(m_splitAudioToggle);

        auto splitLabel = CCLabelBMFont::create("Separate Audio Tracks (Game + Mic in MP4)", "chatFont.fnt");
        splitLabel->setScale(0.50f);
        splitLabel->setAnchorPoint(ccp(0.0f, 0.5f));
        splitLabel->setPosition(ccp(checkX + 24.0f, row1Y));
        page->addChild(splitLabel);

        // Mic Toggle
        m_micToggle = CCMenuItemExt::createTogglerWithStandardSprites(0.68f, [this](auto* toggler) {
            if (ReplayManager::get()->isReplayActive()) {
                Notification::create("Recording settings can only be changed when recording is OFF!", NotificationIcon::Warning, 1.2f)->show();
                toggler->toggle(ReplayConfig::get().recordMic);
                return;
            }
            auto& cfg = ReplayConfig::get();
            cfg.recordMic = !cfg.recordMic;
            cfg.saveToSettings();
            toggler->toggle(cfg.recordMic);
            if (cfg.recordMic) {
                AudioGrabber::get()->restartMicCapture();
            } else {
                AudioGrabber::get()->stopMicCapture();
            }
            this->updateStatusDisplay(0.0f);
        });
        m_micToggle->toggle(ReplayConfig::get().recordMic);
        m_micToggle->setPosition(ccp(checkX, row2Y));
        audioMenu->addChild(m_micToggle);

        auto micLabel = CCLabelBMFont::create("Record Microphone Audio (WASAPI)", "chatFont.fnt");
        micLabel->setScale(0.50f);
        micLabel->setAnchorPoint(ccp(0.0f, 0.5f));
        micLabel->setPosition(ccp(checkX + 24.0f, row2Y));
        page->addChild(micLabel);

        // Microphone Device Selector Row
        auto micDevTitle = CCLabelBMFont::create("Mic Device:", "goldFont.fnt");
        micDevTitle->setScale(0.48f);
        micDevTitle->setAnchorPoint(ccp(0.0f, 0.5f));
        micDevTitle->setPosition(ccp(20.0f, winSize.height - 138.0f));
        page->addChild(micDevTitle);

        m_availableMics = AudioGrabber::getAvailableMicrophones();
        m_currentMicIndex = 0;
        const auto& curId = ReplayConfig::get().micDeviceId;
        for (size_t i = 0; i < m_availableMics.size(); ++i) {
            if (m_availableMics[i].id == curId) {
                m_currentMicIndex = static_cast<int>(i);
                break;
            }
        }

        m_micDevMenu = CCMenu::create();
        m_micDevMenu->setPosition(ccp(270.0f, winSize.height - 138.0f));
        setupMenuPrio(m_micDevMenu);
        page->addChild(m_micDevMenu);

        auto prevSpr = CCSprite::createWithSpriteFrameName("GJ_arrow_03_001.png");
        auto prevBtn = CCMenuItemExt::createSpriteExtra(prevSpr, [this](auto*) {
            this->cycleMicDevice(-1);
        });
        setBtnScale(prevBtn, 0.65f);
        m_micDevMenu->addChild(prevBtn);
        m_micPrevBtn = prevBtn;

        std::string initialName = m_availableMics.empty() ? "Default Device" : m_availableMics[m_currentMicIndex].name;
        if (initialName.length() > 24) {
            initialName = initialName.substr(0, 21) + "...";
        }
        auto devSpr = ButtonSprite::create(initialName.c_str(), "goldFont.fnt", "GJ_button_01.png", 0.75f);
        m_micDeviceBtn = CCMenuItemExt::createSpriteExtra(devSpr, [this](auto*) {
            this->cycleMicDevice(1);
        });
        setBtnScale(m_micDeviceBtn, 0.62f);
        m_micDevMenu->addChild(m_micDeviceBtn);

        auto nextSpr = CCSprite::createWithSpriteFrameName("GJ_arrow_03_001.png");
        nextSpr->setFlipX(true);
        auto nextBtn = CCMenuItemExt::createSpriteExtra(nextSpr, [this](auto*) {
            this->cycleMicDevice(1);
        });
        setBtnScale(nextBtn, 0.65f);
        m_micDevMenu->addChild(nextBtn);
        m_micNextBtn = nextBtn;

        m_micDevMenu->alignItemsHorizontallyWithPadding(6.0f);

        // Volume Sliders (Vanilla Geometry Dash sliders with live percentage labels)
        auto gVolTitle = CCLabelBMFont::create("Game Vol:", "goldFont.fnt");
        gVolTitle->setScale(0.44f);
        gVolTitle->setAnchorPoint(ccp(0.0f, 0.5f));
        gVolTitle->setPosition(ccp(20.0f, winSize.height - 168.0f));
        page->addChild(gVolTitle);

        m_gameVolSlider = Slider::create(this, menu_selector(ReplayPopup::onGameVolumeChanged), 0.38f);
        m_gameVolSlider->setPosition(ccp(118.0f, winSize.height - 168.0f));
        m_gameVolSlider->setValue(std::clamp(ReplayConfig::get().systemVolume / 1.5f, 0.0f, 1.0f));
        m_gameVolSlider->setLiveDragging(true);
        if (m_gameVolSlider->m_touchLogic) {
            m_gameVolSlider->m_touchLogic->setTouchPriority(-505);
        }
        page->addChild(m_gameVolSlider);

        int initGPct = static_cast<int>(ReplayConfig::get().systemVolume * 100.0f + 0.5f);
        m_gameVolLabel = CCLabelBMFont::create((std::to_string(initGPct) + "%").c_str(), "goldFont.fnt");
        m_gameVolLabel->setScale(0.42f);
        m_gameVolLabel->setAnchorPoint(ccp(0.0f, 0.5f));
        m_gameVolLabel->setPosition(ccp(166.0f, winSize.height - 168.0f));
        page->addChild(m_gameVolLabel);

        auto mVolTitle = CCLabelBMFont::create("Mic Vol:", "goldFont.fnt");
        mVolTitle->setScale(0.44f);
        mVolTitle->setAnchorPoint(ccp(0.0f, 0.5f));
        mVolTitle->setPosition(ccp(272.0f, winSize.height - 168.0f));
        page->addChild(mVolTitle);

        m_micVolSlider = Slider::create(this, menu_selector(ReplayPopup::onMicVolumeChanged), 0.38f);
        m_micVolSlider->setPosition(ccp(370.0f, winSize.height - 168.0f));
        m_micVolSlider->setValue(std::clamp(ReplayConfig::get().micVolume / 1.5f, 0.0f, 1.0f));
        m_micVolSlider->setLiveDragging(true);
        if (m_micVolSlider->m_touchLogic) {
            m_micVolSlider->m_touchLogic->setTouchPriority(-505);
        }
        page->addChild(m_micVolSlider);

        int initMPct = static_cast<int>(ReplayConfig::get().micVolume * 100.0f + 0.5f);
        m_micVolLabel = CCLabelBMFont::create((std::to_string(initMPct) + "%").c_str(), "goldFont.fnt");
        m_micVolLabel->setScale(0.42f);
        m_micVolLabel->setAnchorPoint(ccp(0.0f, 0.5f));
        m_micVolLabel->setPosition(ccp(418.0f, winSize.height - 168.0f));
        page->addChild(m_micVolLabel);

        // Save Location Section
        auto storageTitle = CCLabelBMFont::create("Replays Folder Path:", "goldFont.fnt");
        storageTitle->setScale(0.50f);
        storageTitle->setAnchorPoint(ccp(0.0f, 0.5f));
        storageTitle->setPosition(ccp(20.0f, winSize.height - 198.0f));
        page->addChild(storageTitle);

        m_saveFolderInput = TextInput::create(280.0f, "Custom directory path", "chatFont.fnt");
        m_saveFolderInput->setCommonFilter(CommonFilter::Any);
        m_saveFolderInput->setString(ReplayConfig::get().customSaveFolder);
        m_saveFolderInput->setPosition(ccp(centerX - 10.0f, winSize.height - 224.0f));
        setupInputPrio(m_saveFolderInput);
        m_saveFolderInput->setCallback([this](std::string const& str) {
            if (ReplayManager::get()->isReplayActive()) {
                Notification::create("Recording settings can only be changed when recording is OFF!", NotificationIcon::Warning, 1.2f)->show();
                m_saveFolderInput->setString(ReplayConfig::get().customSaveFolder);
                return;
            }
            ReplayConfig::get().customSaveFolder = str;
            ReplayConfig::get().saveToSettings();
            this->updateFolderDisplay();
        });
        page->addChild(m_saveFolderInput);

        // Storage Action Buttons
        auto storageActionMenu = CCMenu::create();
        storageActionMenu->setPosition(ccp(centerX, winSize.height - 258.0f));
        setupMenuPrio(storageActionMenu);
        page->addChild(storageActionMenu);

        auto bSpr = ButtonSprite::create("Browse...", "goldFont.fnt", "GJ_button_01.png", 0.75f);
        auto bBtn = CCMenuItemExt::createSpriteExtra(bSpr, [this](auto*) {
            this->onBrowseFolder();
        });
        setBtnScale(bBtn, 0.65f);
        storageActionMenu->addChild(bBtn);
        m_browseFolderBtn = bBtn;

        auto oSpr = ButtonSprite::create("Open Folder", "goldFont.fnt", "GJ_button_05.png", 0.75f);
        auto oBtn = CCMenuItemExt::createSpriteExtra(oSpr, [](auto*) {
            ReplayManager::get()->openRecordingsFolder();
        });
        setBtnScale(oBtn, 0.65f);
        storageActionMenu->addChild(oBtn);

        auto rSpr = ButtonSprite::create("Reset Default", "goldFont.fnt", "GJ_button_06.png", 0.75f);
        auto rBtn = CCMenuItemExt::createSpriteExtra(rSpr, [this](auto*) {
            this->onResetFolder();
        });
        setBtnScale(rBtn, 0.65f);
        storageActionMenu->addChild(rBtn);
        m_resetFolderBtn = rBtn;

        storageActionMenu->alignItemsHorizontallyWithPadding(8.0f);
    }

    fixAllTouchPriorities(m_mainLayer);

    switchTab(0);
    this->schedule(schedule_selector(ReplayPopup::updateStatusDisplay), 0.5f);
    refreshUI();
    updateStatusDisplay(0.0f);
    updateFolderDisplay();
}

void ReplayPopup::switchTab(int tabIndex) {
    m_currentTab = tabIndex;
    for (int i = 0; i < 4; ++i) {
        if (m_pages[i]) {
            m_pages[i]->setVisible(i == tabIndex);
        }
        if (m_tabButtons[i]) {
            if (auto spr = typeinfo_cast<ButtonSprite*>(m_tabButtons[i]->getNormalImage())) {
                spr->updateBGImage((i == tabIndex) ? "GJ_button_01.png" : "GJ_button_04.png");
            }
        }
    }
}

void ReplayPopup::updateFolderDisplay() {
    auto dir = ReplayConfig::get().getRecordingsDirectory().string();
    if (m_folderPathLabel) {
        // Truncate path if too long
        std::string displayStr = dir;
        if (displayStr.length() > 32) {
            displayStr = "..." + displayStr.substr(displayStr.length() - 29);
        }
        m_folderPathLabel->setString(displayStr.c_str());
    }
    if (m_saveFolderInput) {
        m_saveFolderInput->setString(ReplayConfig::get().customSaveFolder);
    }
}

void ReplayPopup::onBrowseFolder() {
    if (ReplayManager::get()->isReplayActive()) {
        Notification::create("Recording settings can only be changed when recording is OFF!", NotificationIcon::Warning, 1.2f)->show();
        return;
    }
    auto defaultDir = ReplayConfig::get().getRecordingsDirectory();

    file::FilePickOptions options;
    options.defaultPath = defaultDir;

    geode::Ref<ReplayPopup> self = this;
    geode::async::spawn(
        file::pick(file::PickMode::OpenFolder, options),
        [self](Result<std::optional<std::filesystem::path>> result) {
            if (result.isOk() && result.unwrap().has_value()) {
                auto path = std::move(result).unwrap().value();
                ReplayConfig::get().customSaveFolder = path.string();
                ReplayConfig::get().saveToSettings();
                self->updateFolderDisplay();
                Notification::create("Save folder updated", NotificationIcon::Success, 1.0f)->show();
            }
        }
    );
}

void ReplayPopup::onResetFolder() {
    if (ReplayManager::get()->isReplayActive()) {
        Notification::create("Recording settings can only be changed when recording is OFF!", NotificationIcon::Warning, 1.2f)->show();
        return;
    }
    ReplayConfig::get().customSaveFolder = "";
    ReplayConfig::get().saveToSettings();
    updateFolderDisplay();
}

void ReplayPopup::restartReplayScheduled(float) {
    if (ReplayManager::get()->isReplayActive()) {
        ReplayManager::get()->restartReplay();
    }
}

void ReplayPopup::onGameVolumeChanged(CCObject*) {
    if (!m_gameVolSlider) return;
    if (ReplayManager::get()->isReplayActive()) {
        Notification::create("Recording settings can only be changed when recording is OFF!", NotificationIcon::Warning, 1.2f)->show();
        m_gameVolSlider->setValue(std::clamp(ReplayConfig::get().systemVolume / 1.5f, 0.0f, 1.0f));
        return;
    }
    float val = m_gameVolSlider->getValue();
    float vol = val * 1.5f;
    auto& cfg = ReplayConfig::get();
    cfg.systemVolume = vol;
    cfg.saveToSettings();
    if (m_gameVolLabel) {
        int pct = static_cast<int>(vol * 100.0f + 0.5f);
        m_gameVolLabel->setString((std::to_string(pct) + "%").c_str());
    }
}

void ReplayPopup::onMicVolumeChanged(CCObject*) {
    if (!m_micVolSlider) return;
    if (ReplayManager::get()->isReplayActive()) {
        Notification::create("Recording settings can only be changed when recording is OFF!", NotificationIcon::Warning, 1.2f)->show();
        m_micVolSlider->setValue(std::clamp(ReplayConfig::get().micVolume / 1.5f, 0.0f, 1.0f));
        return;
    }
    float val = m_micVolSlider->getValue();
    float vol = val * 1.5f;
    auto& cfg = ReplayConfig::get();
    cfg.micVolume = vol;
    cfg.saveToSettings();
    if (m_micVolLabel) {
        int pct = static_cast<int>(vol * 100.0f + 0.5f);
        m_micVolLabel->setString((std::to_string(pct) + "%").c_str());
    }
}

void ReplayPopup::updateStatusDisplay(float) {
    auto mgr = ReplayManager::get();
    bool isRec = mgr->isReplayActive();

    if (m_statusLabel) {
        if (mgr->isSaving()) {
            m_statusLabel->setString("Saving Replay...");
        } else if (isRec) {
            m_statusLabel->setString("Replay Active (Settings Locked)");
        } else {
            m_statusLabel->setString("Replay Disabled (Ready to Configure)");
        }
    }

    if (m_statusDot) {
        if (mgr->isSaving()) {
            m_statusDot->setColor(ccc3(0, 220, 255)); // Cyan
        } else if (isRec) {
            m_statusDot->setColor(ccc3(50, 255, 50)); // Green
        } else {
            m_statusDot->setColor(ccc3(255, 60, 60)); // Red
        }
    }

    if (m_lockWarningBanner) {
        if (isRec) {
            m_lockWarningBanner->setString("(!) Recording is ACTIVE: settings locked! Stop recording to edit.");
            m_lockWarningBanner->setColor(ccc3(255, 85, 85));
        } else {
            m_lockWarningBanner->setString("Settings Unlocked: modify parameters, then start recording.");
            m_lockWarningBanner->setColor(ccc3(110, 245, 140));
        }
    }

    if (m_toggleBtn) {
        if (auto spr = typeinfo_cast<ButtonSprite*>(m_toggleBtn->getNormalImage())) {
            spr->setString(isRec ? "Stop Recording" : "Start Recording");
            spr->updateBGImage(isRec ? "GJ_button_06.png" : "GJ_button_01.png");
        }
    }

    const auto& cfg = ReplayConfig::get();

    if (m_diskSpaceWarnLabel) {
        if (cfg.isLowDiskSpace()) {
            uint64_t freeMB = cfg.getAvailableDiskSpaceBytes() / (1024 * 1024);
            uint64_t estMB = cfg.getEstimatedFileSizeMB();
            char buf[128];
            snprintf(buf, sizeof(buf), "! Warning: Low disk space! Free: %llu MB | Needed: %llu MB",
                     (unsigned long long)freeMB, (unsigned long long)estMB);
            m_diskSpaceWarnLabel->setString(buf);
            m_diskSpaceWarnLabel->setVisible(true);
        } else {
            m_diskSpaceWarnLabel->setVisible(false);
        }
    }

    if (m_codecBadgeLabel) {
        std::string cName = cfg.getCodecDisplayName();

        const char* rName = "Native";
        if (cfg.resolution == ResolutionType::UHD_4K) rName = "4K";
        else if (cfg.resolution == ResolutionType::QHD_1440P) rName = "1440p";
        else if (cfg.resolution == ResolutionType::FHD_1080P) rName = "1080p";
        else if (cfg.resolution == ResolutionType::HD_720P) rName = "720p";

        const char* rcName = (cfg.rateControl == RateControlMode::CONSTANT_CBR) ? "CBR" : "Adaptive VBR";

        char buf[160];
        snprintf(buf, sizeof(buf), "%s | %s | %d Mbps (%s)", cName.c_str(), rName, cfg.getEffectiveBitrateMbps(), rcName);
        m_codecBadgeLabel->setString(buf);
    }

    if (m_estSizeLabel) {
        m_estSizeLabel->setString(cfg.getEstimatedFileSizeText().c_str());
    }
    if (m_estSizeBitrateLabel) {
        m_estSizeBitrateLabel->setString(cfg.getEstimatedFileSizeDetailedText().c_str());
    }
    if (m_estSizeResLabel) {
        m_estSizeResLabel->setString(cfg.getEstimatedFileSizeDetailedText().c_str());
    }
}

void ReplayPopup::refreshUI() {
    const auto& cfg = ReplayConfig::get();

    // Duration menu highlight
    if (m_durationMenu) {
        for (auto child : CCArrayExt<CCNode*>(m_durationMenu->getChildren())) {
            if (auto btn = typeinfo_cast<CCMenuItemSpriteExtra*>(child)) {
                bool active = (btn->getTag() == cfg.durationSeconds);
                if (auto spr = typeinfo_cast<ButtonSprite*>(btn->getNormalImage())) {
                    spr->updateBGImage(active ? "GJ_button_01.png" : "GJ_button_04.png");
                }
            }
        }
    }

    // Codec menu highlight
    if (m_codecMenu) {
        int curCodecTag = static_cast<int>(cfg.codec);
        for (auto child : CCArrayExt<CCNode*>(m_codecMenu->getChildren())) {
            if (auto btn = typeinfo_cast<CCMenuItemSpriteExtra*>(child)) {
                bool active = (btn->getTag() == curCodecTag && cfg.codec != CodecType::CUSTOM);
                if (auto spr = typeinfo_cast<ButtonSprite*>(btn->getNormalImage())) {
                    spr->updateBGImage(active ? "GJ_button_01.png" : "GJ_button_04.png");
                }
            }
        }
    }

    if (m_hwAccelToggle) {
        m_hwAccelToggle->toggle(cfg.hardwareAcceleration);
    }
    if (m_hwAccelLabel) {
        if (cfg.hardwareAcceleration) {
            std::string text = "Hardware Acceleration (Auto GPU: " + ReplayConfig::getGpuAccelName() + ")";
            m_hwAccelLabel->setString(text.c_str());
            m_hwAccelLabel->setColor(ccc3(255, 255, 255));
        } else {
            m_hwAccelLabel->setString("Hardware Acceleration (Disabled: CPU Software)");
            m_hwAccelLabel->setColor(ccc3(180, 180, 180));
        }
    }

    bool isH264 = (cfg.codec == CodecType::H264);

    // Resolution menu highlight & H.264 4K limit
    if (m_resMenu) {
        int curResTag = static_cast<int>(cfg.resolution);
        for (auto child : CCArrayExt<CCNode*>(m_resMenu->getChildren())) {
            if (auto btn = typeinfo_cast<CCMenuItemSpriteExtra*>(child)) {
                bool active = (btn->getTag() == curResTag);
                bool disabled = (isH264 && btn->getTag() == 1 && cfg.targetFps >= 240);
                btn->setEnabled(!disabled);
                if (auto spr = typeinfo_cast<ButtonSprite*>(btn->getNormalImage())) {
                    if (disabled) {
                        spr->setColor(ccc3(110, 110, 110));
                        spr->setOpacity(130);
                        spr->updateBGImage("GJ_button_04.png");
                    } else {
                        spr->setColor(ccc3(255, 255, 255));
                        spr->setOpacity(255);
                        spr->updateBGImage(active ? "GJ_button_01.png" : "GJ_button_04.png");
                    }
                }
            }
        }
    }

    // FPS menu highlight & H.264 240 FPS limit
    if (m_fpsMenu) {
        for (auto child : CCArrayExt<CCNode*>(m_fpsMenu->getChildren())) {
            if (auto btn = typeinfo_cast<CCMenuItemSpriteExtra*>(child)) {
                bool active = (btn->getTag() == cfg.targetFps);
                bool disabled = (isH264 && btn->getTag() == 240 && cfg.resolution == ResolutionType::UHD_4K);
                btn->setEnabled(!disabled);
                if (auto spr = typeinfo_cast<ButtonSprite*>(btn->getNormalImage())) {
                    if (disabled) {
                        spr->setColor(ccc3(110, 110, 110));
                        spr->setOpacity(130);
                        spr->updateBGImage("GJ_button_04.png");
                    } else {
                        spr->setColor(ccc3(255, 255, 255));
                        spr->setOpacity(255);
                        spr->updateBGImage(active ? "GJ_button_01.png" : "GJ_button_04.png");
                    }
                }
            }
        }
    }

    if (m_codecConflictLabel) {
        bool conflictPossible = isH264 && (cfg.resolution == ResolutionType::UHD_4K || cfg.targetFps >= 240);
        m_codecConflictLabel->setVisible(conflictPossible);
    }

    // Rate Control highlight
    bool isCBR = (cfg.rateControl == RateControlMode::CONSTANT_CBR);
    if (m_rateControlMenu) {
        int curRcTag = static_cast<int>(cfg.rateControl);
        for (auto child : CCArrayExt<CCNode*>(m_rateControlMenu->getChildren())) {
            if (auto btn = typeinfo_cast<CCMenuItemSpriteExtra*>(child)) {
                bool active = (btn->getTag() == curRcTag);
                if (auto spr = typeinfo_cast<ButtonSprite*>(btn->getNormalImage())) {
                    spr->updateBGImage(active ? "GJ_button_01.png" : "GJ_button_04.png");
                }
            }
        }
    }

    // Target Bitrate highlight
    if (m_bitrateMenu) {
        for (auto child : CCArrayExt<CCNode*>(m_bitrateMenu->getChildren())) {
            if (auto btn = typeinfo_cast<CCMenuItemSpriteExtra*>(child)) {
                bool active = (btn->getTag() == cfg.bitrateMbps);
                if (auto spr = typeinfo_cast<ButtonSprite*>(btn->getNormalImage())) {
                    spr->updateBGImage(active ? "GJ_button_01.png" : "GJ_button_04.png");
                }
            }
        }
    }

    // Custom Bitrate input update
    if (m_customBitrateInput) {
        m_customBitrateInput->setString(std::to_string(cfg.bitrateMbps));
    }

    // Peak Bitrate node visibility (hide in CBR mode)
    if (m_peakBitrateNode) {
        m_peakBitrateNode->setVisible(!isCBR);
    }

    // Peak Bitrate highlight
    if (m_maxBitrateMenu) {
        for (auto child : CCArrayExt<CCNode*>(m_maxBitrateMenu->getChildren())) {
            if (auto btn = typeinfo_cast<CCMenuItemSpriteExtra*>(child)) {
                bool active = (btn->getTag() == cfg.maxBitrateMbps);
                if (auto spr = typeinfo_cast<ButtonSprite*>(btn->getNormalImage())) {
                    spr->updateBGImage(active ? "GJ_button_01.png" : "GJ_button_04.png");
                }
            }
        }
    }

    // Volume sliders and percentage update
    if (m_gameVolSlider) {
        m_gameVolSlider->setValue(std::clamp(cfg.systemVolume / 1.5f, 0.0f, 1.0f));
    }
    if (m_gameVolLabel) {
        int gPercent = static_cast<int>(cfg.systemVolume * 100.0f + 0.5f);
        m_gameVolLabel->setString((std::to_string(gPercent) + "%").c_str());
    }

    if (m_micVolSlider) {
        m_micVolSlider->setValue(std::clamp(cfg.micVolume / 1.5f, 0.0f, 1.0f));
    }
    if (m_micVolLabel) {
        int mPercent = static_cast<int>(cfg.micVolume * 100.0f + 0.5f);
        m_micVolLabel->setString((std::to_string(mPercent) + "%").c_str());
    }

    if (m_splitAudioToggle) m_splitAudioToggle->toggle(cfg.splitAudioTracks);
    if (m_micToggle) m_micToggle->toggle(cfg.recordMic);
    refreshMicDeviceButton();
}

void ReplayPopup::cycleMicDevice(int direction) {
    if (ReplayManager::get()->isReplayActive()) {
        Notification::create("Recording settings can only be changed when recording is OFF!", NotificationIcon::Warning, 1.2f)->show();
        return;
    }
    if (m_availableMics.empty()) return;

    int n = static_cast<int>(m_availableMics.size());
    m_currentMicIndex = (m_currentMicIndex + direction + n) % n;

    auto& cfg = ReplayConfig::get();
    cfg.micDeviceId = m_availableMics[m_currentMicIndex].id;
    cfg.micDeviceName = m_availableMics[m_currentMicIndex].name;
    cfg.saveToSettings();

    AudioGrabber::get()->restartMicCapture();

    refreshMicDeviceButton();

    std::string msg = "Mic: " + cfg.micDeviceName;
    Notification::create(msg, NotificationIcon::Info, 1.0f)->show();
}

void ReplayPopup::refreshMicDeviceButton() {
    if (!m_micDeviceBtn || m_availableMics.empty()) return;
    if (m_currentMicIndex < 0 || m_currentMicIndex >= static_cast<int>(m_availableMics.size())) {
        m_currentMicIndex = 0;
    }
    std::string name = m_availableMics[m_currentMicIndex].name;
    if (name.length() > 24) {
        name = name.substr(0, 21) + "...";
    }
    auto spr = ButtonSprite::create(name.c_str(), "goldFont.fnt", "GJ_button_01.png", 0.75f);
    m_micDeviceBtn->setNormalImage(spr);
    m_micDeviceBtn->setContentSize(spr->getContentSize());
    setBtnScale(m_micDeviceBtn, 0.62f);
    if (m_micDevMenu) {
        m_micDevMenu->alignItemsHorizontallyWithPadding(6.0f);
    }
}

} // namespace replay
