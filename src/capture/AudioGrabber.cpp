#include "AudioGrabber.hpp"

#if defined(GEODE_IS_WINDOWS) || defined(GEODE_IS_MACOS)

#include "../config/ReplayConfig.hpp"
#include "../recorder/ReplayManager.hpp"
#include <Geode/binding/FMODAudioEngine.hpp>
#include <Geode/binding/FMODSound.hpp>
#include <Geode/utils/string.hpp>
#include <algorithm>
#include <cstdio>
#include <cstring>

#ifdef GEODE_IS_WINDOWS
#include <windows.h>
#include <avrt.h>
#include <timeapi.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <propsys.h>

#pragma comment(lib, "winmm.lib")
#endif

namespace replay {

std::string AudioGrabber::sanitizeDeviceNameUtf8(std::string_view rawName) {
    std::string ascii;
    ascii.reserve(rawName.size());
    for (size_t i = 0; i < rawName.size(); ++i) {
        unsigned char c = static_cast<unsigned char>(rawName[i]);
        if (c >= 32 && c < 127) {
            ascii += static_cast<char>(c);
        } else if (c == 0xC2 && i + 1 < rawName.size()) {
            unsigned char c2 = static_cast<unsigned char>(rawName[i + 1]);
            if (c2 == 0xAB || c2 == 0xBB) { // « or » in UTF-8
                ascii += '"';
                ++i;
            }
        }
    }

    std::string clean;
    bool inSpace = false;
    for (char c : ascii) {
        if (c == ' ') {
            if (!clean.empty() && !inSpace) clean += ' ';
            inSpace = true;
        } else {
            clean += c;
            inSpace = false;
        }
    }
    while (!clean.empty() && clean.back() == ' ') clean.pop_back();

    if (clean.empty()) return "Microphone Device";
    if (clean.length() > 24) {
        return clean.substr(0, 21) + "...";
    }
    return clean;
}

#ifdef GEODE_IS_WINDOWS
static const PROPERTYKEY kPKEY_Device_FriendlyName = {
    { 0xa45c254e, 0xdf1c, 0x4efd, { 0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0 } }, 14
};

static std::string sanitizeDeviceName(const std::wstring& wname) {
    std::wstring converted = wname;
    auto replaceAll = [](std::wstring& str, const std::wstring& from, const std::wstring& to) {
        size_t startPos = 0;
        while ((startPos = str.find(from, startPos)) != std::wstring::npos) {
            str.replace(startPos, from.length(), to);
            startPos += to.length();
        }
    };
    replaceAll(converted, L"Микрофон", L"Mic");
    replaceAll(converted, L"микрофон", L"Mic");
    replaceAll(converted, L"Связь", L"Comm");
    replaceAll(converted, L"связь", L"Comm");
    replaceAll(converted, L"Наушники", L"Headset");
    replaceAll(converted, L"наушники", L"Headset");
    replaceAll(converted, L"Динамики", L"Speakers");
    replaceAll(converted, L"динамики", L"Speakers");
    return AudioGrabber::sanitizeDeviceNameUtf8(geode::utils::string::wideToUtf8(converted));
}
#endif

static FMOD_RESULT F_CALL customDSPCallback(
    FMOD_DSP_STATE* dsp_state,
    float* inbuffer,
    float* outbuffer,
    unsigned int length,
    int inchannels,
    int* outchannels
) {
    if (outchannels) {
        *outchannels = inchannels;
    }
    if (inbuffer && outbuffer && length > 0 && inchannels > 0) {
        // Pass audio along so the player hears game audio normally
        if (outbuffer != inbuffer) {
            memcpy(outbuffer, inbuffer, static_cast<size_t>(length) * inchannels * sizeof(float));
        }

        int sampleRate = 48000;
        if (dsp_state && dsp_state->functions && dsp_state->functions->getsamplerate) {
            dsp_state->functions->getsamplerate(dsp_state, &sampleRate);
        }
        AudioGrabber::get()->pushGameAudio(inbuffer, length, inchannels, sampleRate);
    } else if (outbuffer && length > 0 && inchannels > 0) {
        memset(outbuffer, 0, static_cast<size_t>(length) * inchannels * sizeof(float));
    }
    return FMOD_OK;
}

AudioGrabber* AudioGrabber::get() {
    static AudioGrabber instance;
    return &instance;
}

AudioGrabber::AudioGrabber() = default;

AudioGrabber::~AudioGrabber() {
    stopMicCapture();
}

void AudioGrabber::initialize() {
    if (m_initialized) return;

    m_initialized = true;
    m_nextDspRetryTime = {};
    setupFmodDSP();
}

void AudioGrabber::cleanup() {
    if (!m_initialized) return;

    m_initialized = false;
    cleanupSaveBypass();
    removeFmodDSP();
    stopMicCapture();
}

void AudioGrabber::cleanupSaveBypass() {
    if (m_saveBypassGroup) {
        FMOD::DSP* bypassHead = nullptr;
        if (m_saveBypassGroup->getDSP(FMOD_CHANNELCONTROL_DSP_HEAD, &bypassHead) == FMOD_OK && bypassHead) {
            bypassHead->disconnectAll(false, true);
        }
        m_saveBypassGroup->release();
        m_saveBypassGroup = nullptr;
    }
    if (m_cachedSaveSound) {
        m_cachedSaveSound->release();
        m_cachedSaveSound = nullptr;
    }
}

bool AudioGrabber::playSaveSoundBypass(const char* soundPath) {
    if (!soundPath || !soundPath[0]) return false;
    auto engine = FMODAudioEngine::sharedEngine();
    if (!engine || !engine->m_system) return false;

    const float sfxVol = std::clamp(engine->m_sfxVolume, 0.0f, 1.0f);
    if (sfxVol <= 0.001f) {
        return true;
    }

    FMOD::ChannelGroup* masterGroup = nullptr;
    if (engine->m_system->getMasterChannelGroup(&masterGroup) != FMOD_OK || !masterGroup) {
        return false;
    }
    FMOD::DSP* masterHead = nullptr;
    if (masterGroup->getDSP(FMOD_CHANNELCONTROL_DSP_HEAD, &masterHead) != FMOD_OK || !masterHead) {
        return false;
    }
    if (m_fmodDSP && masterHead == m_fmodDSP) {
        return false;
    }

    if (!m_saveBypassGroup) {
        if (engine->m_system->createChannelGroup("IR_SaveSoundBypass", &m_saveBypassGroup) != FMOD_OK || !m_saveBypassGroup) {
            m_saveBypassGroup = nullptr;
            return false;
        }
    }

    FMOD::DSP* bypassHead = nullptr;
    if (m_saveBypassGroup->getDSP(FMOD_CHANNELCONTROL_DSP_HEAD, &bypassHead) != FMOD_OK || !bypassHead) {
        return false;
    }

    bool alreadyWired = false;
    int numOut = 0;
    if (bypassHead->getNumOutputs(&numOut) == FMOD_OK && numOut == 1) {
        FMOD::DSP* outDsp = nullptr;
        if (bypassHead->getOutput(0, &outDsp, nullptr) == FMOD_OK && outDsp == masterHead) {
            alreadyWired = true;
        }
    }
    if (!alreadyWired) {
        bypassHead->disconnectAll(false, true);
        if (masterHead->addInput(bypassHead, nullptr, FMOD_DSPCONNECTION_TYPE_STANDARD) != FMOD_OK) {
            return false;
        }
    }

    FMOD::Sound* sound = nullptr;
    if (FMODSound* fs = engine->preloadEffect(soundPath)) {
        sound = fs->m_sound;
    }
    if (!sound) {
        if (!m_cachedSaveSound) {
            std::string fullPath = cocos2d::CCFileUtils::sharedFileUtils()->fullPathForFilename(soundPath, false);
            if (!fullPath.empty()) {
                if (engine->m_system->createSound(fullPath.c_str(), FMOD_DEFAULT, nullptr, &m_cachedSaveSound) != FMOD_OK) {
                    m_cachedSaveSound = nullptr;
                }
            }
        }
        sound = m_cachedSaveSound;
    }
    if (!sound) {
        return false;
    }

    FMOD::Channel* channel = nullptr;
    if (engine->m_system->playSound(sound, m_saveBypassGroup, true, &channel) != FMOD_OK || !channel) {
        return false;
    }
    channel->setVolume(sfxVol);
    channel->setPaused(false);
    return true;
}

void AudioGrabber::setMicAudioCallback(AudioCallback cb) {
    m_micCallback = std::move(cb);
}

void AudioGrabber::ensureDSPAttached() {
    if (!m_initialized || m_fmodDSP) return;
    auto now = std::chrono::steady_clock::now();
    if (now < m_nextDspRetryTime) return;
    setupFmodDSP();
}

void AudioGrabber::setupFmodDSP() {
    if (!m_initialized || m_fmodDSP) return;

    auto engine = FMODAudioEngine::sharedEngine();
    if (!engine || !engine->m_system) return;

    int sampleRate = 48000;
    FMOD_SPEAKERMODE speakerMode;
    int numRawSpeakers = 0;
    if (engine->m_system->getSoftwareFormat(&sampleRate, &speakerMode, &numRawSpeakers) == FMOD_OK && sampleRate > 0) {
        m_gameSampleRate.store(sampleRate, std::memory_order_relaxed);
    }

    FMOD::ChannelGroup* masterGroup = nullptr;
    if (engine->m_system->getMasterChannelGroup(&masterGroup) != FMOD_OK || !masterGroup) {
        m_nextDspRetryTime = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        return;
    }

    FMOD_DSP_DESCRIPTION dspDesc = {};
    dspDesc.pluginsdkversion = FMOD_PLUGIN_SDK_VERSION;
    snprintf(dspDesc.name, sizeof(dspDesc.name), "%s", "GDInstantReplayDSP");
    dspDesc.version = 0x00010000;
    dspDesc.numinputbuffers = 1;
    dspDesc.numoutputbuffers = 1;
    dspDesc.read = customDSPCallback;

    FMOD_RESULT res = engine->m_system->createDSP(&dspDesc, &m_fmodDSP);
    if (res == FMOD_OK && m_fmodDSP) {
        FMOD_RESULT addRes = masterGroup->addDSP(FMOD_CHANNELCONTROL_DSP_TAIL, m_fmodDSP);
        if (addRes == FMOD_OK) {
            m_fmodDSP->setActive(true);
            m_fmodDSP->setBypass(false);
            geode::log::info("Instant Replay: attached FMOD DSP to Master Channel Group tail! (Sample Rate: {} Hz)",
                             m_gameSampleRate.load(std::memory_order_relaxed));
        } else {
            geode::log::warn("Instant Replay: masterGroup->addDSP failed with {}", static_cast<int>(addRes));
            m_fmodDSP->release();
            m_fmodDSP = nullptr;
            m_nextDspRetryTime = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        }
    } else {
        m_fmodDSP = nullptr;
        geode::log::warn("Instant Replay: createDSP failed with {}", static_cast<int>(res));
        m_nextDspRetryTime = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    }
}

void AudioGrabber::removeFmodDSP() {
    if (m_fmodDSP) {
        auto engine = FMODAudioEngine::sharedEngine();
        if (engine && engine->m_system) {
            FMOD::ChannelGroup* masterGroup = nullptr;
            if (engine->m_system->getMasterChannelGroup(&masterGroup) == FMOD_OK && masterGroup) {
                masterGroup->removeDSP(m_fmodDSP);
            }
        }
        m_fmodDSP->release();
        m_fmodDSP = nullptr;
    }
}

void AudioGrabber::resetGameAudioBuffer() noexcept {
    m_gameRing.reset();
}

bool AudioGrabber::hasPendingGameAudio() const noexcept {
    return m_gameRing.hasPending();
}

size_t AudioGrabber::pendingGameAudioFrames() const noexcept {
    return m_gameRing.pendingFrames();
}

void AudioGrabber::pushGameAudio(const float* inbuffer, unsigned int length, int channels, int sampleRate) {
    if (!inbuffer || length == 0 || channels <= 0) return;

    setGameSampleRate(sampleRate);

    if (!ReplayManager::get()->isReplayActive()) return;
    const auto& config = ReplayManager::get()->getSessionConfig();

    float vol = config.systemVolume.load();
    int64_t nowNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    m_gameRing.push(inbuffer, length, channels, vol, sampleRate, nowNs);
}

size_t AudioGrabber::drainGameAudio(
    std::vector<float>& outStereoSamples,
    int& outSampleRate,
    std::chrono::steady_clock::time_point& outTimestamp
) {
    return m_gameRing.drain(outStereoSamples, outSampleRate, outTimestamp);
}

void AudioGrabber::pushMicAudio(
    const float* inbuffer,
    unsigned int length,
    int channels,
    int sampleRate,
    std::chrono::steady_clock::time_point captureTime
) {
    if (!m_micCallback || length == 0 || channels <= 0 || !inbuffer) return;

    if (!ReplayManager::get()->isReplayActive()) return;
    const auto& config = ReplayManager::get()->getSessionConfig();
    if (!config.recordMic.load()) return;

    float vol = config.micVolume.load();

    // 1. Channel conversion to stereo (2 channels) using reusable scratch
    m_micStereoScratch.resize(static_cast<size_t>(length) * 2);
    for (unsigned int i = 0; i < length; ++i) {
        GameAudioRing::downmixFrameToStereo(
            &inbuffer[static_cast<size_t>(i) * channels],
            channels,
            vol,
            m_micStereoScratch[static_cast<size_t>(i) * 2],
            m_micStereoScratch[static_cast<size_t>(i) * 2 + 1]
        );
    }

    if (sampleRate <= 0) sampleRate = kMicSampleRate;

    auto chunk = std::make_shared<AudioChunk>();
    chunk->timestamp = captureTime;

    // 2. Stateful linear resampler to 48000 Hz (preserves fractional phase across packet boundaries)
    m_micResampler.process(m_micStereoScratch.data(), length, sampleRate, kMicSampleRate, chunk->samples);
    if (chunk->samples.empty()) return;

    m_micCallback(std::move(chunk));
}

#ifdef GEODE_IS_WINDOWS
std::vector<AudioDeviceInfo> AudioGrabber::getAvailableMicrophones() {
    std::vector<AudioDeviceInfo> list;
    list.push_back({"default", "Default System Device"});

    HRESULT hrCo = CoInitializeEx(NULL, COINIT_MULTITHREADED);

    // Scope all ComRef instances so Release() runs BEFORE CoUninitialize() (#20).
    {
        ComRef<IMMDeviceEnumerator> enumerator;
        HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), NULL, CLSCTX_ALL,
                                      __uuidof(IMMDeviceEnumerator), (void**)enumerator.put());
        if (SUCCEEDED(hr) && enumerator) {
            ComRef<IMMDeviceCollection> collection;
            hr = enumerator->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, collection.put());
            if (SUCCEEDED(hr) && collection) {
                UINT count = 0;
                collection->GetCount(&count);
                for (UINT i = 0; i < count; ++i) {
                    ComRef<IMMDevice> device;
                    if (SUCCEEDED(collection->Item(i, device.put())) && device) {
                        LPWSTR devId = nullptr;
                        device->GetId(&devId);

                        std::string idStr;
                        if (devId) {
                            idStr = geode::utils::string::wideToUtf8(devId);
                            CoTaskMemFree(devId);
                        }

                        std::string friendlyName;
                        ComRef<IPropertyStore> props;
                        if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, props.put())) && props) {
                            PROPVARIANT pv;
                            PropVariantInit(&pv);
                            if (SUCCEEDED(props->GetValue(kPKEY_Device_FriendlyName, &pv)) && pv.vt == VT_LPWSTR && pv.pwszVal) {
                                friendlyName = sanitizeDeviceName(pv.pwszVal);
                            }
                            PropVariantClear(&pv);
                        }

