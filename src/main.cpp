#include <Geode/Geode.hpp>
#include <Geode/modify/CCEGLView.hpp>
#include <Geode/modify/CCKeyboardDispatcher.hpp>
#include <Geode/modify/FMODAudioEngine.hpp>
#include <Geode/modify/MenuLayer.hpp>
#include <Geode/modify/PauseLayer.hpp>
#include <Geode/loader/SettingV3.hpp>

#include "config/ReplayConfig.hpp"
#include "recorder/ReplayManager.hpp"
#include "capture/FrameGrabber.hpp"
#include "ui/ReplayPopup.hpp"
#include "ui/ReplayNotification.hpp"

using namespace geode::prelude;
using namespace replay;

static bool isKeybindMatch(cocos2d::enumKeyCodes key, KeyboardModifier mods, std::string const& settingKey, Keybind const& defaultBind) {
    constexpr uint8_t kModMask = KeyboardModifier::Shift | KeyboardModifier::Control | KeyboardModifier::Alt | KeyboardModifier::Super;
    uint8_t curMods = mods.value & kModMask;

    if (auto mod = geode::Mod::get()) {
        if (auto setting = mod->getSetting(settingKey)) {
            if (auto kbSetting = geode::cast::typeinfo_pointer_cast<KeybindSettingV3>(setting)) {
                auto const& list = kbSetting->getValue();
                for (auto const& kb : list) {
                    uint8_t kbMods = kb.modifiers.value & kModMask;
                    if (kb.key == key && kbMods == curMods) {
                        return true;
                    }
                }
                return false;
            }
        }
    }

    // Fallback to default bind only if the setting does not exist
    uint8_t defMods = defaultBind.modifiers.value & kModMask;
    return defaultBind.key == key && defMods == curMods;
}

static void triggerSaveReplay() {
    static auto s_last = std::chrono::steady_clock::now() - std::chrono::seconds(5);
    auto now = std::chrono::steady_clock::now();
    if (std::chrono::duration<double>(now - s_last).count() > 0.3) {
        s_last = now;
        geode::log::info("Instant Replay: Keybind triggered Save Replay");
        ReplayManager::get()->saveReplay();
    }
}

static void triggerToggleReplay() {
    static auto s_last = std::chrono::steady_clock::now() - std::chrono::seconds(5);
    auto now = std::chrono::steady_clock::now();
    if (std::chrono::duration<double>(now - s_last).count() > 0.3) {
        s_last = now;
        geode::log::info("Instant Replay: Keybind triggered Toggle Replay");
        ReplayManager::get()->toggleReplay();
        if (ReplayConfig::get().showNotification) {
            bool active = ReplayManager::get()->isReplayActive();
            geode::Notification::create(
                active ? "Instant Replay: Replay Enabled" : "Instant Replay: Replay Disabled",
                active ? geode::NotificationIcon::Success : geode::NotificationIcon::Warning,
                1.5f
            )->show();
        }
    }
}

static void triggerOpenMenu() {
    static auto s_last = std::chrono::steady_clock::now() - std::chrono::seconds(5);
    auto now = std::chrono::steady_clock::now();
    if (std::chrono::duration<double>(now - s_last).count() > 0.25) {
        s_last = now;
        geode::log::info("Instant Replay: Keybind triggered Toggle Menu");
        geode::Loader::get()->queueInMainThread([]() {
            ReplayPopup::toggle();
        });
    }
}

