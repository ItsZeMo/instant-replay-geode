#include "AudioGrabber.hpp"

#ifdef GEODE_IS_MACOS

#include "../config/ReplayConfig.hpp"
#include "../platform/MacPlatformUtils.hpp"
#include "../recorder/ReplayManager.hpp"

#pragma push_macro("CommentType")
#undef CommentType
#define CommentType AppleScriptCommentTypeDummy
#import <AVFoundation/AVFoundation.h>
#import <AudioToolbox/AudioToolbox.h>
#import <CoreAudio/CoreAudio.h>
#import <CoreFoundation/CoreFoundation.h>
#undef CommentType
#pragma pop_macro("CommentType")
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <mach/mach_time.h>
#include <thread>
#include <vector>

namespace replay {

namespace {

constexpr AudioObjectPropertyElement kMainElement = 0;

std::string cfStringToUtf8(CFStringRef cfStr) {
    if (!cfStr) return {};
    const char* fast = CFStringGetCStringPtr(cfStr, kCFStringEncodingUTF8);
    if (fast && fast[0]) return std::string(fast);

    CFIndex len = CFStringGetLength(cfStr);
    CFIndex maxBytes = CFStringGetMaximumSizeForEncoding(len, kCFStringEncodingUTF8) + 1;
    std::string out(static_cast<size_t>(std::max<CFIndex>(maxBytes, 1)), '\0');
    if (CFStringGetCString(cfStr, out.data(), maxBytes, kCFStringEncodingUTF8)) {
        out.resize(std::strlen(out.c_str()));
        return out;
    }
    return {};
}

bool getDeviceUid(AudioDeviceID deviceId, std::string& outUid) {
    CFStringRef uidCf = nullptr;
    UInt32 size = sizeof(uidCf);
    AudioObjectPropertyAddress addr = {
        kAudioDevicePropertyDeviceUID,
        kAudioObjectPropertyScopeGlobal,
        kMainElement
    };
    if (AudioObjectGetPropertyData(deviceId, &addr, 0, nullptr, &size, &uidCf) != noErr || !uidCf) {
        return false;
    }
    outUid = cfStringToUtf8(uidCf);
    CFRelease(uidCf);
    return !outUid.empty();
}

std::string getDeviceName(AudioDeviceID deviceId) {
    CFStringRef nameCf = nullptr;
    UInt32 size = sizeof(nameCf);
    AudioObjectPropertyAddress addr = {
        kAudioObjectPropertyName,
        kAudioObjectPropertyScopeGlobal,
        kMainElement
    };
    if (AudioObjectGetPropertyData(deviceId, &addr, 0, nullptr, &size, &nameCf) != noErr || !nameCf) {
        return {};
    }
    std::string raw = cfStringToUtf8(nameCf);
    CFRelease(nameCf);
    return raw;
}

UInt32 getDeviceInputChannelCount(AudioDeviceID deviceId) {
    AudioObjectPropertyAddress addr = {
        kAudioDevicePropertyStreamConfiguration,
        kAudioDevicePropertyScopeInput,
        kMainElement
    };
    UInt32 dataSize = 0;
    if (AudioObjectGetPropertyDataSize(deviceId, &addr, 0, nullptr, &dataSize) != noErr || dataSize == 0) {
        return 0;
    }
    std::vector<uint8_t> buf(dataSize);
    auto* abl = reinterpret_cast<AudioBufferList*>(buf.data());
    if (AudioObjectGetPropertyData(deviceId, &addr, 0, nullptr, &dataSize, abl) != noErr) {
        return 0;
    }
    UInt32 totalChannels = 0;
    for (UInt32 i = 0; i < abl->mNumberBuffers; ++i) {
        totalChannels += abl->mBuffers[i].mNumberChannels;
    }
    return totalChannels;
}

bool isAudioDeviceAlive(AudioDeviceID deviceId) {
    UInt32 alive = 0;
    UInt32 size = sizeof(alive);
    AudioObjectPropertyAddress addr = {
        kAudioDevicePropertyDeviceIsAlive,
        kAudioObjectPropertyScopeGlobal,
        kMainElement
    };
    if (AudioObjectGetPropertyData(deviceId, &addr, 0, nullptr, &size, &alive) != noErr) {
        return false;
    }
    return alive != 0;
}

double getDeviceNominalSampleRate(AudioDeviceID deviceId) {
    Float64 rate = 48000.0;
    UInt32 size = sizeof(rate);
    AudioObjectPropertyAddress addr = {
        kAudioDevicePropertyNominalSampleRate,
        kAudioObjectPropertyScopeGlobal,
        kMainElement
    };
    if (AudioObjectGetPropertyData(deviceId, &addr, 0, nullptr, &size, &rate) == noErr && rate >= 8000.0) {
        return rate;
    }
    return 48000.0;
}

bool isDeviceUidAvailable(const std::string& uid, AudioDeviceID* outDevId = nullptr) {
    if (uid.empty() || uid == "default") return false;
    CFStringRef uidCf = CFStringCreateWithCString(kCFAllocatorDefault, uid.c_str(), kCFStringEncodingUTF8);
    if (!uidCf) return false;

    AudioDeviceID devId = kAudioObjectUnknown;
    AudioObjectPropertyAddress addr = {
        kAudioHardwarePropertyTranslateUIDToDevice,
        kAudioObjectPropertyScopeGlobal,
        kMainElement
    };
    UInt32 size = sizeof(devId);
    OSStatus err = AudioObjectGetPropertyData(kAudioObjectSystemObject, &addr, sizeof(uidCf), &uidCf, &size, &devId);
    CFRelease(uidCf);
    if (err == noErr && devId != kAudioObjectUnknown &&
        isAudioDeviceAlive(devId) && getDeviceInputChannelCount(devId) > 0) {
        if (outDevId) *outDevId = devId;
        return true;
    }
    return false;
}

AudioDeviceID resolveTargetInputDevice(
    const std::string& targetId,
    bool forceDefaultFallback,
    bool& outUsingFallback
) {
    outUsingFallback = false;
    bool wantsSpecific = (!targetId.empty() && targetId != "default");

    if (wantsSpecific && !forceDefaultFallback) {
        AudioDeviceID specificId = kAudioObjectUnknown;
        if (isDeviceUidAvailable(targetId, &specificId)) {
            return specificId;
        }
        outUsingFallback = true;
    } else if (wantsSpecific && forceDefaultFallback) {
        outUsingFallback = true;
    }

    AudioDeviceID defaultId = kAudioObjectUnknown;
    UInt32 size = sizeof(defaultId);
    AudioObjectPropertyAddress addr = {
        kAudioHardwarePropertyDefaultInputDevice,
        kAudioObjectPropertyScopeGlobal,
        kMainElement
    };
    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &addr, 0, nullptr, &size, &defaultId) == noErr &&
        defaultId != kAudioObjectUnknown &&
        isAudioDeviceAlive(defaultId) &&
        getDeviceInputChannelCount(defaultId) > 0) {
        return defaultId;
    }
    return kAudioObjectUnknown;
}

