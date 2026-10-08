#include "MediaFoundationEncoder.hpp"
#include <codecapi.h>
#include <filesystem>
#include <algorithm>
#include <map>
#include <set>

namespace replay {

static bool s_mfInitialized = false;

void MediaFoundationEncoder::globalInitialize() {
    if (!s_mfInitialized) {
        CoInitializeEx(NULL, COINIT_MULTITHREADED);
        MFStartup(MF_VERSION);
        s_mfInitialized = true;
    }
}

void MediaFoundationEncoder::globalShutdown() {
    if (s_mfInitialized) {
        MFShutdown();
        CoUninitialize();
        s_mfInitialized = false;
    }
}

MediaFoundationEncoder::MediaFoundationEncoder() {
    globalInitialize();
}

MediaFoundationEncoder::~MediaFoundationEncoder() {
    finish();
}

bool MediaFoundationEncoder::start(
    const std::wstring& outputPath,
    int width,
    int height,
    int fps,
    int bitrateMbps,
    int maxBitrateMbps,
    int qualityLevel,
    RateControlMode rateControl,
    bool splitAudio,
    CodecType codec,
    bool hardwareAcceleration,
    const std::string& customCodec,
    int gameSampleRate
) {
    std::lock_guard<std::mutex> lock(m_mutex);
    finishLocked();

    m_width = width;
    m_height = height;
    m_fps = fps;
    m_splitAudio = splitAudio;
    m_videoFrameDurationHns = 10000000LL / fps;
    m_lastVideoHns = 0;
    m_lastGameAudioHns = 0;
    m_lastMicAudioHns = 0;

    IMFAttributes* pAttr = nullptr;
    HRESULT hr = MFCreateAttributes(&pAttr, 4);
    if (FAILED(hr)) return false;

    bool enableHardware = hardwareAcceleration;
    GUID targetSubtype = MFVideoFormat_H264;

    std::string cname = customCodec;
    std::transform(cname.begin(), cname.end(), cname.begin(), [](unsigned char c) { return std::tolower(c); });

    if (codec == CodecType::CUSTOM && !cname.empty()) {
        if (cname.find("cpu") != std::string::npos || 
            cname.find("software") != std::string::npos || 
            cname.find("sw") != std::string::npos) {
            enableHardware = false;
            targetSubtype = MFVideoFormat_H264;
        } else if (cname.find("av1") != std::string::npos) {
            targetSubtype = MFVideoFormat_AV1;
        } else if (cname.find("hevc") != std::string::npos || 
                   cname.find("h265") != std::string::npos || 
                   cname.find("265") != std::string::npos) {
            targetSubtype = MFVideoFormat_HEVC;
        } else if (cname.find("vp9") != std::string::npos) {
            targetSubtype = MFVideoFormat_VP90;
        } else {
            targetSubtype = MFVideoFormat_H264;
        }
    } else if (codec == CodecType::AV1) {
        targetSubtype = MFVideoFormat_AV1;
    } else if (codec == CodecType::HEVC) {
        targetSubtype = MFVideoFormat_HEVC;
    } else {
        targetSubtype = MFVideoFormat_H264;
    }

    pAttr->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, enableHardware ? TRUE : FALSE);
    pAttr->SetUINT32(MF_SINK_WRITER_DISABLE_THROTTLING, TRUE);
    pAttr->SetUINT32(MF_LOW_LATENCY, TRUE);

    hr = MFCreateSinkWriterFromURL(outputPath.c_str(), NULL, pAttr, &m_sinkWriter);
    pAttr->Release();
    if (FAILED(hr) || !m_sinkWriter) return false;

