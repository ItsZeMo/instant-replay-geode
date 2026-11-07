#include "ReplayPopup.hpp"

#ifdef GEODE_IS_WINDOWS

#include "CodecGuidePopup.hpp"
#include <cocos2d.h>
#include <Geode/loader/SettingV3.hpp>
#include <Geode/utils/file.hpp>
#include <Geode/utils/async.hpp>
#include <Geode/utils/string.hpp>
#include <algorithm>
#include <span>

using namespace cocos2d;
using namespace geode::prelude;

namespace replay {

namespace {

static constexpr float kMaxVolume = 2.0f; // 0% - 200% (matches mod.json max: 200)

struct PresetOption {
    const char* label;
    int tag;
};

void setBtnScale(CCMenuItemSpriteExtra* btn, float scale) {
    if (!btn) return;
    btn->setScale(scale);
    btn->m_baseScale = scale;
}

bool isInputFocused(TextInput* input) {
    return input && input->getInputNode() && input->getInputNode()->m_selected;
}

std::string utf8TailWithEllipsis(const std::string& str, size_t maxCodepoints) {
    if (maxCodepoints <= 3 || str.size() <= maxCodepoints) return str;

    std::vector<size_t> leadOffsets;
    leadOffsets.reserve(str.size());
    for (size_t i = 0; i < str.size(); ++i) {
        if ((static_cast<unsigned char>(str[i]) & 0xC0u) != 0x80u) {
            leadOffsets.push_back(i);
        }
    }
    if (leadOffsets.size() <= maxCodepoints) {
        return str;
    }
    size_t keep = maxCodepoints - 3;
    size_t startByte = leadOffsets[leadOffsets.size() - keep];
    return "..." + str.substr(startByte);
}

void dimSpriteNode(CCNode* node, bool dimmed, ccColor3B normalColor = ccc3(255, 255, 255)) {
    if (auto rgba = typeinfo_cast<CCRGBAProtocol*>(node)) {
        rgba->setOpacity(dimmed ? 135 : 255);
        rgba->setColor(dimmed ? ccc3(150, 150, 150) : normalColor);
    }
}

void setMenuItemLocked(CCMenuItemSpriteExtra* btn, bool locked) {
    if (!btn) return;
    btn->setEnabled(!locked);
    dimSpriteNode(btn->getNormalImage(), locked);
}

void setTogglerLocked(CCMenuItemToggler* toggler, bool locked) {
    if (!toggler) return;
    toggler->setEnabled(!locked);
    if (toggler->m_onButton) dimSpriteNode(toggler->m_onButton->getNormalImage(), locked);
    if (toggler->m_offButton) dimSpriteNode(toggler->m_offButton->getNormalImage(), locked);
}

void setSliderLocked(Slider* slider, bool locked) {
    if (!slider) return;
    slider->m_enabled = !locked;
    if (slider->m_touchLogic) {
        slider->m_touchLogic->setEnabled(!locked);
        if (slider->m_touchLogic->m_thumb) {
            slider->m_touchLogic->m_thumb->setEnabled(!locked);
            dimSpriteNode(slider->m_touchLogic->m_thumb->getNormalImage(), locked);
        }
    }
    dimSpriteNode(slider->m_sliderBar, locked);
    dimSpriteNode(slider->m_groove, locked);
}

void setTextInputLocked(TextInput* input, bool locked) {
    if (!input) return;
    if (locked) {
        input->defocus();
    }
    input->setEnabled(!locked);
}

void updateMenuHighlight(
    CCMenu* menu,
    int activeTag,
    bool allowActive = true,
    int disabledTag = -1,
    bool globalLocked = false
) {
    if (!menu) return;
    for (auto child : CCArrayExt<CCNode*>(menu->getChildren())) {
        if (auto btn = typeinfo_cast<CCMenuItemSpriteExtra*>(child)) {
            int tag = btn->getTag();
            bool active = allowActive && (tag == activeTag);
            bool itemDisabled = globalLocked || (disabledTag != -1 && tag == disabledTag);
            btn->setEnabled(!itemDisabled);
            if (auto spr = typeinfo_cast<ButtonSprite*>(btn->getNormalImage())) {
                spr->updateBGImage(active ? "GJ_button_01.png" : "GJ_button_04.png");
                if (itemDisabled) {
                    spr->setColor(active ? ccc3(195, 215, 195) : ccc3(120, 120, 120));
                    spr->setOpacity(135);
                } else {
                    spr->setColor(ccc3(255, 255, 255));
                    spr->setOpacity(255);
                }
            }
        }
    }
}

template <typename Callback>
CCMenu* createPresetMenu(
    CCNode* parent,
    CCPoint pos,
    std::span<const PresetOption> options,
    float btnScale,
    float padding,
    Callback&& onSelect
) {
    auto menu = CCMenu::create();
    menu->setPosition(pos);
    parent->addChild(menu);

    for (const auto& opt : options) {
        auto spr = ButtonSprite::create(opt.label, "goldFont.fnt", "GJ_button_04.png", 0.75f);
        auto btn = CCMenuItemExt::createSpriteExtra(spr, [onSelect, tag = opt.tag](auto*) {
            onSelect(tag);
        });
        setBtnScale(btn, btnScale);
        btn->setTag(opt.tag);
        menu->addChild(btn);
    }
    menu->alignItemsHorizontallyWithPadding(padding);
    return menu;
}

template <typename Callback>
CCMenuItemToggler* addLabeledToggler(
    CCNode* page,
    CCMenu* menu,
    CCPoint pos,
    float togglerScale,
    bool initialValue,
    const char* labelText,
    float labelScale,
    float labelOffsetX,
    CCLabelBMFont** outLabel,
    Callback&& onClick
) {
    auto toggler = CCMenuItemExt::createTogglerWithStandardSprites(togglerScale, std::forward<Callback>(onClick));
    toggler->toggle(initialValue);
    toggler->setPosition(pos);
    menu->addChild(toggler);

    auto label = CCLabelBMFont::create(labelText, "chatFont.fnt");
    label->setScale(labelScale);
    label->setAnchorPoint(ccp(0.0f, 0.5f));
    label->setPosition(ccp(pos.x + labelOffsetX, pos.y));
    page->addChild(label);
    if (outLabel) {
        *outLabel = label;
    }
    return toggler;
}

void createVolumeSliderControl(
    CCNode* page,
    CCNode* target,
    SEL_MenuHandler selector,
    const char* titleText,
    float baseX,
    float rowY,
    float initialVol,
    Slider*& outSlider,
    CCLabelBMFont*& outLabel
) {
    auto title = CCLabelBMFont::create(titleText, "goldFont.fnt");
    title->setScale(0.44f);
    title->setAnchorPoint(ccp(0.0f, 0.5f));
    title->setPosition(ccp(baseX, rowY));
    page->addChild(title);

    outSlider = Slider::create(target, selector, 0.38f);
    outSlider->setPosition(ccp(baseX + 98.0f, rowY));
    outSlider->setValue(std::clamp(initialVol / kMaxVolume, 0.0f, 1.0f));
    outSlider->setLiveDragging(true);
    page->addChild(outSlider);

    int initPct = static_cast<int>(initialVol * 100.0f + 0.5f);
    outLabel = CCLabelBMFont::create((std::to_string(initPct) + "%").c_str(), "goldFont.fnt");
    outLabel->setScale(0.42f);
    outLabel->setAnchorPoint(ccp(0.0f, 0.5f));
    outLabel->setPosition(ccp(baseX + 146.0f, rowY));
    page->addChild(outLabel);
}

void syncVolumeControls(Slider* slider, CCLabelBMFont* label, float vol) {
    if (slider) {
        slider->setValue(std::clamp(vol / kMaxVolume, 0.0f, 1.0f));
    }
    if (label) {
        int pct = static_cast<int>(vol * 100.0f + 0.5f);
        label->setString((std::to_string(pct) + "%").c_str());
    }
}

std::string getSaveKeybindLabel() {
    if (auto mod = geode::Mod::get()) {
        if (auto sett = mod->getSetting("save-replay")) {
            if (auto kbSett = geode::cast::typeinfo_pointer_cast<KeybindSettingV3>(sett)) {
                auto const& list = kbSett->getValue();
                if (!list.empty()) {
                    std::string s = list.front().toString();
                    if (!s.empty()) {
                        return "Save Replay (" + s + ")";
                    }
                }
            }
        }
    }
    return "Save Replay (F6)";
}

} // namespace

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
    if (auto popup = ReplayPopup::create()) {
        popup->show();
    }
}

