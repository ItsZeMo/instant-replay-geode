#include <Geode/Geode.hpp>

#if defined(GEODE_IS_WINDOWS) || defined(GEODE_IS_MACOS) || defined(GEODE_IS_IOS)

#ifdef GEODE_IS_WINDOWS
#include <Geode/modify/CCEGLView.hpp>
#elif defined(GEODE_IS_MACOS)
#include "platform/MacPlatformUtils.hpp"
#elif defined(GEODE_IS_IOS)
#include "platform/IOSPlatformUtils.hpp"
#include <Geode/modify/EndLevelLayer.hpp>
#endif
#include <Geode/modify/FMODAudioEngine.hpp>
#include <Geode/modify/MenuLayer.hpp>
#include <Geode/modify/PauseLayer.hpp>
#include <Geode/loader/SettingV3.hpp>

#include "config/ReplayConfig.hpp"
#include "recorder/ReplayManager.hpp"
#include "capture/FrameGrabber.hpp"
#include "capture/AudioGrabber.hpp"
#include "ui/ReplayPopup.hpp"

using namespace geode::prelude;
using namespace replay;

static bool shouldIgnoreKeybindWhileTyping(Keybind const& kb) {
    if (!CCIMEDispatcher::sharedDispatcher()->hasDelegate()) {
        return false;
    }
    bool hasModifier = (kb.modifiers.value & (KeyboardModifier::Control | KeyboardModifier::Alt | KeyboardModifier::Super)) != 0;
    bool isFKey = (kb.key >= KEY_F1 && kb.key <= KEY_F24);
    return !hasModifier && !isFKey;
}

static void addReplayButtonToMenu(CCNode* menu, CCObject* target, SEL_MenuHandler selector) {
    if (!menu) return;
    auto spr = CCSprite::createWithSpriteFrameName("GJ_completesIcon_001.png");
    if (!spr) return;
    spr->setScale(0.85f);
    spr->setColor(ccc3(100, 220, 255));
    auto btn = CCMenuItemSpriteExtra::create(spr, target, selector);
    btn->setID("instant-replay-button"_spr);
    menu->addChild(btn);
    menu->updateLayout();
}

static void triggerSaveReplay() {
    geode::log::info("Instant Replay: Keybind triggered Save Replay");
    auto res = ReplayManager::get()->saveReplay();
    ReplayManager::showSaveFeedback(res);
}

#ifdef GEODE_IS_IOS
// Touch entry point for saving: there is usually no keyboard for the F6 keybind on iPhone / iPad.
static CCMenuItemSpriteExtra* createSaveReplayButton(CCObject* target, SEL_MenuHandler selector, float scale) {
    auto spr = CCSprite::createWithSpriteFrameName("GJ_adVideoBtn_001.png");
    if (!spr) return nullptr;
    spr->setScale(scale);
    auto btn = CCMenuItemSpriteExtra::create(spr, target, selector);
    btn->setID("save-replay-button"_spr);
    return btn;
}
#endif