struct MicQueueState {
    AudioGrabber* owner = nullptr;
    int channels = 1;
    int sampleRate = AudioGrabber::kMicSampleRate;
    mach_timebase_info_data_t timebase{};
    std::atomic<bool> deviceInvalidated{false};
    std::atomic<bool> devicesChanged{false};
    std::atomic<bool> stopping{false};
    std::atomic<uint64_t> consecutiveSilentFrames{0};
    std::atomic<bool> sawNonZeroAudio{false};
    std::atomic<bool> digitalSilenceAlert{false};
};

OSStatus onAudioObjectPropertyChanged(
    AudioObjectID /*inObjectID*/,
    UInt32 inNumberAddresses,
    const AudioObjectPropertyAddress* inAddresses,
    void* inClientData
) {
    if (auto* state = static_cast<MicQueueState*>(inClientData)) {
        bool deviceListOnly = (inNumberAddresses > 0 && inAddresses != nullptr);
        for (UInt32 i = 0; i < inNumberAddresses; ++i) {
            if (inAddresses[i].mSelector != kAudioHardwarePropertyDevices) {
                deviceListOnly = false;
                break;
            }
        }
        if (deviceListOnly) {
            state->devicesChanged.store(true, std::memory_order_release);
        } else {
            state->deviceInvalidated.store(true, std::memory_order_release);
        }
    }
    return noErr;
}

