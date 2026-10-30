#include "MediaFoundationEncoder.hpp"
#include <codecapi.h>
#include <filesystem>
#include <algorithm>
#include <map>
#include <set>
#include <cstdio>

namespace replay {

namespace {

// Minimal COM smart pointer (WRL is not available in our include paths).
template <typename T>
class ComRef {
public:
    ComRef() = default;
    explicit ComRef(T* p) : m_p(p) {}
    ~ComRef() { reset(); }
    ComRef(const ComRef&) = delete;
    ComRef& operator=(const ComRef&) = delete;

    void reset() {
        if (m_p) {
            m_p->Release();
            m_p = nullptr;
        }
    }
    T** put() { reset(); return &m_p; }
    T* get() const { return m_p; }
    T* operator->() const { return m_p; }
    explicit operator bool() const { return m_p != nullptr; }

private:
    T* m_p = nullptr;
};

std::string hrToString(HRESULT hr) {
    if (hr == HRESULT_FROM_WIN32(ERROR_DISK_FULL) || hr == HRESULT_FROM_WIN32(ERROR_HANDLE_DISK_FULL)) {
        return "disk is full";
    }
    if (hr == HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED)) {
        return "access denied";
    }
    char buf[32];
    snprintf(buf, sizeof(buf), "HRESULT 0x%08X", static_cast<unsigned>(hr));
    return buf;
}

const char* subtypeName(const GUID& subtype) {
    if (subtype == MFVideoFormat_AV1) return "AV1";
    if (subtype == MFVideoFormat_HEVC) return "HEVC";
    if (subtype == MFVideoFormat_VP90) return "VP9";
    return "H.264";
}

} // namespace

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

    fps = std::max(1, fps);
    m_width = width;
    m_height = height;
    m_fps = fps;
    m_splitAudio = splitAudio;
    m_videoFrameDurationHns = 10000000LL / fps;
    m_lastVideoHns = 0;
    m_lastGameAudioHns = 0;
    m_lastMicAudioHns = 0;
    m_activeCodecName.clear();
    m_formatKey.clear();

    bool enableHardware = hardwareAcceleration;
    GUID targetSubtype = MFVideoFormat_H264;

    std::string cname = customCodec;
    std::transform(cname.begin(), cname.end(), cname.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

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

    // Fallback cascade. A missing encoder MFT is usually only detected by
    // SetInputMediaType / BeginWriting, so every candidate gets a full,
    // independent initialisation attempt: AV1 -> HEVC -> H.264 -> H.264 (software).
    struct Attempt { GUID subtype; bool hardware; };
    std::vector<Attempt> attempts;
    auto addAttempt = [&attempts](const GUID& subtype, bool hw) {
        for (const auto& a : attempts) {
            if (a.subtype == subtype && a.hardware == hw) return;
        }
        attempts.push_back({subtype, hw});
    };
    addAttempt(targetSubtype, enableHardware);
    if (targetSubtype == MFVideoFormat_AV1) {
        addAttempt(MFVideoFormat_HEVC, enableHardware);
    }
    addAttempt(MFVideoFormat_H264, enableHardware);
    if (enableHardware) {
        addAttempt(MFVideoFormat_H264, false);
    }

    StartParams params;
    params.width = width;
    params.height = height;
    params.fps = fps;
    params.bitrateMbps = bitrateMbps;
    params.maxBitrateMbps = std::max(maxBitrateMbps, bitrateMbps);
    params.qualityLevel = qualityLevel;
    params.rateControl = rateControl;
    params.splitAudio = splitAudio;
    params.gameSampleRate = (gameSampleRate > 0) ? gameSampleRate : 44100;

    for (size_t i = 0; i < attempts.size(); ++i) {
        const auto& attempt = attempts[i];
        std::string error;
        if (tryStartLocked(outputPath, attempt.subtype, attempt.hardware, params, error)) {
            m_activeCodecName = std::string(subtypeName(attempt.subtype)) + (attempt.hardware ? " (HW)" : " (SW)");
            char key[160];
            snprintf(key, sizeof(key), "%s|%dx%d|%d|a%d|s%d", subtypeName(attempt.subtype), width, height, fps,
                     params.gameSampleRate, splitAudio ? 1 : 0);
            m_formatKey = key;
            m_recording = true;
            if (i > 0) {
                geode::log::warn("Instant Replay: requested codec unavailable, fell back to {}", m_activeCodecName);
            } else {
                geode::log::info("Instant Replay: encoder started with {}", m_activeCodecName);
            }
            return true;
        }

        geode::log::warn("Instant Replay: {} encoder ({}) failed: {}",
                         subtypeName(attempt.subtype), attempt.hardware ? "HW" : "SW", error);
        if (m_sinkWriter) {
            m_sinkWriter->Release();
            m_sinkWriter = nullptr;
        }
        DeleteFileW(outputPath.c_str()); // remove the partially created file before retrying
    }

    geode::log::error("Instant Replay: no usable video encoder found");
    return false;
}

