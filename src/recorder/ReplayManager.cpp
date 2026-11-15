#include "ReplayManager.hpp"

#ifdef GEODE_IS_WINDOWS

#include "../ui/ReplayNotification.hpp"
#include "../ui/ReplayPopup.hpp"
#include <Geode/binding/FMODAudioEngine.hpp>
#include <windows.h>
#include <shellapi.h>
#include <sstream>
#include <iomanip>
#include <ctime>
#include <cmath>
#include <algorithm>

namespace replay {

static constexpr double SEGMENT_DURATION = 30.0; // 30-second rolling chunks
static constexpr double SEGMENT_PREWARM_SECONDS = 25.0; // pre-warm next encoder 5s before rotation
static constexpr double MAX_SEGMENT_DURATION = 120.0;   // safety fallback if prepare stalls
static constexpr double SAVE_PENDING_SWITCH_TIMEOUT = 5.0; // safety fallback for non-blocking F6 switch
static constexpr size_t MAX_QUEUED_VIDEO_BYTES = 64ull * 1024 * 1024;
static constexpr size_t MAX_IN_FLIGHT_ENCODER_FRAMES = 16;
static constexpr size_t MAX_MIC_MIX_SAMPLES = AudioGrabber::kMicSampleRate * 2 * 5; // ~5s stereo cap
static constexpr int MAX_ENCODER_START_FAILURES = 3;
static constexpr int MAX_CONSECUTIVE_WRITE_ERRORS = 5;

static bool isSegmentFile(const std::filesystem::path& path) {
    std::wstring name = path.filename().wstring();
    // Match exact pattern "seg_<digits>.mp4"
    if (name.size() <= 8) return false; // at least "seg_1.mp4"
    if (name.compare(0, 4, L"seg_") != 0) return false;
    if (name.compare(name.size() - 4, 4, L".mp4") != 0) return false;
    for (size_t i = 4; i + 4 < name.size(); ++i) {
        if (name[i] < L'0' || name[i] > L'9') return false;
    }
    return true;
}

ReplayManager* ReplayManager::get() {
    static ReplayManager instance;
    return &instance;
}

ReplayManager::ReplayManager() = default;

ReplayManager::~ReplayManager() {
    // Do NOT touch other singletons or OpenGL here (static destruction order is unspecified).
    // Clean shutdown happens in ReplayManager::shutdown() hooked on $on_game(Exiting).
    m_running.store(false);
    m_prepareCancel.store(true);
    m_cv.notify_all();
    if (m_workerThread.joinable()) {
        m_workerThread.join();
    }
    joinPrepareThread();
    discardPreparedEncoder();
    joinFinalizeThread();
    joinExportThread();
}

std::filesystem::path ReplayManager::getTempDir(bool createIfMissing) const {
    return ReplayConfig::get().getTempDirectory(createIfMissing);
}

void ReplayManager::cleanupTempDir() {
    std::lock_guard<std::mutex> lock(m_segmentMutex);

    auto cleanSingleDir = [this](const std::filesystem::path& dir) {
        if (dir.empty()) return;
        std::error_code ec;
        if (!std::filesystem::exists(dir, ec) || !std::filesystem::is_directory(dir, ec)) return;
        for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
            std::error_code fileEc;
            if (!entry.is_regular_file(fileEc) || !isSegmentFile(entry.path())) {
                continue;
            }
            auto wpath = entry.path().wstring();
            if (m_exportLockedSegments.find(wpath) != m_exportLockedSegments.end()) {
                m_deferredDeletes.push_back(wpath);
            } else {
                std::filesystem::remove(entry.path(), fileEc);
            }
        }
    };

    auto curTempDir = getTempDir(false);
    cleanSingleDir(curTempDir);
    if (!m_lastTempDir.empty() && m_lastTempDir != curTempDir) {
        cleanSingleDir(m_lastTempDir);
    }
}

void ReplayManager::deleteSegmentFileLocked(const std::wstring& path) {
    if (path.empty()) return;
    if (m_exportLockedSegments.find(path) != m_exportLockedSegments.end()) {
        // Export thread is currently reading this segment; defer deletion until export completes
        m_deferredDeletes.push_back(path);
        return;
    }
    std::error_code ec;
    std::filesystem::remove(path, ec);
}

void ReplayManager::flushDeferredDeletesLocked() {
    auto it = m_deferredDeletes.begin();
    while (it != m_deferredDeletes.end()) {
        if (m_exportLockedSegments.find(*it) == m_exportLockedSegments.end()) {
            std::error_code ec;
            std::filesystem::remove(*it, ec);
            it = m_deferredDeletes.erase(it);
        } else {
            ++it;
        }
    }
}

void ReplayManager::releaseExportSegments(const std::vector<std::wstring>& paths) {
    std::lock_guard<std::mutex> lock(m_segmentMutex);
    for (const auto& p : paths) {
        auto it = m_exportLockedSegments.find(p);
        if (it != m_exportLockedSegments.end()) {
            m_exportLockedSegments.erase(it);
        }
    }
    flushDeferredDeletesLocked();
}

void ReplayManager::joinPrepareThread() {
    std::lock_guard<std::mutex> lock(m_prepareMutex);
    if (m_prepareThread.joinable()) {
        m_prepareThread.join();
    }
    m_prepareInFlight.store(false);
}

void ReplayManager::discardPreparedEncoder() {
    PreparedEncoder stale;
    {
        std::lock_guard<std::mutex> lock(m_prepareDataMutex);
        stale = std::move(m_prepared);
        m_prepared = {};
    }
    if (stale.encoder) {
        if (stale.encoder->isRecording()) {
            stale.encoder->finish();
        }
        stale.encoder.reset();
    }
    if (!stale.path.empty()) {
        DeleteFileW(stale.path.c_str());
    }
}

void ReplayManager::joinFinalizeThread() {
    std::lock_guard<std::mutex> lock(m_finalizeMutex);
    if (m_finalizeThread.joinable()) {
        m_finalizeThread.join();
    }
    m_finalizeInFlight.store(false);
    m_pendingFinalize = {};
}

void ReplayManager::joinExportThread() {
    std::lock_guard<std::mutex> lock(m_exportMutex);
    if (m_exportThread.joinable()) {
        m_exportThread.join();
    }
}

void ReplayManager::reclaimInFlightFrames(bool forceClear) {
    if (forceClear) {
        for (auto& f : m_inFlightFrames) {
            if (f && !m_shuttingDown.load()) {
                FrameGrabber::get()->recycleFrame(std::move(f));
            }
        }
        m_inFlightFrames.clear();
        return;
    }

    auto it = m_inFlightFrames.begin();
    while (it != m_inFlightFrames.end()) {
        if (!*it || !(*it)->buffer || FrameGrabber::isMediaBufferUnique((*it)->buffer.get())) {
            if (*it && !m_shuttingDown.load()) {
                FrameGrabber::get()->recycleFrame(std::move(*it));
            }
            it = m_inFlightFrames.erase(it);
        } else {
            ++it;
        }
    }
}

void ReplayManager::initialize() {
    m_shuttingDown.store(false);
    ReplayConfig::refreshNativeFrameHeight();
    ReplayConfig::get().loadFromSettings();
    m_sessionConfig = ReplayConfig::get().snapshot();
    cleanupTempDir();

    // Frame grabber callback (render thread -> worker queue)
    FrameGrabber::get()->setFrameCallback([this](std::shared_ptr<CapturedFrame> frame) {
        if (!frame) return;
        std::shared_ptr<CapturedFrame> dropped;
        {
            std::lock_guard<std::mutex> lock(m_queueMutex);
            if (!m_running.load(std::memory_order_relaxed)) {
                dropped = std::move(frame);
            } else {
                size_t frameBytes = std::max<size_t>(1, frame->byteSize);
                size_t maxFrames = std::clamp<size_t>(MAX_QUEUED_VIDEO_BYTES / frameBytes, 4, 16);
                if (m_frameQueue.size() < maxFrames) {
                    m_frameQueue.push_back(std::move(frame));
                } else {
                    dropped = std::move(frame);
                }
            }
        }
        if (dropped) {
            FrameGrabber::get()->recycleFrame(std::move(dropped));
        } else {
            m_cv.notify_one();
        }
    });

    AudioGrabber::get()->setMicAudioCallback([this](std::shared_ptr<AudioChunk> chunk) {
        if (!chunk) return;
        bool queued = false;
        {
            std::lock_guard<std::mutex> lock(m_queueMutex);
            if (m_running.load(std::memory_order_relaxed)) {
                if (m_micAudioQueue.size() >= 150) {
                    m_micAudioQueue.pop_front();
                }
                m_micAudioQueue.push_back(std::move(chunk));
                queued = true;
            }
        }
        if (queued) {
            m_cv.notify_one();
        }
    });

    AudioGrabber::get()->initialize();

    if (ReplayConfig::get().enabled.load()) {
        startReplay();
    }
}

void ReplayManager::shutdown() {
    if (m_shuttingDown.exchange(true)) return;

    stopReplay();
    joinFinalizeThread();
    joinExportThread();
    AudioGrabber::get()->cleanup();
    FrameGrabber::get()->cleanup(true);
    cleanupTempDir();
}

