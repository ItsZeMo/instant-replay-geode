#include "AudioGrabber.hpp"

#ifdef GEODE_IS_IOS

#include "../platform/IOSPlatformUtils.hpp"
#include "../recorder/ReplayManager.hpp"

#import <AVFoundation/AVFoundation.h>
#import <AudioToolbox/AudioToolbox.h>
#import <Foundation/Foundation.h>

#include <mach/mach_time.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <memory>
#include <string>
#include <thread>

// iOS microphone capture.
//
// FMOD keeps playing game audio through the shared AVAudioSession. Recording needs a record-capable
// category, so while microphone capture runs the session is switched to PlayAndRecord (speaker output,
// Bluetooth A2DP output kept, mixing preserved) and the previous category is restored afterwards.
// iOS terminates any process that touches the microphone without NSMicrophoneUsageDescription in the
// Info.plist of the app iOS attributes the process to, so that key is checked before anything else.

namespace replay {

namespace {

struct MicQueueState {
    AudioGrabber* owner = nullptr;
    int channels = 1;
    int sampleRate = AudioGrabber::kMicSampleRate;
    mach_timebase_info_data_t timebase{};
    std::atomic<bool> stopping{false};
    std::atomic<bool> restartRequested{false}; // route change, interruption end, media services reset
    std::atomic<bool> interrupted{false};
    std::atomic<uint64_t> consecutiveSilentFrames{0};
    std::atomic<bool> sawNonZeroAudio{false};
    std::atomic<bool> digitalSilenceAlert{false};
};

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
                        (static_cast<__uint128_t>(nowHost - inStartTime->mHostTime) * state->timebase.numer) /
                        state->timebase.denom;
                    if (diffNs <= 250000000ULL) {
                        captureTime -= std::chrono::nanoseconds(static_cast<int64_t>(diffNs));
                    }
                }
            }

            state->owner->pushMicAudio(pcm, frames, state->channels, state->sampleRate, captureTime);
        }
    }

    if (inBuffer && !state->stopping.load(std::memory_order_acquire)) {
        AudioQueueEnqueueBuffer(inAQ, inBuffer, 0, nullptr);
    }
}

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
// AVAudioSession's record permission API is deprecated in favour of AVAudioApplication (iOS 17+) but is
// the one available on every iOS version Geode supports (14+).
MicPrivacyAuthorization currentRecordPermission() {
    switch ([AVAudioSession sharedInstance].recordPermission) {
        case AVAudioSessionRecordPermissionGranted: return MicPrivacyAuthorization::Authorized;
        case AVAudioSessionRecordPermissionDenied:  return MicPrivacyAuthorization::Denied;
        case AVAudioSessionRecordPermissionUndetermined:
        default:                                    return MicPrivacyAuthorization::NotDetermined;
    }
}

void requestRecordPermission(std::shared_ptr<std::atomic<int>> result) {
    [[AVAudioSession sharedInstance] requestRecordPermission:^(BOOL granted) {
        result->store(granted ? 1 : 0, std::memory_order_release);
    }];
}
#pragma clang diagnostic pop

struct SessionBackup {
    bool valid = false;
    NSString* category = nil;
    NSString* mode = nil;
    AVAudioSessionCategoryOptions options = 0;
};

bool configureSessionForRecording(SessionBackup& backup) {
    AVAudioSession* session = [AVAudioSession sharedInstance];
    if (!backup.valid) {
        backup.category = session.category;
        backup.mode = session.mode;
        backup.options = session.categoryOptions;
        backup.valid = true;
    }

    // Keep the game's mixing behaviour: Ambient / MixWithOthers sessions keep other apps' audio playing.
    const bool mixes = [backup.category isEqualToString:AVAudioSessionCategoryAmbient] ||
                       (backup.options & AVAudioSessionCategoryOptionMixWithOthers) != 0;
    AVAudioSessionCategoryOptions opts = AVAudioSessionCategoryOptionDefaultToSpeaker |
                                         AVAudioSessionCategoryOptionAllowBluetoothA2DP;
    if (mixes) {
        opts |= AVAudioSessionCategoryOptionMixWithOthers;
    }

    NSError* err = nil;
    if (![session.category isEqualToString:AVAudioSessionCategoryPlayAndRecord] || session.categoryOptions != opts) {
        if (![session setCategory:AVAudioSessionCategoryPlayAndRecord mode:AVAudioSessionModeDefault options:opts error:&err]) {
            geode::log::warn("Instant Replay: cannot switch AVAudioSession to PlayAndRecord: {}",
                             err ? err.localizedDescription.UTF8String : "unknown error");
            return false;
        }
    }
    err = nil;
    if (![session setActive:YES error:&err]) {
        geode::log::warn("Instant Replay: cannot activate AVAudioSession for recording: {}",
                         err ? err.localizedDescription.UTF8String : "unknown error");
        return false;
    }
    return true;
}