// Performs one complete sink writer initialisation for the given codec.
// Caller must hold m_mutex. On failure m_sinkWriter may be left non-null; the caller releases it.
bool MediaFoundationEncoder::tryStartLocked(
    const std::wstring& outputPath,
    const GUID& subtype,
    bool hardware,
    const StartParams& p,
    std::string& error
) {
    ComRef<IMFAttributes> attr;
    HRESULT hr = MFCreateAttributes(attr.put(), 4);
    if (FAILED(hr)) { error = "MFCreateAttributes: " + hrToString(hr); return false; }
    attr->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, hardware ? TRUE : FALSE);
    attr->SetUINT32(MF_SINK_WRITER_DISABLE_THROTTLING, TRUE);
    attr->SetUINT32(MF_LOW_LATENCY, TRUE);

    hr = MFCreateSinkWriterFromURL(outputPath.c_str(), NULL, attr.get(), &m_sinkWriter);
    if (FAILED(hr) || !m_sinkWriter) { error = "MFCreateSinkWriterFromURL: " + hrToString(hr); return false; }

    // 1. Video output stream
    {
        ComRef<IMFMediaType> videoOut;
        hr = MFCreateMediaType(videoOut.put());
        if (FAILED(hr)) { error = "MFCreateMediaType: " + hrToString(hr); return false; }
        videoOut->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        videoOut->SetGUID(MF_MT_SUBTYPE, subtype);
        videoOut->SetUINT32(MF_MT_AVG_BITRATE, static_cast<UINT32>(p.bitrateMbps) * 1000000u);
        videoOut->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        videoOut->SetUINT32(MF_MT_MAX_KEYFRAME_SPACING, static_cast<UINT32>(p.fps));
        MFSetAttributeSize(videoOut.get(), MF_MT_FRAME_SIZE, p.width, p.height);
        MFSetAttributeRatio(videoOut.get(), MF_MT_FRAME_RATE, p.fps, 1);
        MFSetAttributeRatio(videoOut.get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
        hr = m_sinkWriter->AddStream(videoOut.get(), &m_videoStreamIndex);
        if (FAILED(hr)) { error = "AddStream(video): " + hrToString(hr); return false; }
    }

    // 2. Video input format (RGB32 / BGRA uncompressed) - this is where a missing encoder is detected
    {
        ComRef<IMFMediaType> videoIn;
        hr = MFCreateMediaType(videoIn.put());
        if (FAILED(hr)) { error = "MFCreateMediaType: " + hrToString(hr); return false; }
        videoIn->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        videoIn->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
        videoIn->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        MFSetAttributeSize(videoIn.get(), MF_MT_FRAME_SIZE, p.width, p.height);
        MFSetAttributeRatio(videoIn.get(), MF_MT_FRAME_RATE, p.fps, 1);
        MFSetAttributeRatio(videoIn.get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
        // OpenGL readback is bottom-up: negative default stride tells MF the first
        // row in memory is the bottom row of the image.
        videoIn->SetUINT32(MF_MT_DEFAULT_STRIDE, static_cast<UINT32>(-(static_cast<LONG>(p.width) * 4)));
        videoIn->SetUINT32(MF_MT_FIXED_SIZE_SAMPLES, TRUE);
        videoIn->SetUINT32(MF_MT_SAMPLE_SIZE, static_cast<UINT32>(p.width) * p.height * 4);
        hr = m_sinkWriter->SetInputMediaType(m_videoStreamIndex, videoIn.get(), NULL);
        if (FAILED(hr)) { error = "SetInputMediaType(video): " + hrToString(hr); return false; }
    }

    // 3. Rate control + keyframe interval via ICodecAPI (best effort: not every MFT supports every property)
    {
        ICodecAPI* rawCodecApi = nullptr;
        hr = m_sinkWriter->GetServiceForStream(m_videoStreamIndex, GUID_NULL, __uuidof(ICodecAPI), (void**)&rawCodecApi);
        ComRef<ICodecAPI> codecApi(rawCodecApi);
        if (SUCCEEDED(hr) && codecApi) {
            VARIANT var;
            VariantInit(&var);
            var.vt = VT_UI4;
            if (p.rateControl == RateControlMode::ADAPTIVE_VBR) {
                var.ulVal = eAVEncCommonRateControlMode_PeakConstrainedVBR;
                codecApi->SetValue(&CODECAPI_AVEncCommonRateControlMode, &var);
                var.ulVal = static_cast<ULONG>(p.bitrateMbps) * 1000000u;
                codecApi->SetValue(&CODECAPI_AVEncCommonMeanBitRate, &var);
                var.ulVal = static_cast<ULONG>(p.maxBitrateMbps) * 1000000u;
                codecApi->SetValue(&CODECAPI_AVEncCommonMaxBitRate, &var);
            } else if (p.rateControl == RateControlMode::CONSTANT_CBR) {
                var.ulVal = eAVEncCommonRateControlMode_CBR;
                codecApi->SetValue(&CODECAPI_AVEncCommonRateControlMode, &var);
                var.ulVal = static_cast<ULONG>(p.bitrateMbps) * 1000000u;
                codecApi->SetValue(&CODECAPI_AVEncCommonMeanBitRate, &var);
            } else if (p.rateControl == RateControlMode::CONSTANT_QUALITY) {
                var.ulVal = eAVEncCommonRateControlMode_Quality;
                codecApi->SetValue(&CODECAPI_AVEncCommonRateControlMode, &var);
                var.ulVal = static_cast<ULONG>(std::clamp(p.qualityLevel, 1, 100));
                codecApi->SetValue(&CODECAPI_AVEncCommonQuality, &var);
            }
            // 1-second GOP: bounds how far the trimmed replay start can deviate from the requested duration
            var.ulVal = static_cast<ULONG>(p.fps);
            codecApi->SetValue(&CODECAPI_AVEncMPVGOPSize, &var);
        }
    }

    auto addAudioStream = [this, &error](int sampleRate, UINT32 avgBytesPerSec, DWORD* streamIndex, const char* label) -> bool {
        ComRef<IMFMediaType> audioOut;
        HRESULT h = MFCreateMediaType(audioOut.put());
        if (FAILED(h)) { error = "MFCreateMediaType: " + hrToString(h); return false; }
        audioOut->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        audioOut->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC);
        audioOut->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
        audioOut->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, sampleRate);
        audioOut->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2);
        audioOut->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, avgBytesPerSec);
        h = m_sinkWriter->AddStream(audioOut.get(), streamIndex);
        if (FAILED(h)) { error = std::string("AddStream(") + label + "): " + hrToString(h); return false; }

        ComRef<IMFMediaType> audioIn;
        h = MFCreateMediaType(audioIn.put());
        if (FAILED(h)) { error = "MFCreateMediaType: " + hrToString(h); return false; }
        audioIn->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        audioIn->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
        audioIn->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
        audioIn->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, sampleRate);
        audioIn->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2);
        audioIn->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, 4);
        audioIn->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, sampleRate * 4);
        h = m_sinkWriter->SetInputMediaType(*streamIndex, audioIn.get(), NULL);
        if (FAILED(h)) { error = std::string("SetInputMediaType(") + label + "): " + hrToString(h); return false; }
        return true;
    };

    // 4. Audio track 1: game audio (also carries the mixed-in microphone when tracks are not split)
    if (!addAudioStream(p.gameSampleRate, 24000 /* 192 kbps */, &m_gameAudioStreamIndex, "game audio")) {
        return false;
    }

    // 5. Audio track 2: microphone (only when split audio tracks is enabled)
    if (p.splitAudio) {
        if (!addAudioStream(48000, 16000 /* 128 kbps */, &m_micAudioStreamIndex, "mic audio")) {
            return false;
        }
    }

    hr = m_sinkWriter->BeginWriting();
    if (FAILED(hr)) { error = "BeginWriting: " + hrToString(hr); return false; }
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
    // Without split tracks the microphone is mixed into the game track by ReplayManager
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

