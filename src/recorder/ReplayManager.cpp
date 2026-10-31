#include "ReplayManager.hpp"

#ifdef GEODE_IS_WINDOWS

#include "../ui/ReplayNotification.hpp"
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
static constexpr size_t MAX_QUEUED_VIDEO_BYTES = 256ull * 1024 * 1024;
static constexpr size_t MAX_MIC_MIX_SAMPLES = 48000 * 2 * 5; // cap single-track mic FIFO at ~5s stereo
static constexpr int MAX_ENCODER_START_FAILURES = 3;
static constexpr int MAX_CONSECUTIVE_WRITE_ERRORS = 5;

ReplayManager* ReplayManager::get() {
    static ReplayManager instance;
    return &instance;
}

ReplayManager::ReplayManager() = default;

ReplayManager::~ReplayManager() {
    // Do NOT touch other singletons or OpenGL here (static destruction order is unspecified).
    // Clean shutdown happens in ReplayManager::shutdown() hooked on AppDelegate::trySaveGame.
    m_running.store(false);
    m_cv.notify_all();
    if (m_workerThread.joinable()) {
        m_workerThread.join();
    }
    joinFinalizeThread();
    joinExportThread();
}

std::filesystem::path ReplayManager::getTempDir() const {
    return ReplayConfig::get().getTempDirectory();
}