ReplayPopup::~ReplayPopup() {
    m_diskCheckTask.cancel();
    if (s_instance == this) {
        s_instance = nullptr;
    }
}

void ReplayPopup::finalizePendingBitrateClamp() {
    auto& cfg = ReplayConfig::get();
    int b = cfg.bitrateMbps.load();
    if (cfg.maxBitrateMbps.load() < b) {
        cfg.maxBitrateMbps = b;
        m_settingsDirty = true;
    }
}

void ReplayPopup::markSettingsDirty(bool flushToDisk) {
    m_settingsDirty = !flushToDisk;
    ReplayConfig::get().saveToSettings(flushToDisk);
}

void ReplayPopup::onClose(CCObject* sender) {
    m_diskCheckTask.cancel();
    if (s_instance == this) {
        s_instance = nullptr;
    }
    if (!ReplayManager::get()->isReplayActive()) {
        finalizePendingBitrateClamp();
    }
    if (m_settingsDirty) {
        ReplayConfig::get().saveToSettings(true);
        m_settingsDirty = false;
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

bool ReplayPopup::guardLocked() const {
    if (ReplayManager::get()->isReplayActive()) {
        static std::chrono::steady_clock::time_point s_lastWarnTime{};
        auto now = std::chrono::steady_clock::now();
        if (now - s_lastWarnTime >= std::chrono::milliseconds(1500)) {
            s_lastWarnTime = now;
            Notification::create("Recording settings can only be changed when recording is OFF!", NotificationIcon::Warning, 1.2f)->show();
        }
        return true;
    }
    return false;
}

void ReplayPopup::applyLockState(bool locked) {
    if (m_hasAppliedLockState && m_isLocked == locked) return;
    m_hasAppliedLockState = true;
    m_isLocked = locked;

    setTextInputLocked(m_durationInput, locked);
    setTextInputLocked(m_customCodecInput, locked);
    setTextInputLocked(m_customBitrateInput, locked);
    setTextInputLocked(m_saveFolderInput, locked);

    setTogglerLocked(m_hwAccelToggle, locked);
    setTogglerLocked(m_splitAudioToggle, locked);
    setTogglerLocked(m_micToggle, locked);

    setSliderLocked(m_gameVolSlider, locked);
    setSliderLocked(m_micVolSlider, locked);

    setMenuItemLocked(m_quickBrowseBtn, locked);
    setMenuItemLocked(m_storageBrowseBtn, locked);
    setMenuItemLocked(m_storageResetBtn, locked);

    if (m_micDevMenu) {
        for (auto child : CCArrayExt<CCNode*>(m_micDevMenu->getChildren())) {
            if (auto btn = typeinfo_cast<CCMenuItemSpriteExtra*>(child)) {
                setMenuItemLocked(btn, locked);
            }
        }
    }

    if (m_splitAudioLabel) m_splitAudioLabel->setOpacity(locked ? 150 : 255);
    if (m_micRecordLabel) m_micRecordLabel->setOpacity(locked ? 150 : 255);
    if (m_hwAccelLabel) m_hwAccelLabel->setOpacity(locked ? 150 : 255);

    // Re-sync menu button highlights & enabled states
    refreshUI(false);
}

void ReplayPopup::scheduleDiskSpaceCheck(bool immediate) {
    if (immediate) {
        m_diskCheckPending = false;
        m_diskCheckTimer = 0.0f;
        triggerAsyncDiskCheck();
    } else {
        m_diskCheckPending = true;
        m_diskCheckDebounce = 0.35f;
    }
}

void ReplayPopup::triggerAsyncDiskCheck() {
    if (!m_diskCheckInFlight || m_diskCheckInFlight->exchange(true)) {
        return;
    }
    ReplayConfig snap = ReplayConfig::get().snapshot();
    auto busyFlag = m_diskCheckInFlight;

    m_diskCheckTask.spawn(
        geode::async::runtime().spawnBlocking<DiskSpaceResult>([snap = std::move(snap), busyFlag]() {
            DiskSpaceResult res{};
            res.low = snap.isLowDiskSpace(&res.freeMB, &res.neededMB, &res.isTempLow);
            busyFlag->store(false);
            return res;
        }),
        [this](DiskSpaceResult res) {
            this->applyDiskSpaceResult(res.low, res.freeMB, res.neededMB, res.isTempLow);
        }
    );
}

void ReplayPopup::applyDiskSpaceResult(bool low, uint64_t freeMB, uint64_t neededMB, bool isTempLow) {
    m_cachedDiskLow = low;
    m_cachedFreeMB = freeMB;
    m_cachedNeededMB = neededMB;
    m_cachedIsTempLow = isTempLow;
    refreshDiskWarnLabel();
}

void ReplayPopup::refreshDiskWarnLabel() {
    if (!m_diskSpaceWarnLabel) return;
    if (m_cachedDiskLow) {
        char buf[140];
        snprintf(buf, sizeof(buf), "! Warning: Low %s space! Free: %llu MB | Needed: %llu MB",
                 m_cachedIsTempLow ? "TEMP" : "save",
                 static_cast<unsigned long long>(m_cachedFreeMB),
                 static_cast<unsigned long long>(m_cachedNeededMB));
        m_diskSpaceWarnLabel->setString(buf);
        m_diskSpaceWarnLabel->setVisible(true);
    } else {
        m_diskSpaceWarnLabel->setVisible(false);
    }
}

void ReplayPopup::setup() {
    s_instance = this;
    this->setID("replay-popup");

    ReplayConfig::refreshNativeFrameHeight();

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

    setupDashboardTab(m_pages[0], winSize, centerX);
    setupCodecTab(m_pages[1], winSize, centerX);
    setupBitrateTab(m_pages[2], winSize, centerX);
    setupAudioStorageTab(m_pages[3], winSize, centerX);

    switchTab(0);
    this->schedule(schedule_selector(ReplayPopup::updateStatusDisplay), 0.5f);
    refreshUI();
    updateStatusDisplay(0.0f);
    updateFolderDisplay();
    scheduleDiskSpaceCheck(true);
}

void ReplayPopup::setupDashboardTab(CCNode* page, CCSize const& winSize, float centerX) {
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

    // Action Buttons Menu (always unlocked!)
    auto actionMenu = CCMenu::create();
    actionMenu->setPosition(ccp(centerX, winSize.height - 144.0f));
    page->addChild(actionMenu);

    std::string saveLabel = getSaveKeybindLabel();
    auto saveSpr = ButtonSprite::create(saveLabel.c_str(), "goldFont.fnt", "GJ_button_01.png", 0.75f);
    auto saveBtn = CCMenuItemExt::createSpriteExtra(saveSpr, [this](auto*) {
        ReplayManager::get()->saveReplay();
        this->updateStatusDisplay(0.0f);
    });
    setBtnScale(saveBtn, 0.68f);
    actionMenu->addChild(saveBtn);

    auto toggleSpr = ButtonSprite::create("Toggle Replay", "goldFont.fnt", "GJ_button_04.png", 0.75f);
    m_toggleBtn = CCMenuItemExt::createSpriteExtra(toggleSpr, [this](auto*) {
        if (!ReplayManager::get()->isReplayActive()) {
            this->finalizePendingBitrateClamp();
            if (m_settingsDirty) {
                ReplayConfig::get().saveToSettings(true);
                m_settingsDirty = false;
            }
        }
        ReplayManager::get()->toggleReplay();
        this->refreshUI();
        this->updateStatusDisplay(0.0f);
        this->scheduleDiskSpaceCheck(true);
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

    static constexpr PresetOption kDurOpts[] = {
        {"30s", 30}, {"1m", 60}, {"2m", 120}, {"5m", 300}, {"10m", 600}
    };
    m_durationMenu = createPresetMenu(
        page, ccp(centerX + 35.0f, winSize.height - 180.0f), kDurOpts, 0.62f, 5.0f,
        [this](int sec) {
            if (this->guardLocked()) return;
            ReplayConfig::get().durationSeconds = sec;
            this->markSettingsDirty(false);
            if (m_durationInput) {
                m_durationInput->setString(std::to_string(sec));
            }
            this->refreshUI();
            this->updateStatusDisplay(0.0f);
            this->scheduleDiskSpaceCheck(true);
        }
    );

    // Custom Duration Manual Input Row
    auto customDurTitle = CCLabelBMFont::create("Custom (sec):", "goldFont.fnt");
    customDurTitle->setScale(0.48f);
    customDurTitle->setAnchorPoint(ccp(0.0f, 0.5f));
    customDurTitle->setPosition(ccp(25.0f, winSize.height - 215.0f));
    page->addChild(customDurTitle);

    m_durationInput = TextInput::create(75.0f, "sec", "chatFont.fnt");
    m_durationInput->setCommonFilter(CommonFilter::Uint);
    m_durationInput->setString(std::to_string(ReplayConfig::get().durationSeconds.load()));
    m_durationInput->setPosition(ccp(160.0f, winSize.height - 215.0f));
    m_durationInput->setCallback([this](std::string const& str) {
        if (this->guardLocked()) {
            m_durationInput->setString(std::to_string(ReplayConfig::get().durationSeconds.load()));
            return;
        }
        if (!str.empty()) {
            try {
                int sec = std::clamp(std::stoi(str), 5, 3600);
                ReplayConfig::get().durationSeconds = sec;
                this->markSettingsDirty(false);
                this->refreshUI();
                this->updateStatusDisplay(0.0f);
                this->scheduleDiskSpaceCheck(false);
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

    // Estimated File Size badge
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
    page->addChild(folderQuickMenu);

    auto browseSpr = ButtonSprite::create("Browse", "goldFont.fnt", "GJ_button_04.png", 0.75f);
    m_quickBrowseBtn = CCMenuItemExt::createSpriteExtra(browseSpr, [this](auto*) {
        this->onBrowseFolder();
    });
    setBtnScale(m_quickBrowseBtn, 0.62f);
    folderQuickMenu->addChild(m_quickBrowseBtn);
}

void ReplayPopup::setupCodecTab(CCNode* page, CCSize const& winSize, float centerX) {
    auto codecTitle = CCLabelBMFont::create("Codec:", "goldFont.fnt");
    codecTitle->setScale(0.52f);
    codecTitle->setAnchorPoint(ccp(0.0f, 0.5f));
    codecTitle->setPosition(ccp(20.0f, winSize.height - 76.0f));
    page->addChild(codecTitle);

    static constexpr PresetOption kCodecOpts[] = {
        {"AV1", static_cast<int>(CodecType::AV1)},
        {"HEVC", static_cast<int>(CodecType::HEVC)},
        {"H.264", static_cast<int>(CodecType::H264)}
    };
    m_codecMenu = createPresetMenu(
        page, ccp(175.0f, winSize.height - 76.0f), kCodecOpts, 0.62f, 8.0f,
        [this](int tag) {
            if (this->guardLocked()) return;
            auto& cfg = ReplayConfig::get();
            cfg.codec = static_cast<CodecType>(tag);
            cfg.setCustomCodec("");
            cfg.sanitizeCodecSettings();
            this->markSettingsDirty(false);
            if (m_customCodecInput) {
                m_customCodecInput->setString("");
            }
            this->refreshUI();
            this->updateStatusDisplay(0.0f);
        }
    );

    // Hardware Acceleration Checkbox (Auto GPU: NVENC / AMF / QSV)
    m_hwMenu = CCMenu::create();
    m_hwMenu->setPosition(CCPointZero);
    page->addChild(m_hwMenu);

    std::string accelText = "Hardware Acceleration (Auto GPU: " + ReplayConfig::getGpuAccelName() + ")";
    // Note: CCMenuItemToggler::activate() flips the visual state AFTER invoking this callback.
    m_hwAccelToggle = addLabeledToggler(
        page, m_hwMenu, ccp(35.0f, winSize.height - 105.0f), 0.65f,
        ReplayConfig::get().hardwareAcceleration.load(),
        accelText.c_str(), 0.48f, 22.0f, &m_hwAccelLabel,
        [this](auto* toggler) {
            bool curHw = ReplayConfig::get().hardwareAcceleration.load();
            if (this->guardLocked()) {
                toggler->toggle(!curHw);
                return;
            }
            ReplayConfig::get().hardwareAcceleration = !curHw;
            this->markSettingsDirty(false);
            this->refreshUI(false);
            this->updateStatusDisplay(0.0f);
        }
    );

    // Custom Codec Manual Input Row + Guide Button
    auto customCodecTitle = CCLabelBMFont::create("Custom Codec:", "goldFont.fnt");
    customCodecTitle->setScale(0.48f);
    customCodecTitle->setAnchorPoint(ccp(0.0f, 0.5f));
    customCodecTitle->setPosition(ccp(20.0f, winSize.height - 135.0f));
    page->addChild(customCodecTitle);

    m_customCodecInput = TextInput::create(150.0f, "e.g. av1_nvenc", "chatFont.fnt");
    m_customCodecInput->setCommonFilter(CommonFilter::Any);
    m_customCodecInput->setString(ReplayConfig::get().getCustomCodec());
    m_customCodecInput->setPosition(ccp(centerX - 10.0f, winSize.height - 135.0f));
    m_customCodecInput->setCallback([this](std::string const& str) {
        if (this->guardLocked()) {
            m_customCodecInput->setString(ReplayConfig::get().getCustomCodec());
            return;
        }
        auto& c = ReplayConfig::get();
        c.setCustomCodec(str);
        if (!str.empty()) {
            c.codec = CodecType::CUSTOM;
        } else if (c.codec.load() == CodecType::CUSTOM) {
            c.codec = CodecType::AV1;
            c.sanitizeCodecSettings();
        }
        this->markSettingsDirty(false);
        this->refreshUI();
        this->updateStatusDisplay(0.0f);
    });
    page->addChild(m_customCodecInput);

    auto guideMenu = CCMenu::create();
    guideMenu->setPosition(ccp(winSize.width - 65.0f, winSize.height - 135.0f));
    page->addChild(guideMenu);

    auto guideSpr = ButtonSprite::create("? Guide", "goldFont.fnt", "GJ_button_02.png", 0.75f);
    auto guideBtn = CCMenuItemExt::createSpriteExtra(guideSpr, [](auto*) {
        if (auto popup = CodecGuidePopup::create()) {
            popup->show();
        }
    });
    setBtnScale(guideBtn, 0.62f);
    guideMenu->addChild(guideBtn);

    // Resolution Row
    auto resTitle = CCLabelBMFont::create("Resolution:", "goldFont.fnt");
    resTitle->setScale(0.52f);
    resTitle->setAnchorPoint(ccp(0.0f, 0.5f));
    resTitle->setPosition(ccp(20.0f, winSize.height - 162.0f));
    page->addChild(resTitle);

    static constexpr PresetOption kResOpts[] = {
        {"Native", 0}, {"4K", 1}, {"1440p", 2}, {"1080p", 3}, {"720p", 4}
    };
    m_resMenu = createPresetMenu(
        page, ccp(centerX + 35.0f, winSize.height - 162.0f), kResOpts, 0.60f, 4.0f,
        [this](int tag) {
            if (this->guardLocked()) return;
            auto& cfg = ReplayConfig::get();
            cfg.resolution = static_cast<ResolutionType>(tag);
            cfg.sanitizeCodecSettings();
            this->markSettingsDirty(false);
            this->refreshUI();
            this->updateStatusDisplay(0.0f);
            this->scheduleDiskSpaceCheck(true);
        }
    );

    // Target FPS Row
    auto fpsTitle = CCLabelBMFont::create("Target FPS:", "goldFont.fnt");
    fpsTitle->setScale(0.52f);
    fpsTitle->setAnchorPoint(ccp(0.0f, 0.5f));
    fpsTitle->setPosition(ccp(20.0f, winSize.height - 200.0f));
    page->addChild(fpsTitle);

    static constexpr PresetOption kFpsOpts[] = {
        {"60 FPS", 60}, {"120 FPS", 120}, {"240 FPS", 240}
    };
    m_fpsMenu = createPresetMenu(
        page, ccp(centerX + 35.0f, winSize.height - 200.0f), kFpsOpts, 0.62f, 8.0f,
        [this](int fps) {
            if (this->guardLocked()) return;
            auto& cfg = ReplayConfig::get();
            cfg.targetFps = fps;
            cfg.sanitizeCodecSettings();
            this->markSettingsDirty(false);
            this->refreshUI();
            this->updateStatusDisplay(0.0f);
            this->scheduleDiskSpaceCheck(true);
        }
    );

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

    auto codecNotice = CCLabelBMFont::create("Select your preferred codec, GPU acceleration, resolution & FPS.", "chatFont.fnt");
    codecNotice->setScale(0.36f);
    codecNotice->setColor(ccc3(130, 240, 190));
    codecNotice->setPosition(ccp(centerX, winSize.height - 264.0f));
    page->addChild(codecNotice);
}

void ReplayPopup::setupBitrateTab(CCNode* page, CCSize const& winSize, float centerX) {
    auto rcTitle = CCLabelBMFont::create("Rate Control:", "goldFont.fnt");
    rcTitle->setScale(0.52f);
    rcTitle->setAnchorPoint(ccp(0.0f, 0.5f));
    rcTitle->setPosition(ccp(25.0f, winSize.height - 82.0f));
    page->addChild(rcTitle);

    static constexpr PresetOption kRcOpts[] = {
        {"Adaptive (VBR)", 0}, {"Constant (CBR)", 1}, {"Quality (CQ)", 2}
    };
    m_rateControlMenu = createPresetMenu(
        page, ccp(centerX + 40.0f, winSize.height - 82.0f), kRcOpts, 0.56f, 6.0f,
        [this](int tag) {
            if (this->guardLocked()) return;
            ReplayConfig::get().rateControl = static_cast<RateControlMode>(tag);
            this->markSettingsDirty(false);
            this->refreshUI();
            this->updateStatusDisplay(0.0f);
            this->scheduleDiskSpaceCheck(true);
        }
    );

    // Target Bitrate Row
    auto bitTitle = CCLabelBMFont::create("Target Bitrate:", "goldFont.fnt");
    bitTitle->setScale(0.50f);
    bitTitle->setAnchorPoint(ccp(0.0f, 0.5f));
    bitTitle->setPosition(ccp(25.0f, winSize.height - 120.0f));
    page->addChild(bitTitle);

    static constexpr PresetOption kBitrateOpts[] = {
        {"15M", 15}, {"25M", 25}, {"35M", 35}, {"50M", 50}, {"80M", 80}
    };
    m_bitrateMenu = createPresetMenu(
        page, ccp(centerX + 40.0f, winSize.height - 120.0f), kBitrateOpts, 0.62f, 6.0f,
        [this](int b) {
            if (this->guardLocked()) return;
            auto& cfg = ReplayConfig::get();
            cfg.bitrateMbps = b;
            if (cfg.maxBitrateMbps.load() < b) {
                cfg.maxBitrateMbps = b;
            }
            this->markSettingsDirty(false);
            if (m_customBitrateInput) {
                m_customBitrateInput->setString(std::to_string(b));
            }
            this->refreshUI();
            this->updateStatusDisplay(0.0f);
            this->scheduleDiskSpaceCheck(true);
        }
    );

    // Custom Bitrate Manual Input Row
    auto customBitTitle = CCLabelBMFont::create("Custom Mbps:", "goldFont.fnt");
    customBitTitle->setScale(0.48f);
    customBitTitle->setAnchorPoint(ccp(0.0f, 0.5f));
    customBitTitle->setPosition(ccp(25.0f, winSize.height - 156.0f));
    page->addChild(customBitTitle);

    m_customBitrateInput = TextInput::create(75.0f, "Mbps", "chatFont.fnt");
    m_customBitrateInput->setCommonFilter(CommonFilter::Uint);
    m_customBitrateInput->setString(std::to_string(ReplayConfig::get().bitrateMbps.load()));
    m_customBitrateInput->setPosition(ccp(160.0f, winSize.height - 156.0f));
    m_customBitrateInput->setCallback([this](std::string const& str) {
        if (this->guardLocked()) {
            m_customBitrateInput->setString(std::to_string(ReplayConfig::get().bitrateMbps.load()));
            return;
        }
        if (!str.empty()) {
            try {
                int b = std::clamp(std::stoi(str), 5, 250);
                ReplayConfig::get().bitrateMbps = b;
                // Do NOT ratchet maxBitrateMbps up on intermediate keystrokes;
                // maxBitrateMbps >= bitrateMbps is enforced when starting replay or closing popup.
                this->markSettingsDirty(false);
                this->refreshUI();
                this->updateStatusDisplay(0.0f);
                this->scheduleDiskSpaceCheck(false);
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

    static constexpr PresetOption kMaxBitOpts[] = {
        {"35M", 35}, {"50M", 50}, {"60M", 60}, {"80M", 80}, {"120M", 120}
    };
    m_maxBitrateMenu = createPresetMenu(
        m_peakBitrateNode, ccp(centerX + 40.0f, winSize.height - 192.0f), kMaxBitOpts, 0.62f, 6.0f,
        [this](int mb) {
            if (this->guardLocked()) return;
            ReplayConfig::get().maxBitrateMbps = mb;
            this->markSettingsDirty(false);
            this->refreshUI();
            this->updateStatusDisplay(0.0f);
            this->scheduleDiskSpaceCheck(true);
        }
    );

    m_estSizeBitrateLabel = CCLabelBMFont::create("", "goldFont.fnt");
    m_estSizeBitrateLabel->setScale(0.50f);
    m_estSizeBitrateLabel->setColor(ccc3(255, 215, 80));
    m_estSizeBitrateLabel->setPosition(ccp(centerX, winSize.height - 226.0f));
    page->addChild(m_estSizeBitrateLabel);

    auto bitrateNotice = CCLabelBMFont::create("Adaptive VBR dynamically allocates bitrate: saves space in menus, peaks in fast gameplay.", "chatFont.fnt");
    bitrateNotice->setScale(0.38f);
    bitrateNotice->setColor(ccc3(220, 220, 160));
    bitrateNotice->setPosition(ccp(centerX, winSize.height - 252.0f));
    page->addChild(bitrateNotice);
}

void ReplayPopup::setupAudioStorageTab(CCNode* page, CCSize const& winSize, float centerX) {
    m_audioToggleMenu = CCMenu::create();
    m_audioToggleMenu->setPosition(CCPointZero);
    page->addChild(m_audioToggleMenu);

    float checkX = 42.0f;
    float row1Y = winSize.height - 82.0f;
    float row2Y = winSize.height - 110.0f;

    m_splitAudioToggle = addLabeledToggler(
        page, m_audioToggleMenu, ccp(checkX, row1Y), 0.68f,
        ReplayConfig::get().splitAudioTracks.load(),
        "Separate Audio Tracks (Game + Mic in MP4)", 0.50f, 24.0f, &m_splitAudioLabel,
        [this](auto* toggler) {
            bool curSplit = ReplayConfig::get().splitAudioTracks.load();
            if (this->guardLocked()) {
                toggler->toggle(!curSplit);
                return;
            }
            ReplayConfig::get().splitAudioTracks = !curSplit;
            this->markSettingsDirty(false);
            this->updateStatusDisplay(0.0f);
        }
    );

    m_micToggle = addLabeledToggler(
        page, m_audioToggleMenu, ccp(checkX, row2Y), 0.68f,
        ReplayConfig::get().recordMic.load(),
        "Record Microphone Audio (WASAPI)", 0.50f, 24.0f, &m_micRecordLabel,
        [this](auto* toggler) {
            bool curMic = ReplayConfig::get().recordMic.load();
            if (this->guardLocked()) {
                toggler->toggle(!curMic);
                return;
            }
            ReplayConfig::get().recordMic = !curMic;
            this->markSettingsDirty(false);
            this->updateStatusDisplay(0.0f);
        }
    );

    // Microphone Device Selector Row
    auto micDevTitle = CCLabelBMFont::create("Mic Device:", "goldFont.fnt");
    micDevTitle->setScale(0.48f);
    micDevTitle->setAnchorPoint(ccp(0.0f, 0.5f));
    micDevTitle->setPosition(ccp(20.0f, winSize.height - 138.0f));
    page->addChild(micDevTitle);

    m_availableMics = AudioGrabber::getAvailableMicrophones();
    m_currentMicIndex = 0;
    const auto curId = ReplayConfig::get().getMicDeviceId();
    for (size_t i = 0; i < m_availableMics.size(); ++i) {
        if (m_availableMics[i].id == curId) {
            m_currentMicIndex = static_cast<int>(i);
            break;
        }
    }

    m_micDevMenu = CCMenu::create();
    m_micDevMenu->setPosition(ccp(270.0f, winSize.height - 138.0f));
    page->addChild(m_micDevMenu);

    auto prevSpr = CCSprite::createWithSpriteFrameName("GJ_arrow_03_001.png");
    auto prevBtn = CCMenuItemExt::createSpriteExtra(prevSpr, [this](auto*) {
        this->cycleMicDevice(-1);
    });
    setBtnScale(prevBtn, 0.65f);
    m_micDevMenu->addChild(prevBtn);

    m_displayedMicName = m_availableMics.empty() ? "Default Device" : m_availableMics[m_currentMicIndex].name;
    auto devSpr = ButtonSprite::create(m_displayedMicName.c_str(), "goldFont.fnt", "GJ_button_01.png", 0.75f);
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

    m_micDevMenu->alignItemsHorizontallyWithPadding(6.0f);

    // Volume Sliders (0% - 200% range; lightweight in-memory update while dragging; disk flush on popup close)
    float volRowY = winSize.height - 168.0f;
    createVolumeSliderControl(
        page, this, menu_selector(ReplayPopup::onGameVolumeChanged),
        "Game Vol:", 20.0f, volRowY, ReplayConfig::get().systemVolume.load(),
        m_gameVolSlider, m_gameVolLabel
    );
    createVolumeSliderControl(
        page, this, menu_selector(ReplayPopup::onMicVolumeChanged),
        "Mic Vol:", 272.0f, volRowY, ReplayConfig::get().micVolume.load(),
        m_micVolSlider, m_micVolLabel
    );

    // Save Location Section
    auto storageTitle = CCLabelBMFont::create("Replays Folder Path:", "goldFont.fnt");
    storageTitle->setScale(0.50f);
    storageTitle->setAnchorPoint(ccp(0.0f, 0.5f));
    storageTitle->setPosition(ccp(20.0f, winSize.height - 198.0f));
    page->addChild(storageTitle);

    m_saveFolderInput = TextInput::create(280.0f, "Custom directory path", "chatFont.fnt");
    m_saveFolderInput->setCommonFilter(CommonFilter::Any);
    m_saveFolderInput->setString(ReplayConfig::get().getCustomSaveFolder());
    m_saveFolderInput->setPosition(ccp(centerX - 10.0f, winSize.height - 224.0f));
    m_saveFolderInput->setCallback([this](std::string const& str) {
        if (this->guardLocked()) {
            m_saveFolderInput->setString(ReplayConfig::get().getCustomSaveFolder());
            return;
        }
        ReplayConfig::get().setCustomSaveFolder(str);
        this->markSettingsDirty(false);
        this->updateFolderDisplay();
        this->updateStatusDisplay(0.0f);
        this->scheduleDiskSpaceCheck(false);
    });
    page->addChild(m_saveFolderInput);

    // Storage Action Buttons
    auto storageActionMenu = CCMenu::create();
    storageActionMenu->setPosition(ccp(centerX, winSize.height - 258.0f));
    page->addChild(storageActionMenu);

    auto bSpr = ButtonSprite::create("Browse...", "goldFont.fnt", "GJ_button_01.png", 0.75f);
    m_storageBrowseBtn = CCMenuItemExt::createSpriteExtra(bSpr, [this](auto*) {
        this->onBrowseFolder();
    });
    setBtnScale(m_storageBrowseBtn, 0.65f);
    storageActionMenu->addChild(m_storageBrowseBtn);

    auto oSpr = ButtonSprite::create("Open Folder", "goldFont.fnt", "GJ_button_05.png", 0.75f);
    auto oBtn = CCMenuItemExt::createSpriteExtra(oSpr, [](auto*) {
        ReplayManager::get()->openRecordingsFolder();
    });
    setBtnScale(oBtn, 0.65f);
    storageActionMenu->addChild(oBtn);

    auto rSpr = ButtonSprite::create("Reset Default", "goldFont.fnt", "GJ_button_06.png", 0.75f);
    m_storageResetBtn = CCMenuItemExt::createSpriteExtra(rSpr, [this](auto*) {
        this->onResetFolder();
    });
    setBtnScale(m_storageResetBtn, 0.65f);
    storageActionMenu->addChild(m_storageResetBtn);

    storageActionMenu->alignItemsHorizontallyWithPadding(8.0f);
}

void ReplayPopup::switchTab(int tabIndex) {
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
    std::string dir = geode::utils::string::pathToString(ReplayConfig::get().getRecordingsDirectory(false));
    if (m_folderPathLabel) {
        std::string displayStr = utf8TailWithEllipsis(dir, 32);
        m_folderPathLabel->setString(displayStr.c_str());
    }
    if (m_saveFolderInput && !isInputFocused(m_saveFolderInput)) {
        m_saveFolderInput->setString(ReplayConfig::get().getCustomSaveFolder());
    }
}

void ReplayPopup::onBrowseFolder() {
    if (guardLocked()) return;
    auto defaultDir = ReplayConfig::get().getExistingRecordingsPickerPath();

    file::FilePickOptions options;
    options.defaultPath = defaultDir;

    geode::Ref<ReplayPopup> self = this;
    geode::async::spawn(
        file::pick(file::PickMode::OpenFolder, options),
        [self](Result<std::optional<std::filesystem::path>> result) {
            if (!result.isOk() || !result.unwrap().has_value()) return;
            if (self->guardLocked()) return;

            auto path = std::move(result).unwrap().value();
            ReplayConfig::get().setCustomSaveFolder(geode::utils::string::pathToString(path));
            self->markSettingsDirty(true);
            self->updateFolderDisplay();
            self->updateStatusDisplay(0.0f);
            self->scheduleDiskSpaceCheck(true);
            Notification::create("Save folder updated", NotificationIcon::Success, 1.0f)->show();
        }
    );
}

void ReplayPopup::onResetFolder() {
    if (guardLocked()) return;
    ReplayConfig::get().setCustomSaveFolder("");
    markSettingsDirty(true);
    updateFolderDisplay();
    updateStatusDisplay(0.0f);
    scheduleDiskSpaceCheck(true);
}

void ReplayPopup::handleVolumeSlider(Slider* slider, CCLabelBMFont* label, AtomicVal<float>& targetVol) {
    if (!slider) return;
    if (guardLocked()) {
        slider->setValue(std::clamp(targetVol.load() / kMaxVolume, 0.0f, 1.0f));
        return;
    }
    float vol = std::clamp(slider->getValue(), 0.0f, 1.0f) * kMaxVolume;
    targetVol.store(vol);
    m_settingsDirty = true;
    ReplayConfig::get().saveVolumeSettings();
    if (label) {
        int pct = static_cast<int>(vol * 100.0f + 0.5f);
        label->setString((std::to_string(pct) + "%").c_str());
    }
}

void ReplayPopup::onGameVolumeChanged(CCObject*) {
    handleVolumeSlider(m_gameVolSlider, m_gameVolLabel, ReplayConfig::get().systemVolume);
}

void ReplayPopup::onMicVolumeChanged(CCObject*) {
    handleVolumeSlider(m_micVolSlider, m_micVolLabel, ReplayConfig::get().micVolume);
}

void ReplayPopup::updateStatusDisplay(float dt) {
    ReplayConfig::refreshNativeFrameHeight();

    auto mgr = ReplayManager::get();
    bool isRec = mgr->isReplayActive();
    applyLockState(isRec);

    if (dt > 0.0f) {
        m_diskCheckTimer += dt;
        if (m_diskCheckPending) {
            m_diskCheckDebounce -= dt;
            if (m_diskCheckDebounce <= 0.0f) {
                m_diskCheckPending = false;
                m_diskCheckTimer = 0.0f;
                triggerAsyncDiskCheck();
            }
        } else if (m_diskCheckTimer >= 5.0f) {
            m_diskCheckTimer = 0.0f;
            triggerAsyncDiskCheck();
        }
    }

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

    refreshDiskWarnLabel();

    const auto& cfg = ReplayConfig::get();
    if (m_codecBadgeLabel) {
        std::string cName = cfg.getCodecDisplayName();
        const char* rName = ReplayConfig::resolutionDisplayName(cfg.resolution.load());

        RateControlMode rc = cfg.rateControl.load();
        char buf[160];
        if (rc == RateControlMode::CONSTANT_QUALITY) {
            snprintf(buf, sizeof(buf), "%s | %s | CQ %d%% (~%d Mbps)",
                     cName.c_str(), rName, cfg.qualityLevel.load(), cfg.getEffectiveBitrateMbps());
        } else {
            const char* rcName = (rc == RateControlMode::CONSTANT_CBR) ? "CBR" : "Adaptive VBR";
            snprintf(buf, sizeof(buf), "%s | %s | %d Mbps (%s)",
                     cName.c_str(), rName, cfg.getEffectiveBitrateMbps(), rcName);
        }
        m_codecBadgeLabel->setString(buf);
    }

    if (m_estSizeLabel) {
        m_estSizeLabel->setString(cfg.getEstimatedFileSizeText().c_str());
    }
    std::string detailedSizeText = cfg.getEstimatedFileSizeDetailedText();
    if (m_estSizeBitrateLabel) {
        m_estSizeBitrateLabel->setString(detailedSizeText.c_str());
    }
    if (m_estSizeResLabel) {
        m_estSizeResLabel->setString(detailedSizeText.c_str());
    }
}

void ReplayPopup::refreshUI(bool syncTogglers) {
    const auto& cfg = ReplayConfig::get();
    bool locked = ReplayManager::get()->isReplayActive();

    // Duration menu highlight
    int curDur = cfg.durationSeconds.load();
    updateMenuHighlight(m_durationMenu, curDur, true, -1, locked);
    if (m_durationInput && !isInputFocused(m_durationInput)) {
        m_durationInput->setString(std::to_string(curDur));
    }

    // Codec menu highlight
    CodecType curCodec = cfg.codec.load();
    updateMenuHighlight(m_codecMenu, static_cast<int>(curCodec), curCodec != CodecType::CUSTOM, -1, locked);
    if (m_customCodecInput && !isInputFocused(m_customCodecInput)) {
        m_customCodecInput->setString(cfg.getCustomCodec());
    }

    bool hwAccel = cfg.hardwareAcceleration.load();
    if (syncTogglers && m_hwAccelToggle) {
        m_hwAccelToggle->toggle(hwAccel);
    }
    if (m_hwAccelLabel) {
        if (hwAccel) {
            std::string text = "Hardware Acceleration (Auto GPU: " + ReplayConfig::getGpuAccelName() + ")";
            m_hwAccelLabel->setString(text.c_str());
            m_hwAccelLabel->setColor(ccc3(255, 255, 255));
        } else {
            m_hwAccelLabel->setString("Hardware Acceleration (Disabled: CPU Software)");
            m_hwAccelLabel->setColor(ccc3(180, 180, 180));
        }
        m_hwAccelLabel->setOpacity(locked ? 150 : 255);
    }

    bool isH264 = (curCodec == CodecType::H264);
    ResolutionType curRes = cfg.resolution.load();
    int curFps = cfg.targetFps.load();

    // Resolution & FPS menu highlights + H.264 4K @ 240 FPS limit
    int disabledResTag = (isH264 && curFps >= 240) ? static_cast<int>(ResolutionType::UHD_4K) : -1;
    updateMenuHighlight(m_resMenu, static_cast<int>(curRes), true, disabledResTag, locked);

    int disabledFpsTag = (isH264 && curRes == ResolutionType::UHD_4K) ? 240 : -1;
    updateMenuHighlight(m_fpsMenu, curFps, true, disabledFpsTag, locked);

    if (m_codecConflictLabel) {
        bool conflictPossible = isH264 && (curRes == ResolutionType::UHD_4K || curFps >= 240);
        m_codecConflictLabel->setVisible(conflictPossible);
    }

    // Rate Control highlight
    RateControlMode curRc = cfg.rateControl.load();
    bool isVBR = (curRc == RateControlMode::ADAPTIVE_VBR);
    updateMenuHighlight(m_rateControlMenu, static_cast<int>(curRc), true, -1, locked);

    // Target Bitrate highlight
    int curBitrate = cfg.bitrateMbps.load();
    updateMenuHighlight(m_bitrateMenu, curBitrate, true, -1, locked);

    // Never clobber m_customBitrateInput while the user is actively typing in it!
    if (m_customBitrateInput && !isInputFocused(m_customBitrateInput)) {
        m_customBitrateInput->setString(std::to_string(curBitrate));
    }

    // Peak Bitrate node visibility (only in Adaptive VBR mode)
    if (m_peakBitrateNode) {
        m_peakBitrateNode->setVisible(isVBR);
    }

    // Peak Bitrate highlight
    updateMenuHighlight(m_maxBitrateMenu, cfg.maxBitrateMbps.load(), true, -1, locked);

    // Volume sliders and percentage update
    syncVolumeControls(m_gameVolSlider, m_gameVolLabel, cfg.systemVolume.load());
    syncVolumeControls(m_micVolSlider, m_micVolLabel, cfg.micVolume.load());

    if (syncTogglers) {
        if (m_splitAudioToggle) m_splitAudioToggle->toggle(cfg.splitAudioTracks.load());
        if (m_micToggle) m_micToggle->toggle(cfg.recordMic.load());
        setTogglerLocked(m_hwAccelToggle, locked);
        setTogglerLocked(m_splitAudioToggle, locked);
        setTogglerLocked(m_micToggle, locked);
    }
    refreshMicDeviceButton();
}

void ReplayPopup::cycleMicDevice(int direction) {
    if (guardLocked()) return;
    if (m_availableMics.empty()) return;

    int n = static_cast<int>(m_availableMics.size());
    m_currentMicIndex = (m_currentMicIndex + direction + n) % n;

    auto& cfg = ReplayConfig::get();
    cfg.setMicDevice(m_availableMics[m_currentMicIndex].id, m_availableMics[m_currentMicIndex].name);
    markSettingsDirty(false);

    refreshMicDeviceButton();

    std::string msg = "Mic: " + m_availableMics[m_currentMicIndex].name;
    Notification::create(msg, NotificationIcon::Info, 1.0f)->show();
}

void ReplayPopup::refreshMicDeviceButton() {
    if (!m_micDeviceBtn || m_availableMics.empty()) return;
    if (m_currentMicIndex < 0 || m_currentMicIndex >= static_cast<int>(m_availableMics.size())) {
        m_currentMicIndex = 0;
    }
    const std::string& name = m_availableMics[m_currentMicIndex].name;
    if (name == m_displayedMicName) {
        setMenuItemLocked(m_micDeviceBtn, ReplayManager::get()->isReplayActive());
        return;
    }
    m_displayedMicName = name;
    auto spr = ButtonSprite::create(name.c_str(), "goldFont.fnt", "GJ_button_01.png", 0.75f);
    m_micDeviceBtn->setNormalImage(spr);
    m_micDeviceBtn->setContentSize(spr->getContentSize());
    setBtnScale(m_micDeviceBtn, 0.62f);
    setMenuItemLocked(m_micDeviceBtn, ReplayManager::get()->isReplayActive());
    if (m_micDevMenu) {
        m_micDevMenu->alignItemsHorizontallyWithPadding(6.0f);
    }
}

} // namespace replay

#endif // GEODE_IS_WINDOWS