                        if (!idStr.empty()) {
                            if (friendlyName.empty()) {
                                friendlyName = "Microphone " + std::to_string(i + 1);
                            }
                            list.push_back({idStr, friendlyName});
                        }
                    }
                }
            }
        }
    }

    if (SUCCEEDED(hrCo)) {
        CoUninitialize();
    }
    return list;
}
#endif // GEODE_IS_WINDOWS

void AudioGrabber::startMicCapture() {
    if (m_micRunning.load()) return;

    m_micRunning.store(true);
    m_micThread = std::thread(&AudioGrabber::micCaptureThreadFunc, this);
}

void AudioGrabber::stopMicCapture() {
    m_micRunning.store(false);
    if (m_micThread.joinable()) {
        m_micThread.join();
    }
}

#ifdef GEODE_IS_WINDOWS
namespace {

// Endpoint notifications arrive on an MMDevice thread: only raise a flag there, the capture
// thread re-evaluates the device and does all COM teardown itself (W6).
class MicEndpointNotifier final : public IMMNotificationClient {
public:
    explicit MicEndpointNotifier(std::atomic<bool>& changed) noexcept : m_changed(changed) {}

    ULONG STDMETHODCALLTYPE AddRef() override { return static_cast<ULONG>(InterlockedIncrement(&m_refs)); }
    ULONG STDMETHODCALLTYPE Release() override {
        const LONG refs = InterlockedDecrement(&m_refs);
        if (refs == 0) delete this;
        return static_cast<ULONG>(refs);
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IMMNotificationClient)) {
            *ppv = static_cast<IMMNotificationClient*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }

    HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR, DWORD) override { signal(); return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR) override { signal(); return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR) override { signal(); return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow flow, ERole, LPCWSTR) override {
        if (flow == eCapture || flow == eAll) signal();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) override { return S_OK; }

private:
    ~MicEndpointNotifier() = default;
    void signal() noexcept { m_changed.store(true, std::memory_order_release); }

    std::atomic<bool>& m_changed;
    LONG m_refs = 1;
};

std::wstring endpointId(IMMDevice* device) {
    std::wstring id;
    LPWSTR raw = nullptr;
    if (device && SUCCEEDED(device->GetId(&raw)) && raw) {
        id = raw;
        CoTaskMemFree(raw);
    }
    return id;
}

std::string endpointName(IMMDevice* device) {
    std::string name;
    ComRef<IPropertyStore> props;
    if (device && SUCCEEDED(device->OpenPropertyStore(STGM_READ, props.put())) && props) {
        PROPVARIANT pv;
        PropVariantInit(&pv);
        if (SUCCEEDED(props->GetValue(kPKEY_Device_FriendlyName, &pv)) && pv.vt == VT_LPWSTR && pv.pwszVal) {
            name = geode::utils::string::wideToUtf8(pv.pwszVal);
        }
        PropVariantClear(&pv);
    }
    return name.empty() ? std::string("unknown device") : name;
}

