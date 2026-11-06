#include "AudioGrabber.hpp"

#ifdef GEODE_IS_WINDOWS

#include "../config/ReplayConfig.hpp"
#include "../recorder/ReplayManager.hpp"
#include <Geode/binding/FMODAudioEngine.hpp>
#include <Geode/utils/string.hpp>
#include <windows.h>
#include <timeapi.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <propsys.h>
#include <algorithm>

#pragma comment(lib, "winmm.lib")

namespace replay {

static const PROPERTYKEY kPKEY_Device_FriendlyName = {
    { 0xa45c254e, 0xdf1c, 0x4efd, { 0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0 } }, 14
};

// Proper multichannel-to-stereo downmix supporting 1.0, 2.0, 3.0 (L/R/C), 4.0 Quad (FL/FR/BL/BR),
// 5.0 (FL/FR/FC/BL/BR), 5.1 (FL/FR/FC/LFE/BL/BR), 6.1, and 7.1 (FL/FR/FC/LFE/BL/BR/SL/SR) (#22).
static inline void downmixFrameToStereo(const float* frame, int channels, float vol, float& outL, float& outR) noexcept {
    float l = 0.0f;
    float r = 0.0f;
    switch (channels) {
        case 1:
            l = r = frame[0];
            break;
        case 2:
            l = frame[0];
            r = frame[1];
            break;
        case 3: { // 3.0: FL, FR, FC
            float c = frame[2] * 0.7071f;
            l = frame[0] + c;
            r = frame[1] + c;
            break;
        }
        case 4: // 4.0 Quad: FL, FR, BL, BR
            l = frame[0] + frame[2] * 0.7071f;
            r = frame[1] + frame[3] * 0.7071f;
            break;
        case 5: { // 5.0: FL, FR, FC, BL, BR
            float c = frame[2] * 0.7071f;
            l = frame[0] + c + frame[3] * 0.7071f;
            r = frame[1] + c + frame[4] * 0.7071f;
            break;
        }
        case 6: { // 5.1: FL, FR, FC, LFE, BL, BR
            float centerLfe = frame[2] * 0.7071f + frame[3] * 0.5f;
            l = frame[0] + centerLfe + frame[4] * 0.7071f;
            r = frame[1] + centerLfe + frame[5] * 0.7071f;
            break;
        }
        case 7: { // 6.1: FL, FR, FC, LFE, BC, SL, SR
            float mid = frame[2] * 0.7071f + frame[3] * 0.5f + frame[4] * 0.5f;
            l = frame[0] + mid + frame[5] * 0.7071f;
            r = frame[1] + mid + frame[6] * 0.7071f;
            break;
        }
        default: { // >= 8 (7.1: FL, FR, FC, LFE, BL, BR, SL, SR)
            float centerLfe = frame[2] * 0.7071f + frame[3] * 0.5f;
            l = frame[0] + centerLfe + (frame[4] + frame[6]) * 0.7071f;
            r = frame[1] + centerLfe + (frame[5] + frame[7]) * 0.7071f;
            break;
        }
    }
    outL = std::clamp(l * vol, -1.0f, 1.0f);
    outR = std::clamp(r * vol, -1.0f, 1.0f);
}

static std::string sanitizeDeviceName(const std::wstring& wname) {
    std::wstring converted = wname;
    auto replaceAll = [](std::wstring& str, const std::wstring& from, const std::wstring& to) {
        size_t start_pos = 0;
        while ((start_pos = str.find(from, start_pos)) != std::wstring::npos) {
            str.replace(start_pos, from.length(), to);
            start_pos += to.length();
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

    std::string ascii;
    for (wchar_t wc : converted) {
        if (wc >= 32 && wc < 127) {
            ascii += static_cast<char>(wc);
        } else if (wc == L'«' || wc == L'»' || wc == L'"') {
            ascii += '"';
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
    while (!clean.empty() && clean.front() == ' ') clean.erase(clean.begin());
    while (!clean.empty() && clean.back() == ' ') clean.pop_back();

    if (clean.empty()) return "Microphone Device";
    if (clean.length() > 24) {
        return clean.substr(0, 21) + "...";
    }
    return clean;
}

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
            memcpy(outbuffer, inbuffer, length * inchannels * sizeof(float));
        }

        int sampleRate = 44100;
        if (dsp_state && dsp_state->functions && dsp_state->functions->getsamplerate) {
            dsp_state->functions->getsamplerate(dsp_state, &sampleRate);
        }
        if (sampleRate > 0) {
            AudioGrabber::get()->setGameSampleRate(sampleRate);
        }

        // Write into lock-free SPSC ring buffer (zero heap allocation, zero mutex lock)
        AudioGrabber::get()->pushGameAudio(inbuffer, length, inchannels, sampleRate);
    } else if (outbuffer && length > 0 && inchannels > 0) {
        memset(outbuffer, 0, length * inchannels * sizeof(float));
    }
    return FMOD_OK;
}

AudioGrabber* AudioGrabber::get() {
    static AudioGrabber instance;
    return &instance;
}

AudioGrabber::AudioGrabber()
    : m_gameRingBuffer(kGameRingFrames * 2, 0.0f) {
}

AudioGrabber::~AudioGrabber() {
    stopMicCapture();
}

void AudioGrabber::initialize() {
    if (m_initialized) return;

    m_initialized = true;
    m_nextDspRetryTime = {};
    setupFmodDSP();
    // Note: Microphone capture is started by ReplayManager::startReplay() only when
    // both replay is enabled AND recordMic is enabled.
}

void AudioGrabber::cleanup() {
    if (!m_initialized) return;

    m_initialized = false;
    removeFmodDSP();
    stopMicCapture();
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

    int sampleRate = 44100;
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
    strcpy_s(dspDesc.name, "GDInstantReplayDSP");
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
    size_t writePos = m_gameRingWriteFrame.load(std::memory_order_acquire);
    m_gameRingReadFrame.store(writePos, std::memory_order_release);
}

bool AudioGrabber::hasPendingGameAudio() const noexcept {
    size_t writePos = m_gameRingWriteFrame.load(std::memory_order_acquire);
    size_t readPos = m_gameRingReadFrame.load(std::memory_order_relaxed);
    return writePos > readPos;
}

void AudioGrabber::pushGameAudio(const float* inbuffer, unsigned int length, int channels, int sampleRate) {
    if (!inbuffer || length == 0 || channels <= 0) return;

    if (!ReplayManager::get()->isReplayActive()) return;
    const auto& config = ReplayManager::get()->getSessionConfig();

    if (sampleRate > 0) {
        m_gameSampleRate.store(sampleRate, std::memory_order_relaxed);
    }

    float vol = config.systemVolume.load();
    size_t writePos = m_gameRingWriteFrame.load(std::memory_order_relaxed);
    size_t readPos = m_gameRingReadFrame.load(std::memory_order_acquire);

    size_t usedFrames = (writePos >= readPos) ? (writePos - readPos) : 0;
    if (usedFrames >= kGameRingFrames) {
        return; // Ring buffer full; drop rather than block real-time FMOD mixer thread
    }
    size_t availFrames = kGameRingFrames - usedFrames;
    size_t framesToWrite = std::min<size_t>(length, availFrames);

    for (size_t i = 0; i < framesToWrite; ++i) {
        float l = 0.0f;
        float r = 0.0f;
        downmixFrameToStereo(&inbuffer[i * static_cast<size_t>(channels)], channels, vol, l, r);

        size_t slot = (writePos + i) & kGameRingMask;
        m_gameRingBuffer[slot * 2] = l;
        m_gameRingBuffer[slot * 2 + 1] = r;
    }

    auto nowNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    m_lastGamePushTimeNs.store(nowNs, std::memory_order_relaxed);
    m_gameRingWriteFrame.store(writePos + framesToWrite, std::memory_order_release);
}

size_t AudioGrabber::drainGameAudio(
    std::vector<float>& outStereoSamples,
    int& outSampleRate,
    std::chrono::steady_clock::time_point& outTimestamp
) {
    size_t writePos = m_gameRingWriteFrame.load(std::memory_order_acquire);
    size_t readPos = m_gameRingReadFrame.load(std::memory_order_relaxed);
    if (writePos <= readPos) {
        outStereoSamples.clear();
        return 0;
    }

    if (writePos - readPos > kGameRingFrames) {
        readPos = writePos - kGameRingFrames;
    }
    size_t frames = writePos - readPos;
    outStereoSamples.resize(frames * 2);

    for (size_t i = 0; i < frames; ++i) {
        size_t slot = (readPos + i) & kGameRingMask;
        outStereoSamples[i * 2] = m_gameRingBuffer[slot * 2];
        outStereoSamples[i * 2 + 1] = m_gameRingBuffer[slot * 2 + 1];
    }

    m_gameRingReadFrame.store(writePos, std::memory_order_release);

    int rate = m_gameSampleRate.load(std::memory_order_relaxed);
    if (rate <= 0) rate = 44100;
    outSampleRate = rate;

    int64_t endNs = m_lastGamePushTimeNs.load(std::memory_order_relaxed);
    int64_t durationNs = (static_cast<int64_t>(frames) * 1000000000LL) / rate;
    if (endNs > durationNs) {
        outTimestamp = std::chrono::steady_clock::time_point(std::chrono::nanoseconds(endNs - durationNs));
    } else {
        outTimestamp = std::chrono::steady_clock::now();
    }
    return frames;
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
        downmixFrameToStereo(
            &inbuffer[static_cast<size_t>(i) * channels],
            channels,
            vol,
            m_micStereoScratch[static_cast<size_t>(i) * 2],
            m_micStereoScratch[static_cast<size_t>(i) * 2 + 1]
        );
    }

    constexpr int TARGET_RATE = 48000;
    if (sampleRate <= 0) sampleRate = TARGET_RATE;

    auto chunk = std::make_shared<AudioChunk>();
    chunk->channels = 2;
    chunk->sampleRate = TARGET_RATE;
    chunk->timestamp = captureTime;

    // 2. Stateful linear resampler to 48000 Hz (preserves fractional phase across packet boundaries)
    m_micResampler.process(m_micStereoScratch.data(), length, sampleRate, TARGET_RATE, chunk->samples);
    if (chunk->samples.empty()) return;

    m_micCallback(std::move(chunk));
}

std::vector<AudioDeviceInfo> AudioGrabber::getAvailableMicrophones() {
    std::vector<AudioDeviceInfo> list;
    list.push_back({"default", "Default System Device"});

    HRESULT hrCo = CoInitializeEx(NULL, COINIT_MULTITHREADED);

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

    if (SUCCEEDED(hrCo)) {
        CoUninitialize();
    }
    return list;
}

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

void AudioGrabber::micCaptureThreadFunc() {
    HRESULT coHr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    m_micResampler.reset();

    LARGE_INTEGER qpcFreq{};
    QueryPerformanceFrequency(&qpcFreq);

    auto sleepInterruptible = [this](int totalMs) {
        int elapsed = 0;
        while (elapsed < totalMs && m_micRunning.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            elapsed += 50;
        }
    };

    while (m_micRunning.load()) {
        ComRef<IMMDeviceEnumerator> enumerator;
        HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), NULL, CLSCTX_ALL,
                                      __uuidof(IMMDeviceEnumerator), (void**)enumerator.put());
        if (FAILED(hr) || !enumerator) {
            sleepInterruptible(1000);
            continue;
        }