void onAudioQueueInput(
    void* inUserData,
    AudioQueueRef inAQ,
    AudioQueueBufferRef inBuffer,
    const AudioTimeStamp* inStartTime,
    UInt32 /*inNumberPacketDescriptions*/,
    const AudioStreamPacketDescription* /*inPacketDescs*/
) {
    auto* state = static_cast<MicQueueState*>(inUserData);
    if (!state || state->stopping.load(std::memory_order_acquire) || !state->owner) {
        return;
    }

    if (inBuffer && inBuffer->mAudioData && inBuffer->mAudioDataByteSize > 0) {
        const size_t bytesPerFrame = static_cast<size_t>(std::max(1, state->channels)) * sizeof(float);
        const UInt32 frames = static_cast<UInt32>(inBuffer->mAudioDataByteSize / bytesPerFrame);
        if (frames > 0) {
            const float* pcm = static_cast<const float*>(inBuffer->mAudioData);
            if (!state->sawNonZeroAudio.load(std::memory_order_relaxed)) {
                const size_t totalSamples = static_cast<size_t>(frames) * static_cast<size_t>(std::max(1, state->channels));
                bool nonZero = false;
                for (size_t i = 0; i < totalSamples; ++i) {
                    if (pcm[i] != 0.0f) {
                        nonZero = true;
                        break;
                    }
                }
                if (nonZero) {
                    state->sawNonZeroAudio.store(true, std::memory_order_relaxed);
                    state->consecutiveSilentFrames.store(0, std::memory_order_relaxed);
                } else {
                    uint64_t silent = state->consecutiveSilentFrames.fetch_add(frames, std::memory_order_relaxed) + frames;
                    if (silent >= static_cast<uint64_t>(std::max(1, state->sampleRate)) * 3ULL) {
                        state->digitalSilenceAlert.store(true, std::memory_order_release);
                    }
                }
            }

            auto captureTime = std::chrono::steady_clock::now();
            if (inStartTime && (inStartTime->mFlags & kAudioTimeStampHostTimeValid) &&
                inStartTime->mHostTime > 0 && state->timebase.denom > 0) {
                uint64_t nowHost = mach_absolute_time();
                if (nowHost >= inStartTime->mHostTime) {
                    __uint128_t diffNs =
                        (static_cast<__uint128_t>(nowHost - inStartTime->mHostTime) *
                         state->timebase.numer) /
                        state->timebase.denom;
                    if (diffNs <= 250000000ULL) {
                        captureTime -= std::chrono::nanoseconds(static_cast<int64_t>(diffNs));
                    }
                }
            }

            state->owner->pushMicAudio(
                pcm,
                frames,
                state->channels,
                state->sampleRate,
                captureTime);
        }
    }

    if (!state->stopping.load(std::memory_order_acquire)) {
        AudioQueueEnqueueBuffer(inAQ, inBuffer, 0, nullptr);
    }
}

} // namespace

