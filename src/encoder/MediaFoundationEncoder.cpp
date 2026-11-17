#include "MediaFoundationEncoder.hpp"
#include "ConcatTimestampMath.hpp"
#include "MfSegmentConcat.hpp"

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
    if (hr == HRESULT_FROM_WIN32(ERROR_DISK_FULL) ||
        hr == HRESULT_FROM_WIN32(ERROR_HANDLE_DISK_FULL) ||
        hr == STG_E_MEDIUMFULL) {
        return "disk is full";
    }
    if (hr == HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED) ||
        hr == E_ACCESSDENIED ||
        hr == STG_E_ACCESSDENIED) {
        return "access denied";
    }
    if (hr == STG_E_DISKISWRITEPROTECTED ||
        hr == HRESULT_FROM_WIN32(ERROR_WRITE_PROTECT)) {
        return "drive is write-protected";
    }
    char buf[32];
    snprintf(buf, sizeof(buf), "HRESULT 0x%08X", static_cast<unsigned>(hr));
    return buf;
}

const char* subtypeName(const GUID& subtype) noexcept {
    if (subtype == MFVideoFormat_AV1) return "AV1";
    if (subtype == MFVideoFormat_HEVC) return "HEVC";
    if (subtype == MFVideoFormat_VP90) return "VP9";
    return "H.264";
}

GUID guidForCodecFamily(VideoCodecFamily family) noexcept {
    switch (family) {
        case VideoCodecFamily::AV1:  return MFVideoFormat_AV1;
        case VideoCodecFamily::HEVC: return MFVideoFormat_HEVC;
        case VideoCodecFamily::VP9:  return MFVideoFormat_VP90;
        case VideoCodecFamily::H264:
        default:                     return MFVideoFormat_H264;
    }
}

VideoCodecFamily familyForGuid(const GUID& subtype) noexcept {
    if (subtype == MFVideoFormat_AV1) return VideoCodecFamily::AV1;
    if (subtype == MFVideoFormat_HEVC) return VideoCodecFamily::HEVC;
    if (subtype == MFVideoFormat_VP90) return VideoCodecFamily::VP9;
    return VideoCodecFamily::H264;
}

EncoderOpResult makeMfFailure(HRESULT hr, std::string msg) {
    if (msg.empty()) msg = hrToString(hr);
    return {false, hr, std::move(msg), MediaFoundationEncoder::isFatalStorageError(hr)};
}

bool isPermanentCodecUnsupported(HRESULT hr) noexcept {
    return hr == MF_E_TOPO_CODEC_NOT_FOUND ||
           hr == MF_E_INVALIDMEDIATYPE ||
           hr == MF_E_UNSUPPORTED_RATE ||
           hr == REGDB_E_CLASSNOTREG ||
           hr == E_NOTIMPL;
}

const char* requestedRateControlName(RateControlMode mode) noexcept {
    switch (mode) {
        case RateControlMode::ADAPTIVE_VBR: return "AdaptiveVBR";
        case RateControlMode::CONSTANT_CBR: return "CBR";
        case RateControlMode::CONSTANT_QUALITY: return "Quality";
        default: return "Unknown";
    }
}

const char* rateControlModeName(ULONG mode) noexcept {
    switch (mode) {
        case eAVEncCommonRateControlMode_CBR: return "CBR";
        case eAVEncCommonRateControlMode_PeakConstrainedVBR: return "PeakConstrainedVBR";
        case eAVEncCommonRateControlMode_UnconstrainedVBR: return "UnconstrainedVBR";
        case eAVEncCommonRateControlMode_Quality: return "Quality";
        default: return "Other";
    }
}

// Session-wide codec cache: avoids re-probing missing hardware codecs every 30s,
// while never pinning transient HW/session-limit failures to a lower codec or SW (#9, #10).
std::mutex s_codecCacheMutex;
std::string s_cachedRequestKey;
std::string s_confirmedPrimaryKey;
std::string s_lastLoggedCodecKey;
std::string s_lastLoggedRcKey;
GUID s_cachedSubtype = MFVideoFormat_H264;
bool s_cachedHardware = true;
bool s_hasCachedCodec = false;
// Encoders (subtype + HW/SW) that refused the explicit BT.709 media type attributes (W12).
std::set<std::string> s_colorTagRejects;

std::string colorTagKey(const GUID& subtype, bool hardware) {
    return std::string(subtypeName(subtype)) + (hardware ? "|hw" : "|sw");
}

} // namespace

static std::once_flag s_mfInitOnce;

void MediaFoundationEncoder::globalInitialize() {
    std::call_once(s_mfInitOnce, []() {
        MFStartup(MF_VERSION);
    });
}

void MediaFoundationEncoder::resetCodecCache() {
    std::lock_guard<std::mutex> cacheLock(s_codecCacheMutex);
    s_cachedRequestKey.clear();
    s_confirmedPrimaryKey.clear();
    s_lastLoggedCodecKey.clear();
    s_lastLoggedRcKey.clear();
    s_hasCachedCodec = false;
}

bool MediaFoundationEncoder::isFatalStorageError(HRESULT hr) noexcept {
    return hr == HRESULT_FROM_WIN32(ERROR_DISK_FULL) ||
           hr == HRESULT_FROM_WIN32(ERROR_HANDLE_DISK_FULL) ||
           hr == HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED) ||
           hr == HRESULT_FROM_WIN32(ERROR_WRITE_PROTECT) ||
           hr == E_ACCESSDENIED ||
           hr == STG_E_MEDIUMFULL ||
           hr == STG_E_ACCESSDENIED ||
           hr == STG_E_DISKISWRITEPROTECTED;
}