// ---------------------------------------------------------------------------
// Segment merge (remux, no re-encode)
// ---------------------------------------------------------------------------

namespace {

ConcatResult concatenateImpl(
    const std::vector<std::wstring>& segmentPaths,
    const std::wstring& finalOutputPath,
    double trimStartSeconds
) {
    ConcatResult res;
    auto fail = [&res](std::string msg) -> ConcatResult& {
        res.success = false;
        res.error = std::move(msg);
        return res;
    };

    if (segmentPaths.empty()) return fail("no recorded segments");

    ComRef<IMFAttributes> readerAttr;
    HRESULT hr = MFCreateAttributes(readerAttr.put(), 1);
    if (FAILED(hr)) return fail("MFCreateAttributes: " + hrToString(hr));
    readerAttr->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, FALSE);

    // 1. Discover the stream layout from the first segment
    struct StreamInfo { DWORD readerIndex; DWORD writerIndex; bool isVideo; };
    std::vector<StreamInfo> streams;

    ComRef<IMFSinkWriter> writer;
    {
        ComRef<IMFSourceReader> firstReader;
        hr = MFCreateSourceReaderFromURL(segmentPaths[0].c_str(), readerAttr.get(), firstReader.put());
        if (FAILED(hr) || !firstReader) return fail("cannot open first segment: " + hrToString(hr));

        ComRef<IMFAttributes> writerAttr;
        hr = MFCreateAttributes(writerAttr.put(), 1);
        if (FAILED(hr)) return fail("MFCreateAttributes: " + hrToString(hr));
        writerAttr->SetUINT32(MF_SINK_WRITER_DISABLE_THROTTLING, TRUE);

        hr = MFCreateSinkWriterFromURL(finalOutputPath.c_str(), NULL, writerAttr.get(), writer.put());
        if (FAILED(hr) || !writer) return fail("cannot create output file: " + hrToString(hr));

        for (DWORD s = 0; s < 16; ++s) {
            ComRef<IMFMediaType> type;
            hr = firstReader->GetNativeMediaType(s, 0, type.put());
            if (hr == MF_E_INVALIDSTREAMNUMBER) break;
            if (FAILED(hr) || !type) return fail("cannot read stream type: " + hrToString(hr));

            GUID major = GUID_NULL;
            type->GetGUID(MF_MT_MAJOR_TYPE, &major);

            DWORD outIndex = 0;
            hr = writer->AddStream(type.get(), &outIndex);
            if (FAILED(hr)) return fail("cannot add output stream: " + hrToString(hr));
            hr = writer->SetInputMediaType(outIndex, type.get(), NULL);
            if (FAILED(hr)) return fail("cannot configure output stream: " + hrToString(hr));
            streams.push_back({s, outIndex, major == MFMediaType_Video});
        }
        if (streams.empty()) return fail("first segment has no streams");

        hr = writer->BeginWriting();
        if (FAILED(hr)) return fail("BeginWriting: " + hrToString(hr));
    }

