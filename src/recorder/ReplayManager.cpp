#include "ReplayManager.hpp"

#if defined(GEODE_IS_WINDOWS) || defined(GEODE_IS_MACOS)

#include "../ui/ReplayNotification.hpp"
#include "../ui/ReplayPopup.hpp"
#include "../capture/FrameBudget.hpp"
#include <Geode/binding/FMODAudioEngine.hpp>
#include <Geode/utils/async.hpp>
#include <Geode/utils/file.hpp>
#ifdef GEODE_IS_WINDOWS
#include <windows.h>
#elif defined(GEODE_IS_MACOS)
#include "../platform/MacPlatformUtils.hpp"
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif
#include <sstream>
#include <iomanip>
#include <cerrno>
#include <ctime>
#include <cmath>
#include <algorithm>

namespace replay {

namespace {

// RAII thread-scope helper for COM (Windows) and Objective-C autorelease pools (macOS).
struct MediaThreadScope {
#ifdef GEODE_IS_WINDOWS
    HRESULT coHr = E_FAIL;
    explicit MediaThreadScope(DWORD flags = COINIT_MULTITHREADED) noexcept
        : coHr(CoInitializeEx(nullptr, flags)) {}
    ~MediaThreadScope() {
        if (SUCCEEDED(coHr)) CoUninitialize();
    }
#elif defined(GEODE_IS_MACOS)
    MacAutoreleasePoolScope pool;
    explicit MediaThreadScope(int /*unused*/ = 0) noexcept {}
#endif
    MediaThreadScope(const MediaThreadScope&) = delete;
    MediaThreadScope& operator=(const MediaThreadScope&) = delete;
};

#ifdef GEODE_IS_WINDOWS
constexpr DWORD kCoInitApartment = COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE;
#else
constexpr int kCoInitApartment = 0;
#endif

} // namespace

static constexpr double SEGMENT_DURATION = 30.0; // 30-second rolling chunks
static constexpr double SEGMENT_PREWARM_SECONDS = 25.0; // pre-warm next encoder 5s before rotation
static constexpr double MAX_SEGMENT_DURATION = 120.0;   // safety fallback if prepare stalls
static constexpr double SAVE_PENDING_SWITCH_TIMEOUT = 5.0; // safety fallback for non-blocking F6 switch
static constexpr size_t MAX_QUEUED_VIDEO_FRAMES = 16;
static constexpr size_t MIN_QUEUED_VIDEO_FRAMES = 4;
static constexpr size_t BASE_QUEUED_VIDEO_BYTES = 64ull * 1024 * 1024;
static constexpr size_t HARD_MAX_QUEUED_VIDEO_BYTES = 256ull * 1024 * 1024;
static constexpr size_t MAX_IN_FLIGHT_ENCODER_FRAMES = 16;
static constexpr size_t MIN_IN_FLIGHT_ENCODER_FRAMES = 4;
static constexpr size_t MAX_IN_FLIGHT_ENCODER_BYTES = 256ull * 1024 * 1024;
static constexpr size_t MAX_MIC_MIX_FRAMES = static_cast<size_t>(AudioGrabber::kMicSampleRate) * 5; // ~5s stereo cap
static constexpr int MAX_ENCODER_START_FAILURES = 3;
static constexpr int MAX_CONSECUTIVE_WRITE_ERRORS = 5;

static bool isSegmentFile(const std::filesystem::path& path) {
    std::string name = geode::utils::string::pathToString(path.filename());
    // Match exact pattern "seg_<digits>.mp4"
    if (name.size() <= 8) return false; // at least "seg_1.mp4"
    if (name.compare(0, 4, "seg_") != 0) return false;
    if (name.compare(name.size() - 4, 4, ".mp4") != 0) return false;
    for (size_t i = 4; i + 4 < name.size(); ++i) {
        if (name[i] < '0' || name[i] > '9') return false;
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
    joinStopThread();
    joinPrepareThread();
    discardPreparedEncoder();
    joinFinalizeThread();
    joinExportThread();
    releaseSessionLock(false);
}

namespace {
bool parsePidFolderName(const std::string& name, uint32_t& outPid) {
    if (name.size() <= 4 || name.compare(0, 4, "pid_") != 0) return false;
    uint64_t val = 0;
    for (size_t i = 4; i < name.size(); ++i) {
        char c = name[i];
        if (c < '0' || c > '9') return false;
        val = val * 10 + static_cast<uint64_t>(c - '0');
        if (val > 0xFFFFFFFFULL) return false;
    }
    outPid = static_cast<uint32_t>(val);
    return outPid != 0;
}

uint32_t currentPid() noexcept {
#ifdef GEODE_IS_WINDOWS
    return static_cast<uint32_t>(GetCurrentProcessId());
#else
    return static_cast<uint32_t>(::getpid());
#endif
}

bool isPidProcessAlive(uint32_t pid) {
    if (pid == 0) return false;
    if (pid == currentPid()) return true;
#ifdef GEODE_IS_WINDOWS
    HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(pid));
    if (!hProc) {
        return false;
    }
    DWORD exitCode = 0;
    bool alive = GetExitCodeProcess(hProc, &exitCode) && exitCode == STILL_ACTIVE;
    bool sameApp = false;
    if (alive) {
        wchar_t selfExe[MAX_PATH] = {};
        wchar_t otherExe[MAX_PATH] = {};
        DWORD selfLen = GetModuleFileNameW(nullptr, selfExe, MAX_PATH);
        DWORD otherLen = MAX_PATH;
        if (selfLen > 0 && QueryFullProcessImageNameW(hProc, 0, otherExe, &otherLen)) {
            std::filesystem::path selfName = std::filesystem::path(selfExe).filename();
            std::filesystem::path otherName = std::filesystem::path(otherExe).filename();
            sameApp = !selfName.empty() && _wcsicmp(selfName.c_str(), otherName.c_str()) == 0;
        }
    }
    CloseHandle(hProc);
    return sameApp;
#elif defined(GEODE_IS_MACOS)
    // Verify both existence AND executable identity so OS PID reuse after a crash never stalls cleanup (M13).
    return isMacSameAppProcessAlive(static_cast<int>(pid));
#else
    return false;
#endif
}
} // namespace

void ReplayManager::ensureSessionLock(const std::filesystem::path& pidTempDir) {
    if (pidTempDir.empty()) return;
    std::filesystem::path baseToPersist;
    {
        std::lock_guard<std::mutex> lock(m_sessionLockMutex);
#ifdef GEODE_IS_WINDOWS
        if (m_sessionLockHandle != INVALID_HANDLE_VALUE && m_sessionLockDir == pidTempDir) {
            return;
        }

        auto lockPath = pidTempDir / "session.lock";
        HANDLE hNew = CreateFileW(
            lockPath.c_str(),
            GENERIC_READ | GENERIC_WRITE,
            0, // exclusive lock so live instances are never cleaned by another process
            nullptr,
            OPEN_ALWAYS,
            FILE_ATTRIBUTE_NORMAL,
            nullptr
        );
        if (hNew == INVALID_HANDLE_VALUE) {
            geode::log::warn("ReplayManager: failed to lock session file '{}': err={}",
                geode::utils::string::pathToString(lockPath), GetLastError());
            return;
        }

        if (m_sessionLockHandle != INVALID_HANDLE_VALUE) {
            CloseHandle(m_sessionLockHandle);
            m_sessionLockHandle = INVALID_HANDLE_VALUE;
            if (!m_sessionLockDir.empty()) {
                std::error_code ec;
                std::filesystem::remove(m_sessionLockDir / "session.lock", ec);
                std::filesystem::remove(m_sessionLockDir, ec);
            }
        }
        m_sessionLockHandle = hNew;
#else
        if (m_sessionLockFd >= 0 && m_sessionLockDir == pidTempDir) {
            return;
        }

        auto lockPath = pidTempDir / "session.lock";
        int fdNew = ::open(lockPath.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0644);
        if (fdNew < 0) {
            geode::log::warn("ReplayManager: failed to open session file '{}': errno={}",
                geode::utils::string::pathToString(lockPath), errno);
            return;
        }
        if (::flock(fdNew, LOCK_EX | LOCK_NB) != 0) {
            geode::log::warn("ReplayManager: failed to lock session file '{}': errno={}",
                geode::utils::string::pathToString(lockPath), errno);
            ::close(fdNew);
            return;
        }

        if (m_sessionLockFd >= 0) {
            ::flock(m_sessionLockFd, LOCK_UN);
            ::close(m_sessionLockFd);
            m_sessionLockFd = -1;
            if (!m_sessionLockDir.empty()) {
                std::error_code ec;
                std::filesystem::remove(m_sessionLockDir / "session.lock", ec);
                std::filesystem::remove(m_sessionLockDir, ec);
            }
        }
        m_sessionLockFd = fdNew;
#endif
        m_sessionLockDir = pidTempDir;

        auto baseDir = pidTempDir.parent_path().lexically_normal();
        if (!baseDir.empty() && baseDir != m_persistedTempBase) {
            m_persistedTempBase = baseDir;
            baseToPersist = baseDir;
        }
    }

    if (!baseToPersist.empty()) {
        std::string baseUtf8 = geode::utils::string::pathToString(baseToPersist);
        geode::Loader::get()->queueInMainThread([baseUtf8]() {
            if (auto* mod = geode::Mod::get()) {
                mod->setSavedValue<std::string>("last-temp-base", baseUtf8);
            }
        });
    }
}

void ReplayManager::releaseSessionLock(bool removeFiles) {
    std::lock_guard<std::mutex> lock(m_sessionLockMutex);
#ifdef GEODE_IS_WINDOWS
    if (m_sessionLockHandle != INVALID_HANDLE_VALUE) {
        CloseHandle(m_sessionLockHandle);
        m_sessionLockHandle = INVALID_HANDLE_VALUE;
    }
#else
    if (m_sessionLockFd >= 0) {
        ::flock(m_sessionLockFd, LOCK_UN);
        ::close(m_sessionLockFd);
        m_sessionLockFd = -1;
    }
#endif
    if (removeFiles && !m_sessionLockDir.empty()) {
        std::error_code ec;
        std::filesystem::remove(m_sessionLockDir / "session.lock", ec);
        std::filesystem::remove(m_sessionLockDir, ec);
    }
    m_sessionLockDir.clear();
}

std::filesystem::path ReplayManager::getTempDir(bool createIfMissing) const {
    return ReplayConfig::get().getTempDirectory(createIfMissing);
}

void ReplayManager::cleanupTempDir() {
    std::filesystem::path persistedBaseCopy;
    {
        std::lock_guard<std::mutex> slLock(m_sessionLockMutex);
        persistedBaseCopy = m_persistedTempBase;
    }

    std::lock_guard<std::mutex> lock(m_segmentMutex);

    auto cleanSegmentsInDir = [this](const std::filesystem::path& dir) {
        if (dir.empty()) return;
        std::error_code ec;
        if (!std::filesystem::exists(dir, ec) || !std::filesystem::is_directory(dir, ec)) return;
        std::filesystem::directory_iterator it(dir, ec);
        std::filesystem::directory_iterator end;
        while (!ec && it != end) {
            std::error_code fileEc;
            bool isReg = it->is_regular_file(fileEc);
            auto entryPath = it->path();
            it.increment(ec);
            if (!isReg || !isSegmentFile(entryPath)) {
                continue;
            }
            if (m_exportLockedSegments.find(entryPath) != m_exportLockedSegments.end()) {
                m_deferredDeletes.push_back(entryPath);
            } else {
                std::filesystem::remove(entryPath, fileEc);
            }
        }
    };

    auto cleanOrphanedPidDirs = [&cleanSegmentsInDir](const std::filesystem::path& baseDir) {
        if (baseDir.empty()) return;
        std::error_code ec;
        if (!std::filesystem::exists(baseDir, ec) || !std::filesystem::is_directory(baseDir, ec)) return;
        uint32_t selfPid = currentPid();
        std::filesystem::directory_iterator it(baseDir, ec);
        std::filesystem::directory_iterator end;
        while (!ec && it != end) {
            std::error_code dirEc;
            bool isDir = it->is_directory(dirEc);
            auto dirPath = it->path();
            it.increment(ec);
            if (!isDir) continue;

            uint32_t otherPid = 0;
            if (!parsePidFolderName(geode::utils::string::pathToString(dirPath.filename()), otherPid) || otherPid == selfPid) {
                continue;
            }

            auto lockPath = dirPath / "session.lock";
#ifdef GEODE_IS_WINDOWS
            HANDLE hLock = CreateFileW(
                lockPath.c_str(),
                GENERIC_READ | GENERIC_WRITE,
                0,
                nullptr,
                OPEN_EXISTING,
                FILE_ATTRIBUTE_NORMAL,
                nullptr
            );
            if (hLock == INVALID_HANDLE_VALUE) {
                DWORD err = GetLastError();
                if (err == ERROR_SHARING_VIOLATION) {
                    continue; // active instance holds session.lock
                }
                if ((err != ERROR_FILE_NOT_FOUND && err != ERROR_PATH_NOT_FOUND) || isPidProcessAlive(otherPid)) {
                    continue;
                }
            } else if (isPidProcessAlive(otherPid)) {
                CloseHandle(hLock);
                continue;
            }

            cleanSegmentsInDir(dirPath);
            if (hLock != INVALID_HANDLE_VALUE) {
                CloseHandle(hLock);
            }
#else
            int lockFd = ::open(lockPath.c_str(), O_RDWR | O_CLOEXEC);
            if (lockFd < 0) {
                if (errno != ENOENT || isPidProcessAlive(otherPid)) {
                    continue;
                }
            } else {
                if (::flock(lockFd, LOCK_EX | LOCK_NB) != 0) {
                    ::close(lockFd);
                    continue; // active instance holds session.lock
                }
                if (isPidProcessAlive(otherPid)) {
                    ::flock(lockFd, LOCK_UN);
                    ::close(lockFd);
                    continue;
                }
            }

            cleanSegmentsInDir(dirPath);
            if (lockFd >= 0) {
                ::flock(lockFd, LOCK_UN);
                ::close(lockFd);
            }
#endif
            std::error_code rmEc;
            std::filesystem::remove(lockPath, rmEc);
            std::filesystem::remove(dirPath, rmEc); // removes only if empty
        }
    };

    auto curTempDir = getTempDir(false);
    cleanSegmentsInDir(curTempDir);
    if (!m_lastTempDir.empty() && m_lastTempDir != curTempDir) {
        cleanSegmentsInDir(m_lastTempDir);
    }

    // Collect every candidate base folder (custom, system temp, Geode fallback, persisted previous base)
    // so crashed sessions are cleaned up even if the user changed temp folders or hit disk fallback.
    std::vector<std::filesystem::path> candidateBases = ReplayConfig::get().getAllCandidateTempBaseDirectories();
    auto addBase = [&candidateBases](const std::filesystem::path& p) {
        if (p.empty()) return;
        auto norm = p.lexically_normal();
        for (const auto& existing : candidateBases) {
            if (existing == norm) return;
        }
        candidateBases.push_back(std::move(norm));
    };
    addBase(curTempDir.parent_path());
    if (!m_lastTempDir.empty()) {
        addBase(m_lastTempDir.parent_path());
    }
    addBase(persistedBaseCopy);

    for (const auto& baseDir : candidateBases) {
        cleanOrphanedPidDirs(baseDir);
    }
}

void ReplayManager::deleteSegmentFileLocked(const std::filesystem::path& path) {
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

void ReplayManager::releaseExportSegments(const std::vector<std::filesystem::path>& paths) {
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
        std::error_code ec;
        std::filesystem::remove(stale.path, ec);
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

void ReplayManager::joinStopThread() {
    std::lock_guard<std::mutex> lock(m_stopMutex);
    if (m_stopThread.joinable()) {
        m_stopThread.join();
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
        if (!*it || !(*it)->hasBuffer() || FrameGrabber::isFrameBufferUnique(**it)) {
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
    m_pendingRestart.store(false);
    m_stopping.store(false);
    m_saveRequested.store(false);
    m_savePending.store(false);
    ReplayConfig::refreshNativeFrameHeight();
#ifdef GEODE_IS_MACOS
    {
        const auto& caps = getMacVideoEncoderCaps();
        geode::log::info(
            "Instant Replay: VideoToolbox compatibility - AV1(HW={},SW={}), HEVC(HW={},SW={}), H.264(HW={},SW={})",
            caps.av1Hardware, caps.av1Software,
            caps.hevcHardware, caps.hevcSoftware,
            caps.h264Hardware, caps.h264Software);
    }
#endif
    ReplayConfig::get().loadFromSettings();
    m_sessionConfig = ReplayConfig::get().snapshot();
    if (auto* mod = geode::Mod::get()) {
        std::string savedBase = mod->getSavedValue<std::string>("last-temp-base");
        if (!savedBase.empty()) {
            std::lock_guard<std::mutex> slLock(m_sessionLockMutex);
            m_persistedTempBase = ReplayConfig::pathFromUtf8(savedBase).lexically_normal();
        }
    }
    cleanupTempDir();

    // Frame grabber callback (render thread -> worker queue)
    FrameGrabber::get()->setFrameCallback([this](std::shared_ptr<CapturedFrame> frame) {
        if (!frame) return;
        std::shared_ptr<CapturedFrame> dropped;
        bool droppedActiveOverflow = false;
        {
            std::lock_guard<std::mutex> lock(m_queueMutex);
            if (!m_running.load(std::memory_order_relaxed)) {
                dropped = std::move(frame);
            } else {
                const size_t queuedBytes = sumFrameBufferBytes(m_frameQueue, [](const CapturedFrame& f) { return f.byteSize; });
                const size_t effectiveQueueBudget = effectiveFrameByteBudget(
                    BASE_QUEUED_VIDEO_BYTES, frame->byteSize, MIN_QUEUED_VIDEO_FRAMES, HARD_MAX_QUEUED_VIDEO_BYTES);
                if (canEnqueueVideoFrameBytes(m_frameQueue.size(), queuedBytes, frame->byteSize,
                                              MAX_QUEUED_VIDEO_FRAMES, effectiveQueueBudget)) {
                    m_frameQueue.push_back(std::move(frame));
                } else {
                    dropped = std::move(frame);
                    droppedActiveOverflow = true;
                }
            }
        }
        if (droppedActiveOverflow) {
            m_queueDropCount.fetch_add(1, std::memory_order_relaxed);
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
    {
        std::lock_guard<std::mutex> lc(m_lifecycleMutex);
        m_pendingRestart.store(false);
    }

    stopReplay();
    joinFinalizeThread();
    joinExportThread();
    AudioGrabber::get()->cleanup();
    FrameGrabber::get()->cleanup(true);
    cleanupTempDir();
    releaseSessionLock(true);
}

bool ReplayManager::isCaptureBackendReady() noexcept {
#ifdef GEODE_IS_MACOS
    return isMacGLCaptureHookInstalled();
#else
    return true;
#endif
}

void ReplayManager::startReplay() {
    {
        std::lock_guard<std::mutex> lc(m_lifecycleMutex);
        if (m_running.load() || m_shuttingDown.load()) return;
        if (m_stopping.load()) {
            // Stop teardown is still running asynchronously; queue restart without blocking the UI thread
            m_pendingRestart.store(true);
            return;
        }
        m_pendingRestart.store(false);
    }

    // Stop teardown has already finished; harvest any exited thread handle before starting anew
    joinStopThread();
    if (m_running.load() || m_shuttingDown.load() || m_stopping.load()) return;

    if (!isCaptureBackendReady()) {
        geode::log::error("Instant Replay: cannot start recording because the macOS GL capture hook failed to install (M20)");
        ReplayConfig::get().enabled = false;
        ReplayConfig::get().saveToSettings();
        geode::Loader::get()->queueInMainThread([]() {
            geode::Notification::create(
                "Instant Replay: Capture hook failed - cannot record video",
                geode::NotificationIcon::Error,
                4.0f
            )->show();
            if (auto* popup = ReplayPopup::get()) {
                popup->refreshUI();
                popup->updateStatusDisplay(0.0f);
            }
        });
        return;
    }

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
    PlatformEncoder::resetCodecCache();

    m_videoWidth = 0;
    m_videoHeight = 0;
    m_sessionEffBitrateMbps = 0;
    m_sessionEffMaxBitrateMbps = 0;
    // Note: m_segmentIndex is intentionally NOT reset so segment filenames never
    // collide with files still being read by an in-flight export thread.
    m_lastVideoHns = -1;
    m_lastVideoCaptureTime = {};
    m_lastWrittenVideoCaptureTime = {};
    m_staleDropCount.store(0, std::memory_order_relaxed);
    m_queueDropCount.store(0, std::memory_order_relaxed);
    m_inFlightDropCount.store(0, std::memory_order_relaxed);
    m_lastDropLogTime = {};
    m_lastLoggedQueueDrops = 0;
    m_lastLoggedInFlightDrops = 0;
    m_gameAudioSamples = 0;
    m_micAudioSamples = 0;
    m_seamTrimmedGameSamples = 0;
    m_seamTrimmedMicSamples = 0;
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
    m_carryGameChunks.clear();
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
    int initW = 0, initH = 0;
    if (ReplayConfig::getNativeFramebufferSize(initW, initH)) {
        bool clamped = false;
        bool clampedOversizeMac = false;
        std::tie(targetW, targetH) = m_sessionConfig.getSessionResolutionDimensions(
            initW, initH, &clamped, &clampedOversizeMac);
        if (clamped) {
            if (clampedOversizeMac) {
                geode::log::info(
                    "Instant Replay: clamped oversized native output from {}x{} to {}x{} (VideoToolbox 4K fallback)",
                    initW, initH, targetW, targetH);
            } else {
                geode::log::info(
                    "Instant Replay: clamped H.264 output size from {}x{} native to {}x{} (max {}x{})",
                    initW, initH, targetW, targetH, kH264MaxWidth, kH264MaxHeight);
            }
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

void ReplayManager::finishStopCleanup() {
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
    m_carryGameChunks.clear();
    m_carryMicAudio.clear();
    m_savePendingSwitch = false;
    m_lastVideoHns = -1;
    m_lastVideoCaptureTime = {};
    m_lastWrittenVideoCaptureTime = {};
    m_sessionEffBitrateMbps = 0;
    m_sessionEffMaxBitrateMbps = 0;

    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        m_saveRequested.store(false);
        m_savePending.store(false);
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

    bool shouldRestart = false;
    {
        std::lock_guard<std::mutex> lc(m_lifecycleMutex);
        shouldRestart = !m_shuttingDown.load() && m_pendingRestart.exchange(false);
        m_stopping.store(false);
    }

    if (!m_shuttingDown.load()) {
        geode::queueInMainThread([this, shouldRestart]() {
            if (m_shuttingDown.load()) return;
            if (shouldRestart && ReplayConfig::get().enabled.load()) {
                startReplay();
                if (ReplayConfig::get().showNotification.load() && isReplayActive()) {
                    geode::Notification::create(
                        "Instant Replay: Replay Enabled",
                        geode::NotificationIcon::Success,
                        1.5f
                    )->show();
                }
            }
            if (auto* popup = ReplayPopup::get()) {
                popup->refreshUI();
                popup->updateStatusDisplay(0.0f);
                popup->scheduleDiskSpaceCheck(true);
            }
        });
    }
}

void ReplayManager::stopReplay() {
    FrameGrabber::get()->setCapturing(false);
    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        m_running.store(false);
        ++m_sessionId;
    }
    m_prepareCancel.store(true);
    m_cv.notify_all();

    {
        std::lock_guard<std::mutex> lc(m_lifecycleMutex);
        m_pendingRestart.store(false);
        if (m_shuttingDown.load()) {
            m_stopping.store(true);
        } else {
            if (m_stopping.exchange(true)) {
                return;
            }
            joinStopThread();
            std::lock_guard<std::mutex> stopLock(m_stopMutex);
            m_stopThread = std::thread([this]() {
                finishStopCleanup();
            });
            return;
        }
    }

    // Synchronous teardown on game exit so GL/FMOD stay valid (#5 invariant)
    joinStopThread();
    finishStopCleanup();
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

ToggleReplayResult ReplayManager::toggleReplay() {
    auto& cfg = ReplayConfig::get();
    bool handledWhileStopping = false;
    bool restartQueued = false;
    {
        std::lock_guard<std::mutex> lc(m_lifecycleMutex);
        if (m_stopping.load()) {
            handledWhileStopping = true;
            restartQueued = !m_pendingRestart.load();
            m_pendingRestart.store(restartQueued);
        }
    }
    if (handledWhileStopping) {
        cfg.enabled = restartQueued;
        cfg.saveToSettings();
        return restartQueued ? ToggleReplayResult::RestartQueued : ToggleReplayResult::StopPending;
    }

    bool newState = !cfg.enabled.load();
    cfg.enabled = newState;
    cfg.saveToSettings();

    if (newState) {
        if (!isCaptureBackendReady()) {
            startReplay();
            return ToggleReplayResult::Unavailable;
        }
        startReplay();
        return m_pendingRestart.load() ? ToggleReplayResult::RestartQueued : ToggleReplayResult::Enabled;
    }
    stopReplay();
    return ToggleReplayResult::Stopping;
}

void ReplayManager::handleFatalRecordingError(const std::string& reason) {
    geode::log::error("Instant Replay: {}", reason);

    uint64_t failedSession = 0;
    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        m_running.store(false);
        m_saveRequested.store(false);
        m_savePending.store(false);
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
    m_micMix.reset();
    m_micMixResampleScratch.clear();
    m_micMixResampler.reset();
}

void ReplayManager::rebaseMicMixerForNewSegment(
    std::chrono::steady_clock::time_point prevSegStart,
    std::chrono::steady_clock::time_point nextSegStart
) {
    m_micMix.rebaseSegmentOrigin(prevSegStart, nextSegStart);
}

void ReplayManager::appendMicToMix(
    const AudioChunk& chunk,
    int targetRate,
    std::chrono::steady_clock::time_point captureStartTime,
    size_t startFrameOffset,
    size_t maxFrameCount
) {
    if (chunk.samples.size() < 2 || targetRate <= 0) return;
    size_t totalFrames = chunk.samples.size() / 2;
    if (startFrameOffset >= totalFrames) return;
    size_t srcFrames = std::min(totalFrames - startFrameOffset, maxFrameCount);
    if (srcFrames == 0) return;
    const float* srcPtr = chunk.samples.data() + startFrameOffset * 2;

    if (m_micMix.rate != targetRate) {
        resetMicMixer();
        m_micMix.rate = targetRate;
    }

    m_micMixResampleScratch.clear();
    m_micMixResampler.process(srcPtr, srcFrames, AudioGrabber::kMicSampleRate, targetRate, m_micMixResampleScratch);
    const size_t resampledFrames = m_micMixResampleScratch.size() / 2;
    if (resampledFrames == 0) return;

    const double offsetSec = std::chrono::duration<double>(captureStartTime - m_currentSegmentStart).count();
    const int64_t chunkStartSample = static_cast<int64_t>(std::llround(offsetSec * targetRate));
    m_micMix.appendResampled(
        m_micMixResampleScratch.data(), resampledFrames, targetRate, chunkStartSample, MAX_MIC_MIX_FRAMES);
}

void ReplayManager::mixMicInto(float* stereoSamples, size_t numFrames, int64_t targetSamplePos) {
    m_micMix.mixInto(stereoSamples, numFrames, targetSamplePos);
}

ReplayManager::WriteCheckResult ReplayManager::handleEncoderWriteError(
    const char* streamLabel,
    const EncoderOpResult& res,
    int& errorCounter,
    const ReplayConfig& cfg,
    std::string& outFatalError
) {
    std::string codec = m_encoder ? m_encoder->activeCodecName() : "encoder";
    std::string detail = res.detailString();

    if (res.fatalStorage) {
        outFatalError = "Instant Replay stopped: " + codec + " failed writing " +
                        streamLabel + " (" + detail + ")";
        return WriteCheckResult::Fatal;
    }

    const int maxErrs = res.retryable ? (MAX_CONSECUTIVE_WRITE_ERRORS * 10) : MAX_CONSECUTIVE_WRITE_ERRORS;
    errorCounter++;
    if (errorCounter < maxErrs) {
        if (res.retryable && errorCounter == 1) {
            geode::log::debug("Instant Replay: transient {} write backpressure on {} ({})", streamLabel, codec, detail);
        }
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
        PlatformEncoder::resetCodecCache();

        if (startNewSegment(cfg, std::chrono::steady_clock::now(), true)) {
            return WriteCheckResult::Recovered;
        }

        std::string startDetail = m_lastStartError.detailString();
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
    std::vector<float>& silenceScratch,
    bool& outRecovered,
    std::string& outFatalError,
    int64_t maxEndSample
) {
    outRecovered = false;
    if (!m_encoder || rate <= 0) return true;

    m_micMix.discardStaleBefore(m_gameAudioSamples);
    size_t availFrames = m_micMix.availFrames();
    if (availFrames == 0) return true;

    const int64_t maxSegmentSamples = static_cast<int64_t>(MAX_SEGMENT_DURATION * rate);
    const int64_t sampleCeiling = (maxEndSample >= 0) ? std::min(maxEndSample, maxSegmentSamples) : maxSegmentSamples;
    if (m_gameAudioSamples >= sampleCeiling) return true;

    if (m_micMix.hasAnchor && m_micMix.headSample > m_gameAudioSamples) {
        const int64_t gapTarget = std::min(m_micMix.headSample, sampleCeiling);
        if (!checkErrors && (gapTarget - m_gameAudioSamples) > static_cast<int64_t>(rate) * 2) {
            m_micMix.reset();
            return true;
        }
        while (m_gameAudioSamples < gapTarget) {
            int chunk = static_cast<int>(std::min<int64_t>(gapTarget - m_gameAudioSamples, rate));
            silenceScratch.assign(static_cast<size_t>(chunk) * 2, 0.0f);
            int64_t chunkHns = (m_gameAudioSamples * 10000000LL) / rate;
            EncoderOpResult gapRes = m_encoder->writeGameAudio(silenceScratch.data(), chunk, 2, rate, chunkHns);
            if (gapRes) {
                m_gameAudioSamples += chunk;
                if (checkErrors) m_gameAudioWriteErrors = 0;
            } else {
                if (!checkErrors) {
                    m_micMix.reset();
                    return true;
                }
                WriteCheckResult wc = handleEncoderWriteError("game audio", gapRes, m_gameAudioWriteErrors, cfg, outFatalError);
                if (wc == WriteCheckResult::Recovered) {
                    m_micMix.reset();
                    outRecovered = true;
                    return false;
                }
                if (wc == WriteCheckResult::Fatal) {
                    m_micMix.reset();
                    return false;
                }
                break;
            }
        }
    }

    availFrames = m_micMix.availFrames();
    if (availFrames == 0) return true;
    const size_t maxWritableFrames = static_cast<size_t>(std::max<int64_t>(0, sampleCeiling - m_gameAudioSamples));
    const size_t consumeFrames = std::min(availFrames, maxWritableFrames);
    if (consumeFrames == 0) return true;

    int numSamples = static_cast<int>(consumeFrames);
    const float* dataPtr = m_micMix.samples.data() + m_micMix.readPos;
    int64_t ts = (m_gameAudioSamples * 10000000LL) / rate;
    EncoderOpResult res = m_encoder->writeGameAudio(dataPtr, numSamples, 2, rate, ts);
    if (consumeFrames < availFrames) {
        m_micMix.advanceFrames(consumeFrames);
    } else {
        m_micMix.reset();
    }
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

    // Trim PCM strictly older than m_currentSegmentStart (allowing <= 1 ms rounding tolerance on seam splits)
    // uniformly for both initial and continuation segments (#C1).
    if (chunkStartHns < 0) {
        constexpr int64_t kRoundingToleranceHns = 10000LL; // 1 ms
        if (chunkStartHns >= -kRoundingToleranceHns) {
            chunkStartHns = 0;
        } else {
            int64_t& trimCounter = isGameAudio ? m_seamTrimmedGameSamples : m_seamTrimmedMicSamples;
            if (chunkStartHns + chunkDurationHns <= 0) {
                trimCounter += numSamples;
                return true;
            }
            int64_t trimSamples = ((-chunkStartHns) * rate) / 10000000LL;
            if (trimSamples >= numSamples) {
                trimCounter += numSamples;
                return true;
            }
            if (trimSamples > 0) {
                trimCounter += trimSamples;
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

    if (gapHns > 400000LL) { // > 40ms positive gap: always fill with chunked silence so timeline never jumps
        const int64_t targetSample = (std::max<int64_t>(0, chunkStartHns) * rate) / 10000000LL;
        const int64_t maxSegmentSamples = static_cast<int64_t>(MAX_SEGMENT_DURATION * rate);
        const int64_t clampedTarget = std::min(targetSample, maxSegmentSamples);
        while (sampleCounter < clampedTarget) {
            const int silenceSamples = static_cast<int>(std::min<int64_t>(clampedTarget - sampleCounter, rate));
            silenceScratch.assign(static_cast<size_t>(silenceSamples) * kChannels, 0.0f);
            if (isGameAudio && m_mixMicIntoGame) {
                mixMicInto(silenceScratch.data(), static_cast<size_t>(silenceSamples), sampleCounter);
            }
            const int64_t chunkHns = (sampleCounter * 10000000LL) / rate;
            EncoderOpResult silenceRes = isGameAudio
                ? m_encoder->writeGameAudio(silenceScratch.data(), silenceSamples, kChannels, rate, chunkHns)
                : m_encoder->writeMicAudio(silenceScratch.data(), silenceSamples, kChannels, rate, chunkHns);
            if (silenceRes) {
                sampleCounter += silenceSamples;
                errorCounter = 0;
            } else {
                WriteCheckResult wc = handleEncoderWriteError(streamLabel, silenceRes, errorCounter, cfg, outFatalError);
                if (wc == WriteCheckResult::Recovered) {
                    outRecovered = true;
                    return false;
                }
                if (wc == WriteCheckResult::Fatal) return false;
                break;
            }
        }
    } else if (gapHns < -400000LL) { // < -40ms negative gap: sampleCounter is ahead of real time
        int64_t overlapSamples = ((-gapHns) * rate) / 10000000LL;
        if (isGameAudio && m_mixMicIntoGame && overlapSamples > 0) {
            m_micMix.advanceOnGameOverlap(sampleCounter, overlapSamples);
        }
        if (overlapSamples >= numSamples) return true;
        if (overlapSamples > 0) {
            samples += static_cast<size_t>(overlapSamples) * kChannels;
            numSamples -= static_cast<int>(overlapSamples);
        }
    }

    int64_t ts = (sampleCounter * 10000000LL) / rate;
    if (isGameAudio && m_mixMicIntoGame) {
        mixMicInto(samples, static_cast<size_t>(numSamples), sampleCounter);
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
    if (chunkStartHns < -10000LL) { // trim packets strictly older than segment start - 1ms tolerance
        if (chunkStartHns + chunkDurationHns <= 0) {
            m_seamTrimmedMicSamples += static_cast<int64_t>(availFrames);
            return;
        }
        int64_t trimSamples = ((-chunkStartHns) * rate) / 10000000LL;
        if (trimSamples >= static_cast<int64_t>(availFrames)) {
            m_seamTrimmedMicSamples += static_cast<int64_t>(availFrames);
            return;
        }
        if (trimSamples > 0) {
            m_seamTrimmedMicSamples += trimSamples;
            trim = static_cast<size_t>(trimSamples);
        }
    }

    const int64_t srcDeltaNs = ((static_cast<int64_t>(startFrameOffset + trim)) * 1000000000LL) / rate;
    const auto micCaptureStart = chunk.timestamp + std::chrono::nanoseconds(srcDeltaNs);
    appendMicToMix(chunk, encGameRate, micCaptureStart, startFrameOffset + trim, availFrames - trim);
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
            mixMicInto(silenceScratch.data(), static_cast<size_t>(keepAliveSamples), m_gameAudioSamples);
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

bool ReplayManager::consumeAudioQueuesUpTo(
    std::optional<std::chrono::steady_clock::time_point> cutoffTime,
    const ReplayConfig& cfg,
    std::vector<float>& gameAudioScratch,
    std::vector<float>& resampledGameAudioScratch,
    std::vector<float>& silenceScratch,
    bool& outRecovered,
    std::string& outFatalError,
    std::optional<int64_t> maxGameSamplesCap,
    std::optional<int64_t> maxMicSamplesCap
) {
    outRecovered = false;
    if (!m_encoder || !m_encoder->isRecording()) return true;

    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        while (!m_micAudioQueue.empty()) {
            auto pkt = std::move(m_micAudioQueue.front());
            m_micAudioQueue.pop_front();
            if (pkt && pkt->samples.size() >= 2) {
                m_carryMicAudio.push_back(std::move(pkt));
            }
        }
    }

    const int encGameRate = (m_segmentGameSampleRate > 0) ? m_segmentGameSampleRate : 48000;
    constexpr int kMicRate = AudioGrabber::kMicSampleRate;

    int gameSampleRate = 48000;
    std::chrono::steady_clock::time_point gameAudioTime{};
    size_t gameAudioFrames = AudioGrabber::get()->drainGameAudio(gameAudioScratch, gameSampleRate, gameAudioTime);
    if (gameAudioFrames > 0) {
        int srcRate = (gameSampleRate > 0) ? gameSampleRate : 48000;
        CarryGameAudio chunk;
        chunk.rate = encGameRate;
        chunk.timestamp = gameAudioTime;
        if (srcRate != encGameRate) {
            resampledGameAudioScratch.clear();
            m_gameResampler.process(gameAudioScratch.data(), gameAudioFrames,
                                    srcRate, encGameRate, resampledGameAudioScratch);
            chunk.samples.swap(resampledGameAudioScratch);
        } else {
            chunk.samples.assign(gameAudioScratch.data(), gameAudioScratch.data() + gameAudioFrames * 2);
        }
        if (chunk.samples.size() >= 2) {
            m_carryGameChunks.push_back(std::move(chunk));
        }
    }

    auto computePrefixFrames = [](std::chrono::steady_clock::time_point startTs, int rate, int64_t totalFrames,
                                  std::optional<std::chrono::steady_clock::time_point> cut) -> int64_t {
        if (!cut.has_value()) return totalFrames;
        double offsetSec = std::chrono::duration<double>(*cut - startTs).count();
        return std::clamp<int64_t>(static_cast<int64_t>(std::llround(offsetSec * rate)), 0, totalFrames);
    };

    if (m_mixMicIntoGame) {
        while (!m_carryMicAudio.empty()) {
            auto& mic = m_carryMicAudio.front();
            if (!mic || mic->samples.size() < 2) {
                m_carryMicAudio.pop_front();
                continue;
            }
            int64_t totalMicFrames = static_cast<int64_t>(mic->samples.size() / 2);
            int64_t takeFrames = computePrefixFrames(mic->timestamp, kMicRate, totalMicFrames, cutoffTime);
            if (takeFrames <= 0) break;

            feedMicChunkIntoMix(*mic, encGameRate, 0, static_cast<size_t>(takeFrames));
            if (takeFrames >= totalMicFrames) {
                m_carryMicAudio.pop_front();
            } else {
                int64_t deltaNs = (takeFrames * 1000000000LL) / kMicRate;
                mic->samples.erase(mic->samples.begin(), mic->samples.begin() + static_cast<std::ptrdiff_t>(takeFrames * 2));
                mic->timestamp += std::chrono::nanoseconds(deltaNs);
                break;
            }
        }
    }

    size_t gameFramesWritten = 0;
    while (!m_carryGameChunks.empty()) {
        auto& chunk = m_carryGameChunks.front();
        if (chunk.samples.size() < 2) {
            m_carryGameChunks.pop_front();
            continue;
        }
        int64_t totalFrames = static_cast<int64_t>(chunk.samples.size() / 2);
        int64_t takeFrames = computePrefixFrames(chunk.timestamp, chunk.rate, totalFrames, cutoffTime);
        if (maxGameSamplesCap.has_value()) {
            takeFrames = std::min(takeFrames, std::max<int64_t>(0, *maxGameSamplesCap - m_gameAudioSamples));
        }
        if (takeFrames <= 0) break;

        bool rec = false;
        if (!writeAudioWithResync(true, chunk.samples.data(), static_cast<int>(takeFrames), chunk.rate, chunk.timestamp,
                                  cfg, silenceScratch, rec, outFatalError)) {
            if (rec) {
                m_carryGameChunks.clear();
                m_carryMicAudio.clear();
                outRecovered = true;
            }
            return false;
        }
        gameFramesWritten += static_cast<size_t>(takeFrames);

        if (takeFrames >= totalFrames) {
            m_carryGameChunks.pop_front();
        } else {
            int64_t deltaNs = (takeFrames * 1000000000LL) / chunk.rate;
            chunk.samples.erase(chunk.samples.begin(), chunk.samples.begin() + static_cast<std::ptrdiff_t>(takeFrames * 2));
            chunk.timestamp += std::chrono::nanoseconds(deltaNs);
            break;
        }
    }

    if (gameFramesWritten == 0 && m_mixMicIntoGame &&
        m_micMix.availFrames() >= static_cast<size_t>(encGameRate / 10)) {
        int64_t maxEndSample = -1;
        if (cutoffTime.has_value()) {
            double cutSec = std::max(0.0, std::chrono::duration<double>(*cutoffTime - m_currentSegmentStart).count());
            maxEndSample = static_cast<int64_t>(std::llround(cutSec * encGameRate));
        }
        if (maxGameSamplesCap.has_value()) {
            maxEndSample = (maxEndSample >= 0) ? std::min(maxEndSample, *maxGameSamplesCap) : *maxGameSamplesCap;
        }
        bool rec = false;
        if (!flushMicMixFifo(encGameRate, true, cfg, silenceScratch, rec, outFatalError, maxEndSample)) {
            if (rec) {
                m_carryGameChunks.clear();
                m_carryMicAudio.clear();
                outRecovered = true;
            }
            return false;
        }
    }

    if (!m_mixMicIntoGame && cfg.recordMic.load() && cfg.splitAudioTracks.load()) {
        while (!m_carryMicAudio.empty()) {
            auto& mic = m_carryMicAudio.front();
            if (!mic || mic->samples.size() < 2) {
                m_carryMicAudio.pop_front();
                continue;
            }
            int64_t totalMicFrames = static_cast<int64_t>(mic->samples.size() / 2);
            int64_t takeFrames = computePrefixFrames(mic->timestamp, kMicRate, totalMicFrames, cutoffTime);
            if (maxMicSamplesCap.has_value()) {
                takeFrames = std::min(takeFrames, std::max<int64_t>(0, *maxMicSamplesCap - m_micAudioSamples));
            }
            if (takeFrames <= 0) break;

            bool rec = false;
            if (!writeAudioWithResync(false, mic->samples.data(), static_cast<int>(takeFrames),
                                      kMicRate, mic->timestamp, cfg, silenceScratch, rec, outFatalError)) {
                if (rec) {
                    m_carryGameChunks.clear();
                    m_carryMicAudio.clear();
                    outRecovered = true;
                }
                return false;
            }

            if (takeFrames >= totalMicFrames) {
                m_carryMicAudio.pop_front();
            } else {
                int64_t deltaNs = (takeFrames * 1000000000LL) / kMicRate;
                mic->samples.erase(mic->samples.begin(), mic->samples.begin() + static_cast<std::ptrdiff_t>(takeFrames * 2));
                mic->timestamp += std::chrono::nanoseconds(deltaNs);
                break;
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
    std::string& outFatalError,
    std::chrono::steady_clock::time_point now,
    bool bypassHorizon
) {
    if (!m_encoder || !m_encoder->isRecording()) return true;

    std::optional<std::chrono::steady_clock::time_point> cutoff;
    if (!bypassHorizon &&
        m_lastWrittenVideoCaptureTime != std::chrono::steady_clock::time_point{} &&
        (now - m_lastWrittenVideoCaptureTime) < std::chrono::milliseconds(200)) {
        cutoff = m_lastWrittenVideoCaptureTime;
    }

    bool recovered = false;
    if (!consumeAudioQueuesUpTo(cutoff, sessionConfig, gameAudioScratch, resampledGameAudioScratch,
                                silenceScratch, recovered, outFatalError)) {
        return recovered;
    }

    const int encGameRate = (m_segmentGameSampleRate > 0) ? m_segmentGameSampleRate : 48000;
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

    const int encGameRate = (m_segmentGameSampleRate > 0) ? m_segmentGameSampleRate : 48000;
    constexpr int kMicRate = AudioGrabber::kMicSampleRate;
    const double sSec = std::max(0.0, std::chrono::duration<double>(boundaryTime - m_currentSegmentStart).count());
    const int64_t targetGameSamples = static_cast<int64_t>(std::llround(sSec * encGameRate));
    const bool splitMicActive = !m_mixMicIntoGame && cfg.recordMic.load() && cfg.splitAudioTracks.load();
    const int64_t targetMicSamples = static_cast<int64_t>(std::llround(sSec * kMicRate));

    if (!consumeAudioQueuesUpTo(
            boundaryTime, cfg, gameAudioScratch, resampledGameAudioScratch, silenceScratch,
            outRecovered, outFatalError, targetGameSamples,
            splitMicActive ? std::optional<int64_t>(targetMicSamples) : std::nullopt)) {
        return outRecovered;
    }

    const int64_t eGameBeforePad = m_gameAudioSamples;
    const int64_t eMicBeforePad = m_micAudioSamples;
    int64_t padGameSamples = 0;
    int64_t padMicSamples = 0;

    // Close game audio track cleanly on S (pad with silence if FMOD didn't reach targetGameSamples yet)
    while (m_gameAudioSamples < targetGameSamples) {
        const int padChunk = static_cast<int>(std::min<int64_t>(targetGameSamples - m_gameAudioSamples, encGameRate));
        silenceScratch.assign(static_cast<size_t>(padChunk) * 2, 0.0f);
        if (m_mixMicIntoGame) {
            mixMicInto(silenceScratch.data(), static_cast<size_t>(padChunk), m_gameAudioSamples);
        }
        const int64_t chunkHns = (m_gameAudioSamples * 10000000LL) / encGameRate;
        EncoderOpResult res = m_encoder->writeGameAudio(silenceScratch.data(), padChunk, 2, encGameRate, chunkHns);
        if (res) {
            m_gameAudioSamples += padChunk;
            padGameSamples += padChunk;
            m_gameAudioWriteErrors = 0;
        } else {
            WriteCheckResult wc = handleEncoderWriteError("game audio", res, m_gameAudioWriteErrors, cfg, outFatalError);
            if (wc == WriteCheckResult::Recovered) {
                m_carryGameChunks.clear();
                m_carryMicAudio.clear();
                outRecovered = true;
                return true;
            }
            if (wc == WriteCheckResult::Fatal) return false;
            break;
        }
    }

    // Close split mic track cleanly on S (pad with silence if WASAPI mic packet hasn't arrived yet)
    if (splitMicActive) {
        while (m_micAudioSamples < targetMicSamples) {
            const int padChunk = static_cast<int>(std::min<int64_t>(targetMicSamples - m_micAudioSamples, kMicRate));
            silenceScratch.assign(static_cast<size_t>(padChunk) * 2, 0.0f);
            const int64_t chunkHns = (m_micAudioSamples * 10000000LL) / kMicRate;
            EncoderOpResult res = m_encoder->writeMicAudio(silenceScratch.data(), padChunk, 2, kMicRate, chunkHns);
            if (res) {
                m_micAudioSamples += padChunk;
                padMicSamples += padChunk;
                m_micAudioWriteErrors = 0;
            } else {
                WriteCheckResult wc = handleEncoderWriteError("microphone audio", res, m_micAudioWriteErrors, cfg, outFatalError);
                if (wc == WriteCheckResult::Recovered) {
                    m_carryGameChunks.clear();
                    m_carryMicAudio.clear();
                    outRecovered = true;
                    return true;
                }
                if (wc == WriteCheckResult::Fatal) return false;
                break;
            }
        }
    }

    geode::log::debug(
        "ReplayManager seam: S={:.6f}s E_game={:.6f}s E_mic={:.6f}s padGame={} ({:.2f}ms) padMic={} ({:.2f}ms) trimmedGame={} trimmedMic={}",
        sSec,
        static_cast<double>(eGameBeforePad) / encGameRate,
        splitMicActive ? (static_cast<double>(eMicBeforePad) / kMicRate) : -1.0,
        padGameSamples, (static_cast<double>(padGameSamples) * 1000.0) / encGameRate,
        padMicSamples, (static_cast<double>(padMicSamples) * 1000.0) / kMicRate,
        m_seamTrimmedGameSamples,
        m_seamTrimmedMicSamples
    );
    return true;
}

bool ReplayManager::flushCarryAudio(
    const ReplayConfig& cfg,
    std::vector<float>& resampledScratch,
    std::vector<float>& silenceScratch,
    std::string& outFatalError
) {
    if (!m_encoder || !m_encoder->isRecording()) {
        m_carryGameChunks.clear();
        m_carryMicAudio.clear();
        return true;
    }

    const auto now = std::chrono::steady_clock::now();
    std::optional<std::chrono::steady_clock::time_point> horizon;
    if (m_lastWrittenVideoCaptureTime != std::chrono::steady_clock::time_point{} &&
        (now - m_lastWrittenVideoCaptureTime) < std::chrono::milliseconds(200)) {
        horizon = m_lastWrittenVideoCaptureTime;
    }

    std::vector<float> gameScratch;
    bool recovered = false;
    if (!consumeAudioQueuesUpTo(horizon, cfg, gameScratch, resampledScratch, silenceScratch, recovered, outFatalError)) {
        return recovered;
    }
    return true;
}

void ReplayManager::flushTrailingSilenceForClose(const ReplayConfig& cfg) {
    const int gameRate = (m_segmentGameSampleRate > 0) ? m_segmentGameSampleRate : 48000;

    // Ensure short segments (<200ms) never close with completely empty audio tracks
    if (m_gameAudioSamples == 0 && m_lastVideoHns >= 0) {
        if (m_mixMicIntoGame && m_micMix.availFrames() > 0) {
            bool ignoredRec = false;
            std::string ignoredErr;
            std::vector<float> trailingSilence;
            flushMicMixFifo(gameRate, false, cfg, trailingSilence, ignoredRec, ignoredErr);
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
    std::filesystem::path segPath = std::move(m_currentSegmentPath);
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
        bool ok = false;
        {
            MediaThreadScope mediaScope;
            ok = enc->finish();
            enc.reset();
        }

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
        {
            std::lock_guard<std::mutex> ql(m_queueMutex);
            m_finalizeInFlight.store(false);
        }
        purgeOldSegments(cfg);
        m_cv.notify_one();
    });

    return info;
}

void ReplayManager::ensureSessionBitrates(const ReplayConfig& cfg) {
    if (m_sessionEffBitrateMbps <= 0 && m_videoHeight > 0) {
        m_sessionEffBitrateMbps = cfg.getEffectiveBitrateMbpsForHeight(m_videoHeight);
        m_sessionEffMaxBitrateMbps = std::max(
            cfg.getEffectiveMaxBitrateMbpsForHeight(m_videoHeight),
            m_sessionEffBitrateMbps);
    }
}

bool ReplayManager::startNewSegment(
    const ReplayConfig& cfg,
    std::chrono::steady_clock::time_point segStart,
    bool isContinuation
) {
    const auto prevSegStart = m_currentSegmentStart;
    m_lastVideoHns = -1;
    m_lastVideoCaptureTime = {};
    m_staleDropCount.store(0, std::memory_order_relaxed);
    m_queueDropCount.store(0, std::memory_order_relaxed);
    m_inFlightDropCount.store(0, std::memory_order_relaxed);
    m_lastDropLogTime = {};
    m_lastLoggedQueueDrops = 0;
    m_lastLoggedInFlightDrops = 0;
    m_gameAudioSamples = 0;
    m_micAudioSamples = 0;
    m_seamTrimmedGameSamples = 0;
    m_seamTrimmedMicSamples = 0;
    m_currentSegmentStart = segStart;
    m_isContinuationSegment = isContinuation;

    if (m_videoWidth <= 0 || m_videoHeight <= 0) return false; // wait for first frame
    ensureSessionBitrates(cfg);

    auto tempDir = cfg.getTempDirectory(true);
    ensureSessionLock(tempDir);
    {
        std::lock_guard<std::mutex> segLock(m_segmentMutex);
        m_lastTempDir = tempDir;
    }

    m_segmentIndex++;
    m_currentSegmentPath = tempDir / ("seg_" + std::to_string(m_segmentIndex) + ".mp4");

    m_mixMicIntoGame = cfg.recordMic.load() && !cfg.splitAudioTracks.load();
    if (!m_mixMicIntoGame || !isContinuation) {
        resetMicMixer();
    } else {
        rebaseMicMixerForNewSegment(prevSegStart, segStart);
    }

    if (!m_encoder) {
        m_encoder = std::make_unique<PlatformEncoder>();
    }

    int gameSampleRate = AudioGrabber::get()->getEncodedGameSampleRate();
    int effBitrate = m_sessionEffBitrateMbps;
    int effMaxBitrate = std::max(m_sessionEffMaxBitrateMbps, effBitrate);

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
        const auto prevSegStart = m_currentSegmentStart;
        m_mixMicIntoGame = cfg.recordMic.load() && !cfg.splitAudioTracks.load();
        if (!m_mixMicIntoGame || !isContinuation) {
            resetMicMixer();
        } else {
            rebaseMicMixerForNewSegment(prevSegStart, nextSegStart);
        }
        m_lastVideoHns = -1;
        m_lastVideoCaptureTime = {};
        m_gameAudioSamples = 0;
        m_micAudioSamples = 0;
        m_seamTrimmedGameSamples = 0;
        m_seamTrimmedMicSamples = 0;
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
    ensureSessionBitrates(cfg);

    int segIdx = ++m_segmentIndex;
    int width = m_videoWidth;
    int height = m_videoHeight;
    int effBitrate = m_sessionEffBitrateMbps;
    int effMaxBitrate = std::max(m_sessionEffMaxBitrateMbps, effBitrate);
    int gameSampleRate = AudioGrabber::get()->getEncodedGameSampleRate();
    bool hasForcedCodec = m_encoder->hasActiveCodec();
    VideoCodecFamily forcedFamily = m_encoder->activeCodecFamily();
    bool forcedHardware = m_encoder->activeHardware();
    bool allowFallback = !hasForcedCodec;

    m_prepareFailedLastAttempt.store(false);
    m_prepareCancel.store(false);
    m_prepareInFlight.store(true);

    std::lock_guard<std::mutex> lock(m_prepareMutex);
    m_prepareThread = std::thread([this, cfg, segIdx, width, height, effBitrate, effMaxBitrate, gameSampleRate,
                                   hasForcedCodec, forcedFamily, forcedHardware, allowFallback]() {
        std::filesystem::path segPath;
        bool ok = false;
        {
            MediaThreadScope mediaScope;

            auto tempDir = cfg.getTempDirectory(true);
            ensureSessionLock(tempDir);
            segPath = tempDir / ("seg_" + std::to_string(segIdx) + ".mp4");

            auto enc = std::make_unique<PlatformEncoder>();

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
                    hasForcedCodec,
                    forcedFamily,
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
                segPath.clear();
            } else {
                if (enc) {
                    if (enc->isRecording()) {
                        enc->finish();
                    }
                    enc.reset();
                }
                if (!ok && !m_prepareCancel.load()) {
                    m_prepareFailedLastAttempt.store(true);
                }
            }
        }
        if (!segPath.empty()) {
            std::error_code rmEc;
            std::filesystem::remove(segPath, rmEc);
        }
        {
            std::lock_guard<std::mutex> ql(m_queueMutex);
            m_prepareInFlight.store(false);
        }
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
    if (isSaving()) return;
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

void ReplayManager::abortPendingSave() {
    std::lock_guard<std::mutex> lock(m_queueMutex);
    m_saveRequested.store(false);
    m_savePending.store(false);
}

bool ReplayManager::canAcceptEncoderFrame(size_t frameBytes) const {
    const size_t inFlightBytes = sumFrameBufferBytes(m_inFlightFrames, [](const CapturedFrame& f) { return f.byteSize; });
    if (!canAcceptInFlightFrameBytes(
            m_inFlightFrames.size(), inFlightBytes, frameBytes,
            MAX_IN_FLIGHT_ENCODER_FRAMES, MAX_IN_FLIGHT_ENCODER_BYTES, MIN_IN_FLIGHT_ENCODER_FRAMES)) {
        return false;
    }
    const size_t writerByteLimit = std::max<size_t>(
        MAX_IN_FLIGHT_ENCODER_BYTES, frameBytes * MIN_IN_FLIGHT_ENCODER_FRAMES);
    if (m_encoder && m_encoder->isVideoQueueSaturated(MAX_IN_FLIGHT_ENCODER_FRAMES, writerByteLimit)) {
        return false;
    }
    return true;
}

bool ReplayManager::writeEncodedVideoFrame(
    std::shared_ptr<CapturedFrame> frame,
    const ReplayConfig& cfg,
    bool& outRecovered,
    std::string& outFatalError
) {
    outRecovered = false;
    if (!frame || !m_encoder || !m_encoder->isRecording()) {
        if (frame) {
            FrameGrabber::get()->recycleFrame(std::move(frame));
        }
        return true;
    }

    if (frame->timestamp < m_currentSegmentStart) {
        // Stale frame captured before the current segment started; recycle instead of clamping to 0/0.1ms
        auto drops = m_staleDropCount.fetch_add(1, std::memory_order_relaxed) + 1;
        geode::log::debug(
            "ReplayManager: dropped stale video frame ({} us before segment start, total={})",
            std::chrono::duration_cast<std::chrono::microseconds>(m_currentSegmentStart - frame->timestamp).count(),
            drops);
        FrameGrabber::get()->recycleFrame(std::move(frame));
        return true;
    }

    if (!canAcceptEncoderFrame(frame->byteSize)) {
        // Encoder in-flight queue or MF SinkWriter internal queue is saturated; drop cleanly to bound memory (#1, #7)
        m_inFlightDropCount.fetch_add(1, std::memory_order_relaxed);
        FrameGrabber::get()->recycleFrame(std::move(frame));
        return true;
    }

    int64_t ts = std::chrono::duration_cast<std::chrono::nanoseconds>(
        frame->timestamp - m_currentSegmentStart).count() / 100;
    if (ts <= m_lastVideoHns) ts = m_lastVideoHns + 1000;

#ifdef GEODE_IS_WINDOWS
    EncoderOpResult videoRes = m_encoder->writeVideoFrame(frame->buffer.get(), frame->width, frame->height, ts);
#elif defined(GEODE_IS_MACOS)
    EncoderOpResult videoRes = m_encoder->writeVideoFrame(frame->pixelBuffer.get(), frame->width, frame->height, ts);
#endif
    if (videoRes) {
        m_lastVideoHns = ts;
        m_lastVideoCaptureTime = frame->timestamp;
        m_lastWrittenVideoCaptureTime = frame->timestamp;
        m_videoWriteErrors = 0;
        if (FrameGrabber::isFrameBufferUnique(*frame)) {
            FrameGrabber::get()->recycleFrame(std::move(frame));
        } else {
            m_inFlightFrames.push_back(std::move(frame));
        }
        return true;
    }

    WriteCheckResult wc = handleEncoderWriteError("video", videoRes, m_videoWriteErrors, cfg, outFatalError);
    FrameGrabber::get()->recycleFrame(std::move(frame));
    if (wc == WriteCheckResult::Recovered) {
        outRecovered = true;
        return false;
    }
    return wc != WriteCheckResult::Fatal;
}

bool ReplayManager::drainPreBoundaryVideoFrames(
    std::chrono::steady_clock::time_point& boundaryTime,
    const ReplayConfig& cfg,
    bool& outRecovered,
    std::string& outFatalError
) {
    outRecovered = false;
    while (m_encoder && m_encoder->isRecording() && !m_shuttingDown.load()) {
        std::shared_ptr<CapturedFrame> qFrame;
        std::chrono::steady_clock::time_point frontTs{};
        size_t frontBytes = 0;
        bool hasFront = false;
        {
            std::lock_guard<std::mutex> lock(m_queueMutex);
            if (!m_frameQueue.empty()) {
                frontTs = m_frameQueue.front()->timestamp;
                frontBytes = m_frameQueue.front()->byteSize;
                hasFront = true;
                if (frontTs <= boundaryTime &&
                    m_frameQueue.front()->width == m_videoWidth &&
                    m_frameQueue.front()->height == m_videoHeight &&
                    canAcceptEncoderFrame(frontBytes)) {
                    qFrame = std::move(m_frameQueue.front());
                    m_frameQueue.pop_front();
                }
            }
        }

        if (!hasFront || frontTs > boundaryTime) {
            break;
        }
        if (!qFrame) {
            bool sizeMismatch = false;
            {
                std::lock_guard<std::mutex> lock(m_queueMutex);
                if (!m_frameQueue.empty() &&
                    (m_frameQueue.front()->width != m_videoWidth ||
                     m_frameQueue.front()->height != m_videoHeight)) {
                    sizeMismatch = true;
                }
            }
            if (sizeMismatch) {
                boundaryTime = std::min(boundaryTime, frontTs);
                break;
            }
            // Wait briefly for Media Foundation to release an in-flight buffer
            reclaimInFlightFrames(false);
            if (!canAcceptEncoderFrame(frontBytes)) {
                bool freed = false;
                for (int attempt = 0; attempt < 50 && !m_shuttingDown.load(); ++attempt) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
                    reclaimInFlightFrames(false);
                    if (canAcceptEncoderFrame(frontBytes)) {
                        freed = true;
                        break;
                    }
                }
                if (!freed) {
                    // Leave remaining frames in queue for next segment starting right at frontTs
                    boundaryTime = std::min(boundaryTime, frontTs);
                    break;
                }
            }
            continue;
        }

        bool frameRecovered = false;
        if (!writeEncodedVideoFrame(std::move(qFrame), cfg, frameRecovered, outFatalError)) {
            if (frameRecovered) {
                outRecovered = true;
                return true;
            }
            return false;
        }
    }

    if (!outRecovered) {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        if (!m_frameQueue.empty()) {
            boundaryTime = std::min(boundaryTime, m_frameQueue.front()->timestamp);
        } else if (m_lastVideoHns >= 0 &&
                   m_lastVideoCaptureTime != std::chrono::steady_clock::time_point{} &&
                   (boundaryTime - m_lastVideoCaptureTime) < std::chrono::milliseconds(200)) {
            // Align audio boundary right after the last encoded video frame so a PBO-lagged next frame stays >= boundary
            auto tightBoundary = m_lastVideoCaptureTime + std::chrono::microseconds(100);
            if (tightBoundary < boundaryTime) {
                boundaryTime = tightBoundary;
            }
        }
    }
    return true;
}

bool ReplayManager::handleSaveRequest(
    bool continueRecording,
    const ReplayConfig& cfg,
    std::chrono::steady_clock::time_point boundaryTime,
    bool alreadyRecovered,
    std::vector<float>& gameAudioScratch,
    std::vector<float>& resampledGameAudioScratch,
    std::vector<float>& silenceScratch,
    std::string& outFatalError
) {
    PendingFinalizeInfo extraPending{};
    bool wasRecording = (m_encoder && m_encoder->isRecording());

    auto failSave = [this, &outFatalError](std::string err) -> bool {
        m_carryGameChunks.clear();
        m_carryMicAudio.clear();
        m_prepareCancel.store(true);
        joinPrepareThread();
        discardPreparedEncoder();
        abortPendingSave();
        outFatalError = std::move(err);
        return false;
    };

    if (continueRecording && wasRecording && !alreadyRecovered) {
        std::string audioErr;
        bool recovered = false;
        if (!splitAudioAtBoundary(boundaryTime, cfg, gameAudioScratch, resampledGameAudioScratch, silenceScratch, recovered, audioErr)) {
            return failSave(std::move(audioErr));
        }

        if (recovered) {
            m_prepareCancel.store(true);
            joinPrepareThread();
            discardPreparedEncoder();
            m_carryGameChunks.clear();
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
                    if (!flushCarryAudio(cfg, resampledGameAudioScratch, silenceScratch, audioErr)) {
                        return failSave(std::move(audioErr));
                    }
                } else {
                    m_carryGameChunks.clear();
                    m_carryMicAudio.clear();
                    m_encoderFailCount++;
                    if (m_lastStartError.fatalStorage ||
                        m_encoderFailCount >= MAX_ENCODER_START_FAILURES) {
                        return failSave("Instant Replay stopped: " + m_lastStartError.detailString());
                    }
                    m_nextEncoderRetryTime = std::chrono::steady_clock::now() + std::chrono::seconds(2 * m_encoderFailCount);
                }
            } else {
                m_prepareCancel.store(true);
                joinPrepareThread();
                discardPreparedEncoder();

                finalizeCurrentSegment(true, cfg);
                purgeOldSegments(cfg);
                if (m_running.load()) {
                    if (startNewSegment(cfg, boundaryTime, true)) {
                        if (!flushCarryAudio(cfg, resampledGameAudioScratch, silenceScratch, audioErr)) {
                            return failSave(std::move(audioErr));
                        }
                    } else {
                        m_carryGameChunks.clear();
                        m_carryMicAudio.clear();
                        m_encoderFailCount++;
                        if (m_lastStartError.fatalStorage ||
                            m_encoderFailCount >= MAX_ENCODER_START_FAILURES) {
                            return failSave("Instant Replay stopped: " + m_lastStartError.detailString());
                        }
                        m_nextEncoderRetryTime = std::chrono::steady_clock::now() + std::chrono::seconds(2 * m_encoderFailCount);
                    }
                } else {
                    m_carryGameChunks.clear();
                    m_carryMicAudio.clear();
                }
            }
        }
    } else if (alreadyRecovered) {
        m_prepareCancel.store(true);
        joinPrepareThread();
        discardPreparedEncoder();
        m_carryGameChunks.clear();
        m_carryMicAudio.clear();
    } else if (!continueRecording) {
        m_prepareCancel.store(true);
        joinPrepareThread();
        discardPreparedEncoder();
        if (wasRecording) {
            std::string audioErr;
            if (!processPendingAudio(cfg, gameAudioScratch, resampledGameAudioScratch, silenceScratch, audioErr,
                                     std::chrono::steady_clock::now(), true /* bypassHorizon */)) {
                return failSave(std::move(audioErr));
            }
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
    std::vector<std::filesystem::path> preLockedPaths;
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
        abortPendingSave();
        geode::Loader::get()->queueInMainThread([]() {
            geode::Notification::create("Instant Replay: replay is empty", geode::NotificationIcon::Warning)->show();
        });
        return true;
    }

    // Wait for any previously finished export thread, transition savePending -> exportInFlight atomically, and run export off worker thread.
    joinExportThread();
    {
        std::lock_guard<std::mutex> qLock(m_queueMutex);
        m_exportInFlight.store(true);
        m_savePending.store(false);
    }

    bool showNotif = ReplayConfig::get().showNotification.load();
    ReplayConfig cfgCopy = cfg;

    std::lock_guard<std::mutex> exportLock(m_exportMutex);
    m_exportThread = std::thread([this,
                                  candidateSegments = std::move(candidateSegments),
                                  preLockedPaths = std::move(preLockedPaths),
                                  extraPending = std::move(extraPending),
                                  cfgCopy,
                                  showNotif]() mutable {
        struct ExportFlagGuard {
            std::atomic<bool>& flag;
            ~ExportFlagGuard() { flag.store(false); }
        } exportGuard{m_exportInFlight};

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

        std::vector<std::filesystem::path> segsToMerge;
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
        struct tm tmBuf{};
#ifdef GEODE_IS_WINDOWS
        localtime_s(&tmBuf, &t);
#else
        localtime_r(&t, &tmBuf);
#endif
        std::ostringstream oss;
        oss << "replay_" << std::put_time(&tmBuf, "%Y-%m-%d_%H-%M-%S")
            << "_" << std::setw(3) << std::setfill('0') << ms;
        std::string baseStem = oss.str();

        std::filesystem::path finalPath = saveDir / (baseStem + ".mp4");
        std::error_code ec;
        for (int suffix = 2; std::filesystem::exists(finalPath, ec); ++suffix) {
            finalPath = saveDir / (baseStem + "_" + std::to_string(suffix) + ".mp4");
        }

        ConcatResult result;
        {
            MediaThreadScope mediaScope;
            result = PlatformEncoder::concatenateSegments(segsToMerge, finalPath, trimStartSeconds);
        }

        releaseExportSegments(segsToMerge);

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
            if (!AudioGrabber::get()->playSaveSoundBypass("achievement_01.ogg")) {
                if (auto engine = FMODAudioEngine::sharedEngine()) {
                    engine->playEffect("achievement_01.ogg");
                }
            }
        });
    });
    return true;
}

void ReplayManager::workerThreadFunc(ReplayConfig sessionConfig) {
    MediaThreadScope mediaScope;
    PlatformEncoder::globalInitialize();

    std::vector<float> gameAudioScratch;
    std::vector<float> resampledGameAudioScratch;
    std::vector<float> silenceScratch;
    bool hadFatalError = false;

    while (m_running.load()) {
#ifdef GEODE_IS_MACOS
        MacAutoreleasePoolScope iterPool;
#endif
        std::shared_ptr<CapturedFrame> frame;
        {
            std::unique_lock<std::mutex> lock(m_queueMutex);
            auto wakeDeadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(10);
            if (m_savePendingSwitch) {
                auto switchDeadline = m_savePendingSince + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                    std::chrono::duration<double>(SAVE_PENDING_SWITCH_TIMEOUT));
                if (switchDeadline < wakeDeadline) {
                    wakeDeadline = switchDeadline;
                }
            }

            const bool hasEnc = (m_encoder && m_encoder->isRecording());
            const bool inBackoff = (!hasEnc && std::chrono::steady_clock::now() < m_nextEncoderRetryTime);
            if (inBackoff && m_nextEncoderRetryTime < wakeDeadline) {
                wakeDeadline = m_nextEncoderRetryTime;
            }

            m_cv.wait_until(lock, wakeDeadline, [this, hasEnc, inBackoff] {
                if (!m_running.load() || m_saveRequested.load()) return true;
                if (m_savePendingSwitch) {
                    auto nowCheck = std::chrono::steady_clock::now();
                    double waited = std::chrono::duration<double>(nowCheck - m_savePendingSince).count();
                    if (!hasEnc ||
                        m_prepareFailedLastAttempt.load() ||
                        (!m_finalizeInFlight.load() && !m_prepareInFlight.load()) ||
                        waited >= SAVE_PENDING_SWITCH_TIMEOUT) {
                        return true;
                    }
                }
                if (!inBackoff && !m_frameQueue.empty()) return true;
                if (hasEnc && (!m_micAudioQueue.empty() || AudioGrabber::get()->hasPendingGameAudio())) {
                    return true;
                }
                return false;
            });

            if (inBackoff) {
                while (!m_frameQueue.empty()) {
                    FrameGrabber::get()->recycleFrame(std::move(m_frameQueue.front()));
                    m_frameQueue.pop_front();
                }
                m_micAudioQueue.clear();
            } else {
                if (!hasEnc && m_micAudioQueue.size() > 25) {
                    while (m_micAudioQueue.size() > 25) {
                        m_micAudioQueue.pop_front();
                    }
                }
                if (!m_frameQueue.empty()) {
                    frame = std::move(m_frameQueue.front());
                    m_frameQueue.pop_front();
                }
            }
        }

        reclaimInFlightFrames(false);

        if (!m_encoder || !m_encoder->isRecording()) {
            auto curNow = std::chrono::steady_clock::now();
            if (curNow < m_nextEncoderRetryTime) {
                AudioGrabber::get()->resetGameAudioBuffer();
            } else {
                const size_t encRate = static_cast<size_t>(std::max(1, AudioGrabber::get()->getEncodedGameSampleRate()));
                if (AudioGrabber::get()->pendingGameAudioFrames() > encRate / 2) {
                    AudioGrabber::get()->resetGameAudioBuffer();
                }
            }
        }

        auto now = std::chrono::steady_clock::now();
        if (m_lastDropLogTime == std::chrono::steady_clock::time_point{}) {
            m_lastDropLogTime = now;
        } else if (now - m_lastDropLogTime >= std::chrono::seconds(5)) {
            uint64_t qDrops = m_queueDropCount.load(std::memory_order_relaxed);
            uint64_t fDrops = m_inFlightDropCount.load(std::memory_order_relaxed);
            uint64_t sDrops = m_staleDropCount.load(std::memory_order_relaxed);
            uint64_t dq = qDrops - m_lastLoggedQueueDrops;
            uint64_t df = fDrops - m_lastLoggedInFlightDrops;
            if (dq > 0 || df > 0 || sDrops > 0) {
                geode::log::debug(
                    "ReplayManager: 5s drops queue={} inFlight={} (totals: queue={}, inFlight={}, stale={})",
                    dq, df, qDrops, fDrops, sDrops);
            }
            m_lastLoggedQueueDrops = qDrops;
            m_lastLoggedInFlightDrops = fDrops;
            m_lastDropLogTime = now;
        }
        if (!m_savePendingSwitch && m_prepareFailedLastAttempt.exchange(false)) {
            m_prepareFailedCount++;
            m_nextPrepareRetryTime = now + std::chrono::seconds(2 * m_prepareFailedCount);
        }

        auto handleStartFailure = [this, &frame, &hadFatalError, &now]() -> bool {
            std::string detail = m_lastStartError.detailString();
            m_encoderFailCount++;
            if (m_lastStartError.fatalStorage ||
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
                        m_carryGameChunks.clear();
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
                    if (!frame) {
                        if (!drainPreBoundaryVideoFrames(boundaryTime, sessionConfig, splitRecovered, audioFatalErr)) {
                            hadFatalError = true;
                            handleFatalRecordingError(audioFatalErr);
                            break;
                        }
                    }
                    if (!splitRecovered) {
                        if (!splitAudioAtBoundary(boundaryTime, sessionConfig, gameAudioScratch, resampledGameAudioScratch, silenceScratch, splitRecovered, audioFatalErr)) {
                            if (frame) {
                                FrameGrabber::get()->recycleFrame(std::move(frame));
                                frame.reset();
                            }
                            hadFatalError = true;
                            handleFatalRecordingError(audioFatalErr);
                            break;
                        }
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
                            m_carryGameChunks.clear();
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
                bool videoRecovered = false;
                std::string videoFatalErr;
                if (!writeEncodedVideoFrame(std::move(frame), sessionConfig, videoRecovered, videoFatalErr)) {
                    if (videoRecovered) {
                        skipAudioAfterRecovery = true;
                    } else {
                        hadFatalError = true;
                        handleFatalRecordingError(videoFatalErr);
                        break;
                    }
                }
                frame.reset();
            }

            if (!skipAudioAfterRecovery) {
                std::string audioFatalErr;
                if (!processPendingAudio(sessionConfig, gameAudioScratch, resampledGameAudioScratch, silenceScratch, audioFatalErr, now, false)) {
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
                auto boundaryTime = saveNow;
                bool drainRecovered = false;
                std::string saveFatalErr;
                if (activeEnc) {
                    if (!drainPreBoundaryVideoFrames(boundaryTime, sessionConfig, drainRecovered, saveFatalErr)) {
                        hadFatalError = true;
                        abortPendingSave();
                        handleFatalRecordingError(saveFatalErr);
                        break;
                    }
                    if (drainRecovered) {
                        boundaryTime = std::chrono::steady_clock::now();
                    }
                }
                if (!handleSaveRequest(true, sessionConfig, boundaryTime, drainRecovered,
                                       gameAudioScratch, resampledGameAudioScratch, silenceScratch, saveFatalErr)) {
                    hadFatalError = true;
                    handleFatalRecordingError(saveFatalErr);
                    break;
                }
            }
        }
    }

    // If a save was requested right before stopReplay() woke us up (without fatal error), fulfill it cleanly before tearing down
    if (!hadFatalError && (m_saveRequested.exchange(false) || m_savePendingSwitch)) {
        m_savePendingSwitch = false;
        auto boundaryTime = std::chrono::steady_clock::now();
        bool drainRecovered = false;
        std::string saveFatalErr;
        bool drainOk = true;
        if (m_encoder && m_encoder->isRecording()) {
            drainOk = drainPreBoundaryVideoFrames(boundaryTime, sessionConfig, drainRecovered, saveFatalErr);
            if (drainOk && drainRecovered) {
                boundaryTime = std::chrono::steady_clock::now();
            }
        }
        bool saveOk = drainOk && handleSaveRequest(
            false, sessionConfig, boundaryTime, drainRecovered,
            gameAudioScratch, resampledGameAudioScratch, silenceScratch, saveFatalErr);
        if (!saveOk) {
            hadFatalError = true;
            abortPendingSave();
            geode::log::error("Instant Replay: {}", saveFatalErr);
            if (!m_shuttingDown.load()) {
                geode::Loader::get()->queueInMainThread([saveFatalErr]() {
                    geode::Notification::create(saveFatalErr, geode::NotificationIcon::Error, 4.0f)->show();
                });
            }
        }
    } else {
        m_savePendingSwitch = false;
        abortPendingSave();
        if (!hadFatalError && !m_shuttingDown.load() && m_encoder && m_encoder->isRecording()) {
            std::string stopAudioErr;
            (void)processPendingAudio(
                sessionConfig, gameAudioScratch, resampledGameAudioScratch, silenceScratch,
                stopAudioErr, std::chrono::steady_clock::now(), true /* bypassHorizon */);
        }
    }

    // Release all platform media objects held on this thread BEFORE tearing down MediaThreadScope (#20)
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
}

SaveReplayResult ReplayManager::saveReplay() {
    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        if (!m_running.load() || m_shuttingDown.load()) return SaveReplayResult::NotRunning;
        if (m_savePending.load() || m_exportInFlight.load()) return SaveReplayResult::AlreadySaving;
        m_savePending.store(true);
        m_saveRequested.store(true);
    }
    m_cv.notify_all();
    return SaveReplayResult::Started;
}

void ReplayManager::showSaveFeedback(SaveReplayResult result) {
    switch (result) {
        case SaveReplayResult::NotRunning:
            geode::Notification::create(
                "Instant Replay is OFF (Ctrl+F6 to enable)",
                geode::NotificationIcon::Warning,
                2.0f
            )->show();
            break;
        case SaveReplayResult::AlreadySaving:
            geode::Notification::create(
                "Previous replay is still saving...",
                geode::NotificationIcon::Info,
                1.5f
            )->show();
            break;
        case SaveReplayResult::Started:
            break;
    }
}

void ReplayManager::openRecordingsFolder() {
    auto cfg = ReplayConfig::get().snapshot();
    geode::async::runtime().spawnBlocking<void>([cfg]() {
#ifdef GEODE_IS_MACOS
        std::filesystem::path dir = cfg.getRecordingsDirectory(true);
        std::error_code ec;
        bool exists = std::filesystem::exists(dir, ec) && std::filesystem::is_directory(dir, ec);
        // AppKit/NSWorkspace in geode::utils::file::openFolder must execute on the main thread (M17).
        geode::queueInMainThread([dir, exists]() {
            bool opened = exists && geode::utils::file::openFolder(dir);
            if (!opened) {
                std::string dirStr = geode::utils::string::pathToString(dir);
                geode::Notification::create(
                    "Instant Replay: failed to open folder (" + dirStr + ")",
                    geode::NotificationIcon::Error,
                    3.0f
                )->show();
            }
        });
#else
        bool opened = false;
        std::filesystem::path dir;
        {
            MediaThreadScope mediaScope(kCoInitApartment);
            dir = cfg.getRecordingsDirectory(true);
            std::error_code ec;
            bool exists = std::filesystem::exists(dir, ec) && std::filesystem::is_directory(dir, ec);
            opened = exists && geode::utils::file::openFolder(dir);
        }

        if (!opened) {
            std::string dirStr = geode::utils::string::pathToString(dir);
            geode::queueInMainThread([dirStr]() {
                geode::Notification::create(
                    "Instant Replay: failed to open folder (" + dirStr + ")",
                    geode::NotificationIcon::Error,
                    3.0f
                )->show();
            });
        }
#endif
    });
}

} // namespace replay

#endif // GEODE_IS_WINDOWS || GEODE_IS_MACOS