    auto tryAddVideoStream = [this, width, height, fps, bitrateMbps](GUID subtype) -> bool {
        IMFMediaType* pVideoOut = nullptr;
        MFCreateMediaType(&pVideoOut);
        pVideoOut->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        pVideoOut->SetGUID(MF_MT_SUBTYPE, subtype);
        pVideoOut->SetUINT32(MF_MT_AVG_BITRATE, bitrateMbps * 1000000);
        pVideoOut->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        MFSetAttributeSize(pVideoOut, MF_MT_FRAME_SIZE, width, height);
        MFSetAttributeRatio(pVideoOut, MF_MT_FRAME_RATE, fps, 1);
        MFSetAttributeRatio(pVideoOut, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
        HRESULT hRes = m_sinkWriter->AddStream(pVideoOut, &m_videoStreamIndex);
        pVideoOut->Release();
        return SUCCEEDED(hRes);
    };

    if (!tryAddVideoStream(targetSubtype)) {
        // Fallback cascade: AV1 -> HEVC -> H264
        if (targetSubtype == MFVideoFormat_AV1) {
            if (!tryAddVideoStream(MFVideoFormat_HEVC)) {
                if (!tryAddVideoStream(MFVideoFormat_H264)) {
                    finishLocked();
                    return false;
                }
            }
        } else if (targetSubtype == MFVideoFormat_HEVC) {
            if (!tryAddVideoStream(MFVideoFormat_H264)) {
                finishLocked();
                return false;
            }
        } else if (!tryAddVideoStream(MFVideoFormat_H264)) {
            finishLocked();
            return false;
        }
    }

    // 2. Video Input Format (RGB32 / BGRA uncompressed)
    IMFMediaType* pVideoIn = nullptr;
    MFCreateMediaType(&pVideoIn);
    pVideoIn->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    pVideoIn->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
    pVideoIn->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    MFSetAttributeSize(pVideoIn, MF_MT_FRAME_SIZE, width, height);
    MFSetAttributeRatio(pVideoIn, MF_MT_FRAME_RATE, fps, 1);
    MFSetAttributeRatio(pVideoIn, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    // OpenGL readback is bottom-up: negative default stride tells MF the first
    // row in memory is the bottom row of the image.
    pVideoIn->SetUINT32(MF_MT_DEFAULT_STRIDE, static_cast<UINT32>(-(static_cast<LONG>(width) * 4)));
    pVideoIn->SetUINT32(MF_MT_FIXED_SIZE_SAMPLES, TRUE);
    pVideoIn->SetUINT32(MF_MT_SAMPLE_SIZE, static_cast<UINT32>(width) * height * 4);
    hr = m_sinkWriter->SetInputMediaType(m_videoStreamIndex, pVideoIn, NULL);
    pVideoIn->Release();
    if (FAILED(hr)) {
        finishLocked();
        return false;
    }

    // 3. Configure Adaptive Bitrate / Rate Control via ICodecAPI
    ICodecAPI* pCodecAPI = nullptr;
    hr = m_sinkWriter->GetServiceForStream(m_videoStreamIndex, GUID_NULL, __uuidof(ICodecAPI), (void**)&pCodecAPI);
    if (SUCCEEDED(hr) && pCodecAPI) {
        VARIANT var;
        VariantInit(&var);
        var.vt = VT_UI4;
        if (rateControl == RateControlMode::ADAPTIVE_VBR) {
            var.ulVal = eAVEncCommonRateControlMode_PeakConstrainedVBR;
            pCodecAPI->SetValue(&CODECAPI_AVEncCommonRateControlMode, &var);
            var.ulVal = bitrateMbps * 1000000;
            pCodecAPI->SetValue(&CODECAPI_AVEncCommonMeanBitRate, &var);
            var.ulVal = maxBitrateMbps * 1000000;
            pCodecAPI->SetValue(&CODECAPI_AVEncCommonMaxBitRate, &var);
        } else if (rateControl == RateControlMode::CONSTANT_CBR) {
            var.ulVal = eAVEncCommonRateControlMode_CBR;
            pCodecAPI->SetValue(&CODECAPI_AVEncCommonRateControlMode, &var);
            var.ulVal = bitrateMbps * 1000000;
            pCodecAPI->SetValue(&CODECAPI_AVEncCommonMeanBitRate, &var);
        } else if (rateControl == RateControlMode::CONSTANT_QUALITY) {
            var.ulVal = eAVEncCommonRateControlMode_Quality;
            pCodecAPI->SetValue(&CODECAPI_AVEncCommonRateControlMode, &var);
            var.ulVal = static_cast<ULONG>(qualityLevel);
            pCodecAPI->SetValue(&CODECAPI_AVEncCommonQuality, &var);
        }
        pCodecAPI->Release();
    }

    // 4. Audio Track 1: Game / System Audio
    int actualGameSampleRate = (gameSampleRate > 0) ? gameSampleRate : 44100;
    IMFMediaType* pAudioOut1 = nullptr;
    MFCreateMediaType(&pAudioOut1);
    pAudioOut1->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    pAudioOut1->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC);
    pAudioOut1->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    pAudioOut1->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, actualGameSampleRate);
    pAudioOut1->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2);
    pAudioOut1->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, 24000); // 192 kbps
    hr = m_sinkWriter->AddStream(pAudioOut1, &m_gameAudioStreamIndex);
    pAudioOut1->Release();
    if (FAILED(hr)) {
        finishLocked();
        return false;
    }

    IMFMediaType* pAudioIn1 = nullptr;
    MFCreateMediaType(&pAudioIn1);
    pAudioIn1->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    pAudioIn1->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
    pAudioIn1->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    pAudioIn1->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, actualGameSampleRate);
    pAudioIn1->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2);
    pAudioIn1->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, 4);
    pAudioIn1->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, actualGameSampleRate * 4);
    hr = m_sinkWriter->SetInputMediaType(m_gameAudioStreamIndex, pAudioIn1, NULL);
    pAudioIn1->Release();
    if (FAILED(hr)) {
        finishLocked();
        return false;
    }

    // 5. Audio Track 2: Microphone Audio (if split audio is enabled)
    if (m_splitAudio) {
        IMFMediaType* pAudioOut2 = nullptr;
        MFCreateMediaType(&pAudioOut2);
        pAudioOut2->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        pAudioOut2->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC);
        pAudioOut2->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
        pAudioOut2->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, 48000);
        pAudioOut2->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2);
        pAudioOut2->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, 16000); // 128 kbps
        hr = m_sinkWriter->AddStream(pAudioOut2, &m_micAudioStreamIndex);
        pAudioOut2->Release();

        if (SUCCEEDED(hr)) {
            IMFMediaType* pAudioIn2 = nullptr;
            MFCreateMediaType(&pAudioIn2);
            pAudioIn2->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
            pAudioIn2->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
            pAudioIn2->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
            pAudioIn2->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, 48000);
            pAudioIn2->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2);
            pAudioIn2->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, 4);
            pAudioIn2->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, 48000 * 4);
            m_sinkWriter->SetInputMediaType(m_micAudioStreamIndex, pAudioIn2, NULL);
            pAudioIn2->Release();
        }
    }

    hr = m_sinkWriter->BeginWriting();
    if (FAILED(hr)) {
        finishLocked();
        return false;
    }

    m_recording = true;
    return true;
}