void ReplayManager::cleanupTempDir() {
    std::lock_guard<std::mutex> lock(m_segmentMutex);
    std::error_code ec;
    auto tempDir = getTempDir();
    if (std::filesystem::exists(tempDir, ec)) {
        for (const auto& entry : std::filesystem::directory_iterator(tempDir, ec)) {
            auto wpath = entry.path().wstring();
            if (m_exportLockedSegments.find(wpath) != m_exportLockedSegments.end()) {
                m_deferredDeletes.push_back(wpath);
            } else {
                std::filesystem::remove(entry.path(), ec);
            }
        }
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

void ReplayManager::initialize() {
    m_shuttingDown.store(false);
    ReplayConfig::get().loadFromSettings();
    cleanupTempDir();

    // Frame grabber callback (render thread -> worker queue)
    FrameGrabber::get()->setFrameCallback([this](std::shared_ptr<CapturedFrame> frame) {
        if (!m_running.load(std::memory_order_relaxed) || !frame) {
            if (frame) FrameGrabber::get()->recycleFrame(std::move(frame));
            return;
        }
        std::shared_ptr<CapturedFrame> dropped;
        {
            std::lock_guard<std::mutex> lock(m_queueMutex);
            size_t frameBytes = std::max<size_t>(1, frame->data.size());
            size_t maxFrames = std::clamp<size_t>(MAX_QUEUED_VIDEO_BYTES / frameBytes, 4, 60);
            if (m_frameQueue.size() < maxFrames) {
                m_frameQueue.push_back(std::move(frame));
            } else {
                dropped = std::move(frame);
            }
        }
        if (dropped) {
            FrameGrabber::get()->recycleFrame(std::move(dropped));
        } else {
            m_cv.notify_one();
        }
    });

    AudioGrabber::get()->setMicAudioCallback([this](std::shared_ptr<AudioChunk> chunk) {
        if (!m_running.load(std::memory_order_relaxed) || !chunk) return;
        {
            std::lock_guard<std::mutex> lock(m_queueMutex);
            if (m_micAudioQueue.size() >= 150) {
                m_micAudioQueue.pop_front();
            }
            m_micAudioQueue.push_back(std::move(chunk));
        }
        m_cv.notify_one();
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
    AudioGrabber::get()->resetGameAudioBuffer();

    m_videoWidth = 0;
    m_videoHeight = 0;
    // Note: m_segmentIndex is intentionally NOT reset so segment filenames never
    // collide with files still being read by an in-flight export thread.
    m_lastVideoHns = -1;
    m_gameAudioSamples = 0;
    m_micAudioSamples = 0;
    m_encoderFailCount = 0;
    m_writeErrorCount = 0;
    m_nextEncoderRetryTime = {};
    m_currentSegmentPath.clear();
    m_currentFormatKey.clear();
    resetMicMixer();

    ReplayConfig sessionConfig = ReplayConfig::get().snapshot();
    if (sessionConfig.recordMic.load()) {
        AudioGrabber::get()->startMicCapture();
    } else {
        AudioGrabber::get()->stopMicCapture();
    }

    m_running.store(true);
    m_replayStartTime = std::chrono::steady_clock::now();
    m_currentSegmentStart = m_replayStartTime;
    m_workerThread = std::thread(&ReplayManager::workerThreadFunc, this, std::move(sessionConfig));
}

void ReplayManager::stopReplay() {
    bool wasRunning = m_running.exchange(false);
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
        m_encoder->finish();
        m_encoder.reset();
    }

    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        for (auto& f : m_frameQueue) {
            if (!m_shuttingDown.load()) {
                FrameGrabber::get()->recycleFrame(std::move(f));
            }
        }
        m_frameQueue.clear();
        m_micAudioQueue.clear();
    }

    if (wasRunning || !m_activeSegments.empty() || !m_currentSegmentPath.empty()) {
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
    m_running.store(false);
    ReplayConfig::get().enabled = false;
    ReplayConfig::get().saveToSettings();

    geode::Loader::get()->queueInMainThread([reason]() {
        AudioGrabber::get()->stopMicCapture();
        geode::Notification::create(reason, geode::NotificationIcon::Error, 4.0f)->show();
    });
}

// ---------------------------------------------------------------------------
// Single-track microphone mixer (used when recordMic == true && splitAudioTracks == false)
// ---------------------------------------------------------------------------

void ReplayManager::resetMicMixer() {
    m_micMixFifo.clear();
    m_micMixRate = 0;
    m_micResamplePos = 0.0;
    m_micPrev[0] = 0.0f;
    m_micPrev[1] = 0.0f;
    m_micHasPrev = false;
}

void ReplayManager::appendMicToMix(const AudioChunk& chunk, int targetRate) {
    if (chunk.channels <= 0 || chunk.samples.empty() || targetRate <= 0) return;
    int srcRate = (chunk.sampleRate > 0) ? chunk.sampleRate : 48000;
    int srcCh = chunk.channels;
    size_t srcFrames = chunk.samples.size() / static_cast<size_t>(srcCh);
    if (srcFrames == 0) return;

    if (m_micMixRate != targetRate) {
        resetMicMixer();
        m_micMixRate = targetRate;
    }

    auto readStereo = [&](size_t frameIdx, float& outL, float& outR) {
        size_t base = frameIdx * static_cast<size_t>(srcCh);
        outL = chunk.samples[base];
        outR = (srcCh >= 2) ? chunk.samples[base + 1] : outL;
    };

    if (srcRate == targetRate) {
        for (size_t i = 0; i < srcFrames; ++i) {
            float l = 0.0f, r = 0.0f;
            readStereo(i, l, r);
            m_micMixFifo.push_back(l);
            m_micMixFifo.push_back(r);
        }
    } else {
        double step = static_cast<double>(srcRate) / static_cast<double>(targetRate);
        if (!m_micHasPrev) {
            readStereo(0, m_micPrev[0], m_micPrev[1]);
            m_micHasPrev = true;
            m_micResamplePos = 0.0;
        }
        // Position -1.0 corresponds to m_micPrev; positions [0, srcFrames - 1] are in chunk.
        while (m_micResamplePos < static_cast<double>(srcFrames - 1)) {
            float l0 = 0.0f, r0 = 0.0f, l1 = 0.0f, r1 = 0.0f;
            double frac = 0.0;
            if (m_micResamplePos < 0.0) {
                frac = m_micResamplePos + 1.0;
                l0 = m_micPrev[0];
                r0 = m_micPrev[1];
                readStereo(0, l1, r1);
            } else {
                size_t idx = static_cast<size_t>(m_micResamplePos);
                frac = m_micResamplePos - static_cast<double>(idx);
                readStereo(idx, l0, r0);
                readStereo(std::min(idx + 1, srcFrames - 1), l1, r1);
            }
            float f = static_cast<float>(frac);
            m_micMixFifo.push_back(l0 + (l1 - l0) * f);
            m_micMixFifo.push_back(r0 + (r1 - r0) * f);
            m_micResamplePos += step;
        }
        readStereo(srcFrames - 1, m_micPrev[0], m_micPrev[1]);
        m_micResamplePos -= static_cast<double>(srcFrames);
    }

    if (m_micMixFifo.size() > MAX_MIC_MIX_SAMPLES) {
        size_t excess = (m_micMixFifo.size() - MAX_MIC_MIX_SAMPLES) & ~static_cast<size_t>(1);
        m_micMixFifo.erase(m_micMixFifo.begin(), m_micMixFifo.begin() + static_cast<std::ptrdiff_t>(excess));
    }
}

void ReplayManager::mixMicInto(std::vector<float>& gameSamples, int channels) {
    if (m_micMixFifo.empty() || channels <= 0 || gameSamples.empty()) return;
    size_t frames = gameSamples.size() / static_cast<size_t>(channels);
    size_t availFrames = m_micMixFifo.size() / 2;
    size_t mixFrames = std::min(frames, availFrames);

    for (size_t i = 0; i < mixFrames; ++i) {
        float micL = m_micMixFifo[i * 2];
        float micR = m_micMixFifo[i * 2 + 1];
        if (channels >= 2) {
            size_t base = i * static_cast<size_t>(channels);
            gameSamples[base] = std::clamp(gameSamples[base] + micL, -1.0f, 1.0f);
            gameSamples[base + 1] = std::clamp(gameSamples[base + 1] + micR, -1.0f, 1.0f);
        } else {
            float mono = 0.5f * (micL + micR);
            gameSamples[i] = std::clamp(gameSamples[i] + mono, -1.0f, 1.0f);
        }
    }
    m_micMixFifo.erase(m_micMixFifo.begin(), m_micMixFifo.begin() + static_cast<std::ptrdiff_t>(mixFrames * 2));
}

// ---------------------------------------------------------------------------
// Segment handling (worker thread only)
// ---------------------------------------------------------------------------

void ReplayManager::finalizeCurrentSegment(bool syncForExport, const ReplayConfig& cfg) {
    if (!m_encoder || !m_encoder->isRecording()) return;

    // If no game audio was captured in this segment, flush any mixed-in mic audio
    // or write a brief silence packet so the audio track is never empty.
    if (m_gameAudioSamples == 0 && m_lastVideoHns > 0) {
        int rate = AudioGrabber::get()->getGameSampleRate();
        if (rate <= 0) rate = 44100;
        if (m_mixMicIntoGame && m_micMixFifo.size() >= 2) {
            int numSamples = static_cast<int>(m_micMixFifo.size() / 2);
            std::vector<float> mixed(m_micMixFifo.begin(), m_micMixFifo.begin() + numSamples * 2);
            m_micMixFifo.clear();
            m_encoder->writeGameAudio(mixed.data(), numSamples, 2, rate, 0);
            m_gameAudioSamples += numSamples;
        } else {
            int silenceSamples = rate / 2; // 0.5s silence
            std::vector<float> silence(silenceSamples * 2, 0.0f);
            m_encoder->writeGameAudio(silence.data(), silenceSamples, 2, rate, 0);
        }
    }

    if (cfg.recordMic.load() && cfg.splitAudioTracks.load() && m_micAudioSamples == 0 && m_lastVideoHns > 0) {
        int silenceSamples = 48000 / 2; // 0.5s silence
        std::vector<float> silence(silenceSamples * 2, 0.0f);
        m_encoder->writeMicAudio(silence.data(), silenceSamples, 2, 48000, 0);
    }

    auto oldEncoder = std::move(m_encoder);
    std::wstring segPath = std::move(m_currentSegmentPath);
    std::string segFormat = std::move(m_currentFormatKey);
    auto segStart = m_currentSegmentStart;
    bool hasVideo = (m_lastVideoHns >= 0);
    double segDuration = hasVideo ? (static_cast<double>(m_lastVideoHns) / 1e7) : 0.0;

    m_currentSegmentPath.clear();
    m_currentFormatKey.clear();

    // Ensure any previous background segment finalization has completed so segment order is preserved
    joinFinalizeThread();

    if (syncForExport) {
        bool ok = oldEncoder->finish();
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
                if (ok && hasVideo && segDuration > 0.25 && m_running.load()) {
                    m_activeSegments.push_back({segPath, segStart, segDuration, segFormat});
                } else {
                    deleteSegmentFileLocked(segPath);
                }
            }
            purgeOldSegments(cfg);
        });
    }
}