void ReplayManager::startReplay() {
    if (m_running.load() || m_shuttingDown.load()) return;

    ReplayConfig::refreshNativeFrameHeight();

    // Ensure any previous worker thread has exited completely (e.g. after fatal error self-stop)
    if (m_workerThread.joinable()) {
        m_workerThread.join();
    }
    m_prepareCancel.store(true);
    joinPrepareThread();
    discardPreparedEncoder();
    joinFinalizeThread();

    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        for (auto& f : m_frameQueue) {
            FrameGrabber::get()->recycleFrame(std::move(f));
        }
        m_frameQueue.clear();
        m_micAudioQueue.clear();
    }
    reclaimInFlightFrames(true);
    AudioGrabber::get()->resetGameAudioBuffer();
    MediaFoundationEncoder::resetCodecCache();

    m_videoWidth = 0;
    m_videoHeight = 0;
    // Note: m_segmentIndex is intentionally NOT reset so segment filenames never
    // collide with files still being read by an in-flight export thread.
    m_lastVideoHns = -1;
    m_gameAudioSamples = 0;
    m_micAudioSamples = 0;
    m_isContinuationSegment = false;
    m_segmentGameSampleRate = AudioGrabber::get()->getEncodedGameSampleRate();
    m_encoderFailCount = 0;
    m_videoWriteErrors = 0;
    m_gameAudioWriteErrors = 0;
    m_micAudioWriteErrors = 0;
    m_writeRecoveryAttempted = false;
    m_lastStartError = {};
    m_nextEncoderRetryTime = {};
    m_prepareFailedCount = 0;
    m_prepareFailedLastAttempt.store(false);
    m_nextPrepareRetryTime = {};
    m_savePendingSwitch = false;
    m_savePendingSince = {};
    m_carryGameAudio = {};
    m_carryMicAudio.clear();
    m_currentSegmentPath.clear();
    m_currentFormatKey.clear();
    resetMicMixer();
    m_gameResampler.reset();

    m_sessionConfig = ReplayConfig::get().snapshot();
    if (m_sessionConfig.recordMic.load()) {
        AudioGrabber::get()->startMicCapture();
    } else {
        AudioGrabber::get()->stopMicCapture();
    }

    int targetW = 0, targetH = 0;
    if (auto view = cocos2d::CCEGLView::get()) {
        auto fs = view->getFrameSize();
        int initW = static_cast<int>(fs.width);
        int initH = static_cast<int>(fs.height);
        if (initW >= 16 && initH >= 16) {
            std::tie(targetW, targetH) = m_sessionConfig.getResolutionDimensions(initW, initH);
        }
    }
    FrameGrabber::get()->setTargetResolution(targetW, targetH);

    m_currentSegmentStart = std::chrono::steady_clock::now();
    FrameGrabber::get()->setCapturing(true);
    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        ++m_sessionId;
        m_saveRequested.store(false);
        m_running.store(true);
    }
    m_workerThread = std::thread(&ReplayManager::workerThreadFunc, this, m_sessionConfig);
}

void ReplayManager::stopReplay() {
    FrameGrabber::get()->setCapturing(false);
    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        m_running.store(false);
        ++m_sessionId;
    }
    m_cv.notify_all();

    if (m_workerThread.joinable()) {
        m_workerThread.join();
    }
    m_prepareCancel.store(true);
    joinPrepareThread();
    discardPreparedEncoder();
    joinFinalizeThread();

    // Always close the microphone when replay is stopped so the OS mic indicator turns off
    if (!m_shuttingDown.load()) {
        AudioGrabber::get()->stopMicCapture();
    }

    if (m_encoder) {
        if (m_encoder->isRecording()) {
            m_encoder->finish();
        }
        m_encoder.reset();
    }
    reclaimInFlightFrames(true);
    m_carryGameAudio = {};
    m_carryMicAudio.clear();
    m_savePendingSwitch = false;

    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        if (m_saveRequested.exchange(false)) {
            m_saving.store(false);
        }
        for (auto& f : m_frameQueue) {
            if (!m_shuttingDown.load()) {
                FrameGrabber::get()->recycleFrame(std::move(f));
            }
        }
        m_frameQueue.clear();
        m_micAudioQueue.clear();
    }

    {
        std::lock_guard<std::mutex> lock(m_segmentMutex);
        for (const auto& seg : m_activeSegments) {
            deleteSegmentFileLocked(seg.filePath);
        }
        m_activeSegments.clear();

        if (!m_currentSegmentPath.empty()) {
            deleteSegmentFileLocked(m_currentSegmentPath);
            m_currentSegmentPath.clear();
        }
        m_currentFormatKey.clear();
    }
}

void ReplayManager::setReplayEnabled(bool enabled) {
    auto& cfg = ReplayConfig::get();
    cfg.enabled = enabled;
    if (enabled) {
        startReplay();
    } else {
        stopReplay();
    }
}

void ReplayManager::toggleReplay() {
    auto& cfg = ReplayConfig::get();
    bool newState = !cfg.enabled.load();
    cfg.enabled = newState;
    cfg.saveToSettings();

    if (newState) {
        startReplay();
    } else {
        stopReplay();
    }
}

void ReplayManager::handleFatalRecordingError(const std::string& reason) {
    geode::log::error("Instant Replay: {}", reason);

    uint64_t failedSession = 0;
    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        m_running.store(false);
        if (m_saveRequested.exchange(false)) {
            m_saving.store(false);
        }
        failedSession = m_sessionId;
    }
    FrameGrabber::get()->setCapturing(false);

    geode::Loader::get()->queueInMainThread([this, reason, failedSession]() {
        if (m_shuttingDown.load()) return;

        bool sameSession = false;
        {
            std::lock_guard<std::mutex> lock(m_queueMutex);
            sameSession = (m_sessionId == failedSession);
        }
        if (sameSession) {
            ReplayConfig::get().enabled = false;
            stopReplay();
            ReplayConfig::get().saveToSettings();
            if (auto* popup = ReplayPopup::get()) {
                popup->refreshUI();
                popup->updateStatusDisplay(0.0f);
            }
        }
        geode::Notification::create(reason, geode::NotificationIcon::Error, 4.0f)->show();
    });
}

// ---------------------------------------------------------------------------
// Single-track microphone mixer (used when recordMic == true && splitAudioTracks == false)
// ---------------------------------------------------------------------------

void ReplayManager::resetMicMixer() {
    m_micMixFifo.clear();
    m_micMixReadPos = 0;
    m_micMixRate = 0;
    m_micMixResampler.reset();
}

void ReplayManager::appendMicToMix(const AudioChunk& chunk, int targetRate, size_t startFrameOffset, size_t maxFrameCount) {
    if (chunk.samples.size() < 2 || targetRate <= 0) return;
    size_t totalFrames = chunk.samples.size() / 2;
    if (startFrameOffset >= totalFrames) return;
    size_t srcFrames = std::min(totalFrames - startFrameOffset, maxFrameCount);
    if (srcFrames == 0) return;
    const float* srcPtr = chunk.samples.data() + startFrameOffset * 2;

    if (m_micMixRate != targetRate) {
        resetMicMixer();
        m_micMixRate = targetRate;
    }

    m_micMixResampler.process(srcPtr, srcFrames, AudioGrabber::kMicSampleRate, targetRate, m_micMixFifo);

    size_t avail = m_micMixFifo.size() - m_micMixReadPos;
    if (avail > MAX_MIC_MIX_SAMPLES) {
        size_t excess = (avail - MAX_MIC_MIX_SAMPLES) & ~static_cast<size_t>(1);
        m_micMixReadPos += excess;
    }
    if (m_micMixReadPos >= 16384 && m_micMixReadPos >= m_micMixFifo.size() / 2) {
        m_micMixFifo.erase(m_micMixFifo.begin(), m_micMixFifo.begin() + static_cast<std::ptrdiff_t>(m_micMixReadPos));
        m_micMixReadPos = 0;
    }
}

void ReplayManager::mixMicInto(float* stereoSamples, size_t numFrames) {
    size_t availSamples = m_micMixFifo.size() - m_micMixReadPos;
    if (availSamples < 2 || !stereoSamples || numFrames == 0) return;
    size_t availFrames = availSamples / 2;
    size_t mixFrames = std::min(numFrames, availFrames);

    const float* micPtr = m_micMixFifo.data() + m_micMixReadPos;
    for (size_t i = 0; i < mixFrames; ++i) {
        size_t base = i * 2;
        stereoSamples[base] = std::clamp(stereoSamples[base] + micPtr[base], -1.0f, 1.0f);
        stereoSamples[base + 1] = std::clamp(stereoSamples[base + 1] + micPtr[base + 1], -1.0f, 1.0f);
    }
    m_micMixReadPos += mixFrames * 2;
    if (m_micMixReadPos >= m_micMixFifo.size()) {
        m_micMixFifo.clear();
        m_micMixReadPos = 0;
    } else if (m_micMixReadPos >= 16384 && m_micMixReadPos >= m_micMixFifo.size() / 2) {
        m_micMixFifo.erase(m_micMixFifo.begin(), m_micMixFifo.begin() + static_cast<std::ptrdiff_t>(m_micMixReadPos));
        m_micMixReadPos = 0;
    }
}

ReplayManager::WriteCheckResult ReplayManager::handleEncoderWriteError(
    const char* streamLabel,
    const EncoderOpResult& res,
    int& errorCounter,
    const ReplayConfig& cfg,
    std::string& outFatalError
) {
    std::string codec = m_encoder ? m_encoder->activeCodecName() : "encoder";
    std::string detail = res.message.empty() ? MediaFoundationEncoder::formatHr(res.hr) : res.message;

    if (MediaFoundationEncoder::isFatalStorageError(res.hr)) {
        outFatalError = "Instant Replay stopped: " + codec + " failed writing " +
                        streamLabel + " (" + detail + ")";
        return WriteCheckResult::Fatal;
    }

    errorCounter++;
    if (errorCounter < MAX_CONSECUTIVE_WRITE_ERRORS) {
        return WriteCheckResult::ToleratedError;
    }

    geode::log::error("Instant Replay: {} {} write errors on {} ({})",
                      errorCounter, streamLabel, codec, detail);

    if (!m_writeRecoveryAttempted) {
        m_writeRecoveryAttempted = true;
        geode::log::warn("Instant Replay: recreating {} encoder via fallback cascade after {} write errors",
                         codec, streamLabel);

        m_prepareCancel.store(true);
        joinPrepareThread();
        discardPreparedEncoder();

        finalizeCurrentSegment(true, cfg);
        MediaFoundationEncoder::resetCodecCache();

        if (startNewSegment(cfg, std::chrono::steady_clock::now(), true)) {
            return WriteCheckResult::Recovered;
        }

        std::string startDetail = m_lastStartError.message.empty()
            ? MediaFoundationEncoder::formatHr(m_lastStartError.hr)
            : m_lastStartError.message;
        outFatalError = "Instant Replay stopped: " + codec + " " + streamLabel +
                        " write error (" + detail + "); recovery failed (" + startDetail + ")";
        return WriteCheckResult::Fatal;
    }

    outFatalError = "Instant Replay stopped: repeated " + std::string(streamLabel) +
                    " write failure on " + codec + " (" + detail + ")";
    return WriteCheckResult::Fatal;
}