std::string MediaFoundationEncoder::formatHr(HRESULT hr) {
    return hrToString(hr);
}

VideoCodecFamily MediaFoundationEncoder::activeCodecFamily() const noexcept {
    return familyForGuid(m_activeSubtype);
}

std::string MediaFoundationEncoder::activeCodecName() const {
    if (m_activeSubtype == GUID_NULL) return "encoder";
    return std::string(subtypeName(m_activeSubtype)) + (m_activeHardware ? " (HW)" : " (SW)");
}

MediaFoundationEncoder::MediaFoundationEncoder() {
    globalInitialize();
}

MediaFoundationEncoder::~MediaFoundationEncoder() {
    finish();
}

EncoderOpResult MediaFoundationEncoder::start(
    const std::filesystem::path& outputPathFs,
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
    int gameSampleRate,
    const std::function<void()>& onBeforeFallback,
    bool allowFallback,
    bool hasForcedCodec,
    VideoCodecFamily forcedFamily,
    bool forcedHardware
) {
    const std::wstring outputPath = outputPathFs.wstring();
    std::lock_guard<std::mutex> lock(m_mutex);
    finishLocked();

    fps = std::max(1, fps);
    int aacGameSampleRate = (gameSampleRate == 44100) ? 44100 : 48000;

    m_width = width;
    m_height = height;
    m_fps = fps;
    m_gameSampleRate = aacGameSampleRate;
    m_splitAudio = splitAudio;
    m_videoFrameDurationHns = 10000000LL / fps;
    m_formatKey.clear();
    m_activeSubtype = GUID_NULL;
    m_activeHardware = hardwareAcceleration;
    m_activeBitrateMbps = 0;
    m_activeMaxBitrateMbps = 0;
    m_gamePrerollWritten = false;
    m_micPrerollWritten = false;

    bool enableHardware = hardwareAcceleration;
    const CodecTargetSpec resolvedSpec = resolveRequestedCodecSpec(
        static_cast<int>(codec), hardwareAcceleration, customCodec);
    enableHardware = resolvedSpec.hardware;
    GUID targetSubtype = guidForCodecFamily(resolvedSpec.family);

    std::string cname = customCodec;
    std::transform(cname.begin(), cname.end(), cname.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    if (hasForcedCodec) {
        targetSubtype = guidForCodecFamily(forcedFamily);
        enableHardware = forcedHardware;
    }

    char reqKeyBuf[200];
    snprintf(reqKeyBuf, sizeof(reqKeyBuf), "%d|%d|%s|%dx%d@%d",
             static_cast<int>(codec), enableHardware ? 1 : 0, cname.c_str(), width, height, fps);
    std::string requestKey = reqKeyBuf;

    struct Attempt { GUID subtype; bool hardware; };
    std::vector<Attempt> attempts;
    auto addAttempt = [&attempts](const GUID& subtype, bool hw) {
        for (const auto& a : attempts) {
            if (a.subtype == subtype && a.hardware == hw) return;
        }
        attempts.push_back({subtype, hw});
    };

    if (!hasForcedCodec) {
        std::lock_guard<std::mutex> cacheLock(s_codecCacheMutex);
        if (s_hasCachedCodec && s_cachedRequestKey == requestKey && s_cachedHardware == enableHardware) {
            addAttempt(s_cachedSubtype, s_cachedHardware);
        }
    }

    addAttempt(targetSubtype, enableHardware);
    if (allowFallback && !hasForcedCodec) {
        if (targetSubtype == MFVideoFormat_AV1) {
            addAttempt(MFVideoFormat_HEVC, enableHardware);
        }
        addAttempt(MFVideoFormat_H264, enableHardware);
        if (enableHardware) {
            addAttempt(MFVideoFormat_H264, false);
        }
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
    params.gameSampleRate = aacGameSampleRate;

    auto cleanupFailedAttempt = [this, &outputPath]() {
        if (m_sinkWriter) {
            m_sinkWriter->Release();
            m_sinkWriter = nullptr;
        }
        DeleteFileW(outputPath.c_str());
    };

    bool primaryFailedPermanently = false;
    bool syncedBeforeFallback = false;
    HRESULT lastHr = E_FAIL;
    std::string lastErrMsg;

    for (size_t i = 0; i < attempts.size(); ++i) {
        const auto& attempt = attempts[i];
        bool isPrimary = (attempt.subtype == targetSubtype && attempt.hardware == enableHardware);

        // Each cascade attempt gets the bitrate an explicit selection of its codec family would get,
        // instead of reusing the requested family's value (AV1 x0.70 starving an HEVC/H.264 fallback, W1).
        StartParams attemptParams = params;
        if (m_familyBitrates.valid) {
            const EffectiveBitrates familyRates = m_familyBitrates.forFamily(familyForGuid(attempt.subtype));
            if (familyRates.meanMbps > 0) {
                attemptParams.bitrateMbps = familyRates.meanMbps;
                attemptParams.maxBitrateMbps = std::max(familyRates.maxMbps, familyRates.meanMbps);
            }
        }

        {
            std::lock_guard<std::mutex> cacheLock(s_codecCacheMutex);
            attemptParams.typeExtensions = (s_colorTagRejects.count(colorTagKey(attempt.subtype, attempt.hardware)) == 0);
        }

        HRESULT failHr = S_OK;
        std::string error;
        bool started = tryStartLocked(outputPath, attempt.subtype, attempt.hardware, attemptParams, failHr, error);

        // Explicit color metadata is accepted by the Microsoft SW and NVENC encoders, but an unknown
        // MFT could reject it: retry the same codec once without it before falling back (W12).
        if (!started && attemptParams.typeExtensions && !isFatalStorageError(failHr)) {
            cleanupFailedAttempt();
            StartParams untagged = attemptParams;
            untagged.typeExtensions = false;
            HRESULT untaggedHr = S_OK;
            std::string untaggedError;
            if (tryStartLocked(outputPath, attempt.subtype, attempt.hardware, untagged, untaggedHr, untaggedError)) {
                started = true;
                attemptParams = untagged;
                {
                    std::lock_guard<std::mutex> cacheLock(s_codecCacheMutex);
                    s_colorTagRejects.insert(colorTagKey(attempt.subtype, attempt.hardware));
                }
                geode::log::warn("Instant Replay: {} ({}) rejected the BT.709 / profile media type extensions ({}), recording without them",
                                 subtypeName(attempt.subtype), attempt.hardware ? "HW" : "SW", error);
                failHr = S_OK;
                error.clear();
            } else {
                failHr = untaggedHr;
                error = std::move(untaggedError);
            }
        }

        // If the primary hardware codec fails transiently (e.g. concurrent NVENC session limit while
        // the previous segment is still finalizing in the background), wait for background finalization
        // to release the old encoder session and retry the primary codec before falling back (#9, #10).
        if (!started && isPrimary && !isPermanentCodecUnsupported(failHr) && !isFatalStorageError(failHr) &&
            onBeforeFallback && !syncedBeforeFallback) {
            syncedBeforeFallback = true;
            cleanupFailedAttempt();
            onBeforeFallback();
            failHr = S_OK;
            error.clear();
            started = tryStartLocked(outputPath, attempt.subtype, attempt.hardware, attemptParams, failHr, error);
        }

        if (started) {
            std::string codecLabel = std::string(subtypeName(attempt.subtype)) + (attempt.hardware ? " (HW)" : " (SW)");
            char key[180];
            snprintf(key, sizeof(key), "%s|%s|%dx%d|%d|a%d|s%d|pre%lld|c%d",
                     subtypeName(attempt.subtype), attempt.hardware ? "hw" : "sw",
                     width, height, fps, params.gameSampleRate, splitAudio ? 1 : 0,
                     static_cast<long long>(kSegmentAudioPrerollHns / 10000), attemptParams.typeExtensions ? 709 : 0);
            m_formatKey = key;
            m_activeSubtype = attempt.subtype;
            m_activeHardware = attempt.hardware;
            m_activeBitrateMbps = attemptParams.bitrateMbps;
            m_activeMaxBitrateMbps = attemptParams.maxBitrateMbps;
            if (m_readbackMeanMbps > 0) {
                m_activeBitrateMbps = std::min(m_activeBitrateMbps, m_readbackMeanMbps);
            }
            if (m_readbackMaxMbps > 0) {
                m_activeMaxBitrateMbps = std::min(m_activeMaxBitrateMbps, m_readbackMaxMbps);
            }
            m_recording = true;

            {
                std::lock_guard<std::mutex> cacheLock(s_codecCacheMutex);
                if (!hasForcedCodec) {
                    if (isPrimary) {
                        s_cachedRequestKey = requestKey;
                        s_confirmedPrimaryKey = requestKey;
                        s_cachedSubtype = attempt.subtype;
                        s_cachedHardware = attempt.hardware;
                        s_hasCachedCodec = true;
                    } else if (attempt.hardware == enableHardware &&
                               primaryFailedPermanently &&
                               s_confirmedPrimaryKey != requestKey) {
                        // Only cache a fallback codec (e.g. AV1 -> HEVC) when the primary codec is
                        // genuinely unsupported on this GPU and has never succeeded in this session (#9).
                        s_cachedRequestKey = requestKey;
                        s_cachedSubtype = attempt.subtype;
                        s_cachedHardware = attempt.hardware;
                        s_hasCachedCodec = true;
                    }
                }

                // Rotations restart the encoder every 30 s: only a changed codec / size / bitrate is worth
                // an INFO line, repeats go to debug (W8, Windows counterpart of macOS N5).
                char rateKey[48];
                snprintf(rateKey, sizeof(rateKey), "|%d/%d", attemptParams.bitrateMbps, attemptParams.maxBitrateMbps);
                const std::string logKey = requestKey + "->" + codecLabel + rateKey;
                const bool firstStartForKey = (s_lastLoggedCodecKey != logKey);
                s_lastLoggedCodecKey = logKey;
                if (!isPrimary && !hasForcedCodec && firstStartForKey) {
                    geode::log::warn(
                        "Instant Replay: requested codec unavailable, fell back to {} ({}x{} @ {} FPS, effBitrate={} Mbps, effMaxBitrate={} Mbps)",
                        codecLabel, width, height, fps, attemptParams.bitrateMbps, attemptParams.maxBitrateMbps);
                } else if (firstStartForKey) {
                    geode::log::info(
                        "Instant Replay: encoder started with {} ({}x{} @ {} FPS, effBitrate={} Mbps, effMaxBitrate={} Mbps)",
                        codecLabel, width, height, fps, attemptParams.bitrateMbps, attemptParams.maxBitrateMbps);
                } else {
                    geode::log::debug(
                        "Instant Replay: encoder started with {} ({}x{} @ {} FPS, effBitrate={} Mbps, effMaxBitrate={} Mbps)",
                        codecLabel, width, height, fps, attemptParams.bitrateMbps, attemptParams.maxBitrateMbps);
                }
            }
            return {true, S_OK, codecLabel};
        }

        std::string attemptLabel = std::string(subtypeName(attempt.subtype)) + (attempt.hardware ? " (HW)" : " (SW)");
        lastHr = failHr;
        lastErrMsg = attemptLabel + ": " + error;

        if (isPrimary && isPermanentCodecUnsupported(failHr)) {
            primaryFailedPermanently = true;
        }
        geode::log::warn("Instant Replay: {} failed: {}", attemptLabel, error);
        cleanupFailedAttempt();

        if (isFatalStorageError(failHr)) {
            break;
        }
    }

    bool fellBackToOversizedH264 = false;
    if (targetSubtype != MFVideoFormat_H264 && (width > kH264MaxWidth || height > kH264MaxHeight)) {
        for (const auto& attempt : attempts) {
            if (attempt.subtype == MFVideoFormat_H264) {
                fellBackToOversizedH264 = true;
                break;
            }
        }
    }

    if (fellBackToOversizedH264) {
        lastErrMsg = "AV1/HEVC unavailable and " + std::to_string(width) + "x" + std::to_string(height) +
                     " exceeds H.264 " + std::to_string(kH264MaxWidth) + "x" + std::to_string(kH264MaxHeight) +
                     " limit (select HEVC/AV1 or a lower resolution preset)";
    } else if (lastErrMsg.empty()) {
        lastErrMsg = "no usable video encoder found";
    }
    if (allowFallback) {
        geode::log::error("Instant Replay: encoder start failed ({})", lastErrMsg);
    }
    return makeMfFailure(lastHr, lastErrMsg);
}

// Performs one complete sink writer initialisation for the given codec.
// Caller must hold m_mutex. On failure m_sinkWriter may be left non-null; the caller releases it.
bool MediaFoundationEncoder::tryStartLocked(
    const std::wstring& outputPath,
    const GUID& subtype,
    const bool hardware,
    const StartParams& p,
    HRESULT& outHr,
    std::string& error
) {
    auto recordFail = [&outHr, &error](HRESULT hr, std::string msg) -> bool {
        outHr = hr;
        error = std::move(msg);
        return false;
    };
    m_readbackMeanMbps = 0;
    m_readbackMaxMbps = 0;

    ComRef<IMFAttributes> attr;
    HRESULT hr = MFCreateAttributes(attr.put(), 3);
    if (FAILED(hr)) return recordFail(hr, "MFCreateAttributes: " + hrToString(hr));
    attr->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, hardware ? TRUE : FALSE);
    // Disable Sink Writer internal sleep throttling so WriteSample can NEVER deadlock on shutdown
    // or when an audio stream temporarily lacks packets; ReplayManager bounds in-flight video frames
    // and synthesizes silence on idle audio streams (#1).
    attr->SetUINT32(MF_SINK_WRITER_DISABLE_THROTTLING, TRUE);
    attr->SetUINT32(MF_LOW_LATENCY, TRUE);

    hr = MFCreateSinkWriterFromURL(outputPath.c_str(), NULL, attr.get(), &m_sinkWriter);
    if (FAILED(hr) || !m_sinkWriter) return recordFail(hr, "MFCreateSinkWriterFromURL: " + hrToString(hr));

    // 1-second GOP at the target frame rate (up to 240 FPS) instead of clamping 120/240 FPS to 60 (#8)
    const UINT32 gopFrames = static_cast<UINT32>(std::clamp(p.fps, 30, 240));

    // 1. Video output stream
    {
        ComRef<IMFMediaType> videoOut;
        hr = MFCreateMediaType(videoOut.put());
        if (FAILED(hr)) return recordFail(hr, "MFCreateMediaType: " + hrToString(hr));
        videoOut->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        videoOut->SetGUID(MF_MT_SUBTYPE, subtype);
        videoOut->SetUINT32(MF_MT_AVG_BITRATE, static_cast<UINT32>(p.bitrateMbps) * 1000000u);
        videoOut->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        videoOut->SetUINT32(MF_MT_MAX_KEYFRAME_SPACING, gopFrames);
        MFSetAttributeSize(videoOut.get(), MF_MT_FRAME_SIZE, p.width, p.height);
        MFSetAttributeRatio(videoOut.get(), MF_MT_FRAME_RATE, p.fps, 1);
        MFSetAttributeRatio(videoOut.get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
        if (p.typeExtensions) {
            // The sink writer's RGB32 -> NV12 conversion already uses BT.709 limited range for these sizes
            // (measured); signal it so players do not have to guess (W12, Windows counterpart of M18).
            videoOut->SetUINT32(MF_MT_YUV_MATRIX, MFVideoTransferMatrix_BT709);
            videoOut->SetUINT32(MF_MT_VIDEO_PRIMARIES, MFVideoPrimaries_BT709);
            videoOut->SetUINT32(MF_MT_TRANSFER_FUNCTION, MFVideoTransFunc_709);
            videoOut->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_16_235);
            if (subtype == MFVideoFormat_H264) {
                // NVENC defaults H.264 to Baseline with an automatic level and then silently clamps the peak
                // bitrate (measured: 66 -> 22 Mbps at 1080p60, 300 -> 88 Mbps at 4K60). High profile with an
                // explicit level honours the requested bitrates and compresses better (W13).
                const uint64_t mbsPerFrame = static_cast<uint64_t>((p.width + 15) / 16) * static_cast<uint64_t>((p.height + 15) / 16);
                const uint64_t mbsPerSecond = mbsPerFrame * static_cast<uint64_t>(std::max(1, p.fps));
                const UINT32 level = (mbsPerFrame <= 36864 && mbsPerSecond <= 2073600)
                    ? static_cast<UINT32>(eAVEncH264VLevel5_2)
                    : static_cast<UINT32>(eAVEncH264VLevel6_2);
                videoOut->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_High);
                videoOut->SetUINT32(MF_MT_MPEG2_LEVEL, level);
            }
        }
        hr = m_sinkWriter->AddStream(videoOut.get(), &m_videoStreamIndex);
        if (FAILED(hr)) return recordFail(hr, "AddStream(video): " + hrToString(hr));
    }

    // 2. Video input format (RGB32 / BGRA uncompressed) + pre-negotiation encoding parameters
    const UINT32 meanBps = static_cast<UINT32>(p.bitrateMbps) * 1000000u;
    const UINT32 maxBps = static_cast<UINT32>(p.maxBitrateMbps) * 1000000u;
    const UINT32 clampedQuality = static_cast<UINT32>(std::clamp(p.qualityLevel, 1, 100));
    ULONG targetRcMode = eAVEncCommonRateControlMode_PeakConstrainedVBR;
    if (p.rateControl == RateControlMode::CONSTANT_CBR) {
        targetRcMode = eAVEncCommonRateControlMode_CBR;
    } else if (p.rateControl == RateControlMode::CONSTANT_QUALITY) {
        targetRcMode = eAVEncCommonRateControlMode_Quality;
    }

    {
        ComRef<IMFMediaType> videoIn;
        hr = MFCreateMediaType(videoIn.put());
        if (FAILED(hr)) return recordFail(hr, "MFCreateMediaType: " + hrToString(hr));
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
        if (p.typeExtensions) {
            videoIn->SetUINT32(MF_MT_VIDEO_PRIMARIES, MFVideoPrimaries_BT709);
            videoIn->SetUINT32(MF_MT_TRANSFER_FUNCTION, MFVideoTransFunc_sRGB);
            videoIn->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_0_255);
            videoIn->SetUINT32(MF_MT_YUV_MATRIX, MFVideoTransferMatrix_BT709);
        }

        ComRef<IMFAttributes> encParams;
        if (SUCCEEDED(MFCreateAttributes(encParams.put(), 4)) && encParams) {
            encParams->SetUINT32(CODECAPI_AVEncCommonRateControlMode, targetRcMode);
            if (p.rateControl == RateControlMode::ADAPTIVE_VBR) {
                encParams->SetUINT32(CODECAPI_AVEncCommonMeanBitRate, meanBps);
                encParams->SetUINT32(CODECAPI_AVEncCommonMaxBitRate, maxBps);
            } else if (p.rateControl == RateControlMode::CONSTANT_CBR) {
                encParams->SetUINT32(CODECAPI_AVEncCommonMeanBitRate, meanBps);
            } else if (p.rateControl == RateControlMode::CONSTANT_QUALITY) {
                encParams->SetUINT32(CODECAPI_AVEncCommonQuality, clampedQuality);
            }
            encParams->SetUINT32(CODECAPI_AVEncMPVGOPSize, gopFrames);
        }

        hr = m_sinkWriter->SetInputMediaType(m_videoStreamIndex, videoIn.get(), encParams.get());
        if (FAILED(hr) && encParams && !isPermanentCodecUnsupported(hr)) {
            HRESULT retryHr = m_sinkWriter->SetInputMediaType(m_videoStreamIndex, videoIn.get(), NULL);
            if (SUCCEEDED(retryHr)) {
                geode::log::warn(
                    "Instant Replay: {} ({}) rejected pEncodingParameters ({}), falling back to ICodecAPI",
                    subtypeName(subtype), hardware ? "HW" : "SW", hrToString(hr));
                hr = retryHr;
            }
        }
        if (FAILED(hr)) return recordFail(hr, "SetInputMediaType(video): " + hrToString(hr));
    }

    // 3. Rate control + keyframe interval via ICodecAPI (log failures & verify effective RC mode once per config)
    {
        std::string rcLogKey = std::string(subtypeName(subtype)) + "|" + (hardware ? "hw" : "sw") + "|" + requestedRateControlName(p.rateControl);
        bool logRcOnce = false;
        {
            std::lock_guard<std::mutex> cacheLock(s_codecCacheMutex);
            if (s_lastLoggedRcKey != rcLogKey) {
                s_lastLoggedRcKey = rcLogKey;
                logRcOnce = true;
            }
        }

        ComRef<ICodecAPI> codecApi;
        hr = m_sinkWriter->GetServiceForStream(m_videoStreamIndex, GUID_NULL, __uuidof(ICodecAPI), (void**)codecApi.put());
        if (SUCCEEDED(hr) && codecApi) {
            auto setCodecU32 = [&](const GUID& prop, const char* propName, ULONG value) {
                VARIANT var;
                VariantInit(&var);
                var.vt = VT_UI4;
                var.ulVal = value;
                HRESULT setHr = codecApi->SetValue(&prop, &var);
                if (FAILED(setHr)) {
                    geode::log::warn("Instant Replay: {} ({}) ICodecAPI::SetValue({}) failed: {}",
                        subtypeName(subtype), hardware ? "HW" : "SW", propName, hrToString(setHr));
                }
            };

            if (p.rateControl == RateControlMode::ADAPTIVE_VBR) {
                setCodecU32(CODECAPI_AVEncCommonRateControlMode, "RateControlMode(PeakConstrainedVBR)", targetRcMode);
                setCodecU32(CODECAPI_AVEncCommonMeanBitRate, "MeanBitRate", meanBps);
                setCodecU32(CODECAPI_AVEncCommonMaxBitRate, "MaxBitRate", maxBps);
            } else if (p.rateControl == RateControlMode::CONSTANT_CBR) {
                setCodecU32(CODECAPI_AVEncCommonRateControlMode, "RateControlMode(CBR)", targetRcMode);
                setCodecU32(CODECAPI_AVEncCommonMeanBitRate, "MeanBitRate", meanBps);
            } else if (p.rateControl == RateControlMode::CONSTANT_QUALITY) {
                setCodecU32(CODECAPI_AVEncCommonRateControlMode, "RateControlMode(Quality)", targetRcMode);
                setCodecU32(CODECAPI_AVEncCommonQuality, "Quality", clampedQuality);
            }
            setCodecU32(CODECAPI_AVEncMPVGOPSize, "MPVGOPSize", gopFrames);

            // Read back what the encoder actually applies: some MFTs silently clamp to a level / tier
            // limit, which the UI and logs should reflect (W13).
            if (p.rateControl != RateControlMode::CONSTANT_QUALITY) {
                auto readMbps = [&](const GUID& prop) -> int {
                    VARIANT var;
                    VariantInit(&var);
                    int mbps = 0;
                    if (SUCCEEDED(codecApi->GetValue(&prop, &var)) && (var.vt == VT_UI4 || var.vt == VT_I4)) {
                        const ULONG bps = (var.vt == VT_UI4) ? var.ulVal : static_cast<ULONG>(var.lVal);
                        mbps = static_cast<int>((static_cast<uint64_t>(bps) + 500000ull) / 1000000ull);
                    }
                    VariantClear(&var);
                    return mbps;
                };
                m_readbackMeanMbps = readMbps(CODECAPI_AVEncCommonMeanBitRate);
                m_readbackMaxMbps = (p.rateControl == RateControlMode::ADAPTIVE_VBR)
                    ? readMbps(CODECAPI_AVEncCommonMaxBitRate)
                    : m_readbackMeanMbps;
                if (logRcOnce) {
                    const bool clamped = (m_readbackMeanMbps > 0 && m_readbackMeanMbps + 1 < p.bitrateMbps) ||
                                         (m_readbackMaxMbps > 0 && m_readbackMaxMbps + 1 < p.maxBitrateMbps &&
                                          p.rateControl == RateControlMode::ADAPTIVE_VBR);
                    if (clamped) {
                        geode::log::warn(
                            "Instant Replay: {} ({}) clamps the bitrate: mean {} Mbps, max {} Mbps (requested {} / {})",
                            subtypeName(subtype), hardware ? "HW" : "SW", m_readbackMeanMbps, m_readbackMaxMbps,
                            p.bitrateMbps, p.maxBitrateMbps);
                    } else {
                        geode::log::info(
                            "Instant Replay: {} ({}) effective bitrate: mean {} Mbps, max {} Mbps (requested {} / {})",
                            subtypeName(subtype), hardware ? "HW" : "SW", m_readbackMeanMbps, m_readbackMaxMbps,
                            p.bitrateMbps, p.maxBitrateMbps);
                    }
                }
            }

            if (logRcOnce) {
                VARIANT readVar;
                VariantInit(&readVar);
                HRESULT getHr = codecApi->GetValue(&CODECAPI_AVEncCommonRateControlMode, &readVar);
                if (SUCCEEDED(getHr) && (readVar.vt == VT_UI4 || readVar.vt == VT_I4)) {
                    ULONG actual = (readVar.vt == VT_UI4) ? readVar.ulVal : static_cast<ULONG>(readVar.lVal);
                    geode::log::info("Instant Replay: {} ({}) effective rate control: {} (mode={}, requested={})",
                        subtypeName(subtype), hardware ? "HW" : "SW",
                        rateControlModeName(actual), actual, requestedRateControlName(p.rateControl));
                } else {
                    geode::log::info("Instant Replay: {} ({}) rate control readback unavailable ({}, requested={})",
                        subtypeName(subtype), hardware ? "HW" : "SW",
                        hrToString(getHr), requestedRateControlName(p.rateControl));
                }
                VariantClear(&readVar);
            }
        } else if (logRcOnce) {
            geode::log::info("Instant Replay: {} ({}) ICodecAPI unavailable ({}), using pEncodingParameters only",
                subtypeName(subtype), hardware ? "HW" : "SW", hrToString(hr));
        }
    }

    auto addAudioStream = [this, &recordFail](int sampleRate, UINT32 avgBytesPerSec, DWORD* streamIndex, const char* label) -> bool {
        int validRate = (sampleRate == 44100) ? 44100 : 48000;
        ComRef<IMFMediaType> audioOut;
        HRESULT h = MFCreateMediaType(audioOut.put());
        if (FAILED(h)) return recordFail(h, "MFCreateMediaType: " + hrToString(h));
        audioOut->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        audioOut->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC);
        audioOut->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
        audioOut->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, validRate);
        audioOut->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2);
        audioOut->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, avgBytesPerSec);
        h = m_sinkWriter->AddStream(audioOut.get(), streamIndex);
        if (FAILED(h)) return recordFail(h, std::string("AddStream(") + label + "): " + hrToString(h));

        ComRef<IMFMediaType> audioIn;
        h = MFCreateMediaType(audioIn.put());
        if (FAILED(h)) return recordFail(h, "MFCreateMediaType: " + hrToString(h));
        audioIn->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        audioIn->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
        audioIn->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
        audioIn->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, validRate);
        audioIn->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2);
        audioIn->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, 4);
        audioIn->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, validRate * 4);
        h = m_sinkWriter->SetInputMediaType(*streamIndex, audioIn.get(), NULL);
        if (FAILED(h)) return recordFail(h, std::string("SetInputMediaType(") + label + "): " + hrToString(h));
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
    if (FAILED(hr)) return recordFail(hr, "BeginWriting: " + hrToString(hr));
    outHr = S_OK;
    return true;
}