bool MediaFoundationEncoder::createVideoSample(
    const uint8_t* bgraPixels,
    int width,
    int height,
    int64_t timestampHns,
    IMFSample** ppSample
) {
    DWORD bufferSize = width * height * 4;
    IMFMediaBuffer* pBuffer = nullptr;
    HRESULT hr = MFCreateMemoryBuffer(bufferSize, &pBuffer);
    if (FAILED(hr)) return false;

    BYTE* pDst = nullptr;
    hr = pBuffer->Lock(&pDst, NULL, NULL);
    if (SUCCEEDED(hr) && pDst) {
        memcpy(pDst, bgraPixels, bufferSize);
        pBuffer->Unlock();
        pBuffer->SetCurrentLength(bufferSize);

        hr = MFCreateSample(ppSample);
        if (SUCCEEDED(hr) && *ppSample) {
            (*ppSample)->AddBuffer(pBuffer);
            (*ppSample)->SetSampleTime(timestampHns);
            (*ppSample)->SetSampleDuration(m_videoFrameDurationHns);
            pBuffer->Release();
            return true;
        }
    }
    pBuffer->Release();
    return false;
}

bool MediaFoundationEncoder::createAudioSample(
    const float* pcmFloat,
    int numSamples,
    int channels,
    int sampleRate,
    int64_t timestampHns,
    IMFSample** ppSample
) {
    if (numSamples <= 0 || channels <= 0) return false;

    DWORD bufferSize = numSamples * channels * sizeof(int16_t);
    IMFMediaBuffer* pBuffer = nullptr;
    HRESULT hr = MFCreateMemoryBuffer(bufferSize, &pBuffer);
    if (FAILED(hr)) return false;

    BYTE* pDst = nullptr;
    hr = pBuffer->Lock(&pDst, NULL, NULL);
    if (SUCCEEDED(hr) && pDst) {
        int16_t* pSamples16 = reinterpret_cast<int16_t*>(pDst);
        size_t total = static_cast<size_t>(numSamples) * channels;
        for (size_t i = 0; i < total; ++i) {
            float s = std::clamp(pcmFloat[i], -1.0f, 1.0f);
            pSamples16[i] = static_cast<int16_t>(s * 32767.0f);
        }
        pBuffer->Unlock();
        pBuffer->SetCurrentLength(bufferSize);

        hr = MFCreateSample(ppSample);
        if (SUCCEEDED(hr) && *ppSample) {
            (*ppSample)->AddBuffer(pBuffer);
            (*ppSample)->SetSampleTime(timestampHns);
            int rate = (sampleRate > 0) ? sampleRate : 44100;
            int64_t durationHns = (static_cast<int64_t>(numSamples) * 10000000LL) / rate;
            (*ppSample)->SetSampleDuration(durationHns);
            pBuffer->Release();
            return true;
        }
    }
    pBuffer->Release();
    return false;
}

