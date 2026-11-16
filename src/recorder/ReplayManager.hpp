#pragma once

#include <Geode/Geode.hpp>

#ifdef GEODE_IS_WINDOWS

#include "../config/ReplayConfig.hpp"
#include "../encoder/MediaFoundationEncoder.hpp"
#include "../capture/FrameGrabber.hpp"
#include "../capture/AudioGrabber.hpp"
#include "MicMixFifo.hpp"
#include <string>
#include <vector>
#include <deque>
#include <optional>
#include <set>
#include <memory>
#include <mutex>
#include <thread>
#include <atomic>
#include <condition_variable>
#include <filesystem>
#include <future>

namespace replay {

struct SegmentInfo {
    std::wstring filePath;
    std::chrono::steady_clock::time_point startTime;
    double durationSeconds = 0.0;
    std::string formatKey; // segments can only be merged with segments of the same format
};

enum class SaveReplayResult {
    Started,
    NotRunning,
    AlreadySaving
};

enum class ToggleReplayResult {
    Enabled,
    Stopping,
    RestartQueued,
    StopPending
};

class ReplayManager {
public:
    static ReplayManager* get();

    void initialize();
    void shutdown();

    // User actions
    SaveReplayResult saveReplay();
    static void showSaveFeedback(SaveReplayResult result);
    ToggleReplayResult toggleReplay();
    // Applies the "enabled" state (e.g. from Geode settings) to the running recorder
    void setReplayEnabled(bool enabled);
    bool isReplayActive() const noexcept { return m_running.load(); }
    bool isStopping() const noexcept { return m_stopping.load(); }
    bool isRestartPending() const noexcept { return m_pendingRestart.load(); }
    bool isReplayLocked() const noexcept { return m_running.load() || m_stopping.load(); }
    bool isExportInFlight() const noexcept { return m_exportInFlight.load(); }
    bool isSaving() const noexcept { return m_savePending.load() || m_exportInFlight.load(); }

    // Direct folder opener
    void openRecordingsFolder();

    // Active recording session configuration snapshot (read by capture threads)
    const ReplayConfig& getSessionConfig() const noexcept { return m_sessionConfig; }

private:
    ReplayManager();
    ~ReplayManager();

    void startReplay();
    void stopReplay();

    struct PreparedEncoder {
        std::unique_ptr<MediaFoundationEncoder> encoder;
        std::wstring path;
        std::string formatKey;
        std::filesystem::path tempDir;
        int gameSampleRate = 48000;
        int width = 0;
        int height = 0;
    };

    struct PendingFinalizeInfo {
        SegmentInfo segment;
        std::shared_future<bool> future;
    };

    struct CarryGameAudio {
        std::vector<float> samples;
        int rate = 48000;
        std::chrono::steady_clock::time_point timestamp{};
    };

    enum class WriteCheckResult {
        Ok,
        ToleratedError,
        Recovered,
        Fatal
    };

    void workerThreadFunc(ReplayConfig sessionConfig);
    void reclaimInFlightFrames(bool forceClear = false);
    bool canAcceptEncoderFrame(size_t frameBytes) const;
    WriteCheckResult handleEncoderWriteError(
        const char* streamLabel,
        const EncoderOpResult& res,
        int& errorCounter,
        const ReplayConfig& cfg,
        std::string& outFatalError
    );
    bool consumeAudioQueuesUpTo(
        std::optional<std::chrono::steady_clock::time_point> cutoffTime,
        const ReplayConfig& cfg,
        std::vector<float>& gameAudioScratch,
        std::vector<float>& resampledGameAudioScratch,
        std::vector<float>& silenceScratch,
        bool& outRecovered,
        std::string& outFatalError,
        std::optional<int64_t> maxGameSamplesCap = std::nullopt,
        std::optional<int64_t> maxMicSamplesCap = std::nullopt
    );
    bool processPendingAudio(
        const ReplayConfig& sessionConfig,
        std::vector<float>& gameAudioScratch,
        std::vector<float>& resampledGameAudioScratch,
        std::vector<float>& silenceScratch,
        std::string& outFatalError,
        std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now(),
        bool bypassHorizon = false
    );
    bool writeKeepAliveSilence(
        const ReplayConfig& sessionConfig,
        int encGameRate,
        std::vector<float>& silenceScratch,
        bool& outRecovered,
        std::string& outFatalError
    );
    void feedMicChunkIntoMix(
        const AudioChunk& chunk,
        int encGameRate,
        size_t startFrameOffset = 0,
        size_t maxFrameCount = SIZE_MAX
    );
    bool splitAudioAtBoundary(
        std::chrono::steady_clock::time_point boundaryTime,
        const ReplayConfig& cfg,
        std::vector<float>& gameAudioScratch,
        std::vector<float>& resampledGameAudioScratch,
        std::vector<float>& silenceScratch,
        bool& outRecovered,
        std::string& outFatalError
    );
    bool flushCarryAudio(
        const ReplayConfig& cfg,
        std::vector<float>& resampledScratch,
        std::vector<float>& silenceScratch,
        std::string& outFatalError
    );
    void flushTrailingSilenceForClose(const ReplayConfig& cfg);