bool MediaFoundationEncoder::createVideoSample(
    IMFMediaBuffer* videoBuffer,
    int64_t timestampHns,
    IMFSample** ppSample,
    HRESULT& outHr
) {
    if (!videoBuffer || !ppSample) {
        outHr = E_INVALIDARG;
        return false;
    }

    HRESULT hr = MFCreateSample(ppSample);
    outHr = hr;
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
    IMFSample** ppSample,
    HRESULT& outHr
) {
    if (numSamples <= 0 || channels <= 0 || !ppSample || !pcmFloat) {
        outHr = E_INVALIDARG;
        return false;
    }

    DWORD bufferSize = static_cast<DWORD>(numSamples) * channels * sizeof(int16_t);
    ComRef<IMFMediaBuffer> buffer;
    HRESULT hr = MFCreateMemoryBuffer(bufferSize, buffer.put());
    outHr = hr;
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
        outHr = hr;
        if (SUCCEEDED(hr) && *ppSample) {
            (*ppSample)->AddBuffer(buffer.get());
            (*ppSample)->SetSampleTime(timestampHns);
            int rate = (sampleRate > 0) ? sampleRate : 48000;
            int64_t durationHns = (static_cast<int64_t>(numSamples) * 10000000LL) / rate;
            (*ppSample)->SetSampleDuration(durationHns);
            return true;
        }
    } else {
        outHr = FAILED(hr) ? hr : E_FAIL;
    }
    return false;
}