bool ReplayManager::flushMicMixFifo(
    int rate,
    bool checkErrors,
    const ReplayConfig& cfg,
    bool& outRecovered,
    std::string& outFatalError
) {
    outRecovered = false;
    size_t availSamples = m_micMixFifo.size() - m_micMixReadPos;
    if (availSamples < 2 || !m_encoder || rate <= 0) return true;
    int numSamples = static_cast<int>(availSamples / 2);
    const float* dataPtr = m_micMixFifo.data() + m_micMixReadPos;
    int64_t ts = (m_gameAudioSamples * 10000000LL) / rate;
    EncoderOpResult res = m_encoder->writeGameAudio(dataPtr, numSamples, 2, rate, ts);
    m_micMixFifo.clear();
    m_micMixReadPos = 0;
    if (res) {
        m_gameAudioSamples += numSamples;
        if (checkErrors) m_gameAudioWriteErrors = 0;
        return true;
    }
    if (!checkErrors) return true;
    WriteCheckResult wc = handleEncoderWriteError("game audio", res, m_gameAudioWriteErrors, cfg, outFatalError);
    if (wc == WriteCheckResult::Recovered) {
        outRecovered = true;
        return false;
    }
    return wc != WriteCheckResult::Fatal;
}

bool ReplayManager::writeAudioWithResync(
    bool isGameAudio,
    float* samples,
    int numSamples,
    int rate,
    std::chrono::steady_clock::time_point timestamp,
    const ReplayConfig& cfg,
    std::vector<float>& silenceScratch,
    bool& outRecovered,
    std::string& outFatalError
) {
    outRecovered = false;
    constexpr int kChannels = 2;
    if (!samples || numSamples <= 0 || rate <= 0 || !m_encoder) return true;

    int64_t chunkStartHns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        timestamp - m_currentSegmentStart).count() / 100;
    int64_t chunkDurationHns = (static_cast<int64_t>(numSamples) * 10000000LL) / rate;

    // On the initial segment (or after a >100ms recovery stall), trim samples captured before
    // m_currentSegmentStart. On seamless 30s/F6 continuation segments, packets that started a few
    // milliseconds before nextSegStart contain unwritten PCM immediately following the closed segment,
    // so never clip them (#4).
    if (chunkStartHns < 0) {
        if (!m_isContinuationSegment || chunkStartHns < -1000000LL) {
            if (chunkStartHns + chunkDurationHns <= 0) return true;
            int64_t trimSamples = ((-chunkStartHns) * rate) / 10000000LL;
            if (trimSamples >= numSamples) return true;
            if (trimSamples > 0) {
                samples += static_cast<size_t>(trimSamples) * kChannels;
                numSamples -= static_cast<int>(trimSamples);
            }
            chunkStartHns = 0;
        }
    }

    int64_t& sampleCounter = isGameAudio ? m_gameAudioSamples : m_micAudioSamples;
    int& errorCounter = isGameAudio ? m_gameAudioWriteErrors : m_micAudioWriteErrors;
    const char* streamLabel = isGameAudio ? "game audio" : "microphone audio";
    int64_t expectedHns = (sampleCounter * 10000000LL) / rate;
    int64_t gapHns = chunkStartHns - expectedHns;

    if (gapHns > 400000LL) { // > 40ms positive gap
        if (gapHns <= 5000000LL) { // <= 500ms: bridge with silence
            int silenceSamples = static_cast<int>((gapHns * rate) / 10000000LL);
            if (silenceSamples > 0) {
                silenceScratch.assign(static_cast<size_t>(silenceSamples) * kChannels, 0.0f);
                if (isGameAudio && m_mixMicIntoGame) {
                    mixMicInto(silenceScratch.data(), static_cast<size_t>(silenceSamples));
                }
                EncoderOpResult silenceRes = isGameAudio
                    ? m_encoder->writeGameAudio(silenceScratch.data(), silenceSamples, kChannels, rate, expectedHns)
                    : m_encoder->writeMicAudio(silenceScratch.data(), silenceSamples, kChannels, rate, expectedHns);
                if (silenceRes) {
                    sampleCounter += silenceSamples;
                } else if (MediaFoundationEncoder::isFatalStorageError(silenceRes.hr)) {
                    std::string codec = m_encoder ? m_encoder->activeCodecName() : "encoder";
                    outFatalError = "Instant Replay stopped: " + codec + " failed writing " +
                                    streamLabel + " (" + silenceRes.message + ")";
                    return false;
                }
            }
        } else {
            // > 500ms gap: jump sample counter directly to real capture timestamp
            sampleCounter = ( std::max<int64_t>(0, chunkStartHns) * rate ) / 10000000LL;
            if (isGameAudio && m_mixMicIntoGame) {
                m_micMixFifo.clear();
                m_micMixReadPos = 0;
            }
        }
    } else if (gapHns < -400000LL) { // < -40ms negative gap: sampleCounter is ahead of real time
        int64_t overlapSamples = ((-gapHns) * rate) / 10000000LL;
        if (overlapSamples >= numSamples) return true;
        if (overlapSamples > 0) {
            samples += static_cast<size_t>(overlapSamples) * kChannels;
            numSamples -= static_cast<int>(overlapSamples);
        }
    }

    int64_t ts = (sampleCounter * 10000000LL) / rate;
    if (isGameAudio && m_mixMicIntoGame) {
        mixMicInto(samples, static_cast<size_t>(numSamples));
    }

    EncoderOpResult res = isGameAudio
        ? m_encoder->writeGameAudio(samples, numSamples, kChannels, rate, ts)
        : m_encoder->writeMicAudio(samples, numSamples, kChannels, rate, ts);
    if (res) {
        sampleCounter += numSamples;
        errorCounter = 0;
        return true;
    }

    WriteCheckResult wc = handleEncoderWriteError(streamLabel, res, errorCounter, cfg, outFatalError);
    if (wc == WriteCheckResult::Recovered) {
        outRecovered = true;
        return false;
    }
    return wc != WriteCheckResult::Fatal;
}

void ReplayManager::feedMicChunkIntoMix(
    const AudioChunk& chunk,
    int encGameRate,
    size_t startFrameOffset,
    size_t maxFrameCount
) {
    if (chunk.samples.size() < 2 || maxFrameCount == 0) return;
    constexpr int rate = AudioGrabber::kMicSampleRate;
    size_t totalFrames = chunk.samples.size() / 2;
    if (startFrameOffset >= totalFrames) return;
    size_t availFrames = std::min(totalFrames - startFrameOffset, maxFrameCount);

    int64_t chunkStartHns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        chunk.timestamp - m_currentSegmentStart).count() / 100;
    chunkStartHns += (static_cast<int64_t>(startFrameOffset) * 10000000LL) / rate;
    int64_t chunkDurationHns = (static_cast<int64_t>(availFrames) * 10000000LL) / rate;

    size_t trim = 0;
    if (chunkStartHns < 0 && (!m_isContinuationSegment || chunkStartHns < -1000000LL)) {
        if (chunkStartHns + chunkDurationHns <= 0) return;
        int64_t trimSamples = ((-chunkStartHns) * rate) / 10000000LL;
        if (trimSamples >= static_cast<int64_t>(availFrames)) return;
        if (trimSamples > 0) trim = static_cast<size_t>(trimSamples);
    }
    appendMicToMix(chunk, encGameRate, startFrameOffset + trim, availFrames - trim);
}

bool ReplayManager::writeKeepAliveSilence(
    const ReplayConfig& sessionConfig,
    int encGameRate,
    std::vector<float>& silenceScratch,
    bool& outRecovered,
    std::string& outFatalError
) {
    outRecovered = false;
    // Synthesize continuous keep-alive silence on any idle audio stream lagging >200ms behind video (#1).
    // Keeps MP4 muxer streams interleaved without buffering unbounded packets when e.g. no mic is connected.
    if (m_lastVideoHns < 0) return true;

    int64_t gameAudioHns = (m_gameAudioSamples * 10000000LL) / encGameRate;
    if (m_lastVideoHns - gameAudioHns > 2000000LL) {
        int keepAliveSamples = std::max(480, encGameRate / 10); // 100ms
        silenceScratch.assign(static_cast<size_t>(keepAliveSamples) * 2, 0.0f);
        if (m_mixMicIntoGame) {
            mixMicInto(silenceScratch.data(), static_cast<size_t>(keepAliveSamples));
        }
        EncoderOpResult res = m_encoder->writeGameAudio(silenceScratch.data(), keepAliveSamples, 2, encGameRate, gameAudioHns);
        if (res) {
            m_gameAudioSamples += keepAliveSamples;
            m_gameAudioWriteErrors = 0;
        } else {
            WriteCheckResult wc = handleEncoderWriteError("game audio", res, m_gameAudioWriteErrors, sessionConfig, outFatalError);
            if (wc == WriteCheckResult::Recovered) {
                outRecovered = true;
                return false;
            }
            if (wc == WriteCheckResult::Fatal) return false;
        }
    }

    if (sessionConfig.recordMic.load() && sessionConfig.splitAudioTracks.load()) {
        constexpr int kMicRate = AudioGrabber::kMicSampleRate;
        int64_t micAudioHns = (m_micAudioSamples * 10000000LL) / kMicRate;
        if (m_lastVideoHns - micAudioHns > 2000000LL) {
            constexpr int kKeepAliveMicSamples = kMicRate / 10; // 4800 stereo frames (100ms)
            silenceScratch.assign(static_cast<size_t>(kKeepAliveMicSamples) * 2, 0.0f);
            EncoderOpResult res = m_encoder->writeMicAudio(silenceScratch.data(), kKeepAliveMicSamples, 2, kMicRate, micAudioHns);
            if (res) {
                m_micAudioSamples += kKeepAliveMicSamples;
                m_micAudioWriteErrors = 0;
            } else {
                WriteCheckResult wc = handleEncoderWriteError("microphone audio", res, m_micAudioWriteErrors, sessionConfig, outFatalError);
                if (wc == WriteCheckResult::Recovered) {
                    outRecovered = true;
                    return false;
                }
                if (wc == WriteCheckResult::Fatal) return false;
            }
        }
    }

    return true;
}