static bool handleKeyPress(cocos2d::enumKeyCodes key, KeyboardModifier mods) {
    if (key == KEY_None || key == KEY_Unknown) return false;

    // Check if text input has focus
    bool isImeActive = CCIMEDispatcher::sharedDispatcher()->hasDelegate();
    if (isImeActive) {
        // When typing text, only allow F-keys or shortcuts with Ctrl/Alt
        bool hasCtrlOrAlt = (mods.value & (KeyboardModifier::Control | KeyboardModifier::Alt)) != 0;
        bool isFKey = (key >= KEY_F1 && key <= KEY_F24);
        if (!hasCtrlOrAlt && !isFKey) {
            return false;
        }
    }

    // 1. Save Replay (Default: F6)
    if (isKeybindMatch(key, mods, "save-replay", Keybind(KEY_F6, KeyboardModifier::None))) {
        triggerSaveReplay();
        return true;
    }

    // 2. Toggle Replay (Default: Ctrl+F6)
    if (isKeybindMatch(key, mods, "toggle-replay", Keybind(KEY_F6, KeyboardModifier::Control))) {
        triggerToggleReplay();
        return true;
    }

    // 3. Open Menu (Default: F9)
    if (isKeybindMatch(key, mods, "open-menu", Keybind(KEY_F9, KeyboardModifier::None))) {
        triggerOpenMenu();
        return true;
    }

    return false;
}

// Hook CCKeyboardDispatcher for reliable in-game key handling across all screens and gameplay
class $modify(ReplayKeyboardDispatcher, CCKeyboardDispatcher) {
    bool dispatchKeyboardMSG(enumKeyCodes key, bool isKeyDown, bool isKeyRepeat, double timestamp) {
        if (isKeyDown && !isKeyRepeat) {
            uint8_t mods = 0;
            if (this->getShiftKeyPressed()) mods |= KeyboardModifier::Shift;
            if (this->getControlKeyPressed()) mods |= KeyboardModifier::Control;
            if (this->getAltKeyPressed()) mods |= KeyboardModifier::Alt;
            if (this->getCommandKeyPressed()) mods |= KeyboardModifier::Super;

            if (handleKeyPress(key, KeyboardModifier(mods))) {
                return true;
            }
        }
        return CCKeyboardDispatcher::dispatchKeyboardMSG(key, isKeyDown, isKeyRepeat, timestamp);
    }
};

$on_mod(Loaded) {
    // 1. Initialize Media Foundation, audio capture, and replay engine
    ReplayManager::get()->initialize();

    // 2. Global KeyboardInputEvent listener (backup/complementary to CCKeyboardDispatcher hook)
    KeyboardInputEvent().listen([](KeyboardInputData& data) {
        if (data.action == KeyboardInputData::Action::Press) {
            handleKeyPress(data.key, data.modifiers);
        }
        return ListenerResult::Propagate;
    }).leak();

    // 3. Register keybind listeners (compatible with Custom Keybinds mod)
    listenForKeybindSettingPresses("save-replay", [](Keybind const&, bool down, bool repeat, double) {
        if (down && !repeat) triggerSaveReplay();
    });

    listenForKeybindSettingPresses("toggle-replay", [](Keybind const&, bool down, bool repeat, double) {
        if (down && !repeat) triggerToggleReplay();
    });
    listenForKeybindSettingPresses("toggle-buffer", [](Keybind const&, bool down, bool repeat, double) {
        if (down && !repeat) triggerToggleReplay();
    });

    listenForKeybindSettingPresses("open-menu", [](Keybind const&, bool down, bool repeat, double) {
        if (down && !repeat) triggerOpenMenu();
    });

    // 4. Handle replay parameter changes via Geode Mod Settings (only allowed when recording is OFF)
    listenForAllSettingChanges([](std::string_view key, std::shared_ptr<SettingV3>) {
        if (ReplayConfig::s_isSavingSettings) {
            return;
        }
        if (key == "save-replay" || key == "toggle-replay" || key == "toggle-buffer" || key == "open-menu") {
            return;
        }
        if (key == "show-notification") {
            if (auto mod = geode::Mod::get()) {
                ReplayConfig::get().showNotification = mod->getSettingValue<bool>("show-notification");
            }
            return;
        }
        if (key == "enabled") {
            if (auto mod = geode::Mod::get()) {
                bool newEnabled = mod->getSettingValue<bool>("enabled");
                ReplayManager::get()->setReplayEnabled(newEnabled);
            }
            return;
        }

        if (ReplayManager::get()->isReplayActive()) {
            geode::Notification::create(
                "Recording settings can only be changed when recording is OFF!",
                geode::NotificationIcon::Warning,
                1.5f
            )->show();
            ReplayConfig::get().saveToSettings();
            return;
        }

        ReplayConfig::get().loadFromSettings();
        if (key == "record-mic" || key == "mic-device-id") {
            if (ReplayConfig::get().recordMic) {
                AudioGrabber::get()->restartMicCapture();
            } else {
                AudioGrabber::get()->stopMicCapture();
            }
        }
    });

    // 4. Scan existing hooks and enforce priority 100000 on swapBuffers
    for (auto hook : geode::Mod::get()->getHooks()) {
        if (hook->getDisplayName().find("swapBuffers") != std::string::npos) {
            hook->setPriority(100000);
            geode::log::info("Set priority of hook '{}' in $on_mod(Loaded) to {}", hook->getDisplayName(), hook->getPriority());
        }
    }
}

