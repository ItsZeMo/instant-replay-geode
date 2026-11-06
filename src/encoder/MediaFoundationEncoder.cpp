#include "MediaFoundationEncoder.hpp"

#ifdef GEODE_IS_WINDOWS

#include <codecapi.h>
#include <filesystem>
#include <algorithm>
#include <map>
#include <set>
#include <cstdio>

namespace replay {

namespace {

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

// Session-wide cache of the working subtype for a given codec request so segment
// rotation every 30s does not re-probe unsupported codecs (e.g. AV1 on non-AV1 GPUs).
// Software fallback from a transient hardware failure is intentionally NOT cached (#8).
std::mutex s_codecCacheMutex;
std::string s_cachedRequestKey;
GUID s_cachedSubtype = MFVideoFormat_H264;
bool s_cachedHardware = true;
bool s_hasCachedCodec = false;

} // namespace

static std::once_flag s_mfInitOnce;

void MediaFoundationEncoder::globalInitialize() {
    std::call_once(s_mfInitOnce, []() {
        CoInitializeEx(NULL, COINIT_MULTITHREADED);
        MFStartup(MF_VERSION);
    });
}

void MediaFoundationEncoder::resetCodecCache() {
    std::lock_guard<std::mutex> cacheLock(s_codecCacheMutex);
    s_cachedRequestKey.clear();
    s_hasCachedCodec = false;
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

    char reqKeyBuf[200];
    snprintf(reqKeyBuf, sizeof(reqKeyBuf), "%d|%d|%s|%dx%d@%d",
             static_cast<int>(codec), enableHardware ? 1 : 0, cname.c_str(), width, height, fps);
    std::string requestKey = reqKeyBuf;

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

    bool usedCache = false;
    {
        std::lock_guard<std::mutex> cacheLock(s_codecCacheMutex);
        if (s_hasCachedCodec && s_cachedRequestKey == requestKey && s_cachedHardware == enableHardware) {
            addAttempt(s_cachedSubtype, s_cachedHardware);
            usedCache = true;
        }
    }

    addAttempt(targetSubtype, enableHardware);
    if (targetSubtype == MFVideoFormat_AV1) {
        addAttempt(MFVideoFormat_HEVC, enableHardware);
    }
    addAttempt(MFVideoFormat_H264, enableHardware);
    if (enableHardware) {
        addAttempt(MFVideoFormat_H264, false);
    }

    // Microsoft's AAC MFT only accepts 44100 or 48000 Hz (#18).
    int aacGameSampleRate = (gameSampleRate == 44100) ? 44100 : 48000;

    StartParams params;
    params.width = width;
    params.height = height;
    params.fps = fps;
    params.bitrateMbps = bitrateMbps;
    params.maxBitrateMbps = std::max(maxBitrateMbps, bitrateMbps);
    params.qualityLevel = qualityLevel;
    params.rateControl = rateControl;
    params.splitAudio = splitAudio;
    params.gameSampleRate = aacGameSampleRate;

    for (size_t i = 0; i < attempts.size(); ++i) {
        const auto& attempt = attempts[i];
        std::string error;
        if (tryStartLocked(outputPath, attempt.subtype, attempt.hardware, params, error)) {
            std::string activeCodecName = std::string(subtypeName(attempt.subtype)) + (attempt.hardware ? " (HW)" : " (SW)");
            char key[180];
            snprintf(key, sizeof(key), "%s|%s|%dx%d|%d|a%d|s%d",
                     subtypeName(attempt.subtype), attempt.hardware ? "hw" : "sw",
                     width, height, fps, params.gameSampleRate, splitAudio ? 1 : 0);
            m_formatKey = key;
            m_recording = true;

            // Only cache when hardware mode matches requested mode so a transient HW failure
            // (e.g. NVENC session limit during segment rotation) never pins the session to SW (#8).
            if (attempt.hardware == enableHardware) {
                std::lock_guard<std::mutex> cacheLock(s_codecCacheMutex);
                s_cachedRequestKey = requestKey;
                s_cachedSubtype = attempt.subtype;
                s_cachedHardware = attempt.hardware;
                s_hasCachedCodec = true;
            }

            if (!usedCache) {
                if (attempt.subtype != targetSubtype || attempt.hardware != enableHardware) {
                    geode::log::warn("Instant Replay: requested codec unavailable, fell back to {}", activeCodecName);
                } else {
                    geode::log::info("Instant Replay: encoder started with {}", activeCodecName);
                }
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
    const bool hardware,
    const StartParams& p,
    std::string& error
) {
    ComRef<IMFAttributes> attr;
    HRESULT hr = MFCreateAttributes(attr.put(), 3);
    if (FAILED(hr)) { error = "MFCreateAttributes: " + hrToString(hr); return false; }
    attr->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, hardware ? TRUE : FALSE);
    // Note: MF_SINK_WRITER_DISABLE_THROTTLING is intentionally NOT set here so IMFSinkWriter
    // throttles the worker thread instead of growing an unbounded internal queue when overloaded (#14).
    attr->SetUINT32(MF_LOW_LATENCY, TRUE);

    hr = MFCreateSinkWriterFromURL(outputPath.c_str(), NULL, attr.get(), &m_sinkWriter);
    if (FAILED(hr) || !m_sinkWriter) { error = "MFCreateSinkWriterFromURL: " + hrToString(hr); return false; }

    // Bound GOP size to at most 60 frames so keyframes stay frequent even if the game renders slower than p.fps (#17)
    const UINT32 gopFrames = static_cast<UINT32>(std::clamp(p.fps, 15, 60));

    // 1. Video output stream
    {
        ComRef<IMFMediaType> videoOut;
        hr = MFCreateMediaType(videoOut.put());
        if (FAILED(hr)) { error = "MFCreateMediaType: " + hrToString(hr); return false; }
        videoOut->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        videoOut->SetGUID(MF_MT_SUBTYPE, subtype);
        videoOut->SetUINT32(MF_MT_AVG_BITRATE, static_cast<UINT32>(p.bitrateMbps) * 1000000u);
        videoOut->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        videoOut->SetUINT32(MF_MT_MAX_KEYFRAME_SPACING, gopFrames);
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
        ComRef<ICodecAPI> codecApi;
        hr = m_sinkWriter->GetServiceForStream(m_videoStreamIndex, GUID_NULL, __uuidof(ICodecAPI), (void**)codecApi.put());
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
            var.ulVal = static_cast<ULONG>(gopFrames);
            codecApi->SetValue(&CODECAPI_AVEncMPVGOPSize, &var);
        }
    }

    auto addAudioStream = [this, &error](int sampleRate, UINT32 avgBytesPerSec, DWORD* streamIndex, const char* label) -> bool {
        int validRate = (sampleRate == 44100) ? 44100 : 48000;
        ComRef<IMFMediaType> audioOut;
        HRESULT h = MFCreateMediaType(audioOut.put());
        if (FAILED(h)) { error = "MFCreateMediaType: " + hrToString(h); return false; }
        audioOut->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        audioOut->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC);
        audioOut->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
        audioOut->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, validRate);
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
        audioIn->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, validRate);
        audioIn->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2);
        audioIn->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, 4);
        audioIn->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, validRate * 4);
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
    IMFMediaBuffer* videoBuffer,
    int64_t timestampHns,
    IMFSample** ppSample
) {
    if (!videoBuffer || !ppSample) return false;

    HRESULT hr = MFCreateSample(ppSample);
    if (SUCCEEDED(hr) && *ppSample) {
        (*ppSample)->AddBuffer(videoBuffer);
        (*ppSample)->SetSampleTime(timestampHns);
        (*ppSample)->SetSampleDuration(m_videoFrameDurationHns);
        return true;
    }
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
    ComRef<IMFMediaBuffer> buffer;
    HRESULT hr = MFCreateMemoryBuffer(bufferSize, buffer.put());
    if (FAILED(hr) || !buffer) return false;

    BYTE* pDst = nullptr;
    hr = buffer->Lock(&pDst, NULL, NULL);
    if (SUCCEEDED(hr) && pDst) {
        int16_t* pSamples16 = reinterpret_cast<int16_t*>(pDst);
        size_t total = static_cast<size_t>(numSamples) * channels;
        for (size_t i = 0; i < total; ++i) {
            float s = std::clamp(pcmFloat[i], -1.0f, 1.0f);
            pSamples16[i] = static_cast<int16_t>(s * 32767.0f);
        }
        buffer->Unlock();
        buffer->SetCurrentLength(bufferSize);

        hr = MFCreateSample(ppSample);
        if (SUCCEEDED(hr) && *ppSample) {
            (*ppSample)->AddBuffer(buffer.get());
            (*ppSample)->SetSampleTime(timestampHns);
            int rate = (sampleRate > 0) ? sampleRate : 44100;
            int64_t durationHns = (static_cast<int64_t>(numSamples) * 10000000LL) / rate;
            (*ppSample)->SetSampleDuration(durationHns);
            return true;
        }
    }
    return false;
}