bool ReplayManager::processPendingAudio(
    const ReplayConfig& sessionConfig,
    std::vector<float>& gameAudioScratch,
    std::vector<float>& resampledGameAudioScratch,
    std::vector<float>& silenceScratch,
    std::string& outFatalError
) {
    if (!m_encoder || !m_encoder->isRecording()) return true;

    std::vector<std::shared_ptr<AudioChunk>> pendingMicAudio;
    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        while (!m_micAudioQueue.empty()) {
            pendingMicAudio.push_back(std::move(m_micAudioQueue.front()));
            m_micAudioQueue.pop_front();
        }
    }

    int gameSampleRate = 48000;
    std::chrono::steady_clock::time_point gameAudioTime{};
    size_t gameAudioFrames = AudioGrabber::get()->drainGameAudio(gameAudioScratch, gameSampleRate, gameAudioTime);

    const int encGameRate = (m_segmentGameSampleRate > 0) ? m_segmentGameSampleRate : 48000;
    bool recovered = false;

    // 1. In single-track mode (recordMic && !splitAudioTracks), feed mic packets into mix FIFO first
    if (m_mixMicIntoGame) {
        for (const auto& micAudio : pendingMicAudio) {
            if (!micAudio || micAudio->samples.size() < 2) continue;
            feedMicChunkIntoMix(*micAudio, encGameRate);
        }
        pendingMicAudio.clear();
    }

    // 2. Encode drained FMOD game audio
    if (gameAudioFrames > 0) {
        int srcRate = (gameSampleRate > 0) ? gameSampleRate : 48000;
        float* samplesPtr = gameAudioScratch.data();
        int numSamples = static_cast<int>(gameAudioFrames);

        if (srcRate != encGameRate) {
            resampledGameAudioScratch.clear();
            m_gameResampler.process(gameAudioScratch.data(), gameAudioFrames,
                                    srcRate, encGameRate, resampledGameAudioScratch);
            samplesPtr = resampledGameAudioScratch.data();
            numSamples = static_cast<int>(resampledGameAudioScratch.size() / 2);
        }

        if (numSamples > 0) {
            if (!writeAudioWithResync(true, samplesPtr, numSamples, encGameRate, gameAudioTime,
                                      sessionConfig, silenceScratch, recovered, outFatalError)) {
                return recovered;
            }
        }
    } else if (m_mixMicIntoGame) {
        // If no game audio arrived for >=100ms in single-track mode, flush queued mic audio directly
        size_t thresholdSamples = static_cast<size_t>(encGameRate / 10) * 2;
        if ((m_micMixFifo.size() - m_micMixReadPos) >= thresholdSamples) {
            if (!flushMicMixFifo(encGameRate, true, sessionConfig, recovered, outFatalError)) {
                return recovered;
            }
        }
    }

    // 3. Encode separate microphone track (recordMic && splitAudioTracks)
    for (const auto& micAudio : pendingMicAudio) {
        if (!micAudio || micAudio->samples.size() < 2) continue;
        int numSamples = static_cast<int>(micAudio->samples.size() / 2);
        if (!writeAudioWithResync(false, micAudio->samples.data(), numSamples,
                                  AudioGrabber::kMicSampleRate, micAudio->timestamp,
                                  sessionConfig, silenceScratch, recovered, outFatalError)) {
            return recovered;
        }
    }

    if (!writeKeepAliveSilence(sessionConfig, encGameRate, silenceScratch, recovered, outFatalError)) {
        return recovered;
    }
    return true;
}

bool ReplayManager::splitAudioAtBoundary(
    std::chrono::steady_clock::time_point boundaryTime,
    const ReplayConfig& cfg,
    std::vector<float>& gameAudioScratch,
    std::vector<float>& resampledGameAudioScratch,
    std::vector<float>& silenceScratch,
    bool& outRecovered,
    std::string& outFatalError
) {
    outRecovered = false;
    if (!m_encoder || !m_encoder->isRecording()) return true;

    std::vector<std::shared_ptr<AudioChunk>> pendingMicAudio;
    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        while (!m_micAudioQueue.empty()) {
            pendingMicAudio.push_back(std::move(m_micAudioQueue.front()));
            m_micAudioQueue.pop_front();
        }
    }

    int gameSampleRate = 48000;
    std::chrono::steady_clock::time_point gameAudioTime{};
    size_t gameAudioFrames = AudioGrabber::get()->drainGameAudio(gameAudioScratch, gameSampleRate, gameAudioTime);

    const int encGameRate = (m_segmentGameSampleRate > 0) ? m_segmentGameSampleRate : 48000;
    constexpr int kMicRate = AudioGrabber::kMicSampleRate;

    // Split mic packets across boundaryTime: frames before boundaryTime close segment A,
    // remaining frames stay buffered for segment B.
    std::vector<std::shared_ptr<AudioChunk>> closingMicChunks;
    for (const auto& micAudio : pendingMicAudio) {
        if (!micAudio || micAudio->samples.size() < 2) continue;
        int64_t totalMicFrames = static_cast<int64_t>(micAudio->samples.size() / 2);
        double offsetSec = std::chrono::duration<double>(boundaryTime - micAudio->timestamp).count();
        int64_t splitMic = std::clamp<int64_t>(static_cast<int64_t>(std::llround(offsetSec * kMicRate)), 0, totalMicFrames);

        if (splitMic > 0) {
            if (m_mixMicIntoGame) {
                feedMicChunkIntoMix(*micAudio, encGameRate, 0, static_cast<size_t>(splitMic));
            } else {
                closingMicChunks.push_back(micAudio);
                if (splitMic < totalMicFrames) {
                    auto head = std::make_shared<AudioChunk>();
                    head->samples.assign(micAudio->samples.begin(), micAudio->samples.begin() + static_cast<std::ptrdiff_t>(splitMic * 2));
                    head->timestamp = micAudio->timestamp;
                    closingMicChunks.back() = std::move(head);
                }
            }
        }
        if (splitMic < totalMicFrames) {
            auto tail = std::make_shared<AudioChunk>();
            tail->samples.assign(micAudio->samples.begin() + static_cast<std::ptrdiff_t>(splitMic * 2), micAudio->samples.end());
            int64_t deltaNs = (splitMic * 1000000000LL) / kMicRate;
            tail->timestamp = micAudio->timestamp + std::chrono::nanoseconds(deltaNs);
            m_carryMicAudio.push_back(std::move(tail));
        }
    }

    auto clearCarryOnRecover = [this, &outRecovered]() {
        m_carryGameAudio = {};
        m_carryMicAudio.clear();
        outRecovered = true;
    };

    // Resample drained game audio once before splitting to avoid resampler reset clicks at segment join.
    if (gameAudioFrames > 0) {
        int srcRate = (gameSampleRate > 0) ? gameSampleRate : 48000;
        float* samplesPtr = gameAudioScratch.data();
        int64_t totalGameFrames = static_cast<int64_t>(gameAudioFrames);

        if (srcRate != encGameRate) {
            resampledGameAudioScratch.clear();
            m_gameResampler.process(gameAudioScratch.data(), gameAudioFrames,
                                    srcRate, encGameRate, resampledGameAudioScratch);
            samplesPtr = resampledGameAudioScratch.data();
            totalGameFrames = static_cast<int64_t>(resampledGameAudioScratch.size() / 2);
        }

        double offsetSec = std::chrono::duration<double>(boundaryTime - gameAudioTime).count();
        int64_t splitGame = std::clamp<int64_t>(static_cast<int64_t>(std::llround(offsetSec * encGameRate)), 0, totalGameFrames);

        if (splitGame > 0) {
            bool rec = false;
            if (!writeAudioWithResync(true, samplesPtr, static_cast<int>(splitGame), encGameRate, gameAudioTime,
                                      cfg, silenceScratch, rec, outFatalError)) {
                if (rec) {
                    clearCarryOnRecover();
                    return true;
                }
                return false;
            }
        }
        if (splitGame < totalGameFrames) {
            m_carryGameAudio.rate = encGameRate;
            int64_t deltaNs = (splitGame * 1000000000LL) / encGameRate;
            m_carryGameAudio.timestamp = gameAudioTime + std::chrono::nanoseconds(deltaNs);
            m_carryGameAudio.samples.assign(samplesPtr + splitGame * 2, samplesPtr + totalGameFrames * 2);
        }
    } else if (m_mixMicIntoGame) {
        size_t thresholdSamples = static_cast<size_t>(encGameRate / 10) * 2;
        if ((m_micMixFifo.size() - m_micMixReadPos) >= thresholdSamples) {
            bool rec = false;
            if (!flushMicMixFifo(encGameRate, true, cfg, rec, outFatalError)) {
                if (rec) {
                    clearCarryOnRecover();
                    return true;
                }
                return false;
            }
        }
    }

    for (const auto& micAudio : closingMicChunks) {
        int numSamples = static_cast<int>(micAudio->samples.size() / 2);
        if (numSamples > 0) {
            bool rec = false;
            if (!writeAudioWithResync(false, micAudio->samples.data(), numSamples,
                                      kMicRate, micAudio->timestamp,
                                      cfg, silenceScratch, rec, outFatalError)) {
                if (rec) {
                    clearCarryOnRecover();
                    return true;
                }
                return false;
            }
        }
    }

    bool rec = false;
    if (!writeKeepAliveSilence(cfg, encGameRate, silenceScratch, rec, outFatalError)) {
        if (rec) {
            clearCarryOnRecover();
            return true;
        }
        return false;
    }
    return true;
}