EncoderOpResult MediaFoundationEncoder::writeVideoFrame(
    IMFMediaBuffer* videoBuffer,
    int width,
    int height,
    int64_t timestampHns
) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_recording || !m_sinkWriter || !videoBuffer) {
        return makeMfFailure(MF_E_NOT_INITIALIZED, "encoder not recording");
    }
    if (width != m_width || height != m_height) {
        return makeMfFailure(E_INVALIDARG, "frame resolution mismatch");
    }

    ComRef<IMFSample> sample;
    HRESULT hr = E_FAIL;
    if (!createVideoSample(videoBuffer, timestampHns, sample.put(), hr)) {
        return makeMfFailure(hr, hrToString(hr));
    }
    hr = m_sinkWriter->WriteSample(m_videoStreamIndex, sample.get());
    if (FAILED(hr)) {
        return makeMfFailure(hr, hrToString(hr));
    }
    return {true, S_OK, {}};
}

EncoderOpResult MediaFoundationEncoder::writeGameAudio(
    const float* pcmFloat,
    int numSamples,
    int channels,
    int sampleRate,
    int64_t timestampHns
) {
    std::lock_guard<std::mutex> lock(m_mutex);
    return writeAudioLocked(true, pcmFloat, numSamples, channels, sampleRate, timestampHns);
}

