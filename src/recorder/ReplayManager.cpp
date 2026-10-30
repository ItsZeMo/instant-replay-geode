#include "ReplayManager.hpp"
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
static constexpr size_t MAX_QUEUED_VIDEO_BYTES = 384ull * 1024 * 1024;
static constexpr size_t MAX_MIC_MIX_SAMPLES = 48000 * 2 * 5; // cap single-track mic FIFO at ~5s stereo

ReplayManager* ReplayManager::get() {
    static ReplayManager instance;
    return &instance;
}

ReplayManager::ReplayManager() {
}

ReplayManager::~ReplayManager() {
    cleanup();
}

std::filesystem::path ReplayManager::getTempDir() const {
    char* tempPath = nullptr;
    size_t len = 0;
    if (_dupenv_s(&tempPath, &len, "TEMP") == 0 && tempPath) {
        std::filesystem::path dir = std::filesystem::path(tempPath) / "GeometryDash_InstantReplay";
        free(tempPath);
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        return dir;
    }
    std::filesystem::path fallback = geode::dirs::getTempDir() / "InstantReplay";
    std::error_code ec;
    std::filesystem::create_directories(fallback, ec);
    return fallback;
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

void ReplayManager::joinExportThread() {
    std::lock_guard<std::mutex> lock(m_exportMutex);
    if (m_exportThread.joinable()) {
        m_exportThread.join();
    }
}

void ReplayManager::initialize() {
    ReplayConfig::get().loadFromSettings();
    cleanupTempDir();

    // Frame grabber callback (render thread -> worker queue)
    FrameGrabber::get()->setFrameCallback([this](std::shared_ptr<CapturedFrame> frame) {
        if (!m_running.load() || !frame) return;
        {
            std::lock_guard<std::mutex> lock(m_queueMutex);
            size_t frameBytes = std::max<size_t>(1, frame->data.size());
            size_t maxFrames = std::clamp<size_t>(MAX_QUEUED_VIDEO_BYTES / frameBytes, 4, 120);
            if (m_frameQueue.size() < maxFrames) {
                m_frameQueue.push_back(std::move(frame));
            }
        }
        m_cv.notify_one();
    });

    AudioGrabber::get()->setGameAudioCallback([this](std::shared_ptr<AudioChunk> chunk) {
        if (!m_running.load() || !chunk) return;
        {
            std::lock_guard<std::mutex> lock(m_queueMutex);
            if (m_gameAudioQueue.size() >= 50) {
                m_gameAudioQueue.pop_front();
            }
            m_gameAudioQueue.push_back(std::move(chunk));
        }
        m_cv.notify_one();
    });

    AudioGrabber::get()->setMicAudioCallback([this](std::shared_ptr<AudioChunk> chunk) {
        if (!m_running.load() || !chunk) return;
        {
            std::lock_guard<std::mutex> lock(m_queueMutex);
            if (m_micAudioQueue.size() >= 50) {
                m_micAudioQueue.pop_front();
            }
            m_micAudioQueue.push_back(std::move(chunk));
        }
        m_cv.notify_one();
    });

    AudioGrabber::get()->initialize();

    if (ReplayConfig::get().enabled) {
        startReplay();
    }
}

void ReplayManager::cleanup() {
    stopReplay();
    joinExportThread();
    AudioGrabber::get()->cleanup();
    FrameGrabber::get()->cleanup();
    cleanupTempDir();
}

void ReplayManager::startReplay() {
    if (m_running.load()) return;

    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        m_frameQueue.clear();
        m_gameAudioQueue.clear();
        m_micAudioQueue.clear();
    }

    m_videoWidth = 0;
    m_videoHeight = 0;
    // Note: m_segmentIndex is intentionally NOT reset so segment filenames never
    // collide with files still being read by an in-flight export thread.
    m_lastVideoHns = -1;
    m_gameAudioSamples = 0;
    m_micAudioSamples = 0;
    m_currentSegmentPath.clear();
    m_currentFormatKey.clear();
    resetMicMixer();

    if (ReplayConfig::get().recordMic && !AudioGrabber::get()->isMicActive()) {
        AudioGrabber::get()->startMicCapture();
    }

    m_running.store(true);
    m_replayStartTime = std::chrono::steady_clock::now();
    m_currentSegmentStart = m_replayStartTime;
    m_workerThread = std::thread(&ReplayManager::workerThreadFunc, this);
}