bool ReplayManager::flushCarryAudio(
    const ReplayConfig& cfg,
    std::vector<float>& resampledScratch,
    std::vector<float>& silenceScratch,
    std::string& outFatalError
) {
    if (!m_encoder || !m_encoder->isRecording()) {
        m_carryGameAudio = {};
        m_carryMicAudio.clear();
        return true;
    }

    const int encGameRate = (m_segmentGameSampleRate > 0) ? m_segmentGameSampleRate : 48000;
    bool recovered = false;

    if (m_mixMicIntoGame) {
        for (const auto& micAudio : m_carryMicAudio) {
            if (!micAudio || micAudio->samples.size() < 2) continue;
            feedMicChunkIntoMix(*micAudio, encGameRate);
        }
    }

    if (m_carryGameAudio.samples.size() >= 2) {
        float* ptr = m_carryGameAudio.samples.data();
        int numSamples = static_cast<int>(m_carryGameAudio.samples.size() / 2);
        if (m_carryGameAudio.rate != encGameRate && m_carryGameAudio.rate > 0) {
            resampledScratch.clear();
            m_gameResampler.process(ptr, static_cast<size_t>(numSamples),
                                    m_carryGameAudio.rate, encGameRate, resampledScratch);
            ptr = resampledScratch.data();
            numSamples = static_cast<int>(resampledScratch.size() / 2);
        }
        if (numSamples > 0 && !writeAudioWithResync(true, ptr, numSamples, encGameRate, m_carryGameAudio.timestamp,
                                                    cfg, silenceScratch, recovered, outFatalError)) {
            m_carryGameAudio = {};
            m_carryMicAudio.clear();
            return recovered;
        }
    }

    if (!m_mixMicIntoGame && cfg.recordMic.load() && cfg.splitAudioTracks.load()) {
        for (const auto& micAudio : m_carryMicAudio) {
            if (!micAudio || micAudio->samples.size() < 2) continue;
            int numSamples = static_cast<int>(micAudio->samples.size() / 2);
            if (!writeAudioWithResync(false, micAudio->samples.data(), numSamples,
                                      AudioGrabber::kMicSampleRate, micAudio->timestamp,
                                      cfg, silenceScratch, recovered, outFatalError)) {
                m_carryGameAudio = {};
                m_carryMicAudio.clear();
                return recovered;
            }
        }
    }

    m_carryGameAudio = {};
    m_carryMicAudio.clear();
    return true;
}

void ReplayManager::flushTrailingSilenceForClose(const ReplayConfig& cfg) {
    const int gameRate = (m_segmentGameSampleRate > 0) ? m_segmentGameSampleRate : 48000;

    // Ensure short segments (<200ms) never close with completely empty audio tracks
    if (m_gameAudioSamples == 0 && m_lastVideoHns >= 0) {
        if (m_mixMicIntoGame && (m_micMixFifo.size() - m_micMixReadPos) >= 2) {
            bool ignoredRec = false;
            std::string ignoredErr;
            flushMicMixFifo(gameRate, false, cfg, ignoredRec, ignoredErr);
        } else {
            int silenceSamples = std::clamp<int>(
                static_cast<int>((std::max<int64_t>(m_lastVideoHns, 1000000LL) * gameRate) / 10000000LL),
                1024, gameRate / 2);
            std::vector<float> silence(static_cast<size_t>(silenceSamples) * 2, 0.0f);
            m_encoder->writeGameAudio(silence.data(), silenceSamples, 2, gameRate, 0);
        }
    }

    if (cfg.recordMic.load() && cfg.splitAudioTracks.load() && m_micAudioSamples == 0 && m_lastVideoHns >= 0) {
        constexpr int kMicRate = AudioGrabber::kMicSampleRate;
        int silenceSamples = std::clamp<int>(
            static_cast<int>((std::max<int64_t>(m_lastVideoHns, 1000000LL) * kMicRate) / 10000000LL),
            1024, kMicRate / 2);
        std::vector<float> silence(static_cast<size_t>(silenceSamples) * 2, 0.0f);
        m_encoder->writeMicAudio(silence.data(), silenceSamples, 2, kMicRate, 0);
    }
}

// ---------------------------------------------------------------------------
// Segment handling (worker thread only)
// ---------------------------------------------------------------------------

ReplayManager::PendingFinalizeInfo ReplayManager::finalizeCurrentSegment(bool syncForExport, const ReplayConfig& cfg) {
    if (!m_encoder || !m_encoder->isRecording()) return {};

    flushTrailingSilenceForClose(cfg);

    auto oldEncoder = std::move(m_encoder);
    std::wstring segPath = std::move(m_currentSegmentPath);
    std::string segFormat = std::move(m_currentFormatKey);
    auto segStart = m_currentSegmentStart;
    bool hasVideo = (m_lastVideoHns >= 0);
    double frameDur = 1.0 / static_cast<double>(std::max(1, cfg.targetFps.load()));
    double segDuration = hasVideo ? (static_cast<double>(m_lastVideoHns) / 1e7 + frameDur) : 0.0;

    m_currentSegmentPath.clear();
    m_currentFormatKey.clear();

    // Ensure any previous background segment finalization has completed so segment order is preserved
    joinFinalizeThread();

    if (syncForExport) {
        bool ok = oldEncoder->finish();
        oldEncoder.reset();
        reclaimInFlightFrames(false);
        std::lock_guard<std::mutex> lock(m_segmentMutex);
        if (ok && hasVideo && segDuration > 0.25) {
            m_activeSegments.push_back({segPath, segStart, segDuration, segFormat});
        } else {
            deleteSegmentFileLocked(segPath);
        }
        return {};
    }

    auto prom = std::make_shared<std::promise<bool>>();
    std::shared_future<bool> fut = prom->get_future().share();
    PendingFinalizeInfo info{{segPath, segStart, segDuration, segFormat}, fut};

    std::lock_guard<std::mutex> lock(m_finalizeMutex);
    m_pendingFinalize = info;
    m_finalizeInFlight.store(true);
    m_finalizeThread = std::thread([this, enc = std::move(oldEncoder), segPath = std::move(segPath),
                                    segStart, segDuration, segFormat = std::move(segFormat),
                                    hasVideo, cfg, prom = std::move(prom)]() mutable {
        HRESULT coHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        bool ok = enc->finish();
        enc.reset();
        if (SUCCEEDED(coHr)) CoUninitialize();

        bool keep = (ok && hasVideo && segDuration > 0.25);
        {
            std::lock_guard<std::mutex> segLock(m_segmentMutex);
            if (keep) {
                m_activeSegments.push_back({segPath, segStart, segDuration, segFormat});
            } else {
                deleteSegmentFileLocked(segPath);
            }
        }
        prom->set_value(keep);
        m_finalizeInFlight.store(false);
        purgeOldSegments(cfg);
        m_cv.notify_one();
    });

    return info;
}

bool ReplayManager::startNewSegment(
    const ReplayConfig& cfg,
    std::chrono::steady_clock::time_point segStart,
    bool isContinuation
) {
    m_lastVideoHns = -1;
    m_gameAudioSamples = 0;
    m_micAudioSamples = 0;
    m_currentSegmentStart = segStart;
    m_isContinuationSegment = isContinuation;

    if (m_videoWidth <= 0 || m_videoHeight <= 0) return false; // wait for first frame

    auto tempDir = cfg.getTempDirectory(true);
    {
        std::lock_guard<std::mutex> segLock(m_segmentMutex);
        m_lastTempDir = tempDir;
    }

    m_segmentIndex++;
    auto segFile = tempDir / (L"seg_" + std::to_wstring(m_segmentIndex) + L".mp4");
    m_currentSegmentPath = segFile.wstring();

    m_mixMicIntoGame = cfg.recordMic.load() && !cfg.splitAudioTracks.load();
    if (!m_mixMicIntoGame || !isContinuation) {
        resetMicMixer();
    }

    if (!m_encoder) {
        m_encoder = std::make_unique<MediaFoundationEncoder>();
    }

    int gameSampleRate = AudioGrabber::get()->getEncodedGameSampleRate();
    int effBitrate = cfg.getEffectiveBitrateMbps();
    int effMaxBitrate = std::max(cfg.getEffectiveMaxBitrateMbps(), effBitrate);

    m_lastStartError = m_encoder->start(
        m_currentSegmentPath,
        m_videoWidth,
        m_videoHeight,
        cfg.targetFps.load(),
        effBitrate,
        effMaxBitrate,
        cfg.qualityLevel.load(),
        cfg.rateControl.load(),
        cfg.recordMic.load() && cfg.splitAudioTracks.load(),
        cfg.codec.load(),
        cfg.hardwareAcceleration.load(),
        cfg.getCustomCodec(),
        gameSampleRate,
        [this]() { joinFinalizeThread(); }
    );

    if (m_lastStartError) {
        m_currentFormatKey = m_encoder->formatKey();
        m_segmentGameSampleRate = m_encoder->encodedGameSampleRate();
        m_encoderFailCount = 0;
        m_videoWriteErrors = 0;
        m_gameAudioWriteErrors = 0;
        m_micAudioWriteErrors = 0;
        m_prepareFailedCount = 0;
        m_prepareFailedLastAttempt.store(false);
        m_nextPrepareRetryTime = {};
        return true;
    }

    geode::log::error("Instant Replay: failed to start encoder ({}x{}): {}",
                      m_videoWidth, m_videoHeight, m_lastStartError.message);
    m_currentSegmentPath.clear();
    m_currentFormatKey.clear();
    m_isContinuationSegment = false;
    return false;
}