        ComRef<IMMDevice> device;
        std::string targetId = ReplayManager::get()->getSessionConfig().getMicDeviceId();

        if (!targetId.empty() && targetId != "default") {
            std::wstring wideId = geode::utils::string::utf8ToWide(targetId);
            if (!wideId.empty()) {
                if (SUCCEEDED(enumerator->GetDevice(wideId.c_str(), device.put())) && device) {
                    // GetDevice succeeds even for unplugged/disabled endpoints; verify DEVICE_STATE_ACTIVE (#13)
                    DWORD state = 0;
                    if (FAILED(device->GetState(&state)) || state != DEVICE_STATE_ACTIVE) {
                        device.reset();
                    }
                } else {
                    device.reset();
                }
            }
        }

        if (!device) {
            // Prefer eConsole first so Bluetooth headsets are not forced into low-quality HFP communication profile
            hr = enumerator->GetDefaultAudioEndpoint(eCapture, eConsole, device.put());
            if (FAILED(hr)) {
                hr = enumerator->GetDefaultAudioEndpoint(eCapture, eCommunications, device.put());
            }
        }

        if (FAILED(hr) || !device) {
            sleepInterruptible(1000);
            continue;
        }

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
        REFERENCE_TIME hnsBuffer = (hnsDefaultPeriod > 0) ? hnsDefaultPeriod * 2 : 200000;

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
        if (SUCCEEDED(hr) && audioClient) {
            ComRef<IAudioCaptureClient> captureClient;
            hr = audioClient->GetService(__uuidof(IAudioCaptureClient), (void**)captureClient.put());
            if (SUCCEEDED(hr) && captureClient) {
                audioClient->Start();

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

                while (m_micRunning.load()) {
                    if (eventDriven) {
                        WaitForSingleObject(hWakeEvent, 20);
                    } else {
                        std::this_thread::sleep_for(std::chrono::milliseconds(4));
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
                        if (numFrames > 0) {
                            int channels = pwfx->nChannels;
                            int sampleRate = pwfx->nSamplesPerSec;
                            size_t totalSamples = static_cast<size_t>(numFrames) * channels;

                            // Compute packet start timestamp from hardware QPC position or packet duration (#23)
                            auto now = std::chrono::steady_clock::now();
                            int64_t packetDurationNs = (sampleRate > 0)
                                ? (static_cast<int64_t>(numFrames) * 1000000000LL) / sampleRate
                                : 0;
                            auto captureTime = now - std::chrono::nanoseconds(packetDurationNs);
                            if (qpcPositionHns > 0 && qpcFreq.QuadPart > 0) {
                                LARGE_INTEGER qpcNow{};
                                QueryPerformanceCounter(&qpcNow);
                                uint64_t nowHns = static_cast<uint64_t>((qpcNow.QuadPart * 10000000LL) / qpcFreq.QuadPart);
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
                            captureClient->ReleaseBuffer(numFrames);
                        }
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

        if (m_micRunning.load()) {
            m_micResampler.reset();
            sleepInterruptible(deviceLost ? 500 : 1000);
        }
    }

    if (SUCCEEDED(coHr)) {
        CoUninitialize();
    }
}

} // namespace replay

#endif // GEODE_IS_WINDOWS