void restoreSession(SessionBackup& backup) {
    if (!backup.valid) return;
    AVAudioSession* session = [AVAudioSession sharedInstance];
    NSError* err = nil;
    if (backup.category && ![session setCategory:backup.category mode:(backup.mode ?: AVAudioSessionModeDefault)
                                         options:backup.options error:&err]) {
        geode::log::warn("Instant Replay: cannot restore AVAudioSession category: {}",
                         err ? err.localizedDescription.UTF8String : "unknown error");
    }
    backup = {};
}

// Applies the selected input (port UID) as the session's preferred input. Returns false when it is gone.
bool applyPreferredInput(const std::string& targetId, std::string& outActiveName) {
    AVAudioSession* session = [AVAudioSession sharedInstance];
    NSError* err = nil;
    bool found = true;
    if (targetId.empty() || targetId == "default") {
        [session setPreferredInput:nil error:&err];
    } else {
        found = false;
        for (AVAudioSessionPortDescription* port in session.availableInputs) {
            if (port.UID && targetId == port.UID.UTF8String) {
                found = [session setPreferredInput:port error:&err];
                break;
            }
        }
        if (!found) {
            [session setPreferredInput:nil error:nil];
        }
    }
    AVAudioSessionPortDescription* active = session.currentRoute.inputs.firstObject;
    outActiveName = active.portName ? AudioGrabber::sanitizeDeviceNameUtf8(active.portName.UTF8String) : "";
    return found;
}

} // namespace

std::vector<AudioDeviceInfo> AudioGrabber::getAvailableMicrophones() {
    AppleAutoreleasePoolScope pool;
    std::vector<AudioDeviceInfo> list;
    list.push_back({"default", "Default Input"});
    // availableInputs is only populated while a record-capable category is active; with the game's
    // playback category the list stays at "Default Input" until recording has started once.
    NSArray<AVAudioSessionPortDescription*>* inputs = [AVAudioSession sharedInstance].availableInputs;
    size_t idx = 0;
    for (AVAudioSessionPortDescription* port in inputs) {
        if (!port.UID.length) continue;
        std::string name = port.portName.length ? sanitizeDeviceNameUtf8(port.portName.UTF8String) : "";
        if (name.empty()) name = "Microphone " + std::to_string(idx + 1);
        list.push_back({port.UID.UTF8String, std::move(name)});
        ++idx;
    }
    return list;
}

