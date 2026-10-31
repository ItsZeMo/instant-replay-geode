#include "AudioGrabber.hpp"

#ifdef GEODE_IS_WINDOWS

#include "../config/ReplayConfig.hpp"
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

    setupFmodDSP();
    // Note: Microphone capture is started by ReplayManager::startReplay() only when
    // both replay is enabled AND recordMic is enabled.

    m_initialized = true;
}

void AudioGrabber::cleanup() {
    if (!m_initialized) return;

    removeFmodDSP();
    stopMicCapture();

    m_initialized = false;
}

void AudioGrabber::setMicAudioCallback(AudioCallback cb) {
    m_micCallback = std::move(cb);
}

void AudioGrabber::ensureDSPAttached() {
    if (m_fmodDSP) return;
    setupFmodDSP();
}

void AudioGrabber::setupFmodDSP() {
    if (m_fmodDSP) return;

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
        }
    } else {
        geode::log::warn("Instant Replay: createDSP failed with {}", static_cast<int>(res));
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

    const auto& config = ReplayConfig::get();
    if (!config.enabled.load()) return;

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
        if (channels == 2) {
            l = inbuffer[i * 2] * vol;
            r = inbuffer[i * 2 + 1] * vol;
        } else if (channels == 1) {
            l = r = inbuffer[i] * vol;
        } else if (channels >= 4) {
            const float* frame = &inbuffer[i * channels];
            l = frame[0];
            r = frame[1];
            if (channels > 2) {
                float c = frame[2] * 0.7071f;
                l += c;
                r += c;
            }
            if (channels > 3) {
                float lfe = frame[3] * 0.5f;
                l += lfe;
                r += lfe;
            }
            if (channels > 5) {
                l += frame[4] * 0.7071f;
                r += frame[5] * 0.7071f;
            }
            l = std::clamp(l * vol, -1.0f, 1.0f);
            r = std::clamp(r * vol, -1.0f, 1.0f);
        } else {
            l = inbuffer[i * channels] * vol;
            r = inbuffer[i * channels + 1] * vol;
        }

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

void AudioGrabber::resetMicResampler() {
    m_micResamplePos = 0.0;
    m_micPrevSample[0] = 0.0f;
    m_micPrevSample[1] = 0.0f;
    m_micHasPrev = false;
    m_micLastInputRate = 0;
}

void AudioGrabber::pushMicAudio(const float* inbuffer, unsigned int length, int channels, int sampleRate) {
    if (!m_micCallback || length == 0 || channels <= 0 || !inbuffer) return;

    const auto& config = ReplayConfig::get();
    if (!config.enabled.load() || !config.recordMic.load()) return;

    float vol = config.micVolume.load();

    // 1. Channel conversion to stereo (2 channels) using reusable thread-local scratch
    m_micStereoScratch.resize(static_cast<size_t>(length) * 2);
    if (channels == 1) {
        for (unsigned int i = 0; i < length; ++i) {
            float s = std::clamp(inbuffer[i] * vol, -1.0f, 1.0f);
            m_micStereoScratch[i * 2] = s;
            m_micStereoScratch[i * 2 + 1] = s;
        }
    } else if (channels == 2) {
        for (unsigned int i = 0; i < length * 2; ++i) {
            m_micStereoScratch[i] = std::clamp(inbuffer[i] * vol, -1.0f, 1.0f);
        }
    } else {
        for (unsigned int i = 0; i < length; ++i) {
            const float* frame = &inbuffer[i * channels];
            float l = frame[0];
            float r = frame[1];
            if (channels > 2) {
                float c = frame[2] * 0.7071f;
                l += c;
                r += c;
            }
            m_micStereoScratch[i * 2] = std::clamp(l * vol, -1.0f, 1.0f);
            m_micStereoScratch[i * 2 + 1] = std::clamp(r * vol, -1.0f, 1.0f);
        }
    }

    auto chunk = std::make_shared<AudioChunk>();
    chunk->channels = 2;
    chunk->timestamp = std::chrono::steady_clock::now();

    // 2. Stateful linear resampler to 48000 Hz (preserves fractional phase across packet boundaries)
    constexpr int TARGET_RATE = 48000;
    if (sampleRate <= 0) sampleRate = TARGET_RATE;

    if (m_micLastInputRate != sampleRate) {
        resetMicResampler();
        m_micLastInputRate = sampleRate;
    }

    if (sampleRate == TARGET_RATE) {
        chunk->sampleRate = TARGET_RATE;
        chunk->samples.assign(m_micStereoScratch.begin(), m_micStereoScratch.end());
    } else {
        chunk->sampleRate = TARGET_RATE;
        double step = static_cast<double>(sampleRate) / static_cast<double>(TARGET_RATE);
        size_t estOutFrames = static_cast<size_t>(static_cast<double>(length) / step) + 2;
        chunk->samples.reserve(estOutFrames * 2);

        if (!m_micHasPrev) {
            m_micPrevSample[0] = m_micStereoScratch[0];
            m_micPrevSample[1] = m_micStereoScratch[1];
            m_micHasPrev = true;
            m_micResamplePos = 0.0;
        }

        // Position -1.0 corresponds to m_micPrevSample; positions [0, length - 1] are in m_micStereoScratch
        double limit = static_cast<double>(length) - 1.0;
        while (m_micResamplePos < limit) {
            float l0 = 0.0f, r0 = 0.0f, l1 = 0.0f, r1 = 0.0f;
            float frac = 0.0f;
            if (m_micResamplePos < 0.0) {
                frac = static_cast<float>(m_micResamplePos + 1.0);
                l0 = m_micPrevSample[0];
                r0 = m_micPrevSample[1];
                l1 = m_micStereoScratch[0];
                r1 = m_micStereoScratch[1];
            } else {
                size_t idx0 = static_cast<size_t>(m_micResamplePos);
                size_t idx1 = std::min<size_t>(idx0 + 1, length - 1);
                frac = static_cast<float>(m_micResamplePos - static_cast<double>(idx0));
                l0 = m_micStereoScratch[idx0 * 2];
                r0 = m_micStereoScratch[idx0 * 2 + 1];
                l1 = m_micStereoScratch[idx1 * 2];
                r1 = m_micStereoScratch[idx1 * 2 + 1];
            }
            chunk->samples.push_back(l0 + frac * (l1 - l0));
            chunk->samples.push_back(r0 + frac * (r1 - r0));
            m_micResamplePos += step;
        }

        m_micPrevSample[0] = m_micStereoScratch[(length - 1) * 2];
        m_micPrevSample[1] = m_micStereoScratch[(length - 1) * 2 + 1];
        m_micResamplePos -= static_cast<double>(length);

        if (chunk->samples.empty()) return;
    }

    m_micSampleRate.store(TARGET_RATE, std::memory_order_relaxed);
    m_micCallback(std::move(chunk));
}

std::vector<AudioDeviceInfo> AudioGrabber::getAvailableMicrophones() {
    std::vector<AudioDeviceInfo> list;
    list.push_back({"default", "Default System Device"});

    HRESULT hrCo = CoInitializeEx(NULL, COINIT_MULTITHREADED);

    IMMDeviceEnumerator* enumerator = nullptr;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), NULL, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&enumerator);
    if (SUCCEEDED(hr) && enumerator) {
        IMMDeviceCollection* collection = nullptr;
        hr = enumerator->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &collection);
        if (SUCCEEDED(hr) && collection) {
            UINT count = 0;
            collection->GetCount(&count);
            for (UINT i = 0; i < count; ++i) {
                IMMDevice* device = nullptr;
                if (SUCCEEDED(collection->Item(i, &device)) && device) {
                    LPWSTR devId = nullptr;
                    device->GetId(&devId);

                    std::string idStr;
                    if (devId) {
                        idStr = geode::utils::string::wideToUtf8(devId);
                        CoTaskMemFree(devId);
                    }

                    std::string friendlyName;
                    IPropertyStore* props = nullptr;
                    if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, &props)) && props) {
                        PROPVARIANT pv;
                        PropVariantInit(&pv);
                        if (SUCCEEDED(props->GetValue(kPKEY_Device_FriendlyName, &pv)) && pv.vt == VT_LPWSTR && pv.pwszVal) {
                            friendlyName = sanitizeDeviceName(pv.pwszVal);
                        }
                        PropVariantClear(&pv);
                        props->Release();
                    }

                    if (!idStr.empty()) {
                        if (friendlyName.empty()) {
                            friendlyName = "Microphone " + std::to_string(i + 1);
                        }
                        list.push_back({idStr, friendlyName});
                    }
                    device->Release();
                }
            }
            collection->Release();
        }
        enumerator->Release();
    }

    if (SUCCEEDED(hrCo)) {
        CoUninitialize();
    }
    return list;
}