// Capture the finished frame right BEFORE it is presented, after all overlay/ImGui mods (like Mega Hack) have finished drawing
class $modify(ReplayEGLView, CCEGLView) {
    static void onModify(auto& self) {
        // Direct iteration over all hooks in this modify class to guarantee priority = 100000
        for (auto& [name, hook] : self.m_hooks) {
            hook->setPriority(100000);
            geode::log::info("ReplayEGLView onModify: Hook '{}' set to priority {}", name, hook->getPriority());
        }
        (void)self.setHookPriority("cocos2d::CCEGLView::swapBuffers", 100000);
        (void)self.setHookPriority("CCEGLView::swapBuffers", 100000);
        (void)self.setHookPriorityAfter("cocos2d::CCEGLView::swapBuffers", "absolllute.megahack");
        (void)self.setHookPriorityAfter("CCEGLView::swapBuffers", "absolllute.megahack");
    }

    void swapBuffers() {
        static bool s_logged = false;
        if (!s_logged) {
            s_logged = true;
            geode::log::info("ReplayEGLView::swapBuffers active! Capturing frames.");
        }
        FrameGrabber::get()->captureCurrentFrame();
        CCEGLView::swapBuffers();
    }
};

// Ensure FMOD DSP audio capture is attached to the audio engine
class $modify(ReplayFMOD, FMODAudioEngine) {
    void update(float dt) {
        FMODAudioEngine::update(dt);
        AudioGrabber::get()->ensureDSPAttached();
    }
};

// Add Instant Replay button to Main Menu
class $modify(ReplayMenuLayer, MenuLayer) {
    bool init() {
        if (!MenuLayer::init()) return false;

        auto bottomMenu = this->getChildByID("bottom-menu");
        if (bottomMenu) {
            auto spr = CCSprite::createWithSpriteFrameName("GJ_completesIcon_001.png");
            if (spr) {
                spr->setScale(0.85f);
                spr->setColor(ccc3(100, 220, 255));
                auto btn = CCMenuItemSpriteExtra::create(spr, this, menu_selector(ReplayMenuLayer::onOpenReplay));
                btn->setID("instant-replay-button"_spr);
                bottomMenu->addChild(btn);
                bottomMenu->updateLayout();
            }
        }

        return true;
    }

    void onOpenReplay(CCObject*) {
        ReplayPopup::toggle();
    }
};

// Add Instant Replay button to Pause Menu
class $modify(ReplayPauseLayer, PauseLayer) {
    void customSetup() {
        PauseLayer::customSetup();

        auto rightMenu = this->getChildByID("right-button-menu");
        if (rightMenu) {
            auto spr = CCSprite::createWithSpriteFrameName("GJ_completesIcon_001.png");
            if (spr) {
                spr->setScale(0.85f);
                spr->setColor(ccc3(100, 220, 255));
                auto btn = CCMenuItemSpriteExtra::create(spr, this, menu_selector(ReplayPauseLayer::onOpenReplay));
                btn->setID("instant-replay-button"_spr);
                rightMenu->addChild(btn);
                rightMenu->updateLayout();
            }
        }
    }

    void onOpenReplay(CCObject*) {
        ReplayPopup::toggle();
    }
};