bool ReplayManager::activatePreparedOrStartNew(
    const ReplayConfig& cfg,
    std::chrono::steady_clock::time_point nextSegStart,
    bool isContinuation,
    bool allowSyncFallback
) {
    PreparedEncoder prepared;
    {
        std::lock_guard<std::mutex> lock(m_prepareDataMutex);
        if (m_prepared.encoder && m_prepared.encoder->isRecording() &&
            m_prepared.width == m_videoWidth && m_prepared.height == m_videoHeight) {
            prepared = std::move(m_prepared);
            m_prepared = {};
        }
    }

    if (prepared.encoder) {
        m_encoder = std::move(prepared.encoder);
        m_currentSegmentPath = std::move(prepared.path);
        m_currentFormatKey = std::move(prepared.formatKey);
        m_segmentGameSampleRate = prepared.gameSampleRate;
        {
            std::lock_guard<std::mutex> segLock(m_segmentMutex);
            m_lastTempDir = prepared.tempDir;
        }
        m_mixMicIntoGame = cfg.recordMic.load() && !cfg.splitAudioTracks.load();
        if (!m_mixMicIntoGame || !isContinuation) {
            resetMicMixer();
        }
        m_lastVideoHns = -1;
        m_gameAudioSamples = 0;
        m_micAudioSamples = 0;
        m_currentSegmentStart = nextSegStart;
        m_isContinuationSegment = isContinuation;
        m_encoderFailCount = 0;
        m_videoWriteErrors = 0;
        m_gameAudioWriteErrors = 0;
        m_micAudioWriteErrors = 0;
        m_prepareFailedCount = 0;
        m_prepareFailedLastAttempt.store(false);
        m_nextPrepareRetryTime = {};
        return true;
    }

    // Any mismatched stale prepared encoder should be torn down cleanly
    discardPreparedEncoder();

    if (!allowSyncFallback) {
        return false;
    }
    return startNewSegment(cfg, nextSegStart, isContinuation);
}

void ReplayManager::startPrepareNextSegment(const ReplayConfig& cfg) {
    if (!m_encoder || !m_encoder->isRecording()) return;
    if (m_videoWidth <= 0 || m_videoHeight <= 0) return;
    // Never keep more than 2 live encoders (current + finalizing OR current + preparing)
    if (m_prepareInFlight.load() || m_finalizeInFlight.load()) return;

    {
        std::lock_guard<std::mutex> lock(m_prepareDataMutex);
        if (m_prepared.encoder && m_prepared.width == m_videoWidth && m_prepared.height == m_videoHeight) {
            return;
        }
    }
    discardPreparedEncoder();

    joinPrepareThread();

    int segIdx = ++m_segmentIndex;
    int width = m_videoWidth;
    int height = m_videoHeight;
    int gameSampleRate = AudioGrabber::get()->getEncodedGameSampleRate();
    GUID forcedSubtype = m_encoder->activeSubtype();
    bool forcedHardware = m_encoder->activeHardware();
    bool useForcedHardware = (forcedSubtype != GUID_NULL);
    bool allowFallback = (forcedSubtype == GUID_NULL);

    m_prepareFailedLastAttempt.store(false);
    m_prepareCancel.store(false);
    m_prepareInFlight.store(true);

    std::lock_guard<std::mutex> lock(m_prepareMutex);
    m_prepareThread = std::thread([this, cfg, segIdx, width, height, gameSampleRate,
                                   forcedSubtype, useForcedHardware, forcedHardware, allowFallback]() {
        HRESULT coHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);

        auto tempDir = cfg.getTempDirectory(true);
        auto segFile = tempDir / (L"seg_" + std::to_wstring(segIdx) + L".mp4");
        std::wstring segPath = segFile.wstring();

        auto enc = std::make_unique<MediaFoundationEncoder>();
        int effBitrate = cfg.getEffectiveBitrateMbps();
        int effMaxBitrate = std::max(cfg.getEffectiveMaxBitrateMbps(), effBitrate);

        bool ok = false;
        if (!m_prepareCancel.load()) {
            ok = enc->start(
                segPath,
                width,
                height,
                cfg.targetFps.load(),
                effBitrate,
                effMaxBitrate,
                cfg.qualityLevel.load(),
                cfg.rateControl.load(),
                cfg.recordMic.load() && cfg.splitAudioTracks.load(),
                cfg.codec.load(),
                cfg.hardwareAcceleration.load(),
                cfg.getCustomCodec(),
                gameSampleRate,
                nullptr,
                allowFallback,
                forcedSubtype,
                useForcedHardware,
                forcedHardware
            ).ok;
        }

        if (ok && !m_prepareCancel.load()) {
            std::lock_guard<std::mutex> dataLock(m_prepareDataMutex);
            m_prepared.encoder = std::move(enc);
            m_prepared.path = std::move(segPath);
            m_prepared.formatKey = m_prepared.encoder->formatKey();
            m_prepared.gameSampleRate = m_prepared.encoder->encodedGameSampleRate();
            m_prepared.tempDir = std::move(tempDir);
            m_prepared.width = width;
            m_prepared.height = height;
            m_prepareFailedLastAttempt.store(false);
        } else {
            if (enc) {
                if (enc->isRecording()) {
                    enc->finish();
                }
                enc.reset();
            }
            DeleteFileW(segPath.c_str());
            if (!ok && !m_prepareCancel.load()) {
                m_prepareFailedLastAttempt.store(true);
            }
        }

        if (SUCCEEDED(coHr)) CoUninitialize();
        m_prepareInFlight.store(false);
        m_cv.notify_one();
    });
}

bool ReplayManager::rotateSegment(bool syncForExport, const ReplayConfig& cfg,
                                  std::chrono::steady_clock::time_point nextSegStart) {
    bool wasRecording = (m_encoder && m_encoder->isRecording());
    finalizeCurrentSegment(syncForExport, cfg);
    if (syncForExport) {
        purgeOldSegments(cfg);
    }
    return activatePreparedOrStartNew(cfg, nextSegStart, wasRecording, true);
}

void ReplayManager::purgeOldSegments(const ReplayConfig& cfg) {
    if (m_saving.load()) return;
    std::lock_guard<std::mutex> lock(m_segmentMutex);
    auto now = std::chrono::steady_clock::now();
    double maxAge = static_cast<double>(cfg.durationSeconds.load()) + SEGMENT_DURATION * 2.0;

    auto it = m_activeSegments.begin();
    while (it != m_activeSegments.end()) {
        double age = std::chrono::duration<double>(now - it->startTime).count();
        if (age > maxAge) {
            deleteSegmentFileLocked(it->filePath);
            it = m_activeSegments.erase(it);
        } else {
            ++it;
        }
    }
}