bool MediaFoundationEncoder::writeVideoFrame(
    const uint8_t* bgraPixels,
    int width,
    int height,
    int64_t timestampHns
) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_recording || !m_sinkWriter) return false;
    if (width != m_width || height != m_height) return false;

    IMFSample* pSample = nullptr;
    if (createVideoSample(bgraPixels, width, height, timestampHns, &pSample)) {
        HRESULT hr = m_sinkWriter->WriteSample(m_videoStreamIndex, pSample);
        pSample->Release();
        m_lastVideoHns = timestampHns;
        return SUCCEEDED(hr);
    }
    return false;
}

bool MediaFoundationEncoder::writeGameAudio(
    const float* pcmFloat,
    int numSamples,
    int channels,
    int sampleRate,
    int64_t timestampHns
) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_recording || !m_sinkWriter) return false;

    IMFSample* pSample = nullptr;
    if (createAudioSample(pcmFloat, numSamples, channels, sampleRate, timestampHns, &pSample)) {
        HRESULT hr = m_sinkWriter->WriteSample(m_gameAudioStreamIndex, pSample);
        pSample->Release();
        m_lastGameAudioHns = timestampHns;
        return SUCCEEDED(hr);
    }
    return false;
}

bool MediaFoundationEncoder::writeMicAudio(
    const float* pcmFloat,
    int numSamples,
    int channels,
    int sampleRate,
    int64_t timestampHns
) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_recording || !m_sinkWriter || !m_splitAudio) return false;

    IMFSample* pSample = nullptr;
    if (createAudioSample(pcmFloat, numSamples, channels, sampleRate, timestampHns, &pSample)) {
        HRESULT hr = m_sinkWriter->WriteSample(m_micAudioStreamIndex, pSample);
        pSample->Release();
        m_lastMicAudioHns = timestampHns;
        return SUCCEEDED(hr);
    }
    return false;
}

bool MediaFoundationEncoder::finish() {
    std::lock_guard<std::mutex> lock(m_mutex);
    return finishLocked();
}

// Caller must hold m_mutex
bool MediaFoundationEncoder::finishLocked() {
    if (!m_sinkWriter) {
        m_recording = false;
        return true;
    }

    HRESULT hr = S_OK;
    if (m_recording) {
        hr = m_sinkWriter->Finalize();
    }
    m_sinkWriter->Release();
    m_sinkWriter = nullptr;
    m_recording = false;
    return SUCCEEDED(hr);
}