bool ReplayManager::startNewSegment(const ReplayConfig& cfg) {
    m_lastVideoHns = -1;
    m_gameAudioSamples = 0;
    m_micAudioSamples = 0;

    if (m_videoWidth <= 0 || m_videoHeight <= 0) return false; // wait for first frame

    m_segmentIndex++;
    auto segFile = cfg.getTempDirectory() / ("seg_" + std::to_string(m_segmentIndex) + ".mp4");
    m_currentSegmentPath = segFile.wstring();

    m_mixMicIntoGame = cfg.recordMic.load() && !cfg.splitAudioTracks.load();
    if (!m_mixMicIntoGame) {
        resetMicMixer();
    }

    if (!m_encoder) {
        m_encoder = std::make_unique<MediaFoundationEncoder>();
    }

    int gameSampleRate = AudioGrabber::get()->getGameSampleRate();
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
        gameSampleRate
    );

    // Anchor segment start time AFTER encoder initialization so frames/audio do not
    // bunch up at timestamp 0 across slow sink writer creation.
    m_currentSegmentStart = std::chrono::steady_clock::now();

    if (ok) {
        m_currentFormatKey = m_encoder->formatKey();
        m_encoderFailCount = 0;
        m_writeErrorCount = 0;
        return true;
    }

    geode::log::error("Instant Replay: failed to start encoder ({}x{})", m_videoWidth, m_videoHeight);
    m_currentSegmentPath.clear();
    m_currentFormatKey.clear();
    return false;
}

