#pragma once

#include <Geode/Geode.hpp>

#ifdef GEODE_IS_WINDOWS

#include <Geode/ui/Popup.hpp>
#include <Geode/ui/TextInput.hpp>
#include <Geode/utils/async.hpp>
#include "../config/ReplayConfig.hpp"
#include "../recorder/ReplayManager.hpp"
#include "../capture/AudioGrabber.hpp"
#include <memory>
#include <vector>

namespace replay {

class ReplayPopup : public geode::Popup {
public:
    static ReplayPopup* create();
    static void toggle();
    static ReplayPopup* get() noexcept { return s_instance; }

    ~ReplayPopup() override;

    void updateStatusDisplay(float dt = 0.0f);
    void refreshUI(bool syncTogglers = true);

protected:
    bool init(float width, float height);
    void setup();
    void onClose(cocos2d::CCObject* sender) override;

private:
    struct DiskSpaceResult {
        bool low = false;
        bool isTempLow = false;
        uint64_t freeMB = 0;
        uint64_t neededMB = 0;
    };

    inline static ReplayPopup* s_instance = nullptr;

    void setupDashboardTab(cocos2d::CCNode* page, cocos2d::CCSize const& winSize, float centerX);
    void setupCodecTab(cocos2d::CCNode* page, cocos2d::CCSize const& winSize, float centerX);
    void setupBitrateTab(cocos2d::CCNode* page, cocos2d::CCSize const& winSize, float centerX);
    void setupAudioStorageTab(cocos2d::CCNode* page, cocos2d::CCSize const& winSize, float centerX);

    bool guardLocked() const;
    void applyLockState(bool locked);
    void markSettingsDirty(bool flushToDisk = false);
    void finalizePendingBitrateClamp();

    void scheduleDiskSpaceCheck(bool immediate);
    void triggerAsyncDiskCheck();
    void applyDiskSpaceResult(bool low, uint64_t freeMB, uint64_t neededMB, bool isTempLow);
    void refreshDiskWarnLabel();

    void switchTab(int tabIndex);
    void updateFolderDisplay();
    void onBrowseFolder();
    void onResetFolder();
    void cycleMicDevice(int direction);
    void refreshMicDeviceButton();
    void handleVolumeSlider(Slider* slider, cocos2d::CCLabelBMFont* label, AtomicVal<float>& targetVol);
    void onGameVolumeChanged(cocos2d::CCObject* sender);
    void onMicVolumeChanged(cocos2d::CCObject* sender);

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

    // Menus to update active/locked state
    cocos2d::CCMenu* m_durationMenu = nullptr;
    cocos2d::CCMenu* m_codecMenu = nullptr;
    cocos2d::CCMenu* m_resMenu = nullptr;
    cocos2d::CCMenu* m_fpsMenu = nullptr;
    cocos2d::CCMenu* m_rateControlMenu = nullptr;
    cocos2d::CCMenu* m_bitrateMenu = nullptr;
    cocos2d::CCMenu* m_maxBitrateMenu = nullptr;
    cocos2d::CCMenu* m_micDevMenu = nullptr;
    cocos2d::CCMenu* m_hwMenu = nullptr;
    cocos2d::CCMenu* m_audioToggleMenu = nullptr;

    // Individual buttons that lock during recording
    CCMenuItemSpriteExtra* m_quickBrowseBtn = nullptr;
    CCMenuItemSpriteExtra* m_storageBrowseBtn = nullptr;
    CCMenuItemSpriteExtra* m_storageResetBtn = nullptr;

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
    cocos2d::CCLabelBMFont* m_splitAudioLabel = nullptr;
    cocos2d::CCLabelBMFont* m_micRecordLabel = nullptr;

    // Warning banner & action button
    cocos2d::CCLabelBMFont* m_lockWarningBanner = nullptr;
    CCMenuItemSpriteExtra* m_toggleBtn = nullptr;

    // Microphone device selection
    CCMenuItemSpriteExtra* m_micDeviceBtn = nullptr;
    std::string m_displayedMicName;
    std::vector<AudioDeviceInfo> m_availableMics;
    int m_currentMicIndex = 0;

    // Settings dirty tracking & lock state (#17, #24, #31)
    bool m_settingsDirty = false;
    bool m_hasAppliedLockState = false;
    bool m_isLocked = false;

    // Asynchronous disk space monitor (#16)
    geode::async::TaskHolder<DiskSpaceResult> m_diskCheckTask;
    std::shared_ptr<std::atomic<bool>> m_diskCheckInFlight = std::make_shared<std::atomic<bool>>(false);
    float m_diskCheckTimer = 0.0f;
    float m_diskCheckDebounce = 0.0f;
    bool m_diskCheckPending = false;
    bool m_cachedDiskLow = false;
    bool m_cachedIsTempLow = false;
    uint64_t m_cachedFreeMB = 0;
    uint64_t m_cachedNeededMB = 0;
};

} // namespace replay

#endif // GEODE_IS_WINDOWS