    PendingFinalizeInfo finalizeCurrentSegment(bool syncForExport, const ReplayConfig& cfg);
    void ensureSessionBitrates(const ReplayConfig& cfg);
    bool startNewSegment(const ReplayConfig& cfg, std::chrono::steady_clock::time_point segStart, bool isContinuation);
    bool activatePreparedOrStartNew(
        const ReplayConfig& cfg,
        std::chrono::steady_clock::time_point nextSegStart,
        bool isContinuation,
        bool allowSyncFallback
    );
    void startPrepareNextSegment(const ReplayConfig& cfg);
    void joinPrepareThread();
    void discardPreparedEncoder();

    bool rotateSegment(bool syncForExport, const ReplayConfig& cfg,
                       std::chrono::steady_clock::time_point nextSegStart = std::chrono::steady_clock::now());
    bool writeEncodedVideoFrame(
        std::shared_ptr<CapturedFrame> frame,
        const ReplayConfig& cfg,
        bool& outRecovered,
        std::string& outFatalError
    );
    bool drainPreBoundaryVideoFrames(
        std::chrono::steady_clock::time_point& boundaryTime,
        const ReplayConfig& cfg,
        bool& outRecovered,
        std::string& outFatalError
    );
    void abortPendingSave();
    bool handleSaveRequest(
        bool continueRecording,
        const ReplayConfig& cfg,
        std::chrono::steady_clock::time_point boundaryTime,
        bool alreadyRecovered,
        std::vector<float>& gameAudioScratch,
        std::vector<float>& resampledGameAudioScratch,
        std::vector<float>& silenceScratch,
        std::string& outFatalError
    );
    void handleFatalRecordingError(const std::string& reason);
    void purgeOldSegments(const ReplayConfig& cfg);
    void ensureSessionLock(const std::filesystem::path& pidTempDir);
    void releaseSessionLock(bool removeFiles);
    void cleanupTempDir();
    void joinFinalizeThread();
    void joinExportThread();
    void joinStopThread();
    void finishStopCleanup();

    // Segment file ownership: files used by a running export are never deleted
    // directly, deletion is deferred until the export releases them.
    // Both require m_segmentMutex to be held.
    void deleteSegmentFileLocked(const std::wstring& path);
    void flushDeferredDeletesLocked();
    void releaseExportSegments(const std::vector<std::wstring>& paths);

    // Single-track microphone mixer (recordMic && !splitAudioTracks)
    void resetMicMixer();
    void rebaseMicMixerForNewSegment(
        std::chrono::steady_clock::time_point prevSegStart,
        std::chrono::steady_clock::time_point nextSegStart
    );
    void appendMicToMix(
        const AudioChunk& chunk,
        int targetRate,
        std::chrono::steady_clock::time_point captureStartTime,
        size_t startFrameOffset = 0,
        size_t maxFrameCount = SIZE_MAX
    );
    void mixMicInto(float* stereoSamples, size_t numFrames, int64_t targetSamplePos);
    bool flushMicMixFifo(int rate, bool checkErrors, const ReplayConfig& cfg,
                         std::vector<float>& silenceScratch,
                         bool& outRecovered, std::string& outFatalError,
                         int64_t maxEndSample = -1);

    // Shared timestamp resync + encoder write for game and mic audio streams
    bool writeAudioWithResync(bool isGameAudio, float* samples, int numSamples, int rate,
                              std::chrono::steady_clock::time_point timestamp,
                              const ReplayConfig& cfg,
                              std::vector<float>& silenceScratch,
                              bool& outRecovered,
                              std::string& outFatalError);

    std::filesystem::path getTempDir(bool createIfMissing = false) const;

    ReplayConfig m_sessionConfig{};

    std::atomic<bool> m_running{false};
    std::atomic<bool> m_stopping{false};
    std::atomic<bool> m_pendingRestart{false};
    std::atomic<bool> m_savePending{false};
    std::atomic<bool> m_saveRequested{false};
    std::atomic<bool> m_exportInFlight{false};
    std::atomic<bool> m_shuttingDown{false};
    uint64_t m_sessionId = 0; // guarded by m_queueMutex (#12, #13)

