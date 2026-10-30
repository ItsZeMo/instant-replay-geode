#pragma once

#include "../config/ReplayConfig.hpp"
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <codecapi.h>
#include <icodecapi.h>
#include <string>
#include <vector>
#include <memory>
#include <mutex>

namespace replay {

// Result of merging rolling segments into the final MP4.
struct ConcatResult {
    bool success = false;
    std::string error;            // Human readable reason when success == false
    double durationSeconds = 0.0; // Duration actually written to the output file
};

class MediaFoundationEncoder {
public:
    MediaFoundationEncoder();
    ~MediaFoundationEncoder();

    static void globalInitialize();
    static void globalShutdown();

    bool start(
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
        bool hardwareAcceleration = true,
        const std::string& customCodec = "",
        int gameSampleRate = 44100
    );

    bool writeVideoFrame(const uint8_t* bgraPixels, int width, int height, int64_t timestampHns);
    bool writeGameAudio(const float* pcmFloat, int numSamples, int channels, int sampleRate, int64_t timestampHns);
    bool writeMicAudio(const float* pcmFloat, int numSamples, int channels, int sampleRate, int64_t timestampHns);

    bool finish();
    bool isRecording() const { return m_recording; }
    int width() const { return m_width; }
    int height() const { return m_height; }

    // Name of the codec that was actually initialised (after any fallback), e.g. "HEVC (HW)".
    const std::string& activeCodecName() const { return m_activeCodecName; }
    // Identifies the stream layout of the current segment. Segments with different keys
    // cannot be remuxed into one file without re-encoding.
    const std::string& formatKey() const { return m_formatKey; }

    // Remuxes the segments (oldest first) into finalOutputPath without re-encoding.
    // trimStartSeconds drops that much from the beginning of the first segment
    // (cut is aligned to the preceding video keyframe). Any read/write/finalize
    // failure is reported and the partial output file is deleted.
    static ConcatResult concatenateSegments(
        const std::vector<std::wstring>& segmentPaths,
        const std::wstring& finalOutputPath,
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
        int gameSampleRate = 44100;
    };

    bool tryStartLocked(const std::wstring& outputPath, const GUID& subtype, bool hardware,
                        const StartParams& params, std::string& error);
    bool finishLocked();
    bool createVideoSample(const uint8_t* bgraPixels, int width, int height, int64_t timestampHns, IMFSample** ppSample);
    bool createAudioSample(const float* pcmFloat, int numSamples, int channels, int sampleRate, int64_t timestampHns, IMFSample** ppSample);

    IMFSinkWriter* m_sinkWriter = nullptr;
    DWORD m_videoStreamIndex = 0;
    DWORD m_gameAudioStreamIndex = 1;
    DWORD m_micAudioStreamIndex = 2;

    int m_width = 1920;
    int m_height = 1080;
    int m_fps = 60;
    bool m_splitAudio = true;
    bool m_recording = false;
    std::string m_activeCodecName;
    std::string m_formatKey;

    int64_t m_videoFrameDurationHns = 0;
    int64_t m_lastVideoHns = 0;
    int64_t m_lastGameAudioHns = 0;
    int64_t m_lastMicAudioHns = 0;

    std::mutex m_mutex;
};

} // namespace replay