bool ReplayManager::rotateSegment(bool syncForExport, const ReplayConfig& cfg) {
    finalizeCurrentSegment(syncForExport, cfg);
    if (syncForExport) {
        purgeOldSegments(cfg);
    }
    return startNewSegment(cfg);
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

void ReplayManager::handleSaveRequest(bool continueRecording, const ReplayConfig& cfg) {
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
        startNewSegment(cfg);
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
    std::filesystem::path saveDir = cfg.getRecordingsDirectory();
    bool showNotif = cfg.showNotification.load();

    std::lock_guard<std::mutex> exportLock(m_exportMutex);
    m_exportThread = std::thread([this, segsToMerge = std::move(segsToMerge), saveDir = std::move(saveDir),
                                  totalSeconds, trimStartSeconds, showNotif]() {
        auto t = std::time(nullptr);
        struct tm tmBuf;
        localtime_s(&tmBuf, &t);
        std::wstringstream wss;
        wss << L"replay_" << std::put_time(&tmBuf, L"%Y-%m-%d_%H-%M-%S") << L".mp4";

        auto finalPath = saveDir / wss.str();
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
    std::vector<float> silenceScratch;

    while (m_running.load()) {
        std::shared_ptr<CapturedFrame> frame;
        std::vector<std::shared_ptr<AudioChunk>> pendingMicAudio;

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
            while (!m_micAudioQueue.empty()) {
                pendingMicAudio.push_back(std::move(m_micAudioQueue.front()));
                m_micAudioQueue.pop_front();
            }
        }

        int gameSampleRate = 44100;
        std::chrono::steady_clock::time_point gameAudioTime{};
        size_t gameAudioFrames = AudioGrabber::get()->drainGameAudio(gameAudioScratch, gameSampleRate, gameAudioTime);

        // (Re)start encoder when the first frame arrives or the output size changes
        if (frame && (frame->width != m_videoWidth || frame->height != m_videoHeight ||
                      !m_encoder || !m_encoder->isRecording())) {
            auto now = std::chrono::steady_clock::now();
            if (now >= m_nextEncoderRetryTime) {
                m_videoWidth = frame->width;
                m_videoHeight = frame->height;
                if (!rotateSegment(false, sessionConfig)) {
                    m_encoderFailCount++;
                    if (m_encoderFailCount >= MAX_ENCODER_START_FAILURES) {
                        FrameGrabber::get()->recycleFrame(std::move(frame));
                        handleFatalRecordingError("Instant Replay stopped: encoder failed to initialize (check disk space or GPU drivers)");
                        break;
                    }
                    // Exponential backoff before retrying so we never retry-storm on every frame
                    m_nextEncoderRetryTime = now + std::chrono::seconds(2 * m_encoderFailCount);
                } else {
                    // Align first frame timestamp to 0 in the new segment
                    m_currentSegmentStart = frame->timestamp;
                }
            }
        }

        if (m_encoder && m_encoder->isRecording()) {
            if (frame) {
                int64_t ts = std::chrono::duration_cast<std::chrono::nanoseconds>(
                    frame->timestamp - m_currentSegmentStart).count() / 100;
                if (ts < 0) ts = 0;
                if (ts <= m_lastVideoHns) ts = m_lastVideoHns + 1000;

                if (m_encoder->writeVideoFrame(frame->data.data(), frame->width, frame->height, ts)) {
                    m_lastVideoHns = ts;
                    m_writeErrorCount = 0;
                } else {
                    if (++m_writeErrorCount >= MAX_CONSECUTIVE_WRITE_ERRORS) {
                        FrameGrabber::get()->recycleFrame(std::move(frame));
                        handleFatalRecordingError("Instant Replay stopped: disk write error (TEMP drive may be full)");
                        break;
                    }
                }
            }

            // In single-track mode (recordMic && !splitAudioTracks), buffer mic samples first
            // so they can be mixed into the outgoing game audio stream.
            if (m_mixMicIntoGame) {
                int targetRate = AudioGrabber::get()->getGameSampleRate();
                if (targetRate <= 0) targetRate = 44100;
                for (const auto& micAudio : pendingMicAudio) {
                    if (!micAudio || micAudio->channels <= 0 || micAudio->samples.empty()) continue;
                    int rate = micAudio->sampleRate > 0 ? micAudio->sampleRate : 48000;
                    int numSamples = static_cast<int>(micAudio->samples.size() / micAudio->channels);
                    int64_t chunkStartHns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                        micAudio->timestamp - m_currentSegmentStart).count() / 100;
                    int64_t chunkDurationHns = (static_cast<int64_t>(numSamples) * 10000000LL) / rate;
                    if (chunkStartHns + chunkDurationHns <= 0) continue;
                    appendMicToMix(*micAudio, targetRate);
                }
                pendingMicAudio.clear();
            }

            if (gameAudioFrames > 0) {
                constexpr int channels = 2;
                int rate = (gameSampleRate > 0) ? gameSampleRate : 44100;
                int numSamples = static_cast<int>(gameAudioFrames);

                int64_t chunkStartHns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                    gameAudioTime - m_currentSegmentStart).count() / 100;
                int64_t chunkDurationHns = (static_cast<int64_t>(numSamples) * 10000000LL) / rate;

                if (chunkStartHns + chunkDurationHns > 0) {
                    if (chunkStartHns < 0) chunkStartHns = 0;
                    int64_t expectedHns = (m_gameAudioSamples * 10000000LL) / rate;
                    int64_t gapHns = chunkStartHns - expectedHns;

                    // Resync audio timestamp if FMOD paused/froze or at segment start
                    if (gapHns > 400000LL) { // > 40ms gap
                        if (gapHns <= 5000000LL) { // <= 500ms: bridge with silence
                            int silenceSamples = static_cast<int>((gapHns * rate) / 10000000LL);
                            if (silenceSamples > 0) {
                                silenceScratch.assign(static_cast<size_t>(silenceSamples) * channels, 0.0f);
                                if (m_mixMicIntoGame) {
                                    mixMicInto(silenceScratch, channels);
                                }
                                m_encoder->writeGameAudio(silenceScratch.data(), silenceSamples, channels, rate, expectedHns);
                                m_gameAudioSamples += silenceSamples;
                            }
                        } else {
                            // > 500ms gap: jump sample counter directly to real capture timestamp
                            m_gameAudioSamples = (chunkStartHns * rate) / 10000000LL;
                        }
                    }

                    int64_t ts = (m_gameAudioSamples * 10000000LL) / rate;
                    if (m_mixMicIntoGame && !m_micMixFifo.empty()) {
                        mixMicInto(gameAudioScratch, channels);
                    }
                    if (!m_encoder->writeGameAudio(gameAudioScratch.data(), numSamples, channels, rate, ts)) {
                        if (++m_writeErrorCount >= MAX_CONSECUTIVE_WRITE_ERRORS) {
                            if (frame) FrameGrabber::get()->recycleFrame(std::move(frame));
                            handleFatalRecordingError("Instant Replay stopped: audio write error (TEMP drive may be full)");
                            break;
                        }
                    }
                    m_gameAudioSamples += numSamples;
                }
            } else if (m_mixMicIntoGame) {
                // If no game audio arrived for >100ms in single-track mode, flush queued mic audio directly
                int rate = AudioGrabber::get()->getGameSampleRate();
                if (rate <= 0) rate = 44100;
                size_t thresholdSamples = static_cast<size_t>(rate / 10) * 2;
                if (m_micMixFifo.size() >= thresholdSamples) {
                    int numSamples = static_cast<int>(m_micMixFifo.size() / 2);
                    std::vector<float> mixed(m_micMixFifo.begin(), m_micMixFifo.begin() + numSamples * 2);
                    m_micMixFifo.clear();
                    int64_t ts = (m_gameAudioSamples * 10000000LL) / rate;
                    m_encoder->writeGameAudio(mixed.data(), numSamples, 2, rate, ts);
                    m_gameAudioSamples += numSamples;
                }
            }

            for (const auto& micAudio : pendingMicAudio) {
                if (!micAudio || micAudio->channels <= 0 || micAudio->samples.empty()) continue;
                int channels = micAudio->channels;
                int rate = micAudio->sampleRate > 0 ? micAudio->sampleRate : 48000;
                int numSamples = static_cast<int>(micAudio->samples.size() / channels);

                int64_t chunkStartHns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                    micAudio->timestamp - m_currentSegmentStart).count() / 100;
                int64_t chunkDurationHns = (static_cast<int64_t>(numSamples) * 10000000LL) / rate;

                if (chunkStartHns + chunkDurationHns <= 0) continue;
                if (chunkStartHns < 0) chunkStartHns = 0;

                int64_t expectedHns = (m_micAudioSamples * 10000000LL) / rate;
                int64_t gapHns = chunkStartHns - expectedHns;
                if (gapHns > 400000LL) {
                    if (gapHns <= 5000000LL) {
                        int silenceSamples = static_cast<int>((gapHns * rate) / 10000000LL);
                        if (silenceSamples > 0) {
                            silenceScratch.assign(static_cast<size_t>(silenceSamples) * channels, 0.0f);
                            m_encoder->writeMicAudio(silenceScratch.data(), silenceSamples, channels, rate, expectedHns);
                            m_micAudioSamples += silenceSamples;
                        }
                    } else {
                        m_micAudioSamples = (chunkStartHns * rate) / 10000000LL;
                    }
                }

                int64_t ts = (m_micAudioSamples * 10000000LL) / rate;
                m_encoder->writeMicAudio(micAudio->samples.data(), numSamples, channels, rate, ts);
                m_micAudioSamples += numSamples;
            }
        }

        if (frame) {
            FrameGrabber::get()->recycleFrame(std::move(frame));
        }

        if (m_saveRequested.exchange(false)) {
            handleSaveRequest(true, sessionConfig);
        }

        // Time-based rotation (asynchronous finalization so worker thread does not stall)
        auto now = std::chrono::steady_clock::now();
        double elapsed = std::chrono::duration<double>(now - m_currentSegmentStart).count();
        if (elapsed >= SEGMENT_DURATION && m_encoder && m_encoder->isRecording()) {
            rotateSegment(false, sessionConfig);
        }
    }

    // If a save was requested right before stopReplay() woke us up, fulfill it before tearing down
    if (m_saveRequested.exchange(false)) {
        handleSaveRequest(false, sessionConfig);
    }

    joinFinalizeThread();
    if (m_encoder && m_encoder->isRecording()) {
        m_encoder->finish();
    }
    if (SUCCEEDED(coHr)) CoUninitialize();
}

void ReplayManager::saveReplay() {
    if (!m_running.load()) return;
    bool expected = false;
    if (!m_saving.compare_exchange_strong(expected, true)) return;

    m_saveRequested.store(true);
    m_cv.notify_all();
}

double ReplayManager::getCurrentReplaySeconds() const {
    if (!m_running.load()) return 0.0;

    std::lock_guard<std::mutex> lock(m_segmentMutex);
    double total = 0.0;
    for (const auto& seg : m_activeSegments) {
        total += seg.durationSeconds;
    }
    auto now = std::chrono::steady_clock::now();
    total += std::chrono::duration<double>(now - m_currentSegmentStart).count();
    return std::min(total, static_cast<double>(ReplayConfig::get().durationSeconds.load()));
}

void ReplayManager::openRecordingsFolder() {
    auto dir = ReplayConfig::get().getRecordingsDirectory();
    ShellExecuteW(NULL, L"open", dir.c_str(), NULL, NULL, SW_SHOWNORMAL);
}

} // namespace replay

#endif // GEODE_IS_WINDOWS
