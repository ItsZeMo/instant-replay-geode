#pragma once

#include <Geode/Geode.hpp>

#if defined(GEODE_IS_MACOS) || defined(GEODE_IS_IOS)

#include "../config/BitrateMath.hpp"
#include "../config/CodecUtils.hpp"
#include "../config/ReplayConfig.hpp"
#include "VideoEncoderTypes.hpp"
#pragma push_macro("CommentType")
#undef CommentType
#define CommentType AppleScriptCommentTypeDummy
#include <CoreVideo/CoreVideo.h>
#undef CommentType
#pragma pop_macro("CommentType")
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace replay {

class VideoToolboxEncoder {
public:
    VideoToolboxEncoder();
    ~VideoToolboxEncoder();

    VideoToolboxEncoder(const VideoToolboxEncoder&) = delete;
    VideoToolboxEncoder& operator=(const VideoToolboxEncoder&) = delete;

    static void globalInitialize();
    static void resetCodecCache();

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

    EncoderOpResult writeVideoFrame(CVPixelBufferRef pixelBuffer, int width, int height, int64_t timestampHns);
    EncoderOpResult writeGameAudio(const float* pcmFloat, int numSamples, int channels, int sampleRate, int64_t timestampHns);
    EncoderOpResult writeMicAudio(const float* pcmFloat, int numSamples, int channels, int sampleRate, int64_t timestampHns);

    // Per-family session bitrates: each attempt of the fallback cascade (e.g. custom AV1 -> HEVC) encodes at
    // the bitrate an explicit selection of that codec would get (W1 parity, K3). Unset -> start() arguments.
    void setFamilyBitrateTable(const CodecBitrateTable& table) noexcept { m_familyBitrates = table; }
    // Mean / max bitrate the running encoder was started with (after any fallback).
    int activeBitrateMbps() const noexcept { return m_activeBitrateMbps; }
    int activeMaxBitrateMbps() const noexcept { return m_activeMaxBitrateMbps; }

    bool finish();
    bool isRecording() const noexcept { return m_recording; }
    bool isVideoQueueSaturated(size_t maxPendingFrames, size_t maxQueuedBytes) const;
    int encodedGameSampleRate() const noexcept { return m_gameSampleRate; }
    bool hasActiveCodec() const noexcept { return m_hasActiveCodec; }
    VideoCodecFamily activeCodecFamily() const noexcept { return m_activeCodecFamily; }
    bool activeHardware() const noexcept { return m_activeHardware; }
    std::string activeCodecName() const;

    // Identifies the stream layout of the current segment. Segments with different keys
    // cannot be remuxed into one file without re-encoding.
    const std::string& formatKey() const noexcept { return m_formatKey; }

    // Merges segments (oldest first) into finalOutputPath: video passthrough, audio re-encoded into one
    // continuous AAC stream per track (N1). `progressPermille`, when set, rises monotonically from 0 while
    // segments are processed, stays <= 990 until the MP4 is finalized and ends at 1000 on success (K4).
    static ConcatResult concatenateSegments(
        const std::vector<std::filesystem::path>& segmentPaths,
        const std::filesystem::path& finalOutputPath,
        double trimStartSeconds = 0.0,
        std::atomic<int>* progressPermille = nullptr
    );

private:
    struct Impl;
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

    bool tryStartLocked(
        const std::filesystem::path& outputPath,
        VideoCodecFamily family,
        bool hardware,
        const StartParams& params,
        int64_t& outCode,
        bool& outFatalStorage,
        bool& outPermanentUnsupported,
        std::string& outError
    );
    bool finishLocked();
    EncoderOpResult writeAudioLocked(
        bool isGameAudio,
        const float* pcmFloat,
        int numSamples,
        int channels,
        int sampleRate,
        int64_t timestampHns
    );

    std::unique_ptr<Impl> m_impl;
    int m_width = 1920;
    int m_height = 1080;
    int m_fps = 60;
    int m_gameSampleRate = 48000;
    bool m_splitAudio = true;
    bool m_recording = false;
    std::string m_formatKey;
    bool m_hasActiveCodec = false;
    VideoCodecFamily m_activeCodecFamily = VideoCodecFamily::H264;
    bool m_activeHardware = true;
    CodecBitrateTable m_familyBitrates{};
    int m_activeBitrateMbps = 0;
    int m_activeMaxBitrateMbps = 0;

    mutable std::mutex m_mutex;
};

} // namespace replay

#endif // GEODE_IS_MACOS || GEODE_IS_IOS
