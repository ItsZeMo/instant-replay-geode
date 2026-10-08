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

    static bool concatenateSegments(
        const std::vector<std::wstring>& segmentPaths,
        const std::wstring& finalOutputPath
    );

private:
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

    int64_t m_videoFrameDurationHns = 0;
    int64_t m_lastVideoHns = 0;
    int64_t m_lastGameAudioHns = 0;
    int64_t m_lastMicAudioHns = 0;

    std::mutex m_mutex;
};

} // namespace replay