EncoderOpResult MediaFoundationEncoder::writeMicAudio(
    const float* pcmFloat,
    int numSamples,
    int channels,
    int sampleRate,
    int64_t timestampHns
) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_splitAudio) {
        return makeMfFailure(MF_E_NOT_INITIALIZED, "encoder not recording");
    }
    return writeAudioLocked(false, pcmFloat, numSamples, channels, sampleRate, timestampHns);
}

// Caller must hold m_mutex. Every AAC track starts with kSegmentAudioPrerollHns of silence and all
// real audio is shifted by it, because MF's AAC encoder cannot reproduce the first ~11 ms of a stream;
// the exporter drops the pre-roll again (W2).
EncoderOpResult MediaFoundationEncoder::writeAudioLocked(
    bool isGameAudio,
    const float* pcmFloat,
    int numSamples,
    int channels,
    int sampleRate,
    int64_t timestampHns
) {
    if (!m_recording || !m_sinkWriter) {
        return makeMfFailure(MF_E_NOT_INITIALIZED, "encoder not recording");
    }
    const DWORD streamIndex = isGameAudio ? m_gameAudioStreamIndex : m_micAudioStreamIndex;
    bool& prerollWritten = isGameAudio ? m_gamePrerollWritten : m_micPrerollWritten;

    if (!prerollWritten) {
        const int rate = (sampleRate > 0) ? sampleRate : 48000;
        const int prerollFrames = static_cast<int>(segmentAudioPrerollFrames(rate));
        if (prerollFrames > 0) {
            std::vector<float> silence(static_cast<size_t>(prerollFrames) * 2, 0.0f);
            ComRef<IMFSample> prerollSample;
            HRESULT prerollHr = E_FAIL;
            if (!createAudioSample(silence.data(), prerollFrames, 2, rate, 0, prerollSample.put(), prerollHr)) {
                return makeMfFailure(prerollHr, hrToString(prerollHr));
            }
            prerollHr = m_sinkWriter->WriteSample(streamIndex, prerollSample.get());
            if (FAILED(prerollHr)) {
                return makeMfFailure(prerollHr, hrToString(prerollHr));
            }
        }
        prerollWritten = true;
    }

    ComRef<IMFSample> sample;
    HRESULT hr = E_FAIL;
    if (!createAudioSample(pcmFloat, numSamples, channels, sampleRate,
                           timestampHns + kSegmentAudioPrerollHns, sample.put(), hr)) {
        return makeMfFailure(hr, hrToString(hr));
    }
    hr = m_sinkWriter->WriteSample(streamIndex, sample.get());
    if (FAILED(hr)) {
        return makeMfFailure(hr, hrToString(hr));
    }
    return {true, S_OK, {}};
}