static void triggerToggleReplay() {
    geode::log::info("Instant Replay: Keybind triggered Toggle Replay");
    ToggleReplayResult res = ReplayManager::get()->toggleReplay();
    if (auto* popup = ReplayPopup::get()) {
        popup->refreshUI();
        popup->updateStatusDisplay(0.0f);
    }
    if (ReplayConfig::get().showNotification.load()) {
        switch (res) {
            case ToggleReplayResult::Enabled:
                geode::Notification::create("Instant Replay: Replay Enabled", geode::NotificationIcon::Success, 1.5f)->show();
                break;
            case ToggleReplayResult::Stopping:
                geode::Notification::create("Instant Replay: Stopping...", geode::NotificationIcon::Warning, 1.5f)->show();
                break;
            case ToggleReplayResult::RestartQueued:
                geode::Notification::create("Instant Replay: Restarting after stop...", geode::NotificationIcon::Info, 1.5f)->show();
                break;
            case ToggleReplayResult::StopPending:
                geode::Notification::create("Instant Replay: Stop pending...", geode::NotificationIcon::Warning, 1.5f)->show();
                break;
            case ToggleReplayResult::Unavailable:
                break;
        }
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

#ifdef GEODE_IS_MACOS
    // On macOS, CCDirectorCaller calls -[NSOpenGLContext flushBuffer] directly (CCEGLView::swapBuffers is a no-op).
    // Install BEFORE ReplayManager::initialize() so auto-start knows whether the GL capture backend is live (M20).
    bool macHookOk = installMacGLCaptureHook(
        []() { FrameGrabber::get()->captureCurrentFrame(); },
        []() { return FrameGrabber::get()->isCapturing(); }
    );
#elif defined(GEODE_IS_IOS)
    // On iOS, EAGLView::swapBuffers ends in -[EAGLContext presentRenderbuffer:]; swizzling it (no code patch,
    // works JIT-less) captures the finished frame after every overlay. Installed before initialize() (M20).
    bool macHookOk = installIOSGLCaptureHook(
        []() { FrameGrabber::get()->captureCurrentFrame(); },
        []() { return FrameGrabber::get()->isCapturing() && !ReplayManager::get()->isAppSuspended(); }
    );
    // The hardware encoder is unavailable in the background: pause on resign-active, resume when active.
    installIOSAppLifecycleObservers(
        []() { ReplayManager::get()->setAppSuspended(true); },
        []() { ReplayManager::get()->setAppSuspended(false); }
    );
#endif

    const bool enabledBeforeInit = geode::Mod::get() && geode::Mod::get()->getSettingValue<bool>("enabled");
    ReplayManager::get()->initialize();

#if defined(GEODE_IS_MACOS) || defined(GEODE_IS_IOS)
    if (!macHookOk && !enabledBeforeInit) {
        geode::Loader::get()->queueInMainThread([]() {
            geode::Notification::create(
                "Instant Replay: Video capture hook failed - check Geode log",
                geode::NotificationIcon::Error,
                4.0f
            )->show();
        });
    }
#endif

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
        static bool s_enabledChanged = false;
        static bool s_recordingParamChanged = false;

        if (!s_batchPending) {
            s_batchPending = true;
            s_enabledChanged = false;
            s_recordingParamChanged = false;

            geode::Loader::get()->queueInMainThread([]() {
                bool enabledChanged = s_enabledChanged;
                bool paramChanged = s_recordingParamChanged;
                s_batchPending = false;
                s_enabledChanged = false;
                s_recordingParamChanged = false;

                auto mod = geode::Mod::get();
                if (!mod) return;

                bool isActive = ReplayManager::get()->isReplayActive();
                bool isLocked = ReplayManager::get()->isReplayLocked();
                bool targetEnabled = enabledChanged
                    ? mod->getSettingValue<bool>("enabled")
                    : ReplayConfig::get().enabled.load();

                if (isLocked && targetEnabled && paramChanged) {
                    geode::Notification::create(
                        "Recording settings can only be changed when recording is OFF!",
                        geode::NotificationIcon::Warning,
                        1.5f
                    )->show();
                    ReplayConfig::get().saveToSettings(true);
                    return;
                }

                if ((isActive || ReplayManager::get()->isRestartPending()) && !targetEnabled) {
                    ReplayManager::get()->setReplayEnabled(false);
                }

                if (paramChanged) {
#if defined(GEODE_IS_MACOS) || defined(GEODE_IS_IOS)
                    auto prevModCodecStr = mod->getSettingValue<std::string>("codec");
                    ReplayConfig::get().loadFromSettings();
                    if (ReplayConfig::codecFromString(prevModCodecStr) != ReplayConfig::get().codec.load()) {
                        std::string msg = "Selected codec unsupported - switched to " +
                                          ReplayConfig::get().getCodecDisplayName();
                        geode::Notification::create(msg, geode::NotificationIcon::Warning, 2.5f)->show();
                    }
#else
                    ReplayConfig::get().loadFromSettings();
#endif
                }

                if (enabledChanged && targetEnabled && !ReplayManager::get()->isReplayActive()) {
                    ReplayManager::get()->setReplayEnabled(true);
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
#ifdef GEODE_IS_MACOS
    disableMacGLCaptureHook();
#elif defined(GEODE_IS_IOS)
    disableIOSGLCaptureHook();
#endif
    ReplayManager::get()->shutdown();
}

#ifdef GEODE_IS_WINDOWS
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
#endif

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
        addReplayButtonToMenu(this->getChildByID("bottom-menu"), this, menu_selector(ReplayMenuLayer::onOpenReplay));
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
        auto menu = this->getChildByID("right-button-menu");
        addReplayButtonToMenu(menu, this, menu_selector(ReplayPauseLayer::onOpenReplay));
#ifdef GEODE_IS_IOS
        if (menu) {
            if (auto saveBtn = createSaveReplayButton(this, menu_selector(ReplayPauseLayer::onSaveReplay), 0.55f)) {
                menu->addChild(saveBtn);
                menu->updateLayout();
            }
        }
#endif
    }

    void onOpenReplay(CCObject*) {
        ReplayPopup::toggle();
    }

    void onSaveReplay(CCObject*) {
        triggerSaveReplay();
    }
};

#ifdef GEODE_IS_IOS
// Level complete is when a replay is usually saved: put a large Save Replay button in the safe area's
// bottom-right corner of the results screen (it slides in with the drop-down layer).
class $modify(ReplayEndLevelLayer, EndLevelLayer) {
    void customSetup() {
        EndLevelLayer::customSetup();
        if (!m_mainLayer || !ReplayManager::get()->isReplayActive()) return;

        auto btn = createSaveReplayButton(this, menu_selector(ReplayEndLevelLayer::onSaveReplay), 0.7f);
        if (!btn) return;
        // Label left of the icon: keeps the button low in the corner, clear of the results panel on 4:3 iPads.
        auto label = CCLabelBMFont::create("Save Replay", "bigFont.fnt");
        label->setScale(0.3f);
        label->setAnchorPoint(ccp(1.0f, 0.5f));
        label->setPosition(ccp(-4.0f, btn->getContentSize().height / 2.0f));
        btn->addChild(label);

        auto menu = CCMenu::create();
        menu->setID("instant-replay-menu"_spr);
        menu->addChild(btn);
        CCRect safe = geode::utils::getSafeAreaRect();
        menu->setPosition(ccp(safe.getMaxX() - btn->getScaledContentSize().width / 2.0f - 10.0f,
                              safe.getMinY() + btn->getScaledContentSize().height / 2.0f + 8.0f));
        m_mainLayer->addChild(menu, 20);
    }

    void onSaveReplay(CCObject*) {
        triggerSaveReplay();
    }
};
#endif

#endif // GEODE_IS_WINDOWS || GEODE_IS_MACOS || GEODE_IS_IOS