// The device the user wants right now: the selected endpoint while it is active, otherwise the
// system default capture device (eConsole first so Bluetooth headsets stay out of HFP mode).
ComRef<IMMDevice> resolveWantedMic(IMMDeviceEnumerator* enumerator, const std::wstring& selectedId, bool& outIsFallback) {
    outIsFallback = false;
    ComRef<IMMDevice> device;
    if (!selectedId.empty()) {
        if (SUCCEEDED(enumerator->GetDevice(selectedId.c_str(), device.put())) && device) {
            DWORD state = 0;
            if (FAILED(device->GetState(&state)) || state != DEVICE_STATE_ACTIVE) {
                device.reset();
            }
        } else {
            device.reset();
        }
        if (device) return device;
        outIsFallback = true;
    }
    if (FAILED(enumerator->GetDefaultAudioEndpoint(eCapture, eConsole, device.put())) || !device) {
        device.reset();
        if (FAILED(enumerator->GetDefaultAudioEndpoint(eCapture, eCommunications, device.put())) || !device) {
            device.reset();
        }
    }
    return device;
}

bool isAllZeroBytes(const BYTE* data, size_t bytes) noexcept {
    if (!data) return true;
    for (size_t i = 0; i < bytes; ++i) {
        if (data[i] != 0) return false;
    }
    return true;
}

} // namespace