void ReplayManager::stopReplay() {
    if (!m_running.load()) return;

    m_running.store(false);
    m_cv.notify_all();

    if (m_workerThread.joinable()) {
        m_workerThread.join();
    }

    if (m_encoder) {
        m_encoder->finish();
        m_encoder.reset();
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

void ReplayManager::restartReplay() {
    if (!m_running.load()) return;
    if (m_saving.load()) return;

    static auto s_lastRestart = std::chrono::steady_clock::now() - std::chrono::seconds(5);
    auto now = std::chrono::steady_clock::now();
    if (std::chrono::duration<double>(now - s_lastRestart).count() < 0.08) {
        return; // Debounce rapid consecutive calls (e.g. within 80ms)
    }
    s_lastRestart = now;

    geode::log::info("Instant Replay: Replay configuration changed, restarting recording engine...");
    stopReplay();
    startReplay();
}

void ReplayManager::toggleReplay() {
    auto& cfg = ReplayConfig::get();
    cfg.enabled = !cfg.enabled;
    cfg.saveToSettings();

    if (cfg.enabled) {
        startReplay();
    } else {
        stopReplay();
    }
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

void ReplayManager::finalizeCurrentSegment() {
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

    const auto& cfg = ReplayConfig::get();
    if (cfg.recordMic && cfg.splitAudioTracks && m_micAudioSamples == 0 && m_lastVideoHns > 0) {
        int silenceSamples = 48000 / 2; // 0.5s silence
        std::vector<float> silence(silenceSamples * 2, 0.0f);
        m_encoder->writeMicAudio(silence.data(), silenceSamples, 2, 48000, 0);
    }

    bool ok = m_encoder->finish();
    bool hasVideo = m_lastVideoHns >= 0;
    double segDuration = hasVideo ? (static_cast<double>(m_lastVideoHns) / 1e7) : 0.0;

    std::lock_guard<std::mutex> lock(m_segmentMutex);
    if (ok && hasVideo && segDuration > 0.25) {
        m_activeSegments.push_back({m_currentSegmentPath, m_currentSegmentStart, segDuration, m_currentFormatKey});
    } else {
        deleteSegmentFileLocked(m_currentSegmentPath);
    }
    m_currentSegmentPath.clear();
    m_currentFormatKey.clear();
}

void ReplayManager::startNewSegment() {
    m_lastVideoHns = -1;
    m_gameAudioSamples = 0;
    m_micAudioSamples = 0;
    m_currentSegmentStart = std::chrono::steady_clock::now();

    if (m_videoWidth <= 0 || m_videoHeight <= 0) return; // wait for first frame

    m_segmentIndex++;
    auto segFile = getTempDir() / ("seg_" + std::to_string(m_segmentIndex) + ".mp4");
    m_currentSegmentPath = segFile.wstring();

    const auto& cfg = ReplayConfig::get();
    m_mixMicIntoGame = cfg.recordMic && !cfg.splitAudioTracks;
    if (!m_mixMicIntoGame) {
        resetMicMixer();
    }

    if (!m_encoder) {
        m_encoder = std::make_unique<MediaFoundationEncoder>();
    }

    int gameSampleRate = AudioGrabber::get()->getGameSampleRate();
    bool ok = m_encoder->start(
        m_currentSegmentPath,
        m_videoWidth,
        m_videoHeight,
        cfg.targetFps,
        cfg.getEffectiveBitrateMbps(),
        std::max(cfg.getEffectiveMaxBitrateMbps(), cfg.getEffectiveBitrateMbps()),
        cfg.qualityLevel,
        cfg.rateControl,
        cfg.recordMic && cfg.splitAudioTracks,
        cfg.codec,
        cfg.hardwareAcceleration,
        cfg.customCodec,
        gameSampleRate
    );
    if (ok) {
        m_currentFormatKey = m_encoder->formatKey();
    } else {
        geode::log::error("Instant Replay: failed to start encoder ({}x{})", m_videoWidth, m_videoHeight);
        m_currentSegmentPath.clear();
        m_currentFormatKey.clear();
    }
}

void ReplayManager::rotateSegment() {
    finalizeCurrentSegment();
    purgeOldSegments();
    startNewSegment();
}

void ReplayManager::purgeOldSegments() {
    std::lock_guard<std::mutex> lock(m_segmentMutex);
    const auto& cfg = ReplayConfig::get();
    auto now = std::chrono::steady_clock::now();
    double maxAge = static_cast<double>(cfg.durationSeconds) + SEGMENT_DURATION * 2.0;

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

void ReplayManager::handleSaveRequest(bool continueRecording) {
    // 1. Close the running segment so it becomes a valid MP4
    finalizeCurrentSegment();

    // 2. Pick the newest compatible segments covering the requested duration,
    //    and lock their files so stopReplay/purgeOldSegments cannot delete them
    //    while the background export thread is reading them.
    std::vector<std::wstring> segsToMerge;
    double totalSeconds = 0.0;
    double maxDuration = static_cast<double>(std::max(1, ReplayConfig::get().durationSeconds));
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
        startNewSegment();
    }

    if (segsToMerge.empty()) {
        m_saving.store(false);
        geode::Loader::get()->queueInMainThread([]() {
            geode::Notification::create("Instant Replay: replay is empty", geode::NotificationIcon::Warning)->show();
        });
        return;
    }

    if (ReplayConfig::get().isLowDiskSpace()) {
        uint64_t avail = ReplayConfig::get().getAvailableDiskSpaceBytes();
        double availMB = static_cast<double>(avail) / (1024.0 * 1024.0);
        geode::Loader::get()->queueInMainThread([availMB]() {
            char buf[96];
            snprintf(buf, sizeof(buf), "Warning: Low disk space! (%.0f MB free)", availMB);
            geode::Notification::create(buf, geode::NotificationIcon::Warning, 3.0f)->show();
        });
    }

    // 4. Merge in the background. Join any previous finished export thread first.
    joinExportThread();
    std::lock_guard<std::mutex> exportLock(m_exportMutex);
    m_exportThread = std::thread([this, segsToMerge = std::move(segsToMerge), totalSeconds, trimStartSeconds]() {
        auto t = std::time(nullptr);
        struct tm tmBuf;
        localtime_s(&tmBuf, &t);
        std::wstringstream wss;
        wss << L"replay_" << std::put_time(&tmBuf, L"%Y-%m-%d_%H-%M-%S") << L".mp4";

        auto finalPath = ReplayConfig::get().getRecordingsDirectory() / wss.str();
        HRESULT coHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        ConcatResult result = MediaFoundationEncoder::concatenateSegments(segsToMerge, finalPath.wstring(), trimStartSeconds);
        if (SUCCEEDED(coHr)) CoUninitialize();

        releaseExportSegments(segsToMerge);
        m_saving.store(false);

        std::string finalPathStr = geode::utils::string::wideToUtf8(finalPath.wstring());
        double savedDuration = (result.durationSeconds > 0.0)
            ? result.durationSeconds
            : std::max(0.0, totalSeconds - trimStartSeconds);

        geode::Loader::get()->queueInMainThread([result, finalPathStr, savedDuration]() {
            if (!result.success) {
                std::string msg = "Instant Replay: save failed";
                if (!result.error.empty()) {
                    msg += " (" + result.error + ")";
                }
                geode::log::error("{}", msg);
                geode::Notification::create(msg, geode::NotificationIcon::Error, 3.0f)->show();
                return;
            }
            if (ReplayConfig::get().showNotification) {
                ReplayNotification::show(finalPathStr, savedDuration);
            }
            if (auto engine = FMODAudioEngine::sharedEngine()) {
                engine->playEffect("achievement_01.ogg");
            }
        });
    });
}

void ReplayManager::workerThreadFunc() {
    HRESULT coHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    MediaFoundationEncoder::globalInitialize();

    while (m_running.load()) {
        std::shared_ptr<CapturedFrame> frame;
        std::vector<std::shared_ptr<AudioChunk>> pendingGameAudio;
        std::vector<std::shared_ptr<AudioChunk>> pendingMicAudio;

        {
            std::unique_lock<std::mutex> lock(m_queueMutex);
            m_cv.wait_for(lock, std::chrono::milliseconds(20), [this] {
                return !m_running.load() || m_saveRequested.load() || !m_frameQueue.empty() ||
                       !m_gameAudioQueue.empty() || !m_micAudioQueue.empty();
            });

            if (!m_frameQueue.empty()) {
                frame = std::move(m_frameQueue.front());
                m_frameQueue.pop_front();
            }
            while (!m_gameAudioQueue.empty()) {
                pendingGameAudio.push_back(std::move(m_gameAudioQueue.front()));
                m_gameAudioQueue.pop_front();
            }
            while (!m_micAudioQueue.empty()) {
                pendingMicAudio.push_back(std::move(m_micAudioQueue.front()));
                m_micAudioQueue.pop_front();
            }
        }

        // (Re)start encoder when the first frame arrives or the output size changes
        if (frame && (frame->width != m_videoWidth || frame->height != m_videoHeight ||
                      !m_encoder || !m_encoder->isRecording())) {
            bool sizeChanged = frame->width != m_videoWidth || frame->height != m_videoHeight;
            m_videoWidth = frame->width;
            m_videoHeight = frame->height;
            if (sizeChanged || !m_encoder || !m_encoder->isRecording()) {
                rotateSegment();
            }
        }

        if (m_encoder && m_encoder->isRecording()) {
            if (frame) {
                // Real capture time -> correct playback speed even if the game drops below target FPS
                int64_t ts = std::chrono::duration_cast<std::chrono::nanoseconds>(
                    frame->timestamp - m_currentSegmentStart).count() / 100;
                if (ts <= m_lastVideoHns) ts = m_lastVideoHns + 1000;
                if (ts < 0) ts = 0;
                if (m_encoder->writeVideoFrame(frame->data.data(), frame->width, frame->height, ts)) {
                    m_lastVideoHns = ts;
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

            for (const auto& gameAudio : pendingGameAudio) {
                if (!gameAudio || gameAudio->channels <= 0 || gameAudio->samples.empty()) continue;
                int channels = gameAudio->channels;
                int rate = gameAudio->sampleRate > 0 ? gameAudio->sampleRate : 44100;
                int numSamples = static_cast<int>(gameAudio->samples.size() / channels);

                int64_t chunkStartHns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                    gameAudio->timestamp - m_currentSegmentStart).count() / 100;
                int64_t chunkDurationHns = (static_cast<int64_t>(numSamples) * 10000000LL) / rate;

                // Skip chunks captured entirely before current segment start
                if (chunkStartHns + chunkDurationHns <= 0) continue;

                // Initial gap padding
                if (m_gameAudioSamples == 0 && chunkStartHns > 0) {
                    int64_t silenceHns = std::min<int64_t>(chunkStartHns, 5000000LL);
                    int silenceSamples = static_cast<int>((silenceHns * rate) / 10000000LL);
                    if (silenceSamples > 0) {
                        std::vector<float> silence(silenceSamples * channels, 0.0f);
                        if (m_mixMicIntoGame) {
                            mixMicInto(silence, channels);
                        }
                        m_encoder->writeGameAudio(silence.data(), silenceSamples, channels, rate, 0);
                        m_gameAudioSamples += silenceSamples;
                    }
                }

                int64_t ts = (m_gameAudioSamples * 10000000LL) / rate;
                if (m_mixMicIntoGame && !m_micMixFifo.empty()) {
                    std::vector<float> mixed = gameAudio->samples;
                    mixMicInto(mixed, channels);
                    m_encoder->writeGameAudio(mixed.data(), numSamples, channels, rate, ts);
                } else {
                    m_encoder->writeGameAudio(gameAudio->samples.data(), numSamples, channels, rate, ts);
                }
                m_gameAudioSamples += numSamples;
            }

            // If no game audio arrived for >100ms in single-track mode, flush queued mic audio directly
            if (m_mixMicIntoGame && pendingGameAudio.empty()) {
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

                // Skip chunks captured entirely before current segment start
                if (chunkStartHns + chunkDurationHns <= 0) continue;

                // Initial gap padding
                if (m_micAudioSamples == 0 && chunkStartHns > 0) {
                    int64_t silenceHns = std::min<int64_t>(chunkStartHns, 5000000LL);
                    int silenceSamples = static_cast<int>((silenceHns * rate) / 10000000LL);
                    if (silenceSamples > 0) {
                        std::vector<float> silence(silenceSamples * channels, 0.0f);
                        m_encoder->writeMicAudio(silence.data(), silenceSamples, channels, rate, 0);
                        m_micAudioSamples += silenceSamples;
                    }
                }

                int64_t ts = (m_micAudioSamples * 10000000LL) / rate;
                m_encoder->writeMicAudio(micAudio->samples.data(), numSamples, channels, rate, ts);
                m_micAudioSamples += numSamples;
            }
        }

        if (m_saveRequested.exchange(false)) {
            handleSaveRequest(true);
        }

        // Time-based rotation
        auto now = std::chrono::steady_clock::now();
        double elapsed = std::chrono::duration<double>(now - m_currentSegmentStart).count();
        if (elapsed >= SEGMENT_DURATION && m_encoder && m_encoder->isRecording()) {
            rotateSegment();
        }
    }

    // If a save was requested right before stopReplay() woke us up, fulfill it before tearing down
    if (m_saveRequested.exchange(false)) {
        handleSaveRequest(false);
    }

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
    return std::min(total, static_cast<double>(ReplayConfig::get().durationSeconds));
}

int ReplayManager::getActiveSegmentCount() const {
    std::lock_guard<std::mutex> lock(m_segmentMutex);
    return static_cast<int>(m_activeSegments.size()) + 1;
}

std::string ReplayManager::getStatusText() const {
    if (m_saving.load()) {
        return "Saving Replay to MP4...";
    }
    if (!m_running.load()) {
        return "Replay Disabled";
    }
    int sec = static_cast<int>(getCurrentReplaySeconds());
    int m = sec / 60;
    int s = sec % 60;
    char buf[64];
    snprintf(buf, sizeof(buf), "Replay: %d:%02d / %d:%02d", m, s, ReplayConfig::get().durationSeconds / 60, ReplayConfig::get().durationSeconds % 60);
    return std::string(buf);
}

void ReplayManager::openRecordingsFolder() {
    auto dir = ReplayConfig::get().getRecordingsDirectory();
    ShellExecuteW(NULL, L"open", dir.c_str(), NULL, NULL, SW_SHOWNORMAL);
}

} // namespace replay
