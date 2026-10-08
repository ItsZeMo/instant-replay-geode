#include "AudioGrabber.hpp"
#include "../config/ReplayConfig.hpp"
#include <Geode/binding/FMODAudioEngine.hpp>
#include <windows.h>
#include <timeapi.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <propsys.h>

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

        // Push to game audio capture queue
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

AudioGrabber::AudioGrabber() {
}

AudioGrabber::~AudioGrabber() {
    cleanup();
}

void AudioGrabber::initialize() {
    if (m_initialized) return;

    setupFmodDSP();
    if (ReplayConfig::get().recordMic) {
        startMicCapture();
    }

    m_initialized = true;
}

void AudioGrabber::cleanup() {
    if (!m_initialized) return;

    removeFmodDSP();
    stopMicCapture();

    m_initialized = false;
}

void AudioGrabber::setGameAudioCallback(AudioCallback cb) {
    m_gameCallback = std::move(cb);
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
        m_gameSampleRate.store(sampleRate);
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
            geode::log::info("Instant Replay: attached FMOD DSP to Master Channel Group tail! (Sample Rate: {} Hz)", m_gameSampleRate.load());
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

void AudioGrabber::pushGameAudio(const float* inbuffer, unsigned int length, int channels, int sampleRate) {
    if (!m_gameCallback) return;

    const auto& config = ReplayConfig::get();
    if (!config.enabled) return;

    auto chunk = std::make_shared<AudioChunk>();
    chunk->channels = 2; // Always normalize to stereo for Media Foundation AAC
    chunk->sampleRate = (sampleRate > 0) ? sampleRate : m_gameSampleRate.load();
    chunk->timestamp = std::chrono::steady_clock::now();
    chunk->samples.resize(length * 2);

    float vol = config.systemVolume;

    if (channels == 2) {
        for (size_t i = 0; i < length * 2; ++i) {
            chunk->samples[i] = inbuffer[i] * vol;
        }
    } else if (channels == 1) {
        for (unsigned int i = 0; i < length; ++i) {
            float s = inbuffer[i] * vol;
            chunk->samples[i * 2] = s;
            chunk->samples[i * 2 + 1] = s;
        }
    } else if (channels >= 4) {
        // Surround downmix to stereo:
        // L = L_front + 0.707 * Center + 0.5 * LFE + 0.707 * L_surround
        // R = R_front + 0.707 * Center + 0.5 * LFE + 0.707 * R_surround
        for (unsigned int i = 0; i < length; ++i) {
            const float* frame = &inbuffer[i * channels];
            float l = frame[0];
            float r = frame[1];
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
            chunk->samples[i * 2] = std::clamp(l * vol, -1.0f, 1.0f);
            chunk->samples[i * 2 + 1] = std::clamp(r * vol, -1.0f, 1.0f);
        }
    } else {
        for (unsigned int i = 0; i < length; ++i) {
            chunk->samples[i * 2] = inbuffer[i * channels] * vol;
            chunk->samples[i * 2 + 1] = inbuffer[i * channels + 1] * vol;
        }
    }

    m_gameCallback(chunk);
}

void AudioGrabber::pushMicAudio(const float* inbuffer, unsigned int length, int channels, int sampleRate) {
    if (!m_micCallback || length == 0 || channels <= 0 || !inbuffer) return;

    const auto& config = ReplayConfig::get();
    if (!config.enabled || !config.recordMic) return;

    float vol = config.micVolume;

    // 1. Channel conversion to stereo (2 channels)
    std::vector<float> stereo(length * 2);
    if (channels == 1) {
        for (unsigned int i = 0; i < length; ++i) {
            float s = std::clamp(inbuffer[i] * vol, -1.0f, 1.0f);
            stereo[i * 2] = s;
            stereo[i * 2 + 1] = s;
        }
    } else if (channels == 2) {
        for (unsigned int i = 0; i < length * 2; ++i) {
            stereo[i] = std::clamp(inbuffer[i] * vol, -1.0f, 1.0f);
        }
    } else {
        // Downmix >2 channels to stereo
        for (unsigned int i = 0; i < length; ++i) {
            const float* frame = &inbuffer[i * channels];
            float l = frame[0];
            float r = frame[1];
            if (channels > 2) {
                float c = frame[2] * 0.7071f;
                l += c;
                r += c;
            }
            stereo[i * 2] = std::clamp(l * vol, -1.0f, 1.0f);
            stereo[i * 2 + 1] = std::clamp(r * vol, -1.0f, 1.0f);
        }
    }

    auto chunk = std::make_shared<AudioChunk>();
    chunk->channels = 2;
    chunk->timestamp = std::chrono::steady_clock::now();

    // 2. Resample to standard 48000 Hz if hardware uses a different rate
    constexpr int TARGET_RATE = 48000;
    if (sampleRate == TARGET_RATE || sampleRate <= 0) {
        chunk->sampleRate = TARGET_RATE;
        chunk->samples = std::move(stereo);
    } else {
        size_t outFrames = static_cast<size_t>(std::llround(static_cast<double>(length) * static_cast<double>(TARGET_RATE) / static_cast<double>(sampleRate)));
        if (outFrames == 0) return;
        chunk->sampleRate = TARGET_RATE;
        chunk->samples.resize(outFrames * 2);

        double ratio = static_cast<double>(sampleRate) / static_cast<double>(TARGET_RATE);
        for (size_t i = 0; i < outFrames; ++i) {
            double srcPos = i * ratio;
            size_t idx0 = static_cast<size_t>(srcPos);
            size_t idx1 = std::min(idx0 + 1, static_cast<size_t>(length - 1));
            float frac = static_cast<float>(srcPos - idx0);

            float l0 = stereo[idx0 * 2];
            float l1 = stereo[idx1 * 2];
            float r0 = stereo[idx0 * 2 + 1];
            float r1 = stereo[idx1 * 2 + 1];

            chunk->samples[i * 2] = l0 + frac * (l1 - l0);
            chunk->samples[i * 2 + 1] = r0 + frac * (r1 - r0);
        }
    }

    m_micSampleRate.store(TARGET_RATE);
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
                        int len = WideCharToMultiByte(CP_UTF8, 0, devId, -1, nullptr, 0, nullptr, nullptr);
                        if (len > 0) {
                            idStr.resize(len - 1);
                            WideCharToMultiByte(CP_UTF8, 0, devId, -1, idStr.data(), len, nullptr, nullptr);
                        }
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
    startMicCapture();
}

void AudioGrabber::startMicCapture() {
    if (m_micRunning.load()) return;

    m_micRunning.store(true);
    m_micThread = std::thread(&AudioGrabber::micCaptureThreadFunc, this);
}

void AudioGrabber::stopMicCapture() {
    if (!m_micRunning.load()) return;

    m_micRunning.store(false);
    if (m_micThread.joinable()) {
        m_micThread.join();
    }
}

void AudioGrabber::micCaptureThreadFunc() {
    CoInitializeEx(NULL, COINIT_MULTITHREADED);

    IMMDeviceEnumerator* enumerator = nullptr;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), NULL, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&enumerator);
    if (SUCCEEDED(hr) && enumerator) {
        IMMDevice* device = nullptr;
        std::string targetId = ReplayConfig::get().micDeviceId;

        if (!targetId.empty() && targetId != "default") {
            int wlen = MultiByteToWideChar(CP_UTF8, 0, targetId.c_str(), -1, nullptr, 0);
            if (wlen > 0) {
                std::wstring wideId(wlen, 0);
                MultiByteToWideChar(CP_UTF8, 0, targetId.c_str(), -1, wideId.data(), wlen);
                hr = enumerator->GetDevice(wideId.c_str(), &device);
                if (FAILED(hr)) {
                    device = nullptr;
                }
            }
        }

        if (!device) {
            hr = enumerator->GetDefaultAudioEndpoint(eCapture, eCommunications, &device);
            if (FAILED(hr)) {
                hr = enumerator->GetDefaultAudioEndpoint(eCapture, eConsole, &device);
            }
        }

        if (SUCCEEDED(hr) && device) {
            IAudioClient* audioClient = nullptr;
            hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, NULL, (void**)&audioClient);
            if (SUCCEEDED(hr) && audioClient) {
                WAVEFORMATEX* pwfx = nullptr;
                audioClient->GetMixFormat(&pwfx);

                REFERENCE_TIME hnsDefaultPeriod = 0, hnsMinPeriod = 0;
                audioClient->GetDevicePeriod(&hnsDefaultPeriod, &hnsMinPeriod);
                REFERENCE_TIME hnsBuffer = (hnsDefaultPeriod > 0) ? hnsDefaultPeriod * 2 : 200000; // ~20ms low latency buffer

                HANDLE hWakeEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
                bool eventDriven = false;
                hr = audioClient->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, hnsBuffer, 0, pwfx, NULL);
                if (SUCCEEDED(hr) && SUCCEEDED(audioClient->SetEventHandle(hWakeEvent))) {
                    eventDriven = true;
                } else {
                    // Fall back to polling mode
                    audioClient->Release();
                    audioClient = nullptr;
                    if (SUCCEEDED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, NULL, (void**)&audioClient))) {
                        hr = audioClient->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, hnsBuffer, 0, pwfx, NULL);
                    }
                }

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

                        timeBeginPeriod(1);

                        while (m_micRunning.load()) {
                            if (eventDriven) {
                                WaitForSingleObject(hWakeEvent, 20);
                            } else {
                                std::this_thread::sleep_for(std::chrono::milliseconds(4));
                            }

                            UINT32 packetLength = 0;
                            hr = captureClient->GetNextPacketSize(&packetLength);
                            while (SUCCEEDED(hr) && packetLength > 0 && m_micRunning.load()) {
                                BYTE* pData = nullptr;
                                UINT32 numFrames = 0;
                                DWORD flags = 0;
                                hr = captureClient->GetBuffer(&pData, &numFrames, &flags, NULL, NULL);
                                if (SUCCEEDED(hr) && numFrames > 0) {
                                    int channels = pwfx->nChannels;
                                    int sampleRate = pwfx->nSamplesPerSec;

                                    if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
                                        std::vector<float> silence(numFrames * channels, 0.0f);
                                        pushMicAudio(silence.data(), numFrames, channels, sampleRate);
                                    } else if (pData) {
                                        if (isFloat) {
                                            float* floatData = reinterpret_cast<float*>(pData);
                                            pushMicAudio(floatData, numFrames, channels, sampleRate);
                                        } else {
                                            int bits = pwfx->wBitsPerSample;
                                            if (bits == 16) {
                                                const int16_t* src = reinterpret_cast<const int16_t*>(pData);
                                                std::vector<float> converted(numFrames * channels);
                                                for (size_t i = 0; i < numFrames * channels; ++i) {
                                                    converted[i] = src[i] / 32768.0f;
                                                }
                                                pushMicAudio(converted.data(), numFrames, channels, sampleRate);
                                            } else if (bits == 24) {
                                                const uint8_t* src = reinterpret_cast<const uint8_t*>(pData);
                                                std::vector<float> converted(numFrames * channels);
                                                for (size_t i = 0; i < numFrames * channels; ++i) {
                                                    int32_t val = (src[i * 3 + 0]) | (src[i * 3 + 1] << 8) | (src[i * 3 + 2] << 16);
                                                    if (val & 0x800000) val |= 0xFF000000;
                                                    converted[i] = static_cast<float>(val) / 8388608.0f;
                                                }
                                                pushMicAudio(converted.data(), numFrames, channels, sampleRate);
                                            } else if (bits == 32) {
                                                const int32_t* src = reinterpret_cast<const int32_t*>(pData);
                                                std::vector<float> converted(numFrames * channels);
                                                for (size_t i = 0; i < numFrames * channels; ++i) {
                                                    converted[i] = static_cast<float>(src[i]) / 2147483648.0f;
                                                }
                                                pushMicAudio(converted.data(), numFrames, channels, sampleRate);
                                            }
                                        }
                                    }

                                    captureClient->ReleaseBuffer(numFrames);
                                }
                                hr = captureClient->GetNextPacketSize(&packetLength);
                            }
                        }

                        timeEndPeriod(1);
                        audioClient->Stop();
                        captureClient->Release();
                    }
                }
                if (hWakeEvent) CloseHandle(hWakeEvent);
                if (pwfx) CoTaskMemFree(pwfx);
                if (audioClient) audioClient->Release();
            }
            device->Release();
        }
        enumerator->Release();
    }

    CoUninitialize();
}

} // namespace replay