void ReplayManager::handleSaveRequest(
    bool continueRecording,
    const ReplayConfig& cfg,
    std::chrono::steady_clock::time_point boundaryTime,
    std::vector<float>& gameAudioScratch,
    std::vector<float>& resampledGameAudioScratch,
    std::vector<float>& silenceScratch
) {
    PendingFinalizeInfo extraPending{};
    bool wasRecording = (m_encoder && m_encoder->isRecording());

    if (continueRecording && wasRecording) {
        std::string ignoredErr;
        bool recovered = false;
        splitAudioAtBoundary(boundaryTime, cfg, gameAudioScratch, resampledGameAudioScratch, silenceScratch, recovered, ignoredErr);

        if (recovered) {
            m_prepareCancel.store(true);
            joinPrepareThread();
            discardPreparedEncoder();
            m_carryGameAudio = {};
            m_carryMicAudio.clear();
        } else {
            bool canHotSwap = false;
            {
                std::lock_guard<std::mutex> lock(m_prepareDataMutex);
                canHotSwap = (m_prepared.encoder && m_prepared.encoder->isRecording() &&
                              m_prepared.width == m_videoWidth && m_prepared.height == m_videoHeight);
            }

            if (canHotSwap && !m_finalizeInFlight.load()) {
                extraPending = finalizeCurrentSegment(false, cfg);
                if (activatePreparedOrStartNew(cfg, boundaryTime, true, false)) {
                    flushCarryAudio(cfg, resampledGameAudioScratch, silenceScratch, ignoredErr);
                } else {
                    m_carryGameAudio = {};
                    m_carryMicAudio.clear();
                }
            } else {
                m_prepareCancel.store(true);
                joinPrepareThread();
                discardPreparedEncoder();

                finalizeCurrentSegment(true, cfg);
                purgeOldSegments(cfg);
                if (m_running.load()) {
                    if (startNewSegment(cfg, boundaryTime, true)) {
                        flushCarryAudio(cfg, resampledGameAudioScratch, silenceScratch, ignoredErr);
                    } else {
                        m_carryGameAudio = {};
                        m_carryMicAudio.clear();
                        m_encoderFailCount++;
                        m_nextEncoderRetryTime = std::chrono::steady_clock::now() + std::chrono::seconds(2 * m_encoderFailCount);
                    }
                } else {
                    m_carryGameAudio = {};
                    m_carryMicAudio.clear();
                }
            }
        }
    } else if (!continueRecording) {
        m_prepareCancel.store(true);
        joinPrepareThread();
        discardPreparedEncoder();
        if (wasRecording) {
            std::string ignoredErr;
            processPendingAudio(cfg, gameAudioScratch, resampledGameAudioScratch, silenceScratch, ignoredErr);
            finalizeCurrentSegment(true, cfg);
        } else {
            std::lock_guard<std::mutex> lock(m_finalizeMutex);
            if (m_finalizeInFlight.load() && m_pendingFinalize.future.valid()) {
                extraPending = m_pendingFinalize;
            }
        }
    } else {
        // B4: Not currently recording (e.g. during encoder-start backoff or waiting for first frame);
        // pass any in-flight background finalization future to export so completed segments are never missed.
        std::lock_guard<std::mutex> lock(m_finalizeMutex);
        if (m_finalizeInFlight.load() && m_pendingFinalize.future.valid()) {
            extraPending = m_pendingFinalize;
        }
    }

    // Snapshot candidate segments and lock their files immediately under m_segmentMutex so neither
    // purgeOldSegments nor stopReplay can delete them while background finalization or export runs.
    std::vector<SegmentInfo> candidateSegments;
    std::vector<std::wstring> preLockedPaths;
    {
        std::lock_guard<std::mutex> lock(m_segmentMutex);
        candidateSegments = m_activeSegments;
        for (const auto& seg : candidateSegments) {
            m_exportLockedSegments.insert(seg.filePath);
            preLockedPaths.push_back(seg.filePath);
        }
        if (extraPending.future.valid() && !extraPending.segment.filePath.empty()) {
            m_exportLockedSegments.insert(extraPending.segment.filePath);
            preLockedPaths.push_back(extraPending.segment.filePath);
        }
    }

    if (candidateSegments.empty() && !extraPending.future.valid()) {
        m_saving.store(false);
        geode::Loader::get()->queueInMainThread([]() {
            geode::Notification::create("Instant Replay: replay is empty", geode::NotificationIcon::Warning)->show();
        });
        return;
    }

    // Wait for any previous finished export thread, then run finalization wait, disk checks, and merge off worker thread.
    joinExportThread();
    bool showNotif = ReplayConfig::get().showNotification.load();
    ReplayConfig cfgCopy = cfg;

    std::lock_guard<std::mutex> exportLock(m_exportMutex);
    m_exportThread = std::thread([this,
                                  candidateSegments = std::move(candidateSegments),
                                  preLockedPaths = std::move(preLockedPaths),
                                  extraPending = std::move(extraPending),
                                  cfgCopy,
                                  showNotif]() mutable {
        if (extraPending.future.valid()) {
            bool keepExtra = extraPending.future.get();
            if (keepExtra && !extraPending.segment.filePath.empty()) {
                bool alreadyPresent = false;
                for (const auto& s : candidateSegments) {
                    if (s.filePath == extraPending.segment.filePath) {
                        alreadyPresent = true;
                        break;
                    }
                }
                if (!alreadyPresent) {
                    candidateSegments.push_back(extraPending.segment);
                }
            }
        }

        std::vector<std::wstring> segsToMerge;
        double totalSeconds = 0.0;
        double maxDuration = static_cast<double>(std::max(1, cfgCopy.durationSeconds.load()));
        std::string requiredFormat;
        for (auto it = candidateSegments.rbegin(); it != candidateSegments.rend(); ++it) {
            if (requiredFormat.empty()) {
                requiredFormat = it->formatKey;
            } else if (!it->formatKey.empty() && it->formatKey != requiredFormat) {
                break;
            }
            segsToMerge.insert(segsToMerge.begin(), it->filePath);
            totalSeconds += it->durationSeconds;
            if (totalSeconds >= maxDuration) break;
        }
        double trimStartSeconds = std::max(0.0, totalSeconds - maxDuration);

        // Shrink export locks down to only the segments selected for merging
        {
            std::lock_guard<std::mutex> lock(m_segmentMutex);
            for (const auto& p : segsToMerge) {
                m_exportLockedSegments.insert(p);
            }
            for (const auto& p : preLockedPaths) {
                auto it = m_exportLockedSegments.find(p);
                if (it != m_exportLockedSegments.end()) {
                    m_exportLockedSegments.erase(it);
                }
            }
            flushDeferredDeletesLocked();
        }

        if (segsToMerge.empty()) {
            m_saving.store(false);
            geode::Loader::get()->queueInMainThread([]() {
                geode::Notification::create("Instant Replay: replay is empty", geode::NotificationIcon::Warning)->show();
            });
            return;
        }

        uint64_t freeMB = 0, neededMB = 0;
        bool isTempLow = false;
        if (cfgCopy.isLowDiskSpace(&freeMB, &neededMB, &isTempLow)) {
            geode::Loader::get()->queueInMainThread([freeMB, isTempLow]() {
                char buf[112];
                snprintf(buf, sizeof(buf), "Warning: Low %s disk space! (%llu MB free)",
                         isTempLow ? "TEMP" : "save", static_cast<unsigned long long>(freeMB));
                geode::Notification::create(buf, geode::NotificationIcon::Warning, 3.0f)->show();
            });
        }

        std::filesystem::path saveDir = cfgCopy.getRecordingsDirectory(true);

        auto sysNow = std::chrono::system_clock::now();
        auto t = std::chrono::system_clock::to_time_t(sysNow);
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(sysNow.time_since_epoch()).count() % 1000;
        if (ms < 0) ms += 1000;
        struct tm tmBuf;
        localtime_s(&tmBuf, &t);
        std::wstringstream wss;
        wss << L"replay_" << std::put_time(&tmBuf, L"%Y-%m-%d_%H-%M-%S")
            << L"_" << std::setw(3) << std::setfill(L'0') << ms;
        std::wstring baseStem = wss.str();

        std::filesystem::path finalPath = saveDir / (baseStem + L".mp4");
        std::error_code ec;
        for (int suffix = 2; std::filesystem::exists(finalPath, ec); ++suffix) {
            finalPath = saveDir / (baseStem + L"_" + std::to_wstring(suffix) + L".mp4");
        }

        HRESULT coHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        ConcatResult result = MediaFoundationEncoder::concatenateSegments(segsToMerge, finalPath.wstring(), trimStartSeconds);
        if (SUCCEEDED(coHr)) CoUninitialize();

        releaseExportSegments(segsToMerge);
        m_saving.store(false);

        std::string finalPathStr = geode::utils::string::pathToString(finalPath);
        double savedDuration = (result.durationSeconds > 0.0)
            ? result.durationSeconds
            : std::max(0.0, totalSeconds - trimStartSeconds);

        geode::Loader::get()->queueInMainThread([result, finalPathStr, savedDuration, showNotif]() {
            if (!result.success) {
                std::string msg = "Instant Replay: save failed";
                if (!result.error.empty()) {
                    msg += " (" + result.error + ")";
                }
                geode::log::error("{}", msg);
                geode::Notification::create(msg, geode::NotificationIcon::Error, 3.0f)->show();
                return;
            }
            if (showNotif) {
                ReplayNotification::show(finalPathStr, savedDuration);
            }
            if (auto engine = FMODAudioEngine::sharedEngine()) {
                engine->playEffect("achievement_01.ogg");
            }
        });
    });
}