// Logs one line per microphone state transition (W6).
static void updateWinMicStatus(std::atomic<MicCaptureStatus>& status, MicCaptureStatus next, const std::string& deviceName) {
    const MicCaptureStatus prev = status.exchange(next, std::memory_order_acq_rel);
    if (prev == next) return;
    switch (next) {
        case MicCaptureStatus::Active:
            geode::log::info("Instant Replay: microphone capture active on '{}'", deviceName);
            break;
        case MicCaptureStatus::UsingFallbackDevice:
            geode::log::warn("Instant Replay: selected microphone unavailable, using default input '{}'", deviceName);
            break;
        case MicCaptureStatus::NoInputDevice:
            geode::log::warn("Instant Replay: no microphone input device available");
            break;
        case MicCaptureStatus::DigitalSilenceDetected:
            geode::log::warn(
                "Instant Replay: microphone '{}' delivers digital silence (check Settings > Privacy > Microphone access for desktop apps)",
                deviceName);
            break;
        default:
            break;
    }
}

void AudioGrabber::micCaptureThreadFunc() {
    HRESULT coHr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    DWORD mmcssTaskIdx = 0;
    HANDLE hMmcss = AvSetMmThreadCharacteristicsW(L"Audio", &mmcssTaskIdx);
    m_micResampler.reset();

    LARGE_INTEGER qpcFreq{};
    QueryPerformanceFrequency(&qpcFreq);

    // Follow device arrival/removal and default-device changes, so a reconnected selected mic is
    // picked up again and "Default System Device" follows Windows' default (W6).
    std::atomic<bool> deviceChanged{false};
    MicEndpointNotifier* notifier = nullptr;
    ComRef<IMMDeviceEnumerator> notifyEnumerator;
    if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), NULL, CLSCTX_ALL,
                                   __uuidof(IMMDeviceEnumerator), (void**)notifyEnumerator.put())) && notifyEnumerator) {
        notifier = new MicEndpointNotifier(deviceChanged);
        if (FAILED(notifyEnumerator->RegisterEndpointNotificationCallback(notifier))) {
            notifier->Release();
            notifier = nullptr;
        }
    }

    auto sleepInterruptible = [this, &deviceChanged](int totalMs) {
        int elapsed = 0;
        while (elapsed < totalMs && m_micRunning.load() && !deviceChanged.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            elapsed += 50;
        }
    };

    const std::string selectedIdUtf8 = ReplayManager::get()->getSessionConfig().getMicDeviceId();
    const std::wstring selectedId = (!selectedIdUtf8.empty() && selectedIdUtf8 != "default")
        ? geode::utils::string::utf8ToWide(selectedIdUtf8)
        : std::wstring();
    std::wstring lastDeviceId;

    while (m_micRunning.load()) {
        deviceChanged.store(false, std::memory_order_release);
        ComRef<IMMDeviceEnumerator> enumerator;
        HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), NULL, CLSCTX_ALL,
                                      __uuidof(IMMDeviceEnumerator), (void**)enumerator.put());
        if (FAILED(hr) || !enumerator) {
            sleepInterruptible(1000);
            continue;
        }

        bool usingFallback = false;
        ComRef<IMMDevice> device = resolveWantedMic(enumerator.get(), selectedId, usingFallback);
        if (!device) {
            updateWinMicStatus(m_micStatus, MicCaptureStatus::NoInputDevice, {});
            lastDeviceId.clear();
            sleepInterruptible(1000);
            continue;
        }
        const std::wstring deviceId = endpointId(device.get());
        const std::string deviceName = endpointName(device.get());

        ComRef<IAudioClient> audioClient;
        hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, NULL, (void**)audioClient.put());
        if (FAILED(hr) || !audioClient) {
            sleepInterruptible(1000);
            continue;
        }

        WAVEFORMATEX* pwfx = nullptr;
        hr = audioClient->GetMixFormat(&pwfx);
        if (FAILED(hr) || !pwfx) {
            sleepInterruptible(1000);
            continue;
        }

        REFERENCE_TIME hnsDefaultPeriod = 0, hnsMinPeriod = 0;
        audioClient->GetDevicePeriod(&hnsDefaultPeriod, &hnsMinPeriod);
        // Reserve >= 100 ms shared buffer so scheduler jitter never drops mic packets while waking per period
        REFERENCE_TIME hnsBuffer = std::max<REFERENCE_TIME>(1000000LL, (hnsDefaultPeriod > 0) ? hnsDefaultPeriod * 10 : 1000000LL);

        HANDLE hWakeEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
        bool eventDriven = false;
        hr = audioClient->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, hnsBuffer, 0, pwfx, NULL);
        if (SUCCEEDED(hr) && hWakeEvent && SUCCEEDED(audioClient->SetEventHandle(hWakeEvent))) {
            eventDriven = true;
        } else {
            audioClient.reset();
            if (SUCCEEDED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, NULL, (void**)audioClient.put()))) {
                hr = audioClient->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, hnsBuffer, 0, pwfx, NULL);
            }
        }

        bool deviceLost = false;
        bool switchDevice = false;
        if (SUCCEEDED(hr) && audioClient) {
            ComRef<IAudioCaptureClient> captureClient;
            hr = audioClient->GetService(__uuidof(IAudioCaptureClient), (void**)captureClient.put());
            if (SUCCEEDED(hr) && captureClient) {
                audioClient->Start();

                const MicCaptureStatus liveStatus = usingFallback ? MicCaptureStatus::UsingFallbackDevice : MicCaptureStatus::Active;
                if (deviceId != lastDeviceId && !lastDeviceId.empty() &&
                    m_micStatus.load(std::memory_order_acquire) == liveStatus) {
                    geode::log::info("Instant Replay: microphone capture switched to '{}'", deviceName);
                }
                updateWinMicStatus(m_micStatus, liveStatus, deviceName);
                lastDeviceId = deviceId;

                bool isFloat = false;
                if (pwfx->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
                    isFloat = true;
                } else if (pwfx->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
                    auto ex = reinterpret_cast<WAVEFORMATEXTENSIBLE*>(pwfx);
                    if (IsEqualGUID(ex->SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT)) {
                        isFloat = true;
                    }
                }

                if (!eventDriven) {
                    timeBeginPeriod(1);
                }

                // Digital silence detector: ~3 s of AUDCLNT_BUFFERFLAGS_SILENT or exact zeros usually means
                // Windows privacy settings block microphone access for desktop apps (W6).
                const int64_t silenceThresholdFrames = static_cast<int64_t>(std::max<DWORD>(1, pwfx->nSamplesPerSec)) * 3;
                int64_t silentFrames = 0;

                // The first packet after IAudioClient::Start always carries DATA_DISCONTINUITY (W8).
                bool firstPacketAfterStart = true;
                uint64_t discontinuitiesSinceReport = 0;
                std::chrono::steady_clock::time_point lastDiscontinuityReport{};

                while (m_micRunning.load()) {
                    if (eventDriven) {
                        WaitForSingleObject(hWakeEvent, 20);
                    } else {
                        std::this_thread::sleep_for(std::chrono::milliseconds(4));
                    }

                    if (deviceChanged.exchange(false, std::memory_order_acq_rel)) {
                        bool wantedFallback = false;
                        ComRef<IMMDevice> wanted = resolveWantedMic(enumerator.get(), selectedId, wantedFallback);
                        if (wanted && endpointId(wanted.get()) != deviceId) {
                            switchDevice = true;
                            break;
                        }
                    }

                    UINT32 packetLength = 0;
                    hr = captureClient->GetNextPacketSize(&packetLength);
                    if (FAILED(hr)) {
                        deviceLost = true;
                        break;
                    }

                    while (SUCCEEDED(hr) && packetLength > 0 && m_micRunning.load()) {
                        BYTE* pData = nullptr;
                        UINT32 numFrames = 0;
                        DWORD flags = 0;
                        UINT64 qpcPositionHns = 0;
                        hr = captureClient->GetBuffer(&pData, &numFrames, &flags, NULL, &qpcPositionHns);
                        if (FAILED(hr)) {
                            deviceLost = true;
                            break;
                        }
                        if (flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) {
                            if (firstPacketAfterStart) {
                                geode::log::debug("AudioGrabber: WASAPI mic stream start discontinuity (numFrames={})", numFrames);
                            } else {
                                ++discontinuitiesSinceReport;
                                const auto reportNow = std::chrono::steady_clock::now();
                                if (lastDiscontinuityReport == std::chrono::steady_clock::time_point{} ||
                                    reportNow - lastDiscontinuityReport >= std::chrono::seconds(10)) {
                                    geode::log::warn(
                                        "AudioGrabber: WASAPI mic capture data discontinuity (numFrames={}, {} since last report)",
                                        numFrames, discontinuitiesSinceReport);
                                    discontinuitiesSinceReport = 0;
                                    lastDiscontinuityReport = reportNow;
                                }
                            }
                        }
                        firstPacketAfterStart = false;
                        if (numFrames > 0) {
                            int channels = pwfx->nChannels;
                            int sampleRate = pwfx->nSamplesPerSec;
                            size_t totalSamples = static_cast<size_t>(numFrames) * channels;

                            const bool silentPacket = (flags & AUDCLNT_BUFFERFLAGS_SILENT) ||
                                isAllZeroBytes(pData, static_cast<size_t>(numFrames) * pwfx->nBlockAlign);
                            if (silentPacket) {
                                silentFrames += numFrames;
                                if (silentFrames >= silenceThresholdFrames) {
                                    updateWinMicStatus(m_micStatus, MicCaptureStatus::DigitalSilenceDetected, deviceName);
                                }
                            } else {
                                silentFrames = 0;
                                if (m_micStatus.load(std::memory_order_acquire) == MicCaptureStatus::DigitalSilenceDetected) {
                                    updateWinMicStatus(m_micStatus, liveStatus, deviceName);
                                }
                            }

                            // Compute packet start timestamp using overflow-safe 64-bit QPC arithmetic (#6)
                            auto now = std::chrono::steady_clock::now();
                            int64_t packetDurationNs = (sampleRate > 0)
                                ? (static_cast<int64_t>(numFrames) * 1000000000LL) / sampleRate
                                : 0;
                            auto captureTime = now - std::chrono::nanoseconds(packetDurationNs);
                            if (qpcPositionHns > 0 && qpcFreq.QuadPart > 0) {
                                LARGE_INTEGER qpcNow{};
                                QueryPerformanceCounter(&qpcNow);
                                uint64_t ticks = static_cast<uint64_t>(qpcNow.QuadPart);
                                uint64_t freq = static_cast<uint64_t>(qpcFreq.QuadPart);
                                uint64_t nowHns = (ticks / freq) * 10000000ULL +
                                                  ((ticks % freq) * 10000000ULL) / freq;
                                if (nowHns >= qpcPositionHns) {
                                    uint64_t ageHns = nowHns - qpcPositionHns;
                                    if (ageHns < 5000000ULL) { // < 500ms
                                        captureTime = now - std::chrono::nanoseconds(static_cast<int64_t>(ageHns) * 100LL);
                                    }
                                }
                            }

                            if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
                                m_micConvertScratch.assign(totalSamples, 0.0f);
                                pushMicAudio(m_micConvertScratch.data(), numFrames, channels, sampleRate, captureTime);
                            } else if (pData) {
                                if (isFloat) {
                                    pushMicAudio(reinterpret_cast<const float*>(pData), numFrames, channels, sampleRate, captureTime);
                                } else {
                                    int bits = pwfx->wBitsPerSample;
                                    m_micConvertScratch.resize(totalSamples);
                                    if (bits == 16) {
                                        const int16_t* src = reinterpret_cast<const int16_t*>(pData);
                                        for (size_t i = 0; i < totalSamples; ++i) {
                                            m_micConvertScratch[i] = src[i] / 32768.0f;
                                        }
                                        pushMicAudio(m_micConvertScratch.data(), numFrames, channels, sampleRate, captureTime);
                                    } else if (bits == 24) {
                                        const uint8_t* src = reinterpret_cast<const uint8_t*>(pData);
                                        for (size_t i = 0; i < totalSamples; ++i) {
                                            int32_t val = (src[i * 3 + 0]) | (src[i * 3 + 1] << 8) | (src[i * 3 + 2] << 16);
                                            if (val & 0x800000) val |= 0xFF000000;
                                            m_micConvertScratch[i] = static_cast<float>(val) / 8388608.0f;
                                        }
                                        pushMicAudio(m_micConvertScratch.data(), numFrames, channels, sampleRate, captureTime);
                                    } else if (bits == 32) {
                                        const int32_t* src = reinterpret_cast<const int32_t*>(pData);
                                        for (size_t i = 0; i < totalSamples; ++i) {
                                            m_micConvertScratch[i] = static_cast<float>(src[i]) / 2147483648.0f;
                                        }
                                        pushMicAudio(m_micConvertScratch.data(), numFrames, channels, sampleRate, captureTime);
                                    }
                                }
                            }
                        }
                        captureClient->ReleaseBuffer(numFrames);

                        hr = captureClient->GetNextPacketSize(&packetLength);
                        if (FAILED(hr)) {
                            deviceLost = true;
                            break;
                        }
                    }

                    if (deviceLost) break;
                }

                if (!eventDriven) {
                    timeEndPeriod(1);
                }
                audioClient->Stop();
            }
        }

        if (hWakeEvent) CloseHandle(hWakeEvent);
        if (pwfx) CoTaskMemFree(pwfx);

        if (deviceLost) {
            geode::log::info("Instant Replay: microphone '{}' was lost, re-selecting input device", deviceName);
        }
        if (m_micRunning.load()) {
            m_micResampler.reset();
            if (!switchDevice) {
                sleepInterruptible(deviceLost ? 500 : 1000);
            }
        }
    }

    if (notifier) {
        if (notifyEnumerator) {
            notifyEnumerator->UnregisterEndpointNotificationCallback(notifier);
        }
        notifier->Release();
        notifier = nullptr;
    }
    notifyEnumerator.reset();
    m_micStatus.store(MicCaptureStatus::Idle, std::memory_order_release);

    if (hMmcss) {
        AvRevertMmThreadCharacteristics(hMmcss);
        hMmcss = nullptr;
    }
    if (SUCCEEDED(coHr)) {
        CoUninitialize();
    }
}

#endif // GEODE_IS_WINDOWS

} // namespace replay

#endif // GEODE_IS_WINDOWS || GEODE_IS_MACOS
