#pragma once

#include <Geode/Geode.hpp>

#ifdef GEODE_IS_WINDOWS

#include "../config/ReplayConfig.hpp"
#include "../encoder/MediaFoundationEncoder.hpp"
#include "../capture/FrameGrabber.hpp"
#include "../capture/AudioGrabber.hpp"
#include <string>
#include <vector>
#include <deque>
#include <set>
#include <memory>
#include <mutex>
#include <thread>
#include <atomic>
#include <condition_variable>
#include <filesystem>

namespace replay {

struct SegmentInfo {
    std::wstring filePath;
    std::chrono::steady_clock::time_point startTime;
    double durationSeconds = 0.0;
    std::string formatKey; // segments can only be merged with segments of the same format
};

class ReplayManager {
public:
    static ReplayManager* get();

    void initialize();
    void shutdown();

    // User actions
    void saveReplay();
    void toggleReplay();
    // Applies the "enabled" state (e.g. from Geode settings) to the running recorder
    void setReplayEnabled(bool enabled);
    bool isReplayActive() const { return m_running.load(); }
    bool isSaving() const { return m_saving.load(); }

    // Direct folder opener
    void openRecordingsFolder();

    // Active recording session configuration snapshot (read by capture threads)
    const ReplayConfig& getSessionConfig() const noexcept { return m_sessionConfig; }

private:
    ReplayManager();
    ~ReplayManager();

    void startReplay();
    void stopReplay();

    void workerThreadFunc(ReplayConfig sessionConfig);
    void finalizeCurrentSegment(bool syncForExport, const ReplayConfig& cfg);
    bool startNewSegment(const ReplayConfig& cfg, std::chrono::steady_clock::time_point segStart);
    bool rotateSegment(bool syncForExport, const ReplayConfig& cfg,
                       std::chrono::steady_clock::time_point nextSegStart = std::chrono::steady_clock::now());
    void handleSaveRequest(bool continueRecording, const ReplayConfig& cfg);
    void handleFatalRecordingError(const std::string& reason);
    void purgeOldSegments(const ReplayConfig& cfg);
    void cleanupTempDir();
    void joinFinalizeThread();
    void joinExportThread();

    // Segment file ownership: files used by a running export are never deleted
    // directly, deletion is deferred until the export releases them.
    // Both require m_segmentMutex to be held.
    void deleteSegmentFileLocked(const std::wstring& path);
    void flushDeferredDeletesLocked();
    void releaseExportSegments(const std::vector<std::wstring>& paths);

    // Single-track microphone mixer (recordMic && !splitAudioTracks)
    void resetMicMixer();
    void appendMicToMix(const AudioChunk& chunk, int targetRate, size_t startFrameOffset = 0);
    void mixMicInto(float* gameSamples, size_t numFrames, int channels);
    bool flushMicMixFifo(int rate, bool checkErrors);

    // Shared timestamp resync + encoder write for game and mic audio streams
    bool writeAudioWithResync(bool isGameAudio, float* samples, int numSamples, int channels,
                              int rate, std::chrono::steady_clock::time_point timestamp,
                              std::vector<float>& silenceScratch);

    std::filesystem::path getTempDir(bool createIfMissing = false) const;

    ReplayConfig m_sessionConfig{};

    std::atomic<bool> m_running{false};
    std::atomic<bool> m_saving{false};
    std::atomic<bool> m_saveRequested{false};
    std::atomic<bool> m_shuttingDown{false};

    std::unique_ptr<MediaFoundationEncoder> m_encoder;
    std::wstring m_currentSegmentPath;
    std::string m_currentFormatKey;
    std::chrono::steady_clock::time_point m_currentSegmentStart;
    int m_segmentIndex = 0; // monotonic for the whole session: names never collide with files still being exported

    // Encoder video size = size of the frames actually delivered by FrameGrabber
    int m_videoWidth = 0;
    int m_videoHeight = 0;
    int64_t m_lastVideoHns = -1;
    int64_t m_gameAudioSamples = 0;
    int64_t m_micAudioSamples = 0;

    // Encoder start backoff & write failure tracking (worker thread only)
    int m_encoderFailCount = 0;
    int m_writeErrorCount = 0;
    std::chrono::steady_clock::time_point m_nextEncoderRetryTime{};

    std::vector<SegmentInfo> m_activeSegments;
    std::multiset<std::wstring> m_exportLockedSegments;
    std::vector<std::wstring> m_deferredDeletes;
    mutable std::mutex m_segmentMutex;

    // Asynchronous segment finalizer thread (prevents 30s rotation from stalling worker thread)
    std::thread m_finalizeThread;
    std::mutex m_finalizeMutex;

    // Export (merge) thread - owned, joined before a new export and on shutdown
    std::thread m_exportThread;
    std::mutex m_exportMutex;

    // Microphone mix & game audio resampler state (worker thread only)
    bool m_mixMicIntoGame = false;
    std::vector<float> m_micMixFifo; // interleaved stereo at m_micMixRate
    size_t m_micMixReadPos = 0;
    int m_micMixRate = 0;
    StereoResampler m_micMixResampler{};
    StereoResampler m_gameResampler{};

    // Queues for worker thread (game audio uses lock-free SPSC ring buffer in AudioGrabber)
    std::deque<std::shared_ptr<CapturedFrame>> m_frameQueue;
    std::deque<std::shared_ptr<AudioChunk>> m_micAudioQueue;
    std::mutex m_queueMutex;
    std::condition_variable m_cv;

    std::thread m_workerThread;
};

} // namespace replay

#endif // GEODE_IS_WINDOWS