void ReplayManager::workerThreadFunc(ReplayConfig sessionConfig) {
    HRESULT coHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    MediaFoundationEncoder::globalInitialize();

    std::vector<float> gameAudioScratch;
    std::vector<float> resampledGameAudioScratch;
    std::vector<float> silenceScratch;
    bool hadFatalError = false;

    while (m_running.load()) {
        std::shared_ptr<CapturedFrame> frame;
        {
            std::unique_lock<std::mutex> lock(m_queueMutex);
            m_cv.wait_for(lock, std::chrono::milliseconds(10), [this] {
                return !m_running.load() || m_saveRequested.load() ||
                       (m_savePendingSwitch && !m_prepareInFlight.load()) ||
                       !m_frameQueue.empty() || !m_micAudioQueue.empty() ||
                       AudioGrabber::get()->hasPendingGameAudio();
            });

            if (!m_frameQueue.empty()) {
                frame = std::move(m_frameQueue.front());
                m_frameQueue.pop_front();
            }
        }

        reclaimInFlightFrames(false);

        auto now = std::chrono::steady_clock::now();
        if (!m_savePendingSwitch && m_prepareFailedLastAttempt.exchange(false)) {
            m_prepareFailedCount++;
            m_nextPrepareRetryTime = now + std::chrono::seconds(2 * m_prepareFailedCount);
        }

        auto handleStartFailure = [this, &frame, &hadFatalError, &now]() -> bool {
            std::string detail = m_lastStartError.message.empty()
                ? MediaFoundationEncoder::formatHr(m_lastStartError.hr)
                : m_lastStartError.message;
            m_encoderFailCount++;
            if (MediaFoundationEncoder::isFatalStorageError(m_lastStartError.hr) ||
                m_encoderFailCount >= MAX_ENCODER_START_FAILURES) {
                if (frame) {
                    FrameGrabber::get()->recycleFrame(std::move(frame));
                    frame.reset();
                }
                hadFatalError = true;
                handleFatalRecordingError("Instant Replay stopped: " + detail);
                return true;
            }
            m_nextEncoderRetryTime = now + std::chrono::seconds(2 * m_encoderFailCount);
            return false;
        };

        // (Re)start encoder when the first frame arrives or the output size changes
        if (frame && (frame->width != m_videoWidth || frame->height != m_videoHeight ||
                      !m_encoder || !m_encoder->isRecording())) {
            if (frame->width != m_videoWidth || frame->height != m_videoHeight) {
                m_prepareCancel.store(true);
                joinPrepareThread();
                discardPreparedEncoder();
                m_prepareFailedCount = 0;
                m_prepareFailedLastAttempt.store(false);
            }

            if (now >= m_nextEncoderRetryTime) {
                bool hadActiveEnc = (m_encoder && m_encoder->isRecording());
                bool splitRecovered = false;
                if (hadActiveEnc) {
                    std::string ignoredErr;
                    splitAudioAtBoundary(frame->timestamp, sessionConfig, gameAudioScratch, resampledGameAudioScratch, silenceScratch, splitRecovered, ignoredErr);
                }
                if (!splitRecovered) {
                    m_videoWidth = frame->width;
                    m_videoHeight = frame->height;
                    if (rotateSegment(false, sessionConfig, frame->timestamp)) {
                        if (hadActiveEnc) {
                            std::string ignoredErr;
                            flushCarryAudio(sessionConfig, resampledGameAudioScratch, silenceScratch, ignoredErr);
                        }
                    } else {
                        m_carryGameAudio = {};
                        m_carryMicAudio.clear();
                        if (handleStartFailure()) break;
                    }
                }
            }
        }

        // Pre-warm next encoder ahead of the 30s rotation mark and rotate cleanly on time boundary
        if (m_encoder && m_encoder->isRecording()) {
            double elapsed = std::chrono::duration<double>(now - m_currentSegmentStart).count();
            if (elapsed >= SEGMENT_PREWARM_SECONDS && !m_prepareInFlight.load() && !m_finalizeInFlight.load() &&
                m_prepareFailedCount < 2 && now >= m_nextPrepareRetryTime) {
                startPrepareNextSegment(sessionConfig);
            }

            if (elapsed >= SEGMENT_DURATION) {
                bool hasPreparedReady = false;
                {
                    std::lock_guard<std::mutex> lock(m_prepareDataMutex);
                    hasPreparedReady = (m_prepared.encoder && m_prepared.encoder->isRecording() &&
                                        m_prepared.width == m_videoWidth && m_prepared.height == m_videoHeight);
                }
                bool prepareExhausted = (!m_prepareInFlight.load() && m_prepareFailedCount >= 2);
                bool hitMaxDuration = (elapsed >= MAX_SEGMENT_DURATION);

                if (hasPreparedReady || prepareExhausted || hitMaxDuration) {
                    if (hitMaxDuration && !hasPreparedReady) {
                        m_prepareCancel.store(true);
                        joinPrepareThread();
                        discardPreparedEncoder();
                    }

                    auto boundaryTime = frame ? frame->timestamp : now;
                    std::string audioFatalErr;
                    bool splitRecovered = false;
                    if (!splitAudioAtBoundary(boundaryTime, sessionConfig, gameAudioScratch, resampledGameAudioScratch, silenceScratch, splitRecovered, audioFatalErr)) {
                        if (frame) {
                            FrameGrabber::get()->recycleFrame(std::move(frame));
                            frame.reset();
                        }
                        hadFatalError = true;
                        handleFatalRecordingError(audioFatalErr);
                        break;
                    }

                    if (!splitRecovered) {
                        bool wasHealthy = (m_videoWriteErrors == 0 && m_gameAudioWriteErrors == 0 && m_micAudioWriteErrors == 0);
                        bool rotated = false;
                        if (hasPreparedReady && !m_finalizeInFlight.load()) {
                            finalizeCurrentSegment(false, sessionConfig);
                            rotated = activatePreparedOrStartNew(sessionConfig, boundaryTime, true, true);
                        } else {
                            finalizeCurrentSegment(true, sessionConfig);
                            rotated = activatePreparedOrStartNew(sessionConfig, boundaryTime, true, true);
                        }

                        if (rotated) {
                            if (wasHealthy) {
                                m_writeRecoveryAttempted = false;
                            }
                            if (!flushCarryAudio(sessionConfig, resampledGameAudioScratch, silenceScratch, audioFatalErr)) {
                                if (frame) {
                                    FrameGrabber::get()->recycleFrame(std::move(frame));
                                    frame.reset();
                                }
                                hadFatalError = true;
                                handleFatalRecordingError(audioFatalErr);
                                break;
                            }
                        } else {
                            m_carryGameAudio = {};
                            m_carryMicAudio.clear();
                            if (handleStartFailure()) break;
                        }
                    }
                }
            }
        }

        if (m_encoder && m_encoder->isRecording()) {
            bool skipAudioAfterRecovery = false;
            if (frame) {
                int64_t ts = std::chrono::duration_cast<std::chrono::nanoseconds>(
                    frame->timestamp - m_currentSegmentStart).count() / 100;
                if (ts < 0) ts = 0;
                if (ts <= m_lastVideoHns) ts = m_lastVideoHns + 1000;

                if (m_inFlightFrames.size() >= MAX_IN_FLIGHT_ENCODER_FRAMES) {
                    // Encoder queue is saturated; drop frame cleanly to bound memory (#1, #7)
                    FrameGrabber::get()->recycleFrame(std::move(frame));
                    frame.reset();
                } else {
                    EncoderOpResult videoRes = m_encoder->writeVideoFrame(frame->buffer.get(), frame->width, frame->height, ts);
                    if (videoRes) {
                        m_lastVideoHns = ts;
                        m_videoWriteErrors = 0;
                        if (FrameGrabber::isMediaBufferUnique(frame->buffer.get())) {
                            FrameGrabber::get()->recycleFrame(std::move(frame));
                        } else {
                            m_inFlightFrames.push_back(std::move(frame));
                        }
                        frame.reset();
                    } else {
                        std::string videoFatalErr;
                        WriteCheckResult wc = handleEncoderWriteError("video", videoRes, m_videoWriteErrors, sessionConfig, videoFatalErr);
                        if (wc == WriteCheckResult::Recovered) {
                            FrameGrabber::get()->recycleFrame(std::move(frame));
                            frame.reset();
                            skipAudioAfterRecovery = true;
                        } else if (wc == WriteCheckResult::Fatal) {
                            FrameGrabber::get()->recycleFrame(std::move(frame));
                            frame.reset();
                            hadFatalError = true;
                            handleFatalRecordingError(videoFatalErr);
                            break;
                        }
                    }
                }
            }

            if (!skipAudioAfterRecovery) {
                std::string audioFatalErr;
                if (!processPendingAudio(sessionConfig, gameAudioScratch, resampledGameAudioScratch, silenceScratch, audioFatalErr)) {
                    if (frame) {
                        FrameGrabber::get()->recycleFrame(std::move(frame));
                        frame.reset();
                    }
                    hadFatalError = true;
                    handleFatalRecordingError(audioFatalErr);
                    break;
                }
            }
        }

        if (frame) {
            FrameGrabber::get()->recycleFrame(std::move(frame));
            frame.reset();
        }

        if (m_saveRequested.exchange(false)) {
            if (!m_savePendingSwitch) {
                m_savePendingSwitch = true;
                m_savePendingSince = std::chrono::steady_clock::now();
            }
        }

        if (m_savePendingSwitch) {
            bool activeEnc = (m_encoder && m_encoder->isRecording());
            bool preparedReady = false;
            {
                std::lock_guard<std::mutex> lock(m_prepareDataMutex);
                preparedReady = (m_prepared.encoder && m_prepared.encoder->isRecording() &&
                                 m_prepared.width == m_videoWidth && m_prepared.height == m_videoHeight);
            }
            auto saveNow = std::chrono::steady_clock::now();
            double waitSec = std::chrono::duration<double>(saveNow - m_savePendingSince).count();

            bool readyToExecute = false;
            if (!activeEnc) {
                readyToExecute = true;
            } else if (preparedReady && !m_finalizeInFlight.load()) {
                readyToExecute = true;
            } else if (m_prepareFailedLastAttempt.exchange(false) || waitSec >= SAVE_PENDING_SWITCH_TIMEOUT) {
                readyToExecute = true;
            } else if (!m_prepareInFlight.load() && !m_finalizeInFlight.load()) {
                startPrepareNextSegment(sessionConfig);
            }

            if (readyToExecute) {
                m_savePendingSwitch = false;
                handleSaveRequest(true, sessionConfig, saveNow, gameAudioScratch, resampledGameAudioScratch, silenceScratch);
            }
        }
    }

    // If a save was requested right before stopReplay() woke us up, fulfill it before tearing down
    if (!hadFatalError && (m_saveRequested.exchange(false) || m_savePendingSwitch)) {
        m_savePendingSwitch = false;
        handleSaveRequest(false, sessionConfig, std::chrono::steady_clock::now(),
                          gameAudioScratch, resampledGameAudioScratch, silenceScratch);
    }

    // Release all COM objects held on this thread BEFORE CoUninitialize (#20)
    m_prepareCancel.store(true);
    joinPrepareThread();
    discardPreparedEncoder();
    joinFinalizeThread();
    if (m_encoder) {
        if (m_encoder->isRecording()) {
            m_encoder->finish();
        }
        m_encoder.reset();
    }
    reclaimInFlightFrames(true);

    if (SUCCEEDED(coHr)) CoUninitialize();
}

void ReplayManager::saveReplay() {
    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        if (!m_running.load()) return;
        bool expected = false;
        if (!m_saving.compare_exchange_strong(expected, true)) return;
        m_saveRequested.store(true);
    }
    m_cv.notify_all();
}

void ReplayManager::openRecordingsFolder() {
    auto dir = ReplayConfig::get().getRecordingsDirectory(true);
    ShellExecuteW(NULL, L"open", dir.c_str(), NULL, NULL, SW_SHOWNORMAL);
}

} // namespace replay

#endif // GEODE_IS_WINDOWS