std::vector<AudioDeviceInfo> AudioGrabber::getAvailableMicrophones() {
    MacAutoreleasePoolScope pool;
    std::vector<AudioDeviceInfo> list;
    list.push_back({"default", "Default System Device"});

    AudioObjectPropertyAddress devicesAddr = {
        kAudioHardwarePropertyDevices,
        kAudioObjectPropertyScopeGlobal,
        kMainElement
    };
    UInt32 dataSize = 0;
    if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &devicesAddr, 0, nullptr, &dataSize) != noErr ||
        dataSize < sizeof(AudioDeviceID)) {
        return list;
    }

    const size_t count = dataSize / sizeof(AudioDeviceID);
    std::vector<AudioDeviceID> devices(count);
    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &devicesAddr, 0, nullptr, &dataSize, devices.data()) != noErr) {
        return list;
    }

    size_t idx = 0;
    for (AudioDeviceID devId : devices) {
        if (!isAudioDeviceAlive(devId)) continue;
        if (getDeviceInputChannelCount(devId) == 0) continue;

        std::string uid;
        if (!getDeviceUid(devId, uid)) continue;

        std::string rawName = getDeviceName(devId);
        std::string cleanName = rawName.empty()
            ? ("Microphone " + std::to_string(idx + 1))
            : sanitizeDeviceNameUtf8(rawName);
        list.push_back({std::move(uid), std::move(cleanName)});
        ++idx;
    }

    return list;
}

