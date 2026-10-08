#pragma once

#include <Geode/Geode.hpp>
#include <Geode/ui/Popup.hpp>
#include <Geode/ui/TextInput.hpp>
#include "../config/ReplayConfig.hpp"
#include "../recorder/ReplayManager.hpp"
#include "../capture/AudioGrabber.hpp"

namespace replay {

class ReplayPopup : public geode::Popup {
public:
    static ReplayPopup* create();
    static void toggle();
    static ReplayPopup* get() { return s_instance; }

    ~ReplayPopup() override;

protected:
    bool init(float width, float height);
    void setup();
    void onClose(cocos2d::CCObject* sender) override;

private:
    inline static ReplayPopup* s_instance = nullptr;

    void switchTab(int tabIndex);
    void updateStatusDisplay(float dt = 0.0f);
    void refreshUI();
    void updateFolderDisplay();
    void onBrowseFolder();
    void onResetFolder();
    void cycleMicDevice(int direction);
    void refreshMicDeviceButton();
    void onGameVolumeChanged(cocos2d::CCObject* sender);
    void onMicVolumeChanged(cocos2d::CCObject* sender);
    void restartReplayScheduled(float dt);

    int m_currentTab = 0;
    cocos2d::CCNode* m_pages[4] = {nullptr, nullptr, nullptr, nullptr};
    CCMenuItemSpriteExtra* m_tabButtons[4] = {nullptr, nullptr, nullptr, nullptr};

    // Live labels
    cocos2d::CCLabelBMFont* m_statusLabel = nullptr;
    cocos2d::CCSprite* m_statusDot = nullptr;
    cocos2d::CCLabelBMFont* m_codecBadgeLabel = nullptr;
    cocos2d::CCLabelBMFont* m_folderPathLabel = nullptr;
    cocos2d::CCLabelBMFont* m_estSizeLabel = nullptr;
    cocos2d::CCLabelBMFont* m_estSizeBitrateLabel = nullptr;
    cocos2d::CCLabelBMFont* m_estSizeResLabel = nullptr;
    cocos2d::CCLabelBMFont* m_diskSpaceWarnLabel = nullptr;
    cocos2d::CCLabelBMFont* m_codecConflictLabel = nullptr;

    // Text inputs for manual customization
    geode::TextInput* m_durationInput = nullptr;
    geode::TextInput* m_customCodecInput = nullptr;
    geode::TextInput* m_saveFolderInput = nullptr;
    geode::TextInput* m_customBitrateInput = nullptr;

    // Menus to update active state
    cocos2d::CCMenu* m_durationMenu = nullptr;
    cocos2d::CCMenu* m_codecMenu = nullptr;
    cocos2d::CCMenu* m_resMenu = nullptr;
    cocos2d::CCMenu* m_fpsMenu = nullptr;
    cocos2d::CCMenu* m_rateControlMenu = nullptr;
    cocos2d::CCMenu* m_bitrateMenu = nullptr;
    cocos2d::CCMenu* m_maxBitrateMenu = nullptr;
    cocos2d::CCMenu* m_micDevMenu = nullptr;

    // Sliders for Audio Volume
    Slider* m_gameVolSlider = nullptr;
    Slider* m_micVolSlider = nullptr;
    cocos2d::CCLabelBMFont* m_gameVolLabel = nullptr;
    cocos2d::CCLabelBMFont* m_micVolLabel = nullptr;

    // Nodes for dynamic visibility
    cocos2d::CCNode* m_peakBitrateNode = nullptr;

    CCMenuItemToggler* m_splitAudioToggle = nullptr;
    CCMenuItemToggler* m_micToggle = nullptr;
    CCMenuItemToggler* m_hwAccelToggle = nullptr;
    cocos2d::CCLabelBMFont* m_hwAccelLabel = nullptr;

    // Warning banner and page lock notices
    cocos2d::CCLabelBMFont* m_lockWarningBanner = nullptr;
    cocos2d::CCLabelBMFont* m_pageNoticeLabels[4] = {nullptr, nullptr, nullptr, nullptr};

    // Action buttons that need enable/disable state
    CCMenuItemSpriteExtra* m_toggleBtn = nullptr;
    CCMenuItemSpriteExtra* m_quickBrowseBtn = nullptr;
    CCMenuItemSpriteExtra* m_browseFolderBtn = nullptr;
    CCMenuItemSpriteExtra* m_resetFolderBtn = nullptr;
    CCMenuItemSpriteExtra* m_micPrevBtn = nullptr;
    CCMenuItemSpriteExtra* m_micNextBtn = nullptr;

    // Microphone device selection
    CCMenuItemSpriteExtra* m_micDeviceBtn = nullptr;
    std::vector<AudioDeviceInfo> m_availableMics;
    int m_currentMicIndex = 0;
};

} // namespace replay
