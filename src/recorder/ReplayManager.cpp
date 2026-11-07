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
    m_cv.notify_all();
    if (m_workerThread.joinable()) {
        m_workerThread.join();
    }
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

void ReplayManager::joinFinalizeThread() {
    std::lock_guard<std::mutex> lock(m_finalizeMutex);
    if (m_finalizeThread.joinable()) {
        m_finalizeThread.join();
    }
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
    m_nextEncoderRetryTime = {};
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

void ReplayManager::appendMicToMix(const AudioChunk& chunk, int targetRate, size_t startFrameOffset) {
    if (chunk.samples.size() < 2 || targetRate <= 0) return;
    size_t totalFrames = chunk.samples.size() / 2;
    if (startFrameOffset >= totalFrames) return;
    size_t srcFrames = totalFrames - startFrameOffset;
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

bool ReplayManager::flushMicMixFifo(int rate, bool checkErrors) {
    size_t availSamples = m_micMixFifo.size() - m_micMixReadPos;
    if (availSamples < 2 || !m_encoder || rate <= 0) return true;
    int numSamples = static_cast<int>(availSamples / 2);
    const float* dataPtr = m_micMixFifo.data() + m_micMixReadPos;
    int64_t ts = (m_gameAudioSamples * 10000000LL) / rate;
    bool ok = m_encoder->writeGameAudio(dataPtr, numSamples, 2, rate, ts);
    m_micMixFifo.clear();
    m_micMixReadPos = 0;
    if (ok) {
        m_gameAudioSamples += numSamples;
        if (checkErrors) m_gameAudioWriteErrors = 0;
        return true;
    }
    if (checkErrors && ++m_gameAudioWriteErrors >= MAX_CONSECUTIVE_WRITE_ERRORS) {
        return false;
    }
    return true;
}

bool ReplayManager::writeAudioWithResync(
    bool isGameAudio,
    float* samples,
    int numSamples,
    int rate,
    std::chrono::steady_clock::time_point timestamp,
    std::vector<float>& silenceScratch
) {
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
                bool silenceOk = isGameAudio
                    ? m_encoder->writeGameAudio(silenceScratch.data(), silenceSamples, kChannels, rate, expectedHns)
                    : m_encoder->writeMicAudio(silenceScratch.data(), silenceSamples, kChannels, rate, expectedHns);
                if (silenceOk) {
                    sampleCounter += silenceSamples;
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

    bool ok = isGameAudio
        ? m_encoder->writeGameAudio(samples, numSamples, kChannels, rate, ts)
        : m_encoder->writeMicAudio(samples, numSamples, kChannels, rate, ts);
    if (ok) {
        sampleCounter += numSamples;
        errorCounter = 0;
        return true;
    }

    if (++errorCounter >= MAX_CONSECUTIVE_WRITE_ERRORS) {
        return false;
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

    // 1. In single-track mode (recordMic && !splitAudioTracks), feed mic packets into mix FIFO first
    if (m_mixMicIntoGame) {
        for (const auto& micAudio : pendingMicAudio) {
            if (!micAudio || micAudio->samples.size() < 2) continue;
            constexpr int rate = AudioGrabber::kMicSampleRate;
            int numSamples = static_cast<int>(micAudio->samples.size() / 2);
            int64_t chunkStartHns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                micAudio->timestamp - m_currentSegmentStart).count() / 100;
            int64_t chunkDurationHns = (static_cast<int64_t>(numSamples) * 10000000LL) / rate;

            size_t startOffset = 0;
            if (chunkStartHns < 0 && (!m_isContinuationSegment || chunkStartHns < -1000000LL)) {
                if (chunkStartHns + chunkDurationHns <= 0) continue;
                int64_t trim = ((-chunkStartHns) * rate) / 10000000LL;
                if (trim >= numSamples) continue;
                if (trim > 0) startOffset = static_cast<size_t>(trim);
            }
            appendMicToMix(*micAudio, encGameRate, startOffset);
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
            if (!writeAudioWithResync(true, samplesPtr, numSamples, encGameRate, gameAudioTime, silenceScratch)) {
                outFatalError = "Instant Replay stopped: audio write error (TEMP drive may be full)";
                return false;
            }
        }
    } else if (m_mixMicIntoGame) {
        // If no game audio arrived for >=100ms in single-track mode, flush queued mic audio directly
        size_t thresholdSamples = static_cast<size_t>(encGameRate / 10) * 2;
        if ((m_micMixFifo.size() - m_micMixReadPos) >= thresholdSamples) {
            if (!flushMicMixFifo(encGameRate, true)) {
                outFatalError = "Instant Replay stopped: audio write error (TEMP drive may be full)";
                return false;
            }
        }
    }

    // 3. Encode separate microphone track (recordMic && splitAudioTracks)
    for (const auto& micAudio : pendingMicAudio) {
        if (!micAudio || micAudio->samples.size() < 2) continue;
        int numSamples = static_cast<int>(micAudio->samples.size() / 2);
        if (!writeAudioWithResync(false, micAudio->samples.data(), numSamples,
                                  AudioGrabber::kMicSampleRate, micAudio->timestamp, silenceScratch)) {
            outFatalError = "Instant Replay stopped: microphone audio write error (TEMP drive may be full)";
            return false;
        }
    }

    // 4. Synthesize continuous keep-alive silence on any idle audio stream lagging >200ms behind video (#1).
    //    Keeps MP4 muxer streams interleaved without buffering unbounded packets when e.g. no mic is connected.
    if (m_lastVideoHns >= 0) {
        int64_t gameAudioHns = (m_gameAudioSamples * 10000000LL) / encGameRate;
        if (m_lastVideoHns - gameAudioHns > 2000000LL) {
            int keepAliveSamples = std::max(480, encGameRate / 10); // 100ms
            silenceScratch.assign(static_cast<size_t>(keepAliveSamples) * 2, 0.0f);
            if (m_mixMicIntoGame) {
                mixMicInto(silenceScratch.data(), static_cast<size_t>(keepAliveSamples));
            }
            if (m_encoder->writeGameAudio(silenceScratch.data(), keepAliveSamples, 2, encGameRate, gameAudioHns)) {
                m_gameAudioSamples += keepAliveSamples;
                m_gameAudioWriteErrors = 0;
            } else if (++m_gameAudioWriteErrors >= MAX_CONSECUTIVE_WRITE_ERRORS) {
                outFatalError = "Instant Replay stopped: audio write error (TEMP drive may be full)";
                return false;
            }
        }

        if (sessionConfig.recordMic.load() && sessionConfig.splitAudioTracks.load()) {
            constexpr int kMicRate = AudioGrabber::kMicSampleRate;
            int64_t micAudioHns = (m_micAudioSamples * 10000000LL) / kMicRate;
            if (m_lastVideoHns - micAudioHns > 2000000LL) {
                constexpr int kKeepAliveMicSamples = kMicRate / 10; // 4800 stereo frames (100ms)
                silenceScratch.assign(static_cast<size_t>(kKeepAliveMicSamples) * 2, 0.0f);
                if (m_encoder->writeMicAudio(silenceScratch.data(), kKeepAliveMicSamples, 2, kMicRate, micAudioHns)) {
                    m_micAudioSamples += kKeepAliveMicSamples;
                    m_micAudioWriteErrors = 0;
                } else if (++m_micAudioWriteErrors >= MAX_CONSECUTIVE_WRITE_ERRORS) {
                    outFatalError = "Instant Replay stopped: microphone audio write error (TEMP drive may be full)";
                    return false;
                }
            }
        }
    }

    return true;
}

// ---------------------------------------------------------------------------
// Segment handling (worker thread only)
// ---------------------------------------------------------------------------

void ReplayManager::finalizeCurrentSegment(bool syncForExport, const ReplayConfig& cfg) {
    if (!m_encoder || !m_encoder->isRecording()) return;

    const int gameRate = (m_segmentGameSampleRate > 0) ? m_segmentGameSampleRate : 48000;

    // Ensure short segments (<200ms) never close with completely empty audio tracks
    if (m_gameAudioSamples == 0 && m_lastVideoHns >= 0) {
        if (m_mixMicIntoGame && (m_micMixFifo.size() - m_micMixReadPos) >= 2) {
            flushMicMixFifo(gameRate, false);
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
    } else {
        std::lock_guard<std::mutex> lock(m_finalizeMutex);
        m_finalizeThread = std::thread([this, enc = std::move(oldEncoder), segPath = std::move(segPath),
                                        segStart, segDuration, segFormat = std::move(segFormat), hasVideo, cfg]() mutable {
            HRESULT coHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            bool ok = enc->finish();
            enc.reset();
            if (SUCCEEDED(coHr)) CoUninitialize();

            {
                std::lock_guard<std::mutex> segLock(m_segmentMutex);
                if (ok && hasVideo && segDuration > 0.25) {
                    m_activeSegments.push_back({segPath, segStart, segDuration, segFormat});
                } else {
                    deleteSegmentFileLocked(segPath);
                }
            }
            purgeOldSegments(cfg);
        });
    }
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
    if (!m_mixMicIntoGame) {
        resetMicMixer();
    }

    if (!m_encoder) {
        m_encoder = std::make_unique<MediaFoundationEncoder>();
    }

    int gameSampleRate = AudioGrabber::get()->getEncodedGameSampleRate();
    int effBitrate = cfg.getEffectiveBitrateMbps();
    int effMaxBitrate = std::max(cfg.getEffectiveMaxBitrateMbps(), effBitrate);

    bool ok = m_encoder->start(
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

    if (ok) {
        m_currentFormatKey = m_encoder->formatKey();
        m_segmentGameSampleRate = m_encoder->encodedGameSampleRate();
        m_encoderFailCount = 0;
        m_videoWriteErrors = 0;
        m_gameAudioWriteErrors = 0;
        m_micAudioWriteErrors = 0;
        return true;
    }

    geode::log::error("Instant Replay: failed to start encoder ({}x{})", m_videoWidth, m_videoHeight);
    m_currentSegmentPath.clear();
    m_currentFormatKey.clear();
    m_isContinuationSegment = false;
    return false;
}

bool ReplayManager::rotateSegment(bool syncForExport, const ReplayConfig& cfg,
                                  std::chrono::steady_clock::time_point nextSegStart) {
    bool wasRecording = (m_encoder && m_encoder->isRecording());
    finalizeCurrentSegment(syncForExport, cfg);
    if (syncForExport) {
        purgeOldSegments(cfg);
    }
    return startNewSegment(cfg, nextSegStart, wasRecording);
}

void ReplayManager::purgeOldSegments(const ReplayConfig& cfg) {
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
    std::vector<float>& gameAudioScratch,
    std::vector<float>& resampledGameAudioScratch,
    std::vector<float>& silenceScratch
) {
    bool wasRecording = (m_encoder && m_encoder->isRecording());
    if (wasRecording) {
        std::string ignoredErr;
        processPendingAudio(cfg, gameAudioScratch, resampledGameAudioScratch, silenceScratch, ignoredErr);
    }
    auto nextSegStart = std::chrono::steady_clock::now();

    // 1. Synchronously close the running segment so it becomes a valid MP4
    finalizeCurrentSegment(true, cfg);

    // 2. Pick the newest compatible segments covering the requested duration,
    //    and lock their files so stopReplay/purgeOldSegments cannot delete them
    //    while the background export thread is reading them.
    std::vector<std::wstring> segsToMerge;
    double totalSeconds = 0.0;
    double maxDuration = static_cast<double>(std::max(1, cfg.durationSeconds.load()));
    {
        std::lock_guard<std::mutex> lock(m_segmentMutex);
        std::string requiredFormat;
        for (auto it = m_activeSegments.rbegin(); it != m_activeSegments.rend(); ++it) {
            if (requiredFormat.empty()) {
                requiredFormat = it->formatKey;
            } else if (!it->formatKey.empty() && it->formatKey != requiredFormat) {
                // Stop before a segment recorded with incompatible stream settings
                break;
            }
            segsToMerge.insert(segsToMerge.begin(), it->filePath);
            totalSeconds += it->durationSeconds;
            if (totalSeconds >= maxDuration) break;
        }
        for (const auto& p : segsToMerge) {
            m_exportLockedSegments.insert(p);
        }
    }

    double trimStartSeconds = std::max(0.0, totalSeconds - maxDuration);

    // 3. Immediately keep recording into a fresh segment (unless we are stopping)
    if (continueRecording && m_running.load()) {
        startNewSegment(cfg, nextSegStart, wasRecording);
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
    if (cfg.isLowDiskSpace(&freeMB, &neededMB, &isTempLow)) {
        geode::Loader::get()->queueInMainThread([freeMB, isTempLow]() {
            char buf[112];
            snprintf(buf, sizeof(buf), "Warning: Low %s disk space! (%llu MB free)",
                     isTempLow ? "TEMP" : "save", static_cast<unsigned long long>(freeMB));
            geode::Notification::create(buf, geode::NotificationIcon::Warning, 3.0f)->show();
        });
    }

    // 4. Merge in the background. Join any previous finished export thread first.
    joinExportThread();
    std::filesystem::path saveDir = cfg.getRecordingsDirectory(true);
    bool showNotif = ReplayConfig::get().showNotification.load();

    std::lock_guard<std::mutex> exportLock(m_exportMutex);
    m_exportThread = std::thread([this, segsToMerge = std::move(segsToMerge), saveDir = std::move(saveDir),
                                  totalSeconds, trimStartSeconds, showNotif]() {
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
                return !m_running.load() || m_saveRequested.load() || !m_frameQueue.empty() ||
                       !m_micAudioQueue.empty() || AudioGrabber::get()->hasPendingGameAudio();
            });

            if (!m_frameQueue.empty()) {
                frame = std::move(m_frameQueue.front());
                m_frameQueue.pop_front();
            }
        }

        reclaimInFlightFrames(false);

        // (Re)start encoder when the first frame arrives or the output size changes
        if (frame && (frame->width != m_videoWidth || frame->height != m_videoHeight ||
                      !m_encoder || !m_encoder->isRecording())) {
            auto now = std::chrono::steady_clock::now();
            if (now >= m_nextEncoderRetryTime) {
                if (m_encoder && m_encoder->isRecording()) {
                    std::string ignoredErr;
                    processPendingAudio(sessionConfig, gameAudioScratch, resampledGameAudioScratch, silenceScratch, ignoredErr);
                }
                m_videoWidth = frame->width;
                m_videoHeight = frame->height;
                if (!rotateSegment(false, sessionConfig, frame->timestamp)) {
                    m_encoderFailCount++;
                    if (m_encoderFailCount >= MAX_ENCODER_START_FAILURES) {
                        FrameGrabber::get()->recycleFrame(std::move(frame));
                        frame.reset();
                        hadFatalError = true;
                        handleFatalRecordingError("Instant Replay stopped: encoder failed to initialize (check disk space or GPU drivers)");
                        break;
                    }
                    // Exponential backoff before retrying so we never retry-storm on every frame
                    m_nextEncoderRetryTime = now + std::chrono::seconds(2 * m_encoderFailCount);
                }
            }
        }

        if (m_encoder && m_encoder->isRecording()) {
            if (frame) {
                int64_t ts = std::chrono::duration_cast<std::chrono::nanoseconds>(
                    frame->timestamp - m_currentSegmentStart).count() / 100;
                if (ts < 0) ts = 0;
                if (ts <= m_lastVideoHns) ts = m_lastVideoHns + 1000;

                if (m_inFlightFrames.size() >= MAX_IN_FLIGHT_ENCODER_FRAMES) {
                    // Encoder queue is saturated; drop frame cleanly to bound memory (#1, #7)
                    FrameGrabber::get()->recycleFrame(std::move(frame));
                    frame.reset();
                } else if (m_encoder->writeVideoFrame(frame->buffer.get(), frame->width, frame->height, ts)) {
                    m_lastVideoHns = ts;
                    m_videoWriteErrors = 0;
                    if (FrameGrabber::isMediaBufferUnique(frame->buffer.get())) {
                        FrameGrabber::get()->recycleFrame(std::move(frame));
                    } else {
                        m_inFlightFrames.push_back(std::move(frame));
                    }
                    frame.reset();
                } else {
                    if (++m_videoWriteErrors >= MAX_CONSECUTIVE_WRITE_ERRORS) {
                        FrameGrabber::get()->recycleFrame(std::move(frame));
                        frame.reset();
                        hadFatalError = true;
                        handleFatalRecordingError("Instant Replay stopped: video write error (TEMP drive may be full)");
                        break;
                    }
                }
            }

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

        if (frame) {
            FrameGrabber::get()->recycleFrame(std::move(frame));
            frame.reset();
        }

        if (m_saveRequested.exchange(false)) {
            handleSaveRequest(true, sessionConfig, gameAudioScratch, resampledGameAudioScratch, silenceScratch);
        }

        // Time-based rotation (drain latest audio into closing segment first so splice is seamless, #4)
        auto now = std::chrono::steady_clock::now();
        double elapsed = std::chrono::duration<double>(now - m_currentSegmentStart).count();
        if (elapsed >= SEGMENT_DURATION && m_encoder && m_encoder->isRecording()) {
            std::string audioFatalErr;
            if (!processPendingAudio(sessionConfig, gameAudioScratch, resampledGameAudioScratch, silenceScratch, audioFatalErr)) {
                hadFatalError = true;
                handleFatalRecordingError(audioFatalErr);
                break;
            }
            rotateSegment(false, sessionConfig, std::chrono::steady_clock::now());
        }
    }

    // If a save was requested right before stopReplay() woke us up, fulfill it before tearing down
    if (!hadFatalError && m_saveRequested.exchange(false)) {
        handleSaveRequest(false, sessionConfig, gameAudioScratch, resampledGameAudioScratch, silenceScratch);
    }

    // Release all COM objects held on this thread BEFORE CoUninitialize (#20)
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