void AudioGrabber::micCaptureThreadFunc() {
    AppleAutoreleasePoolScope pool;
    m_micResampler.reset();
    // Every capture run re-evaluates permission and silence from scratch (K1).
    m_micStatus.store(MicCaptureStatus::Idle, std::memory_order_release);

    auto stopsCapture = [this]() noexcept {
        return micStatusStopsCapture(m_micStatus.load(std::memory_order_acquire));
    };
    auto setStatusUnlessStopped = [this](MicCaptureStatus next) noexcept {
        MicCaptureStatus cur = m_micStatus.load(std::memory_order_acquire);
        while (!micStatusStopsCapture(cur) &&
               !m_micStatus.compare_exchange_weak(cur, next, std::memory_order_acq_rel, std::memory_order_acquire)) {
        }
    };
    auto sleepInterruptible = [this](int totalMs) {
        int elapsed = 0;
        while (elapsed < totalMs && m_micRunning.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            elapsed += 50;
        }
    };

    const bool hasUsageKey = iosHostHasInfoKey("NSMicrophoneUsageDescription");
    MicPrivacyAuthorization auth = currentRecordPermission();
    geode::log::info("Instant Replay: iOS microphone permission={}, hostHasUsageDescription={}",
                     auth == MicPrivacyAuthorization::Authorized ? "Granted"
                         : auth == MicPrivacyAuthorization::Denied ? "Denied" : "Undetermined",
                     hasUsageKey);

    if (!hasUsageKey) {
        // Touching the microphone would get the whole game killed by iOS privacy enforcement. A grant cannot exist
        // without the key, so a "granted" state here belongs to some other configuration: do not rely on it.
        m_micStatus.store(MicCaptureStatus::BlockedNoUsageDescription, std::memory_order_release);
        static std::atomic<bool> s_notified{false};
        if (!s_notified.exchange(true)) {
            geode::log::warn("Instant Replay: the app running Geometry Dash declares no NSMicrophoneUsageDescription; "
                             "microphone recording is unavailable with this launcher");
            geode::queueInMainThread([]() {
                geode::Notification::create(
                    "Instant Replay: Mic unavailable - this launcher has no microphone permission entry",
                    geode::NotificationIcon::Warning, 5.0f)->show();
            });
        }
        return;
    }

    if (auth == MicPrivacyAuthorization::NotDetermined) {
        geode::log::info("Instant Replay: requesting iOS microphone permission");
        auto result = std::make_shared<std::atomic<int>>(-1);
        requestRecordPermission(result);
        // The system prompt is modal; wait for the answer while recording stays active.
        while (m_micRunning.load() && result->load(std::memory_order_acquire) < 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        if (!m_micRunning.load()) return;
        auth = result->load() == 1 ? MicPrivacyAuthorization::Authorized : MicPrivacyAuthorization::Denied;
    }

    if (micAuthorizationBlocksCapture(auth)) {
        m_micStatus.store(MicCaptureStatus::PermissionDenied, std::memory_order_release);
        static std::atomic<bool> s_notifiedDenied{false};
        if (!s_notifiedDenied.exchange(true)) {
            geode::log::warn("Instant Replay: iOS microphone permission denied; microphone capture disabled for this launch");
            geode::queueInMainThread([]() {
                geode::Notification::create(
                    "Instant Replay: Mic access denied - allow it in iOS Settings > Privacy & Security > Microphone",
                    geode::NotificationIcon::Warning, 5.0f)->show();
            });
        }
        return;
    }

    SessionBackup sessionBackup;
    std::string loggedFallbackTarget;
    static std::atomic<bool> s_warnedDigitalSilence{false};

    while (m_micRunning.load() && !stopsCapture()) {
        if (!configureSessionForRecording(sessionBackup)) {
            setStatusUnlessStopped(MicCaptureStatus::NoInputDevice);
            sleepInterruptible(2000);
            continue;
        }

        AVAudioSession* session = [AVAudioSession sharedInstance];
        if (!session.isInputAvailable) {
            setStatusUnlessStopped(MicCaptureStatus::NoInputDevice);
            sleepInterruptible(1000);
            continue;
        }

        const std::string targetId = ReplayManager::get()->getSessionConfig().getMicDeviceId();
        std::string activeName;
        const bool usingFallback = !applyPreferredInput(targetId, activeName);
        if (usingFallback && loggedFallbackTarget != targetId) {
            loggedFallbackTarget = targetId;
            geode::log::warn("Instant Replay: selected microphone unavailable - using '{}'",
                             activeName.empty() ? "default input" : activeName);
            geode::queueInMainThread([activeName]() {
                geode::Notification::create(
                    "Instant Replay: Mic unavailable - using " + (activeName.empty() ? std::string("Default Input") : activeName),
                    geode::NotificationIcon::Warning, 3.0f)->show();
            });
        } else if (!usingFallback) {
            loggedFallbackTarget.clear();
        }

        const int captureRate = std::max(8000, static_cast<int>(std::lround(session.sampleRate > 0 ? session.sampleRate : 48000.0)));
        const int captureChannels = session.inputNumberOfChannels >= 2 ? 2 : 1;

        AudioStreamBasicDescription asbd{};
        asbd.mSampleRate = static_cast<Float64>(captureRate);
        asbd.mFormatID = kAudioFormatLinearPCM;
        asbd.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
        asbd.mBitsPerChannel = 32;
        asbd.mChannelsPerFrame = static_cast<UInt32>(captureChannels);
        asbd.mBytesPerFrame = static_cast<UInt32>(captureChannels) * sizeof(float);
        asbd.mFramesPerPacket = 1;
        asbd.mBytesPerPacket = asbd.mBytesPerFrame;

        // Heap state shared with the notification blocks: NSNotificationCenter may still be running a block on
        // another thread while the observer is removed, so the blocks keep their own reference.
        auto qShared = std::make_shared<MicQueueState>();
        MicQueueState& qState = *qShared;
        qState.owner = this;
        qState.channels = captureChannels;
        qState.sampleRate = captureRate;
        mach_timebase_info(&qState.timebase);

        // Session events that invalidate the running queue.
        std::shared_ptr<MicQueueState> qPtr = qShared;
        NSNotificationCenter* nc = [NSNotificationCenter defaultCenter];
        id interruptionObs = [nc addObserverForName:AVAudioSessionInterruptionNotification object:nil queue:nil
                                         usingBlock:^(NSNotification* note) {
            NSNumber* type = note.userInfo[AVAudioSessionInterruptionTypeKey];
            if (type.unsignedIntegerValue == AVAudioSessionInterruptionTypeBegan) {
                qPtr->interrupted.store(true, std::memory_order_release);
            } else {
                qPtr->interrupted.store(false, std::memory_order_release);
                qPtr->restartRequested.store(true, std::memory_order_release);
            }
        }];
        id routeObs = [nc addObserverForName:AVAudioSessionRouteChangeNotification object:nil queue:nil
                                  usingBlock:^(NSNotification* note) {
            NSNumber* reason = note.userInfo[AVAudioSessionRouteChangeReasonKey];
            switch (reason.unsignedIntegerValue) {
                case AVAudioSessionRouteChangeReasonNewDeviceAvailable:
                case AVAudioSessionRouteChangeReasonOldDeviceUnavailable:
                case AVAudioSessionRouteChangeReasonOverride:
                    qPtr->restartRequested.store(true, std::memory_order_release);
                    break;
                default:
                    break;
            }
        }];
        id resetObs = [nc addObserverForName:AVAudioSessionMediaServicesWereResetNotification object:nil queue:nil
                                  usingBlock:^(NSNotification*) {
            qPtr->restartRequested.store(true, std::memory_order_release);
        }];

        AudioQueueRef queue = nullptr;
        // AudioQueueDispose(queue, true) below is synchronous, so no input callback outlives this iteration.
        OSStatus err = AudioQueueNewInput(&asbd, onAudioQueueInput, qShared.get(), nullptr, nullptr, 0, &queue);
        MicSilenceTracker silenceTracker; // per queue: silence reported once, recovery once (K1)
        bool started = false;

        if (err == noErr && queue) {
            // 6 buffers x 20 ms = 120 ms of AudioQueue buffering against scheduling jitter.
            constexpr int kBufferCount = 6;
            const UInt32 framesPerBuf = std::max<UInt32>(256, static_cast<UInt32>(captureRate / 50));
            const UInt32 bytesPerBuf = framesPerBuf * asbd.mBytesPerFrame;
            bool allocOk = true;
            for (int i = 0; allocOk && i < kBufferCount; ++i) {
                AudioQueueBufferRef buf = nullptr;
                if (AudioQueueAllocateBuffer(queue, bytesPerBuf, &buf) != noErr || !buf ||
                    AudioQueueEnqueueBuffer(queue, buf, 0, nullptr) != noErr) {
                    allocOk = false;
                }
            }

            OSStatus startErr = allocOk ? AudioQueueStart(queue, nullptr) : -1;
            if (startErr == noErr) {
                started = true;
                setStatusUnlessStopped(usingFallback ? MicCaptureStatus::UsingFallbackDevice : MicCaptureStatus::Active);
                geode::log::info("Instant Replay: iOS mic capture started ({}ch @ {} Hz, input '{}'{})",
                                 captureChannels, captureRate, activeName, usingFallback ? ", fallback" : "");

                while (m_micRunning.load() && !stopsCapture() &&
                       !qState.restartRequested.load(std::memory_order_acquire)) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));

                    const MicSilenceTracker::Event silenceEvent = silenceTracker.poll(
                        qState.digitalSilenceAlert.load(std::memory_order_acquire),
                        qState.sawNonZeroAudio.load(std::memory_order_acquire));
                    if (silenceEvent == MicSilenceTracker::Event::EnteredSilence) {
                        setStatusUnlessStopped(MicCaptureStatus::DigitalSilenceDetected);
                        if (!s_warnedDigitalSilence.exchange(true)) {
                            geode::log::warn("Instant Replay: microphone captured >3s of pure digital silence; "
                                             "capture keeps running and recovers when audio arrives");
                        }
                    } else if (silenceEvent == MicSilenceTracker::Event::Recovered) {
                        setStatusUnlessStopped(usingFallback ? MicCaptureStatus::UsingFallbackDevice : MicCaptureStatus::Active);
                        geode::log::info("Instant Replay: microphone audio resumed after digital silence");
                    }

                    if (ReplayManager::get()->getSessionConfig().getMicDeviceId() != targetId) {
                        break;
                    }
                }
            } else {
                geode::log::warn("Instant Replay: AudioQueueStart failed (OSStatus {})", static_cast<int>(startErr));
            }

            qState.stopping.store(true, std::memory_order_release);
            AudioQueueStop(queue, true);
            AudioQueueDispose(queue, true);
        } else {
            geode::log::warn("Instant Replay: AudioQueueNewInput failed (OSStatus {})", static_cast<int>(err));
        }

        [nc removeObserver:interruptionObs];
        [nc removeObserver:routeObs];
        [nc removeObserver:resetObs];

        if (m_micRunning.load()) {
            m_micResampler.reset();
            // While another app or a call holds the microphone, wait for the interruption to end.
            int waited = 0;
            while (m_micRunning.load() && qState.interrupted.load(std::memory_order_acquire) && waited < 30000) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                waited += 100;
            }
            sleepInterruptible(started ? 300 : 1000);
        }
    }

    restoreSession(sessionBackup);
    setStatusUnlessStopped(MicCaptureStatus::Idle);
}

} // namespace replay

#endif // GEODE_IS_IOS
