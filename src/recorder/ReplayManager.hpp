#pragma once

#include "../config/ReplayConfig.hpp"
#include "../encoder/MediaFoundationEncoder.hpp"
#include "../capture/FrameGrabber.hpp"
#include "../capture/AudioGrabber.hpp"
#include <string>
#include <vector>
#include <deque>
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
};

class ReplayManager {
public:
    static ReplayManager* get();

    void initialize();
    void cleanup();

    // User actions
    void saveReplay();
    void toggleReplay();
    void toggleBuffer() { toggleReplay(); }
    void restartReplay();
    void restartBuffer() { restartReplay(); }
    bool isReplayActive() const { return m_running.load(); }
    bool isBufferActive() const { return isReplayActive(); }
    bool isSaving() const { return m_saving.load(); }

    // Live statistics
    double getCurrentReplaySeconds() const;
    double getCurrentBufferedSeconds() const { return getCurrentReplaySeconds(); }
    int getActiveSegmentCount() const;
    std::string getStatusText() const;

    // Direct folder opener
    void openRecordingsFolder();

private:
    ReplayManager();
    ~ReplayManager();

    void startReplay();
    void stopReplay();
    void startBuffer() { startReplay(); }
    void stopBuffer() { stopReplay(); }

    void workerThreadFunc();
    void finalizeCurrentSegment();
    void startNewSegment();
    void rotateSegment();
    void handleSaveRequest();
    void purgeOldSegments();
    void cleanupTempDir();

    std::filesystem::path getTempDir() const;

    std::atomic<bool> m_running{false};
    std::atomic<bool> m_saving{false};
    std::atomic<bool> m_saveRequested{false};

    std::unique_ptr<MediaFoundationEncoder> m_encoder;
    std::wstring m_currentSegmentPath;
    std::chrono::steady_clock::time_point m_currentSegmentStart;
    int m_segmentIndex = 0;

    // Encoder video size = size of the frames actually delivered by FrameGrabber
    int m_videoWidth = 0;
    int m_videoHeight = 0;
    int64_t m_lastVideoHns = -1;
    int64_t m_gameAudioSamples = 0;
    int64_t m_micAudioSamples = 0;

    std::vector<SegmentInfo> m_activeSegments;
    mutable std::mutex m_segmentMutex;

    // Queues for worker thread
    std::deque<std::shared_ptr<CapturedFrame>> m_frameQueue;
    std::deque<std::shared_ptr<AudioChunk>> m_gameAudioQueue;
    std::deque<std::shared_ptr<AudioChunk>> m_micAudioQueue;
    std::mutex m_queueMutex;
    std::condition_variable m_cv;

    std::thread m_workerThread;
    std::chrono::steady_clock::time_point m_replayStartTime;
};

} // namespace replay