bool MediaFoundationEncoder::isVideoQueueSaturated(size_t maxPendingFrames, size_t maxQueuedBytes) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_recording || !m_sinkWriter) return false;
    MF_SINK_WRITER_STATISTICS stats{};
    stats.cb = sizeof(stats);
    if (FAILED(m_sinkWriter->GetStatistics(m_videoStreamIndex, &stats))) {
        return false;
    }
    const uint64_t pendingFrames = (stats.qwNumSamplesReceived >= stats.qwNumSamplesEncoded)
        ? (stats.qwNumSamplesReceived - stats.qwNumSamplesEncoded)
        : 0;
    return pendingFrames >= maxPendingFrames ||
           static_cast<size_t>(stats.dwByteCountQueued) >= maxQueuedBytes;
}

bool MediaFoundationEncoder::videoQueueStats(uint64_t& outPendingFrames, uint64_t& outQueuedBytes) const {
    outPendingFrames = 0;
    outQueuedBytes = 0;
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_recording || !m_sinkWriter) return false;
    MF_SINK_WRITER_STATISTICS stats{};
    stats.cb = sizeof(stats);
    if (FAILED(m_sinkWriter->GetStatistics(m_videoStreamIndex, &stats))) return false;
    outPendingFrames = (stats.qwNumSamplesReceived >= stats.qwNumSamplesEncoded)
        ? (stats.qwNumSamplesReceived - stats.qwNumSamplesEncoded)
        : 0;
    outQueuedBytes = stats.dwByteCountQueued;
    return true;
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
// Segment merge (video remux + continuous AAC re-encode, see MfSegmentConcat.cpp)
// ---------------------------------------------------------------------------

