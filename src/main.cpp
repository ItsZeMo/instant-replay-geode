#include <Geode/Geode.hpp>

#ifdef GEODE_IS_WINDOWS

#include <Geode/modify/CCEGLView.hpp>
#include <Geode/modify/FMODAudioEngine.hpp>
#include <Geode/modify/MenuLayer.hpp>
#include <Geode/modify/PauseLayer.hpp>
#include <Geode/loader/SettingV3.hpp>

#include "config/ReplayConfig.hpp"
#include "recorder/ReplayManager.hpp"
#include "capture/FrameGrabber.hpp"
#include "capture/AudioGrabber.hpp"
#include "ui/ReplayPopup.hpp"
#include "ui/ReplayNotification.hpp"

using namespace geode::prelude;
using namespace replay;

static bool shouldIgnoreKeybindWhileTyping(Keybind const& kb) {
    if (!CCIMEDispatcher::sharedDispatcher()->hasDelegate()) {
        return false;
    }
    bool hasCtrlOrAlt = (kb.modifiers.value & (KeyboardModifier::Control | KeyboardModifier::Alt)) != 0;
    bool isFKey = (kb.key >= KEY_F1 && kb.key <= KEY_F24);
    return !hasCtrlOrAlt && !isFKey;
}

static void triggerSaveReplay() {
    geode::log::info("Instant Replay: Keybind triggered Save Replay");
    ReplayManager::get()->saveReplay();
}

static void triggerToggleReplay() {
    geode::log::info("Instant Replay: Keybind triggered Toggle Replay");
    ReplayManager::get()->toggleReplay();
    if (ReplayConfig::get().showNotification.load()) {
        bool active = ReplayManager::get()->isReplayActive();
        geode::Notification::create(
            active ? "Instant Replay: Replay Enabled" : "Instant Replay: Replay Disabled",
            active ? geode::NotificationIcon::Success : geode::NotificationIcon::Warning,
            1.5f
        )->show();
    }
}

static void triggerOpenMenu() {
    geode::log::info("Instant Replay: Keybind triggered Toggle Menu");
    geode::Loader::get()->queueInMainThread([]() {
        ReplayPopup::toggle();
    });
}

$on_mod(Loaded) {
    // 1. Construct dependency singletons before ReplayManager so C++ reverse static
    //    destruction order keeps them alive until after ReplayManager is destroyed.
    (void)ReplayConfig::get();
    (void)FrameGrabber::get();
    (void)AudioGrabber::get();
    ReplayManager::get()->initialize();

    // 2. Register keybind listeners (Geode 5.10+ native keybind setting system)
    listenForKeybindSettingPresses("save-replay", [](Keybind const& kb, bool down, bool repeat, double) {
        if (down && !repeat && !shouldIgnoreKeybindWhileTyping(kb)) {
            triggerSaveReplay();
        }
    });

    listenForKeybindSettingPresses("toggle-replay", [](Keybind const& kb, bool down, bool repeat, double) {
        if (down && !repeat && !shouldIgnoreKeybindWhileTyping(kb)) {
            triggerToggleReplay();
        }
    });

    listenForKeybindSettingPresses("open-menu", [](Keybind const& kb, bool down, bool repeat, double) {
        if (down && !repeat && !shouldIgnoreKeybindWhileTyping(kb)) {
            triggerOpenMenu();
        }
    });

    // 3. Handle replay parameter changes via Geode Mod Settings.
    //    Geode fires setting listeners one-by-one in mod.json order ("enabled" is first),
    //    so we coalesce a synchronous Apply batch onto the next main-thread tick.
    listenForAllSettingChanges([](std::string_view key, std::shared_ptr<SettingV3>) {
        if (ReplayConfig::s_isSavingSettings.load()) {
            return;
        }
        if (key == "save-replay" || key == "toggle-replay" || key == "open-menu") {
            return;
        }
        if (key == "show-notification") {
            if (auto mod = geode::Mod::get()) {
                ReplayConfig::get().showNotification = mod->getSettingValue<bool>("show-notification");
            }
            return;
        }

        static bool s_batchPending = false;
        static bool s_wasActiveBeforeBatch = false;
        static bool s_enabledChanged = false;
        static bool s_recordingParamChanged = false;

        if (!s_batchPending) {
            s_batchPending = true;
            s_wasActiveBeforeBatch = ReplayManager::get()->isReplayActive();
            s_enabledChanged = false;
            s_recordingParamChanged = false;

            geode::Loader::get()->queueInMainThread([]() {
                bool wasActive = s_wasActiveBeforeBatch;
                bool enabledChanged = s_enabledChanged;
                bool paramChanged = s_recordingParamChanged;
                s_batchPending = false;
                s_enabledChanged = false;
                s_recordingParamChanged = false;

                auto mod = geode::Mod::get();
                if (!mod) return;

                bool targetEnabled = enabledChanged
                    ? mod->getSettingValue<bool>("enabled")
                    : ReplayConfig::get().enabled.load();

                if (wasActive && targetEnabled && paramChanged) {
                    geode::Notification::create(
                        "Recording settings can only be changed when recording is OFF!",
                        geode::NotificationIcon::Warning,
                        1.5f
                    )->show();
                    ReplayConfig::get().saveToSettings();
                    return;
                }

                if (wasActive && !targetEnabled) {
                    ReplayManager::get()->setReplayEnabled(false);
                }

                if (paramChanged) {
                    ReplayConfig::get().loadFromSettings();
                }

                if (enabledChanged && targetEnabled != ReplayManager::get()->isReplayActive()) {
                    ReplayManager::get()->setReplayEnabled(targetEnabled);
                }

                if (auto* popup = ReplayPopup::get()) {
                    popup->refreshUI();
                    popup->updateStatusDisplay(0.0f);
                }
            });
        }

        if (key == "enabled") {
            s_enabledChanged = true;
        } else {
            s_recordingParamChanged = true;
        }
    });
}

// Perform clean shutdown on game exit while OpenGL context and FMOD are still alive
$on_game(Exiting) {
    ReplayManager::get()->shutdown();
}

// Capture the finished frame right BEFORE it is presented, after all overlay/ImGui mods (like Mega Hack) have finished drawing
class $modify(ReplayEGLView, CCEGLView) {
    static void onModify(auto& self) {
        (void)self.setHookPriority("cocos2d::CCEGLView::swapBuffers", 100000);
        (void)self.setHookPriorityAfter("cocos2d::CCEGLView::swapBuffers", "absolllute.megahack");
    }

    void swapBuffers() {
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

#endif // GEODE_IS_WINDOWS
