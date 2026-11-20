#pragma once

#include <Geode/Geode.hpp>

#if defined(GEODE_IS_WINDOWS) || defined(GEODE_IS_MACOS) || defined(GEODE_IS_IOS)

#include "../config/ReplayConfig.hpp"
#include "../encoder/PlatformEncoder.hpp"
#include "../capture/FrameGrabber.hpp"
#include "../capture/AudioGrabber.hpp"
#include "MicMixFifo.hpp"
#include <algorithm>
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
    std::filesystem::path filePath;
    std::chrono::steady_clock::time_point startTime;
    double durationSeconds = 0.0;
    std::string formatKey; // segments can only be merged with segments of the same format
    // Real audio frames written per track (excludes encoder pre-roll / AAC padding); the Windows
    // exporter uses them to stitch decoded audio sample-exactly (W2). -1 = unknown / no track.
    int64_t gameAudioFrames = -1;
    int64_t micAudioFrames = -1;
};

// Snapshot of the encoder that is actually recording (after any codec fallback), for the UI (W4).
struct ActiveEncoderInfo {
    bool valid = false;
    VideoCodecFamily family = VideoCodecFamily::H264;
    bool hardware = true;
    int width = 0;
    int height = 0;
    int meanMbps = 0;
    int maxMbps = 0;
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
    StopPending,
    Unavailable
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
    // Export progress in percent (0-100) while a replay is being merged, -1 when unknown/idle (W3).
    int getExportProgressPercent() const noexcept {
        const int permille = m_exportProgressPermille.load(std::memory_order_relaxed);
        return (permille < 0) ? -1 : std::min(100, permille / 10);
    }
    static bool isCaptureBackendReady() noexcept;
    bool isBackendUnavailable() const noexcept { return m_backendUnavailable.load() || !isCaptureBackendReady(); }
    // Recording was stopped by a fatal error this session; the user's "enabled" setting is kept, so
    // toggling retries and the next game start records again (W19).
    bool isStoppedByError() const noexcept { return m_stoppedByError.load(); }

    // Direct folder opener
    void openRecordingsFolder();

    // App lifecycle (iOS): pauses recording while the app is inactive or in the background, where the
    // hardware video encoder is unavailable. The current segment is closed first so the replay buffer
    // survives; recording continues in a new segment once the app is active again. Main thread only.
    void setAppSuspended(bool suspended);
    bool isAppSuspended() const noexcept { return m_appSuspended.load(std::memory_order_acquire); }

#ifdef GEODE_IS_IOS
    // Opens the system share sheet for the newest replay (this launch's last save, else the newest
    // replay_*.mp4 in the recordings folder). Main thread only.
    void shareLastReplay();
#endif

    // Most recent replay written this launch (empty if none), e.g. for the iOS share sheet.
    std::filesystem::path getLastSavedReplayPath() const {
        std::lock_guard<std::mutex> lock(m_lastSavedMutex);
        return m_lastSavedReplayPath;
    }

    ActiveEncoderInfo getActiveEncoderInfo() const {
        std::lock_guard<std::mutex> lock(m_activeEncoderInfoMutex);
        return m_activeEncoderInfo;
    }

    // Active recording session configuration snapshot (read by capture threads)
    const ReplayConfig& getSessionConfig() const noexcept { return m_sessionConfig; }

private:
    ReplayManager();
    ~ReplayManager();

    void startReplay();
    void stopReplay();
    void reportCaptureBackendUnavailable();

