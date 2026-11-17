#pragma once

#include <Geode/Geode.hpp>

#ifdef GEODE_IS_WINDOWS

#include "../config/BitrateMath.hpp"
#include "../config/CodecUtils.hpp"
#include "../config/ReplayConfig.hpp"
#include "VideoEncoderTypes.hpp"
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <codecapi.h>
#include <icodecapi.h>
#include <filesystem>
#include <string>
#include <vector>
#include <memory>
#include <mutex>
#include <functional>

namespace replay {

class MediaFoundationEncoder {
public:
    MediaFoundationEncoder();
    ~MediaFoundationEncoder();

    static void globalInitialize();
    static void resetCodecCache();
    static bool isFatalStorageError(HRESULT hr) noexcept;
    static std::string formatHr(HRESULT hr);

    EncoderOpResult start(
        const std::filesystem::path& outputPath,
        int width,
        int height,
        int fps,
        int bitrateMbps,
        int maxBitrateMbps,
        int qualityLevel,
        RateControlMode rateControl,
        bool splitAudio,
        CodecType codec,
        bool hardwareAcceleration = true,
        const std::string& customCodec = "",
        int gameSampleRate = 48000,
        const std::function<void()>& onBeforeFallback = nullptr,
        bool allowFallback = true,
        bool hasForcedCodec = false,
        VideoCodecFamily forcedFamily = VideoCodecFamily::H264,
        bool forcedHardware = true
    );

    // Per-family session bitrates used by the fallback cascade in start() (W1). When unset, every
    // attempt uses the bitrate passed to start().
    void setFamilyBitrateTable(const CodecBitrateTable& table) noexcept { m_familyBitrates = table; }

    EncoderOpResult writeVideoFrame(IMFMediaBuffer* videoBuffer, int width, int height, int64_t timestampHns);
    EncoderOpResult writeGameAudio(const float* pcmFloat, int numSamples, int channels, int sampleRate, int64_t timestampHns);
    EncoderOpResult writeMicAudio(const float* pcmFloat, int numSamples, int channels, int sampleRate, int64_t timestampHns);

    bool finish();
    bool isRecording() const noexcept { return m_recording; }
    bool isVideoQueueSaturated(size_t maxPendingFrames, size_t maxQueuedBytes) const;
    int encodedGameSampleRate() const noexcept { return m_gameSampleRate; }
    bool hasActiveCodec() const noexcept { return m_activeSubtype != GUID_NULL; }
    VideoCodecFamily activeCodecFamily() const noexcept;
    const GUID& activeSubtype() const noexcept { return m_activeSubtype; }
    bool activeHardware() const noexcept { return m_activeHardware; }
    int activeBitrateMbps() const noexcept { return m_activeBitrateMbps; }
    int activeMaxBitrateMbps() const noexcept { return m_activeMaxBitrateMbps; }
    std::string activeCodecName() const;

    // Identifies the stream layout of the current segment. Segments with different keys
    // cannot be remuxed into one file without re-encoding.
    const std::string& formatKey() const noexcept { return m_formatKey; }

    // Remuxes the segments (oldest first) into finalOutputPath without re-encoding.
    // trimStartSeconds drops that much from the beginning of the first segment
    // (cut is aligned to the nearest video keyframe). Any read/write/finalize
    // failure is reported and the partial output file is deleted.
    static ConcatResult concatenateSegments(
        const std::vector<std::filesystem::path>& segmentPaths,
        const std::filesystem::path& finalOutputPath,
        double trimStartSeconds = 0.0
    );

private:
    struct StartParams {
        int width = 0;
        int height = 0;
        int fps = 60;
        int bitrateMbps = 0;
        int maxBitrateMbps = 0;
        int qualityLevel = 80;
        RateControlMode rateControl = RateControlMode::ADAPTIVE_VBR;
        bool splitAudio = false;
        int gameSampleRate = 48000;
    };

    bool tryStartLocked(const std::wstring& outputPath, const GUID& subtype, bool hardware,
                        const StartParams& params, HRESULT& outHr, std::string& error);
    bool finishLocked();
    bool createVideoSample(IMFMediaBuffer* videoBuffer, int64_t timestampHns, IMFSample** ppSample, HRESULT& outHr);
    bool createAudioSample(const float* pcmFloat, int numSamples, int channels, int sampleRate,
                           int64_t timestampHns, IMFSample** ppSample, HRESULT& outHr);

    IMFSinkWriter* m_sinkWriter = nullptr;
    DWORD m_videoStreamIndex = 0;
    DWORD m_gameAudioStreamIndex = 1;
    DWORD m_micAudioStreamIndex = 2;

    int m_width = 1920;
    int m_height = 1080;
    int m_fps = 60;
    int m_gameSampleRate = 48000;
    bool m_splitAudio = true;
    bool m_recording = false;
    std::string m_formatKey;
    GUID m_activeSubtype = GUID_NULL;
    bool m_activeHardware = true;
    int m_activeBitrateMbps = 0;
    int m_activeMaxBitrateMbps = 0;
    CodecBitrateTable m_familyBitrates{};

    int64_t m_videoFrameDurationHns = 0;

    mutable std::mutex m_mutex;
};

} // namespace replay

#endif // GEODE_IS_WINDOWS