    // 2. Find the cut point inside the first segment: prefer the nearest video keyframe
    //    at or before trimHns so the output begins with a decodable frame, while never
    //    leaving a multi-second prefix untrimmed if keyframe markers are sparse.
    LONGLONG cutHns = 0;
    const LONGLONG trimHns = static_cast<LONGLONG>(std::max(0.0, trimStartSeconds) * 1e7);
    auto videoIt = std::find_if(streams.begin(), streams.end(), [](const StreamInfo& s) { return s.isVideo; });
    if (trimHns > 0 && videoIt != streams.end()) {
        ComRef<IMFSourceReader> scan;
        hr = MFCreateSourceReaderFromURL(segmentPaths[0].c_str(), readerAttr.get(), scan.put());
        if (FAILED(hr) || !scan) return fail("cannot open first segment: " + hrToString(hr));
        scan->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS), FALSE);
        scan->SetStreamSelection(videoIt->readerIndex, TRUE);
        LONGLONG lastKeyBefore = -1;
        LONGLONG firstKeyAfter = -1;
        int emptyScans = 0;
        while (true) {
            DWORD flags = 0;
            LONGLONG ts = 0;
            ComRef<IMFSample> sample;
            hr = scan->ReadSample(videoIt->readerIndex, 0, nullptr, &flags, &ts, sample.put());
            if (FAILED(hr) || (flags & MF_SOURCE_READERF_ERROR)) return fail("cannot scan first segment: " + hrToString(hr));
            if (sample) {
                UINT32 clean = 0;
                bool isKey = SUCCEEDED(sample->GetUINT32(MFSampleExtension_CleanPoint, &clean)) && clean != 0;
                if (ts <= trimHns) {
                    if (isKey) lastKeyBefore = ts;
                } else {
                    if (isKey && firstKeyAfter < 0) {
                        firstKeyAfter = ts;
                        break;
                    }
                    if (ts - trimHns > 20000000LL) break;
                }
                emptyScans = 0;
            } else if (++emptyScans > 100000) {
                return fail("reader stalled while scanning first segment");
            }
            if (flags & MF_SOURCE_READERF_ENDOFSTREAM) break;
        }

        constexpr LONGLONG kMaxKeyframeDriftHns = 20000000LL; // 2s
        if (lastKeyBefore >= 0 && (trimHns - lastKeyBefore) <= kMaxKeyframeDriftHns) {
            cutHns = lastKeyBefore;
        } else if (firstKeyAfter >= 0 && (firstKeyAfter - trimHns) <= kMaxKeyframeDriftHns) {
            cutHns = firstKeyAfter;
        } else {
            cutHns = trimHns;
        }
    } else if (trimHns > 0) {
        cutHns = trimHns;
    }

    // 3. Remux every segment, rebasing timestamps so they continue seamlessly
    std::map<DWORD, LONGLONG> lastTimes;
    LONGLONG outputBase = 0;
    size_t samplesWritten = 0;

    for (size_t segIdx = 0; segIdx < segmentPaths.size(); ++segIdx) {
        const auto& path = segmentPaths[segIdx];
        const std::string segLabel = "segment " + std::to_string(segIdx + 1) + "/" + std::to_string(segmentPaths.size());

        ComRef<IMFSourceReader> reader;
        hr = MFCreateSourceReaderFromURL(path.c_str(), readerAttr.get(), reader.put());
        if (FAILED(hr) || !reader) return fail("cannot open " + segLabel + ": " + hrToString(hr));

        reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS), FALSE);
        std::map<DWORD, DWORD> streamMap;
        std::set<DWORD> remaining;
        for (const auto& st : streams) {
            ComRef<IMFMediaType> native;
            hr = reader->GetNativeMediaType(st.readerIndex, 0, native.put());
            if (FAILED(hr) || !native) return fail(segLabel + " is missing a stream: " + hrToString(hr));
            reader->SetCurrentMediaType(st.readerIndex, NULL, native.get());
            hr = reader->SetStreamSelection(st.readerIndex, TRUE);
            if (FAILED(hr)) return fail("cannot select stream in " + segLabel + ": " + hrToString(hr));
            streamMap[st.readerIndex] = st.writerIndex;
            remaining.insert(st.readerIndex);
        }

        const LONGLONG segCut = (segIdx == 0) ? cutHns : 0;
        LONGLONG segEnd = 0;
        int emptyReads = 0;

        while (!remaining.empty()) {
            DWORD actualStream = 0;
            DWORD flags = 0;
            LONGLONG timestamp = 0;
            ComRef<IMFSample> sample;
            hr = reader->ReadSample(
                static_cast<DWORD>(MF_SOURCE_READER_ANY_STREAM), 0, &actualStream, &flags, &timestamp, sample.put());
            if (FAILED(hr) || (flags & MF_SOURCE_READERF_ERROR)) {
                return fail("cannot read " + segLabel + ": " + hrToString(hr));
            }
            if (flags & MF_SOURCE_READERF_ENDOFSTREAM) {
                remaining.erase(actualStream);
            }
            if (!sample) {
                if (++emptyReads > 100000) return fail("reader stalled on " + segLabel);
                continue;
            }
            emptyReads = 0;

            auto mapIt = streamMap.find(actualStream);
            if (mapIt == streamMap.end()) continue;
            if (timestamp < segCut) continue; // trimmed away (before the replay start)

            LONGLONG sampleDuration = 0;
            sample->GetSampleDuration(&sampleDuration);

            LONGLONG sampleTime = timestamp - segCut + outputBase;
            auto lastIt = lastTimes.find(actualStream);
            if (lastIt != lastTimes.end() && sampleTime <= lastIt->second) {
                sampleTime = lastIt->second + 1000LL;
            }
            sample->SetSampleTime(sampleTime);

            UINT64 dts = 0;
            if (SUCCEEDED(sample->GetUINT64(MFSampleExtension_DecodeTimestamp, &dts))) {
                LONGLONG newDts = static_cast<LONGLONG>(dts) - segCut + outputBase;
                sample->SetUINT64(MFSampleExtension_DecodeTimestamp, static_cast<UINT64>(newDts));
            }

            hr = writer->WriteSample(mapIt->second, sample.get());
            if (FAILED(hr)) {
                return fail("write failed (" + segLabel + "): " + hrToString(hr));
            }
            ++samplesWritten;
            lastTimes[actualStream] = sampleTime;
            segEnd = std::max(segEnd, timestamp - segCut + sampleDuration);
        }

        outputBase += segEnd;
    }

    if (samplesWritten == 0) return fail("no media samples were written");

    hr = writer->Finalize();
    if (FAILED(hr)) return fail("finalizing MP4 failed: " + hrToString(hr));

    res.success = true;
    res.durationSeconds = static_cast<double>(outputBase) / 1e7;
    return res;
}

} // namespace

ConcatResult MediaFoundationEncoder::concatenateSegments(
    const std::vector<std::wstring>& segmentPaths,
    const std::wstring& finalOutputPath,
    double trimStartSeconds
) {
    ConcatResult res;
    try {
        res = concatenateImpl(segmentPaths, finalOutputPath, trimStartSeconds);
    } catch (const std::exception& e) {
        res.success = false;
        res.error = std::string("exception: ") + e.what();
    }
    if (!res.success) {
        // Never leave a broken/partial MP4 behind
        DeleteFileW(finalOutputPath.c_str());
    }
    return res;
}

} // namespace replay