bool MediaFoundationEncoder::concatenateSegments(
    const std::vector<std::wstring>& segmentPaths,
    const std::wstring& finalOutputPath
) {
    if (segmentPaths.empty()) return false;

    if (segmentPaths.size() == 1) {
        return CopyFileW(segmentPaths[0].c_str(), finalOutputPath.c_str(), FALSE) != FALSE;
    }

    IMFAttributes* pReaderAttr = nullptr;
    MFCreateAttributes(&pReaderAttr, 2);
    pReaderAttr->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, FALSE);
    pReaderAttr->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);

    IMFSourceReader* pReader = nullptr;
    HRESULT hr = MFCreateSourceReaderFromURL(segmentPaths[0].c_str(), pReaderAttr, &pReader);
    if (FAILED(hr) || !pReader) {
        pReaderAttr->Release();
        return CopyFileW(segmentPaths.back().c_str(), finalOutputPath.c_str(), FALSE) != FALSE;
    }

    IMFSinkWriter* pWriter = nullptr;
    IMFAttributes* pAttr = nullptr;
    MFCreateAttributes(&pAttr, 2);
    pAttr->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
    pAttr->SetUINT32(MF_SINK_WRITER_DISABLE_THROTTLING, TRUE);

    hr = MFCreateSinkWriterFromURL(finalOutputPath.c_str(), NULL, pAttr, &pWriter);
    pAttr->Release();
    if (FAILED(hr) || !pWriter) {
        pReader->Release();
        pReaderAttr->Release();
        return CopyFileW(segmentPaths.back().c_str(), finalOutputPath.c_str(), FALSE) != FALSE;
    }

    std::vector<DWORD> validStreams;
    std::map<DWORD, DWORD> streamMap;
    for (DWORD s = 0; s < 4; ++s) {
        IMFMediaType* pType = nullptr;
        if (SUCCEEDED(pReader->GetNativeMediaType(s, 0, &pType)) && pType) {
            DWORD outStream = 0;
            if (SUCCEEDED(pWriter->AddStream(pType, &outStream))) {
                if (SUCCEEDED(pWriter->SetInputMediaType(outStream, pType, NULL))) {
                    validStreams.push_back(s);
                    streamMap[s] = outStream;
                }
            }
            pType->Release();
        }
    }
    pReader->Release();

    if (validStreams.empty() || FAILED(pWriter->BeginWriting())) {
        if (pWriter) { pWriter->Release(); }
        pReaderAttr->Release();
        return CopyFileW(segmentPaths.back().c_str(), finalOutputPath.c_str(), FALSE) != FALSE;
    }

    std::map<DWORD, LONGLONG> streamLastTimes;
    for (DWORD s : validStreams) {
        streamLastTimes[s] = -1;
    }

    LONGLONG segBaseOffset = 0;

    for (const auto& path : segmentPaths) {
        IMFSourceReader* segReader = nullptr;
        hr = MFCreateSourceReaderFromURL(path.c_str(), pReaderAttr, &segReader);
        if (FAILED(hr) || !segReader) {
            continue;
        }

        for (DWORD s : validStreams) {
            IMFMediaType* pNativeType = nullptr;
            if (SUCCEEDED(segReader->GetNativeMediaType(s, 0, &pNativeType)) && pNativeType) {
                segReader->SetCurrentMediaType(s, NULL, pNativeType);
                segReader->SetStreamSelection(s, TRUE);
                pNativeType->Release();
            }
        }

        LONGLONG currentSegBase = segBaseOffset;
        LONGLONG segMaxDuration = 0;
        std::set<DWORD> remainingStreams(validStreams.begin(), validStreams.end());

        while (!remainingStreams.empty()) {
            DWORD actualStream = 0;
            DWORD flags = 0;
            LONGLONG timestamp = 0;
            IMFSample* sample = nullptr;
            hr = segReader->ReadSample(
                MF_SOURCE_READER_ANY_STREAM,
                0,
                &actualStream,
                &flags,
                &timestamp,
                &sample
            );
            if (FAILED(hr) || (flags & MF_SOURCE_READERF_ERROR)) {
                break;
            }
            if (flags & MF_SOURCE_READERF_ENDOFSTREAM) {
                remainingStreams.erase(actualStream);
            }
            if (sample) {
                LONGLONG sampleDuration = 0;
                sample->GetSampleDuration(&sampleDuration);

                LONGLONG sampleTime = timestamp + currentSegBase;
                if (streamLastTimes[actualStream] >= 0 && sampleTime <= streamLastTimes[actualStream]) {
                    sampleTime = streamLastTimes[actualStream] + 1000LL;
                }
                sample->SetSampleTime(sampleTime);

                LONGLONG dts = 0;
                if (SUCCEEDED(sample->GetUINT64(MFSampleExtension_DecodeTimestamp, (UINT64*)&dts))) {
                    LONGLONG newDts = dts + currentSegBase;
                    sample->SetUINT64(MFSampleExtension_DecodeTimestamp, (UINT64)newDts);
                }

                DWORD targetStream = streamMap.count(actualStream) ? streamMap[actualStream] : actualStream;
                pWriter->WriteSample(targetStream, sample);
                
                streamLastTimes[actualStream] = sampleTime;
                if (timestamp + sampleDuration > segMaxDuration) {
                    segMaxDuration = timestamp + sampleDuration;
                }
                sample->Release();
            }
        }
        segReader->Release();

        segBaseOffset += segMaxDuration;
    }
    pReaderAttr->Release();

    pWriter->Finalize();
    pWriter->Release();
    return true;
}

} // namespace replay
