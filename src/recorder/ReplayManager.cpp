#include "ReplayManager.hpp"
#include "../ui/ReplayNotification.hpp"
#include <Geode/binding/FMODAudioEngine.hpp>
#include <windows.h>
#include <shellapi.h>
#include <sstream>
#include <iomanip>
#include <ctime>
#include <algorithm>

namespace replay {

static constexpr double SEGMENT_DURATION = 30.0; // 30-second rolling chunks
static constexpr size_t MAX_QUEUED_VIDEO_BYTES = 384ull * 1024 * 1024;

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
    std::error_code ec;
    auto tempDir = getTempDir();
    if (std::filesystem::exists(tempDir, ec)) {
        for (const auto& entry : std::filesystem::directory_iterator(tempDir, ec)) {
            std::filesystem::remove(entry.path(), ec);
        }
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
    m_segmentIndex = 0;
    m_lastVideoHns = -1;
    m_gameAudioSamples = 0;
    m_micAudioSamples = 0;
    m_currentSegmentPath.clear();

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

    std::lock_guard<std::mutex> lock(m_segmentMutex);
    for (const auto& seg : m_activeSegments) {
        std::error_code ec;
        std::filesystem::remove(seg.filePath, ec);
    }
    m_activeSegments.clear();

    if (!m_currentSegmentPath.empty()) {
        std::error_code ec;
        std::filesystem::remove(m_currentSegmentPath, ec);
        m_currentSegmentPath.clear();
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
// Segment handling (worker thread only)
// ---------------------------------------------------------------------------

void ReplayManager::finalizeCurrentSegment() {
    if (!m_encoder || !m_encoder->isRecording()) return;

    // Fallback: If no game audio samples were recorded in this segment,
    // write a brief silence packet so the audio track is not completely empty,
    // preventing VLC sync freeze.
    if (m_gameAudioSamples == 0 && m_lastVideoHns > 0) {
        int rate = AudioGrabber::get()->getGameSampleRate();
        int silenceSamples = rate / 2; // 0.5s silence
        std::vector<float> silence(silenceSamples * 2, 0.0f);
        m_encoder->writeGameAudio(silence.data(), silenceSamples, 2, rate, 0);
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

    if (ok && hasVideo && segDuration > 0.25) {
        std::lock_guard<std::mutex> lock(m_segmentMutex);
        m_activeSegments.push_back({m_currentSegmentPath, m_currentSegmentStart, segDuration});
    } else {
        std::error_code ec;
        std::filesystem::remove(m_currentSegmentPath, ec);
    }
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
    if (!ok) {
        geode::log::error("Instant Replay: failed to start encoder ({}x{})", m_videoWidth, m_videoHeight);
    }
}

void ReplayManager::rotateSegment() {
    finalizeCurrentSegment();
    purgeOldSegments();
    startNewSegment();
}

void ReplayManager::purgeOldSegments() {
    if (m_saving.load()) return; // never delete files while they are being merged

    std::lock_guard<std::mutex> lock(m_segmentMutex);
    const auto& cfg = ReplayConfig::get();
    auto now = std::chrono::steady_clock::now();
    double maxAge = static_cast<double>(cfg.durationSeconds) + SEGMENT_DURATION * 2.0;

    auto it = m_activeSegments.begin();
    while (it != m_activeSegments.end()) {
        double age = std::chrono::duration<double>(now - it->startTime).count();
        if (age > maxAge) {
            std::error_code ec;
            std::filesystem::remove(it->filePath, ec);
            it = m_activeSegments.erase(it);
        } else {
            ++it;
        }
    }
}

void ReplayManager::handleSaveRequest() {
    // 1. Close the running segment so it becomes a valid MP4
    finalizeCurrentSegment();

    // 2. Pick the newest segments covering the requested duration
    std::vector<std::wstring> segsToMerge;
    double totalSeconds = 0.0;
    double maxDuration = static_cast<double>(ReplayConfig::get().durationSeconds);
    {
        std::lock_guard<std::mutex> lock(m_segmentMutex);
        for (auto it = m_activeSegments.rbegin(); it != m_activeSegments.rend(); ++it) {
            segsToMerge.insert(segsToMerge.begin(), it->filePath);
            totalSeconds += it->durationSeconds;
            if (totalSeconds >= maxDuration) break;
        }
    }

    // 3. Immediately keep recording into a fresh segment
    startNewSegment();

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

    // 4. Merge in the background (segments are protected from purge while m_saving)
    std::thread([this, segsToMerge = std::move(segsToMerge), totalSeconds]() {
        auto t = std::time(nullptr);
        struct tm tmBuf;
        localtime_s(&tmBuf, &t);
        std::wstringstream wss;
        wss << L"replay_" << std::put_time(&tmBuf, L"%Y-%m-%d_%H-%M-%S") << L".mp4";

        auto finalPath = ReplayConfig::get().getRecordingsDirectory() / wss.str();
        HRESULT coHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        bool success = MediaFoundationEncoder::concatenateSegments(segsToMerge, finalPath.wstring());
        if (SUCCEEDED(coHr)) CoUninitialize();
        m_saving.store(false);

        std::string finalPathStr = geode::utils::string::wideToUtf8(finalPath.wstring());
        geode::Loader::get()->queueInMainThread([success, finalPathStr, totalSeconds]() {
            if (!success) {
                geode::Notification::create("Instant Replay: save failed", geode::NotificationIcon::Error)->show();
                return;
            }
            if (ReplayConfig::get().showNotification) {
                ReplayNotification::show(finalPathStr, totalSeconds);
            }
            if (auto engine = FMODAudioEngine::sharedEngine()) {
                engine->playEffect("achievement_01.ogg");
            }
        });
    }).detach();
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
                        m_encoder->writeGameAudio(silence.data(), silenceSamples, channels, rate, 0);
                        m_gameAudioSamples += silenceSamples;
                    }
                }

                int64_t ts = (m_gameAudioSamples * 10000000LL) / rate;
                m_encoder->writeGameAudio(gameAudio->samples.data(), numSamples, channels, rate, ts);
                m_gameAudioSamples += numSamples;
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
            handleSaveRequest();
        }

        // Time-based rotation
        auto now = std::chrono::steady_clock::now();
        double elapsed = std::chrono::duration<double>(now - m_currentSegmentStart).count();
        if (elapsed >= SEGMENT_DURATION && m_encoder && m_encoder->isRecording()) {
            rotateSegment();
        }
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