void AudioGrabber::micCaptureThreadFunc() {
    MacAutoreleasePoolScope pool;
    m_micResampler.reset();

    auto isMicBlocked = [](MicCaptureStatus st) noexcept {
        return st == MicCaptureStatus::PermissionDenied ||
               st == MicCaptureStatus::BlockedNoUsageDescription;
    };
    auto showBlockedNoUsageNotification = []() {
        geode::queueInMainThread([]() {
            geode::Notification::create(
                "Mic blocked by macOS for this launch. Start GD through Steam (it asks for mic access) or turn off Record Microphone.",
                geode::NotificationIcon::Warning,
                5.0f
            )->show();
        });
    };

    // Check macOS Microphone TCC permission before touching CoreAudio input (M3, N2).
    const bool noUsageKey =
        ([[NSBundle mainBundle] objectForInfoDictionaryKey:@"NSMicrophoneUsageDescription"] == nil);
    AVAuthorizationStatus authStatus = [AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeAudio];
    const char* authLabel = "Unknown";
    switch (authStatus) {
        case AVAuthorizationStatusNotDetermined: authLabel = "NotDetermined"; break;
        case AVAuthorizationStatusRestricted: authLabel = "Restricted"; break;
        case AVAuthorizationStatusDenied: authLabel = "Denied"; break;
        case AVAuthorizationStatusAuthorized: authLabel = "Authorized"; break;
    }
    geode::log::info(
        "Instant Replay: macOS Microphone TCC status={}, hasUsageDescription={}",
        authLabel, !noUsageKey);

    if (authStatus == AVAuthorizationStatusDenied || authStatus == AVAuthorizationStatusRestricted) {
        m_micStatus.store(
            noUsageKey ? MicCaptureStatus::BlockedNoUsageDescription : MicCaptureStatus::PermissionDenied,
            std::memory_order_release);
        static std::atomic<bool> s_notifiedDenied{false};
        if (!s_notifiedDenied.exchange(true, std::memory_order_acq_rel)) {
            if (noUsageKey) {
                geode::log::warn(
                    "Instant Replay: macOS Microphone permission is {} and NSMicrophoneUsageDescription is missing; "
                    "launch Geometry Dash through Steam or disable Record Microphone",
                    authLabel);
                showBlockedNoUsageNotification();
            } else {
                geode::log::warn(
                    "Instant Replay: macOS Microphone permission is denied/restricted; microphone capture disabled "
                    "(enable Geometry Dash in System Settings -> Privacy & Security -> Microphone)");
                geode::queueInMainThread([]() {
                    geode::Notification::create(
                        "Instant Replay: Mic permission denied in macOS Privacy Settings",
                        geode::NotificationIcon::Warning,
                        4.0f
                    )->show();
                });
            }
        }
        return;
    }

    if (authStatus == AVAuthorizationStatusNotDetermined) {
        static std::atomic<bool> s_warnedNotDetermined{false};
        if (!s_warnedNotDetermined.exchange(true, std::memory_order_acq_rel)) {
            if (!noUsageKey) {
                geode::log::info("Instant Replay: requesting macOS Microphone permission");
                [AVCaptureDevice requestAccessForMediaType:AVMediaTypeAudio completionHandler:^(BOOL granted) {
                    if (!granted) {
                        m_micStatus.store(MicCaptureStatus::PermissionDenied, std::memory_order_release);
                    }
                }];
            } else {
                geode::log::warn(
                    "Instant Replay: macOS Microphone TCC is NotDetermined without NSMicrophoneUsageDescription; "
                    "starting capture (works when launched via Steam, otherwise mic audio will be silent)");
            }
        }
    }

    auto sleepInterruptible = [this](int totalMs) {
        int elapsed = 0;
        while (elapsed < totalMs && m_micRunning.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            elapsed += 50;
        }
    };

    const AudioObjectPropertyAddress defaultInAddr = {
        kAudioHardwarePropertyDefaultInputDevice,
        kAudioObjectPropertyScopeGlobal,
        kMainElement
    };
    const AudioObjectPropertyAddress devicesAddr = {
        kAudioHardwarePropertyDevices,
        kAudioObjectPropertyScopeGlobal,
        kMainElement
    };
    const AudioObjectPropertyAddress aliveAddr = {
        kAudioDevicePropertyDeviceIsAlive,
        kAudioObjectPropertyScopeGlobal,
        kMainElement
    };

    bool forceDefaultFallback = false;
    bool fallbackFromSetDevErr = false;
    std::string loggedFallbackTarget;
    std::string lastTargetSeen;
    static std::atomic<bool> s_warnedDigitalSilence{false};

    while (m_micRunning.load() &&
           !isMicBlocked(m_micStatus.load(std::memory_order_acquire))) {
        std::string targetId = ReplayManager::get()->getSessionConfig().getMicDeviceId();
        if (targetId != lastTargetSeen) {
            lastTargetSeen = targetId;
            forceDefaultFallback = false;
            fallbackFromSetDevErr = false;
        }

        bool usingFallback = false;
        AudioDeviceID devId = resolveTargetInputDevice(targetId, forceDefaultFallback, usingFallback);
        if (devId == kAudioObjectUnknown) {
            m_micStatus.store(MicCaptureStatus::NoInputDevice, std::memory_order_release);
            sleepInterruptible(1000);
            continue;
        }

        if (usingFallback && loggedFallbackTarget != targetId) {
            loggedFallbackTarget = targetId;
            std::string fbName = sanitizeDeviceNameUtf8(getDeviceName(devId));
            geode::log::warn(
                "Instant Replay: selected microphone unavailable - falling back to system default '{}'",
                fbName.empty() ? "Default Input" : fbName);
            geode::queueInMainThread([fbName]() {
                std::string msg = "Instant Replay: Mic unavailable - using " +
                                  (fbName.empty() ? std::string("Default Input") : fbName);
                geode::Notification::create(msg, geode::NotificationIcon::Warning, 3.0f)->show();
            });
        } else if (!usingFallback) {
            loggedFallbackTarget.clear();
        }

        std::string devUid;
        if (!getDeviceUid(devId, devUid)) {
            m_micStatus.store(MicCaptureStatus::NoInputDevice, std::memory_order_release);
            sleepInterruptible(1000);
            continue;
        }

        UInt32 inChannels = getDeviceInputChannelCount(devId);
        int captureChannels = (inChannels >= 2) ? 2 : 1;
        int captureRate = std::max(8000, static_cast<int>(std::lround(getDeviceNominalSampleRate(devId))));

        AudioStreamBasicDescription asbd{};
        asbd.mSampleRate = static_cast<Float64>(captureRate);
        asbd.mFormatID = kAudioFormatLinearPCM;
        asbd.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
        asbd.mBitsPerChannel = 32;
        asbd.mChannelsPerFrame = static_cast<UInt32>(captureChannels);
        asbd.mBytesPerFrame = static_cast<UInt32>(captureChannels) * sizeof(float);
        asbd.mFramesPerPacket = 1;
        asbd.mBytesPerPacket = asbd.mBytesPerFrame;

        MicQueueState qState;
        qState.owner = this;
        qState.channels = captureChannels;
        qState.sampleRate = captureRate;
        mach_timebase_info(&qState.timebase);

        AudioQueueRef queue = nullptr;
        OSStatus err = AudioQueueNewInput(
            &asbd,
            onAudioQueueInput,
            &qState,
            nullptr,
            nullptr,
            0,
            &queue);

        bool deviceLost = false;
        bool registeredDefaultListener = false;
        bool registeredDevicesListener = false;
        bool registeredAliveListener = false;
        bool reportedDigitalSilence = false;

        if (err == noErr && queue) {
            bool setDeviceOk = true;
            if (!usingFallback && !targetId.empty() && targetId != "default") {
                CFStringRef uidCf = CFStringCreateWithCString(kCFAllocatorDefault, devUid.c_str(), kCFStringEncodingUTF8);
                if (uidCf) {
                    OSStatus setDevErr = AudioQueueSetProperty(
                        queue, kAudioQueueProperty_CurrentDevice, &uidCf, sizeof(uidCf));
                    CFRelease(uidCf);
                    if (setDevErr != noErr) {
                        geode::log::warn(
                            "Instant Replay: AudioQueueSetProperty(CurrentDevice) failed (OSStatus {}), falling back to default mic",
                            static_cast<int>(setDevErr));
                        setDeviceOk = false;
                        forceDefaultFallback = true;
                        fallbackFromSetDevErr = true;
                    }
                } else {
                    setDeviceOk = false;
                    forceDefaultFallback = true;
                    fallbackFromSetDevErr = true;
                }
            } else {
                if (AudioObjectAddPropertyListener(
                        kAudioObjectSystemObject, &defaultInAddr, onAudioObjectPropertyChanged, &qState) == noErr) {
                    registeredDefaultListener = true;
                }
            }

            if (usingFallback) {
                if (AudioObjectAddPropertyListener(
                        kAudioObjectSystemObject, &devicesAddr, onAudioObjectPropertyChanged, &qState) == noErr) {
                    registeredDevicesListener = true;
                }
            }

            if (setDeviceOk && AudioObjectAddPropertyListener(
                    devId, &aliveAddr, onAudioObjectPropertyChanged, &qState) == noErr) {
                registeredAliveListener = true;
            }

            // 6 buffers x 20ms = 120ms AudioQueue buffering prevents drops during OS scheduling jitter
            constexpr int kBufferCount = 6;
            const UInt32 framesPerBuf = std::max<UInt32>(256, static_cast<UInt32>(captureRate / 50));
            const UInt32 bytesPerBuf = framesPerBuf * asbd.mBytesPerFrame;

            bool allocOk = setDeviceOk;
            for (int i = 0; allocOk && i < kBufferCount; ++i) {
                AudioQueueBufferRef buf = nullptr;
                if (AudioQueueAllocateBuffer(queue, bytesPerBuf, &buf) != noErr || !buf ||
                    AudioQueueEnqueueBuffer(queue, buf, 0, nullptr) != noErr) {
                    allocOk = false;
                    break;
                }
            }

            if (allocOk && AudioQueueStart(queue, nullptr) == noErr) {
                m_micStatus.store(
                    usingFallback ? MicCaptureStatus::UsingFallbackDevice : MicCaptureStatus::Active,
                    std::memory_order_release);
                geode::log::info(
                    "Instant Replay: CoreAudio mic capture started ({}ch @ {} Hz{})",
                    captureChannels, captureRate, usingFallback ? ", fallback default" : "");

                int pollCount = 0;
                while (m_micRunning.load() &&
                       !qState.deviceInvalidated.load(std::memory_order_acquire) &&
                       !isMicBlocked(m_micStatus.load(std::memory_order_acquire))) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                    ++pollCount;

                    if (isMicBlocked(m_micStatus.load(std::memory_order_acquire))) {
                        break;
                    }

                    if (qState.digitalSilenceAlert.load(std::memory_order_acquire) && !reportedDigitalSilence) {
                        reportedDigitalSilence = true;
                        const MicCaptureStatus silenceStatus = noUsageKey
                            ? MicCaptureStatus::BlockedNoUsageDescription
                            : MicCaptureStatus::DigitalSilenceDetected;
                        m_micStatus.store(silenceStatus, std::memory_order_release);
                        if (!s_warnedDigitalSilence.exchange(true, std::memory_order_acq_rel)) {
                            if (noUsageKey) {
                                geode::log::warn(
                                    "Instant Replay: microphone captured >3s of pure digital silence without NSMicrophoneUsageDescription; "
                                    "macOS TCC blocked access for this launch (start GD through Steam or turn off Record Microphone)");
                                showBlockedNoUsageNotification();
                            } else {
                                geode::log::warn(
                                    "Instant Replay: microphone captured >3s of pure digital silence (check macOS Microphone TCC permission or hardware mute)");
                                geode::queueInMainThread([]() {
                                    geode::Notification::create(
                                        "Instant Replay: Mic audio is silent (check Privacy Settings or mute)",
                                        geode::NotificationIcon::Warning,
                                        3.5f
                                    )->show();
                                });
                            }
                        }
                    } else if (reportedDigitalSilence && qState.sawNonZeroAudio.load(std::memory_order_relaxed)) {
                        reportedDigitalSilence = false;
                        m_micStatus.store(
                            usingFallback ? MicCaptureStatus::UsingFallbackDevice : MicCaptureStatus::Active,
                            std::memory_order_release);
                    }

                    std::string latestTarget = ReplayManager::get()->getSessionConfig().getMicDeviceId();
                    if (latestTarget != targetId) {
                        forceDefaultFallback = false;
                        fallbackFromSetDevErr = false;
                        break;
                    }
                    if (!isAudioDeviceAlive(devId)) {
                        deviceLost = true;
                        break;
                    }
                    bool devListChanged = qState.devicesChanged.exchange(false, std::memory_order_acq_rel);
                    if (usingFallback && (devListChanged || (!fallbackFromSetDevErr && (pollCount % 20 == 0)))) {
                        if (isDeviceUidAvailable(targetId)) {
                            geode::log::info("Instant Replay: preferred microphone reconnected, reattaching");
                            forceDefaultFallback = false;
                            fallbackFromSetDevErr = false;
                            break;
                        }
                    }
                }
                if (qState.deviceInvalidated.load(std::memory_order_acquire)) {
                    deviceLost = true;
                }
            } else {
                deviceLost = true;
            }

            qState.stopping.store(true, std::memory_order_release);
            AudioQueueStop(queue, true);
            AudioQueueDispose(queue, true);
        } else {
            deviceLost = true;
        }

        if (registeredAliveListener) {
            AudioObjectRemovePropertyListener(devId, &aliveAddr, onAudioObjectPropertyChanged, &qState);
        }
        if (registeredDevicesListener) {
            AudioObjectRemovePropertyListener(
                kAudioObjectSystemObject, &devicesAddr, onAudioObjectPropertyChanged, &qState);
        }
        if (registeredDefaultListener) {
            AudioObjectRemovePropertyListener(
                kAudioObjectSystemObject, &defaultInAddr, onAudioObjectPropertyChanged, &qState);
        }

        if (m_micRunning.load()) {
            m_micResampler.reset();
            sleepInterruptible(deviceLost ? 500 : 1000);
        }
    }

    if (!isMicBlocked(m_micStatus.load(std::memory_order_acquire))) {
        m_micStatus.store(MicCaptureStatus::Idle, std::memory_order_release);
    }
}

} // namespace replay

#endif // GEODE_IS_MACOS