    struct PreparedEncoder {
        std::unique_ptr<PlatformEncoder> encoder;
        std::filesystem::path path;
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
    void logFrameDropWindow(uint64_t queueDrops, uint64_t inFlightDrops, std::chrono::steady_clock::time_point now);
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
    void publishActiveEncoderInfo(const ReplayConfig& cfg);
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
    // Worker thread: closes the current segment and drops capture input while the app is suspended.
    void enterSuspendedState(
        const ReplayConfig& cfg,
        std::vector<float>& gameAudioScratch,
        std::vector<float>& resampledGameAudioScratch,
        std::vector<float>& silenceScratch
    );
    void purgeOldSegments(const ReplayConfig& cfg);
    void ensureSessionLock(const std::filesystem::path& pidTempDir);
    void releaseSessionLock(bool removeFiles);
    void cleanupTempDir();
    void joinFinalizeThread();
    void joinExportThread();
    void waitForExportBeforeExit();
    void cleanupStalePartialReplays();
    void joinStopThread();
    void finishStopCleanup();

    // Segment file ownership: files used by a running export are never deleted
    // directly, deletion is deferred until the export releases them.
    // Both require m_segmentMutex to be held.
    void deleteSegmentFileLocked(const std::filesystem::path& path);
    void flushDeferredDeletesLocked();
    void releaseExportSegments(const std::vector<std::filesystem::path>& paths);

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
    std::atomic<int> m_exportProgressPermille{-1};
    std::atomic<bool> m_shuttingDown{false};
    std::atomic<bool> m_backendUnavailable{false};
    std::atomic<bool> m_stoppedByError{false};
    std::atomic<bool> m_appSuspended{false};
    bool m_workerSuspended = false; // worker thread only
    uint64_t m_sessionId = 0; // guarded by m_queueMutex (#12, #13)
    mutable std::mutex m_lastSavedMutex;
    std::filesystem::path m_lastSavedReplayPath;

    // Asynchronous teardown thread & lifecycle state guard for non-blocking Stop/Start
    std::thread m_stopThread;
    std::mutex m_stopMutex;
    std::mutex m_lifecycleMutex;

    std::unique_ptr<PlatformEncoder> m_encoder;
    std::deque<std::shared_ptr<CapturedFrame>> m_inFlightFrames; // worker thread only (#1, #7)
    std::filesystem::path m_currentSegmentPath;
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
    CodecBitrateTable m_sessionBitrateTable{}; // per-family session bitrates for codec fallbacks (W1)
    bool m_codecFallbackNotified = false;      // one fallback notification per session (W4)
    ActiveEncoderInfo m_activeEncoderInfo{};
    mutable std::mutex m_activeEncoderInfoMutex;
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
    std::multiset<std::filesystem::path> m_exportLockedSegments;
    std::vector<std::filesystem::path> m_deferredDeletes;
    std::filesystem::path m_lastTempDir;
    mutable std::mutex m_segmentMutex;

    // Exclusive per-PID session lock inside pid_<PID> temp folder
#ifdef GEODE_IS_WINDOWS
    HANDLE m_sessionLockHandle = INVALID_HANDLE_VALUE;
#else
    int m_sessionLockFd = -1;
#endif
    std::filesystem::path m_sessionLockDir;
    std::filesystem::path m_persistedTempBase;
    std::mutex m_sessionLockMutex;

    // Debug frame drop counters (stale, queue overflow, encoder backpressure)
    std::atomic<uint64_t> m_staleDropCount{0};
    std::atomic<uint64_t> m_queueDropCount{0};
    std::atomic<uint64_t> m_inFlightDropCount{0};
    std::atomic<uint64_t> m_micQueueDroppedFrames{0}; // mic audio dropped by the worker queue limit (W15)
    uint64_t m_lastLoggedMicQueueDrops = 0;
    std::chrono::steady_clock::time_point m_lastDropLogTime{};
    uint64_t m_lastLoggedQueueDrops = 0;
    uint64_t m_lastLoggedInFlightDrops = 0;
    // Rotation diagnostics for drop bursts (worker thread only, W11)
    uint64_t m_rotationCount = 0;
    std::chrono::steady_clock::time_point m_lastRotationTime{};
    std::chrono::steady_clock::time_point m_prepareStartedAt{};

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

#endif // GEODE_IS_WINDOWS || GEODE_IS_MACOS || GEODE_IS_IOS