ConcatResult MediaFoundationEncoder::concatenateSegments(
    const std::vector<SegmentDesc>& segments,
    const std::filesystem::path& finalOutputPath,
    double trimStartSeconds,
    std::atomic<int>* progressPermille
) {
    std::vector<mfconcat::SegmentInput> inputs;
    inputs.reserve(segments.size());
    for (const auto& seg : segments) {
        mfconcat::SegmentInput in;
        in.path = seg.path.wstring();
        in.durationSeconds = seg.durationSeconds;
        in.audioFrames[0] = seg.gameAudioFrames;
        in.audioFrames[1] = seg.micAudioFrames;
        inputs.push_back(std::move(in));
    }

    mfconcat::Options options;
    options.audioPrerollHns = kSegmentAudioPrerollHns;
    options.progressPermille = progressPermille;
    options.logInfo = [](const std::string& msg) { geode::log::info("{}", msg); };
    options.logWarn = [](const std::string& msg) { geode::log::warn("{}", msg); };

    ConcatResult res;
    try {
        res = mfconcat::concatenate(inputs, finalOutputPath.wstring(), trimStartSeconds, options);
    } catch (const std::exception& e) {
        res.success = false;
        res.error = std::string("exception: ") + e.what();
    }
    if (!res.success) {
        std::error_code ec;
        std::filesystem::remove(finalOutputPath, ec);
    }
    return res;
}

ConcatResult MediaFoundationEncoder::concatenateSegments(
    const std::vector<std::filesystem::path>& segmentPaths,
    const std::filesystem::path& finalOutputPath,
    double trimStartSeconds
) {
    std::vector<SegmentDesc> segments;
    segments.reserve(segmentPaths.size());
    for (const auto& p : segmentPaths) {
        SegmentDesc desc;
        desc.path = p;
        segments.push_back(std::move(desc));
    }
    return concatenateSegments(segments, finalOutputPath, trimStartSeconds, nullptr);
}

} // namespace replay

#endif // GEODE_IS_WINDOWS