bool MediaFoundationEncoder::writeVideoFrame(
    IMFMediaBuffer* videoBuffer,
    int width,
    int height,
    int64_t timestampHns
) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_recording || !m_sinkWriter || !videoBuffer) return false;
    if (width != m_width || height != m_height) return false;

    ComRef<IMFSample> sample;
    if (createVideoSample(videoBuffer, timestampHns, sample.put())) {
        HRESULT hr = m_sinkWriter->WriteSample(m_videoStreamIndex, sample.get());
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

    ComRef<IMFSample> sample;
    if (createAudioSample(pcmFloat, numSamples, channels, sampleRate, timestampHns, sample.put())) {
        HRESULT hr = m_sinkWriter->WriteSample(m_gameAudioStreamIndex, sample.get());
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

    ComRef<IMFSample> sample;
    if (createAudioSample(pcmFloat, numSamples, channels, sampleRate, timestampHns, sample.put())) {
        HRESULT hr = m_sinkWriter->WriteSample(m_micAudioStreamIndex, sample.get());
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

    // 2. Find the cut point inside the first segment: always cut on a video keyframe
    //    so the output begins with a decodable IDR/keyframe (#17).
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
                    if (ts - trimHns > 50000000LL) break; // scan up to 5s past trimHns
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
        } else if (lastKeyBefore >= 0) {
            cutHns = lastKeyBefore;
        } else if (firstKeyAfter >= 0) {
            cutHns = firstKeyAfter;
        } else {
            cutHns = trimHns;
        }
    } else if (trimHns > 0) {
        cutHns = trimHns;
    }

    // 3. Remux every segment, rebasing timestamps so they continue seamlessly
    std::map<DWORD, LONGLONG> lastAudioTimes;
    LONGLONG outputBase = 0;
    size_t samplesWritten = 0;

    for (size_t segIdx = 0; segIdx < segmentPaths.size(); ++segIdx) {
        const auto& path = segmentPaths[segIdx];
        const std::string segLabel = "segment " + std::to_string(segIdx + 1) + "/" + std::to_string(segmentPaths.size());

        ComRef<IMFSourceReader> reader;
        hr = MFCreateSourceReaderFromURL(path.c_str(), readerAttr.get(), reader.put());
        if (FAILED(hr) || !reader) return fail("cannot open " + segLabel + ": " + hrToString(hr));

        reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS), FALSE);
        struct MappedStream { DWORD writerIndex; bool isVideo; };
        std::map<DWORD, MappedStream> streamMap;
        std::set<DWORD> remaining;
        for (const auto& st : streams) {
            ComRef<IMFMediaType> native;
            hr = reader->GetNativeMediaType(st.readerIndex, 0, native.put());
            if (FAILED(hr) || !native) return fail(segLabel + " is missing a stream: " + hrToString(hr));
            reader->SetCurrentMediaType(st.readerIndex, NULL, native.get());
            hr = reader->SetStreamSelection(st.readerIndex, TRUE);
            if (FAILED(hr)) return fail("cannot select stream in " + segLabel + ": " + hrToString(hr));
            streamMap[st.readerIndex] = {st.writerIndex, st.isVideo};
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

            // Preserve relative presentation timestamps on video streams so B-frame decode order
            // is never clobbered; only enforce strict monotonicity on audio streams (#19).
            LONGLONG sampleTime = std::max<LONGLONG>(0, timestamp - segCut + outputBase);
            if (!mapIt->second.isVideo) {
                auto lastIt = lastAudioTimes.find(actualStream);
                if (lastIt != lastAudioTimes.end() && sampleTime <= lastIt->second) {
                    sampleTime = lastIt->second + 1000LL;
                }
                lastAudioTimes[actualStream] = sampleTime;
            }
            sample->SetSampleTime(sampleTime);

            UINT64 dts = 0;
            if (SUCCEEDED(sample->GetUINT64(MFSampleExtension_DecodeTimestamp, &dts))) {
                LONGLONG newDts = std::max<LONGLONG>(0, static_cast<LONGLONG>(dts) - segCut + outputBase);
                sample->SetUINT64(MFSampleExtension_DecodeTimestamp, static_cast<UINT64>(newDts));
            }

            hr = writer->WriteSample(mapIt->second.writerIndex, sample.get());
            if (FAILED(hr)) {
                return fail("write failed (" + segLabel + "): " + hrToString(hr));
            }
            ++samplesWritten;
            segEnd = std::max(segEnd, timestamp - segCut + std::max<LONGLONG>(sampleDuration, 1000LL));
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

#endif // GEODE_IS_WINDOWS