void AudioGrabber::restartMicCapture() {
    stopMicCapture();
    if (ReplayConfig::get().enabled.load() && ReplayConfig::get().recordMic.load()) {
        startMicCapture();
    }
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
    resetMicResampler();

    auto sleepInterruptible = [this](int totalMs) {
        int elapsed = 0;
        while (elapsed < totalMs && m_micRunning.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            elapsed += 50;
        }
    };

    while (m_micRunning.load()) {
        IMMDeviceEnumerator* enumerator = nullptr;
        HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), NULL, CLSCTX_ALL,
                                      __uuidof(IMMDeviceEnumerator), (void**)&enumerator);
        if (FAILED(hr) || !enumerator) {
            sleepInterruptible(1000);
            continue;
        }

        IMMDevice* device = nullptr;
        std::string targetId = ReplayConfig::get().getMicDeviceId();

        if (!targetId.empty() && targetId != "default") {
            std::wstring wideId = geode::utils::string::utf8ToWide(targetId);
            if (!wideId.empty()) {
                if (FAILED(enumerator->GetDevice(wideId.c_str(), &device))) {
                    device = nullptr;
                }
            }
        }

        if (!device) {
            // Prefer eConsole first so Bluetooth headsets are not forced into low-quality HFP communication profile
            hr = enumerator->GetDefaultAudioEndpoint(eCapture, eConsole, &device);
            if (FAILED(hr)) {
                hr = enumerator->GetDefaultAudioEndpoint(eCapture, eCommunications, &device);
            }
        }

        if (FAILED(hr) || !device) {
            enumerator->Release();
            sleepInterruptible(1000);
            continue;
        }

        IAudioClient* audioClient = nullptr;
        hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, NULL, (void**)&audioClient);
        if (FAILED(hr) || !audioClient) {
            device->Release();
            enumerator->Release();
            sleepInterruptible(1000);
            continue;
        }

        WAVEFORMATEX* pwfx = nullptr;
        hr = audioClient->GetMixFormat(&pwfx);
        if (FAILED(hr) || !pwfx) {
            audioClient->Release();
            device->Release();
            enumerator->Release();
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
            audioClient->Release();
            audioClient = nullptr;
            if (SUCCEEDED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, NULL, (void**)&audioClient))) {
                hr = audioClient->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, hnsBuffer, 0, pwfx, NULL);
            }
        }

        bool deviceLost = false;
        if (SUCCEEDED(hr) && audioClient) {
            IAudioCaptureClient* captureClient = nullptr;
            hr = audioClient->GetService(__uuidof(IAudioCaptureClient), (void**)&captureClient);
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
                        hr = captureClient->GetBuffer(&pData, &numFrames, &flags, NULL, NULL);
                        if (FAILED(hr)) {
                            deviceLost = true;
                            break;
                        }
                        if (numFrames > 0) {
                            int channels = pwfx->nChannels;
                            int sampleRate = pwfx->nSamplesPerSec;
                            size_t totalSamples = static_cast<size_t>(numFrames) * channels;

                            if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
                                m_micConvertScratch.assign(totalSamples, 0.0f);
                                pushMicAudio(m_micConvertScratch.data(), numFrames, channels, sampleRate);
                            } else if (pData) {
                                if (isFloat) {
                                    pushMicAudio(reinterpret_cast<const float*>(pData), numFrames, channels, sampleRate);
                                } else {
                                    int bits = pwfx->wBitsPerSample;
                                    m_micConvertScratch.resize(totalSamples);
                                    if (bits == 16) {
                                        const int16_t* src = reinterpret_cast<const int16_t*>(pData);
                                        for (size_t i = 0; i < totalSamples; ++i) {
                                            m_micConvertScratch[i] = src[i] / 32768.0f;
                                        }
                                        pushMicAudio(m_micConvertScratch.data(), numFrames, channels, sampleRate);
                                    } else if (bits == 24) {
                                        const uint8_t* src = reinterpret_cast<const uint8_t*>(pData);
                                        for (size_t i = 0; i < totalSamples; ++i) {
                                            int32_t val = (src[i * 3 + 0]) | (src[i * 3 + 1] << 8) | (src[i * 3 + 2] << 16);
                                            if (val & 0x800000) val |= 0xFF000000;
                                            m_micConvertScratch[i] = static_cast<float>(val) / 8388608.0f;
                                        }
                                        pushMicAudio(m_micConvertScratch.data(), numFrames, channels, sampleRate);
                                    } else if (bits == 32) {
                                        const int32_t* src = reinterpret_cast<const int32_t*>(pData);
                                        for (size_t i = 0; i < totalSamples; ++i) {
                                            m_micConvertScratch[i] = static_cast<float>(src[i]) / 2147483648.0f;
                                        }
                                        pushMicAudio(m_micConvertScratch.data(), numFrames, channels, sampleRate);
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
                captureClient->Release();
            }
        }

        if (hWakeEvent) CloseHandle(hWakeEvent);
        if (pwfx) CoTaskMemFree(pwfx);
        if (audioClient) audioClient->Release();
        device->Release();
        enumerator->Release();

        if (m_micRunning.load()) {
            resetMicResampler();
            sleepInterruptible(deviceLost ? 500 : 1000);
        }
    }

    if (SUCCEEDED(coHr)) {
        CoUninitialize();
    }
}

} // namespace replay

#endif // GEODE_IS_WINDOWS