    // Asynchronous teardown thread & lifecycle state guard for non-blocking Stop/Start
    std::thread m_stopThread;
    std::mutex m_stopMutex;
    std::mutex m_lifecycleMutex;

    std::unique_ptr<MediaFoundationEncoder> m_encoder;
    std::deque<std::shared_ptr<CapturedFrame>> m_inFlightFrames; // worker thread only (#1, #7)
    std::wstring m_currentSegmentPath;
    std::string m_currentFormatKey;
    std::chrono::steady_clock::time_point m_currentSegmentStart{};
    std::chrono::steady_clock::time_point m_lastVideoCaptureTime{};
    std::chrono::steady_clock::time_point m_lastWrittenVideoCaptureTime{}; // session-wide horizon (not reset on rotation)
    bool m_isContinuationSegment = false;
    int m_segmentGameSampleRate = 48000;
    int m_segmentIndex = 0; // monotonic across sessions so names never collide with in-flight exports

    // Pre-warmed next segment encoder (background m_prepareThread -> worker handoff)
    PreparedEncoder m_prepared;
    std::thread m_prepareThread;
    std::mutex m_prepareMutex;
    std::mutex m_prepareDataMutex;
    std::atomic<bool> m_prepareInFlight{false};
    std::atomic<bool> m_prepareCancel{false};
    std::atomic<bool> m_prepareFailedLastAttempt{false};
    int m_prepareFailedCount = 0;
    std::chrono::steady_clock::time_point m_nextPrepareRetryTime{};

    // Non-blocking F6 switch state (worker thread only)
    bool m_savePendingSwitch = false;
    std::chrono::steady_clock::time_point m_savePendingSince{};

    // Audio buffered past the video capture horizon / split across segment boundaries (worker thread only)
    std::deque<CarryGameAudio> m_carryGameChunks;
    std::deque<std::shared_ptr<AudioChunk>> m_carryMicAudio;

    // Encoder video size & session-locked video bitrate (worker thread only)
    int m_videoWidth = 0;
    int m_videoHeight = 0;
    int m_sessionEffBitrateMbps = 0;
    int m_sessionEffMaxBitrateMbps = 0;
    int64_t m_lastVideoHns = -1;
    int64_t m_gameAudioSamples = 0;
    int64_t m_micAudioSamples = 0;
    int64_t m_seamTrimmedGameSamples = 0;
    int64_t m_seamTrimmedMicSamples = 0;

    // Encoder start backoff & per-stream write failure tracking (worker thread only, #3)
    int m_encoderFailCount = 0;
    int m_videoWriteErrors = 0;
    int m_gameAudioWriteErrors = 0;
    int m_micAudioWriteErrors = 0;
    bool m_writeRecoveryAttempted = false;
    EncoderOpResult m_lastStartError{};
    std::chrono::steady_clock::time_point m_nextEncoderRetryTime{};

    std::vector<SegmentInfo> m_activeSegments;
    std::multiset<std::wstring> m_exportLockedSegments;
    std::vector<std::wstring> m_deferredDeletes;
    std::filesystem::path m_lastTempDir;
    mutable std::mutex m_segmentMutex;

    // Exclusive per-PID session lock inside pid_<PID> temp folder
    HANDLE m_sessionLockHandle = INVALID_HANDLE_VALUE;
    std::filesystem::path m_sessionLockDir;
    std::filesystem::path m_persistedTempBase;
    std::mutex m_sessionLockMutex;

    // Debug frame drop counters (stale, queue overflow, encoder backpressure)
    std::atomic<uint64_t> m_staleDropCount{0};
    std::atomic<uint64_t> m_queueDropCount{0};
    std::atomic<uint64_t> m_inFlightDropCount{0};
    std::chrono::steady_clock::time_point m_lastDropLogTime{};
    uint64_t m_lastLoggedQueueDrops = 0;
    uint64_t m_lastLoggedInFlightDrops = 0;

    // Asynchronous segment finalizer thread (prevents 30s rotation from stalling worker thread)
    std::thread m_finalizeThread;
    std::mutex m_finalizeMutex;
    std::atomic<bool> m_finalizeInFlight{false};
    PendingFinalizeInfo m_pendingFinalize{};

    // Export (merge) thread - owned, joined before a new export and on shutdown
    std::thread m_exportThread;
    std::mutex m_exportMutex;

    // Microphone mix & game audio resampler state (worker thread only)
    bool m_mixMicIntoGame = false;
    MicMixFifo m_micMix;
    std::vector<float> m_micMixResampleScratch;
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
