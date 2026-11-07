#pragma once

#include <Geode/Geode.hpp>

#ifdef GEODE_IS_WINDOWS

#include <Geode/loader/SettingV3.hpp>
#include <Geode/utils/string.hpp>
#include <windows.h>
#include <shlobj.h>
#include <string>
#include <string_view>
#include <vector>
#include <filesystem>
#include <chrono>
#include <atomic>
#include <mutex>
#include <algorithm>
#include <cctype>

namespace replay {

// Minimal COM smart pointer (shared across WASAPI, Media Foundation, and frame buffer pooling).
template <typename T>
class ComRef {
public:
    ComRef() = default;
    explicit ComRef(T* p) noexcept : m_p(p) {}
    ~ComRef() { reset(); }
    ComRef(const ComRef&) = delete;
    ComRef& operator=(const ComRef&) = delete;

    ComRef(ComRef&& other) noexcept : m_p(other.m_p) {
        other.m_p = nullptr;
    }
    ComRef& operator=(ComRef&& other) noexcept {
        if (this != &other) {
            reset();
            m_p = other.m_p;
            other.m_p = nullptr;
        }
        return *this;
    }

    void reset() noexcept {
        if (m_p) {
            m_p->Release();
            m_p = nullptr;
        }
    }
    T** put() noexcept { reset(); return &m_p; }
    T* get() const noexcept { return m_p; }
    T* operator->() const noexcept { return m_p; }
    explicit operator bool() const noexcept { return m_p != nullptr; }

private:
    T* m_p = nullptr;
};

enum class CodecType {
    AV1,
    HEVC,
    H264,
    CUSTOM
};

enum class ResolutionType {
    NATIVE,
    UHD_4K,   // 3840x2160
    QHD_1440P,// 2560x1440
    FHD_1080P,// 1920x1080
    HD_720P   // 1280x720
};

enum class RateControlMode {
    ADAPTIVE_VBR,     // Variable Bitrate: adapts dynamically to scene complexity
    CONSTANT_CBR,     // Constant Bitrate: fixed bitrate
    CONSTANT_QUALITY  // Constant Quality (CQ mode)
};

// Copyable wrapper around std::atomic<T> for lock-free cross-thread reads/writes
// while allowing ReplayConfig to be copied by value as an immutable snapshot.
template <typename T>
struct AtomicVal {
    std::atomic<T> v;
    constexpr AtomicVal() noexcept : v(T{}) {}
    constexpr AtomicVal(T init) noexcept : v(init) {}
    AtomicVal(const AtomicVal& other) noexcept : v(other.v.load(std::memory_order_relaxed)) {}
    AtomicVal& operator=(const AtomicVal& other) noexcept {
        v.store(other.v.load(std::memory_order_relaxed), std::memory_order_relaxed);
        return *this;
    }
    AtomicVal& operator=(T val) noexcept {
        v.store(val, std::memory_order_relaxed);
        return *this;
    }
    operator T() const noexcept { return v.load(std::memory_order_relaxed); }
    T load() const noexcept { return v.load(std::memory_order_relaxed); }
    void store(T val) noexcept { v.store(val, std::memory_order_relaxed); }
};

struct ReplayConfig {
    AtomicVal<bool> enabled{true};
    AtomicVal<int> durationSeconds{120}; // 2 minutes
    AtomicVal<CodecType> codec{CodecType::AV1};
    AtomicVal<bool> hardwareAcceleration{true}; // Auto GPU hardware acceleration (NVENC, AMF, QSV)
    std::string customCodec = ""; // e.g. "av1_nvenc", "amf_av1", "qsv_av1", "hevc"
    AtomicVal<ResolutionType> resolution{ResolutionType::NATIVE};
    AtomicVal<int> bitrateMbps{35};
    AtomicVal<int> maxBitrateMbps{60};
    AtomicVal<int> qualityLevel{80};
    AtomicVal<RateControlMode> rateControl{RateControlMode::ADAPTIVE_VBR};
    AtomicVal<int> targetFps{60};

    // Audio settings
    AtomicVal<bool> splitAudioTracks{true}; // Separate track 1 (Game) & track 2 (Mic)
    AtomicVal<bool> recordMic{false};       // Opt-in by default
    std::string micDeviceId = "default";
    std::string micDeviceName = "Default System Device";
    AtomicVal<float> systemVolume{1.0f};
    AtomicVal<float> micVolume{1.0f};

    // UI & storage settings
    AtomicVal<bool> showNotification{true};
    std::string customSaveFolder = "";
    std::string customTempFolder = "";

    static inline std::atomic<int> s_nativeFrameHeight{1080};

    // Must ONLY be called on the main / OpenGL render thread (#18).
    static void refreshNativeFrameHeight() noexcept {
        if (auto view = cocos2d::CCEGLView::get()) {
            auto fs = view->getFrameSize();
            if (fs.height > 0.0f) {
                s_nativeFrameHeight.store(static_cast<int>(fs.height + 0.5f), std::memory_order_relaxed);
            }
        }
    }

    static const char* codecToString(CodecType c) noexcept {
        switch (c) {
            case CodecType::CUSTOM: return "custom";
            case CodecType::AV1:    return "av1";
            case CodecType::HEVC:   return "hevc";
            case CodecType::H264:
            default:                return "h264";
        }
    }

    static CodecType codecFromString(std::string_view s) noexcept {
        if (s == "av1" || s == "av1_nvenc" || s == "auto_hw") return CodecType::AV1;
        if (s == "hevc" || s == "hevc_nvenc") return CodecType::HEVC;
        if (s == "custom") return CodecType::CUSTOM;
        return CodecType::H264;
    }

    static const char* resolutionToString(ResolutionType r) noexcept {
        switch (r) {
            case ResolutionType::UHD_4K:    return "2160p";
            case ResolutionType::QHD_1440P: return "1440p";
            case ResolutionType::FHD_1080P: return "1080p";
            case ResolutionType::HD_720P:   return "720p";
            case ResolutionType::NATIVE:
            default:                        return "native";
        }
    }

    static ResolutionType resolutionFromString(std::string_view s) noexcept {
        if (s == "2160p") return ResolutionType::UHD_4K;
        if (s == "1440p") return ResolutionType::QHD_1440P;
        if (s == "1080p") return ResolutionType::FHD_1080P;
        if (s == "720p")  return ResolutionType::HD_720P;
        return ResolutionType::NATIVE;
    }

    static const char* resolutionDisplayName(ResolutionType r) noexcept {
        switch (r) {
            case ResolutionType::UHD_4K:    return "4K";
            case ResolutionType::QHD_1440P: return "1440p";
            case ResolutionType::FHD_1080P: return "1080p";
            case ResolutionType::HD_720P:   return "720p";
            case ResolutionType::NATIVE:
            default:                        return "Native";
        }
    }

    static const char* rateControlToString(RateControlMode rc) noexcept {
        switch (rc) {
            case RateControlMode::CONSTANT_CBR:     return "constant_cbr";
            case RateControlMode::CONSTANT_QUALITY: return "constant_quality";
            case RateControlMode::ADAPTIVE_VBR:
            default:                                return "adaptive_vbr";
        }
    }

    static RateControlMode rateControlFromString(std::string_view s) noexcept {
        if (s == "constant_cbr")     return RateControlMode::CONSTANT_CBR;
        if (s == "constant_quality") return RateControlMode::CONSTANT_QUALITY;
        return RateControlMode::ADAPTIVE_VBR;
    }

    ReplayConfig() = default;

    ReplayConfig(const ReplayConfig& other) {
        *this = other;
    }

    ReplayConfig& operator=(const ReplayConfig& other) {
        if (this == &other) return *this;
        std::scoped_lock lock(m_mutex, other.m_mutex);
        enabled = other.enabled;
        durationSeconds = other.durationSeconds;
        codec = other.codec;
        hardwareAcceleration = other.hardwareAcceleration;
        customCodec = other.customCodec;
        resolution = other.resolution;
        bitrateMbps = other.bitrateMbps;
        maxBitrateMbps = other.maxBitrateMbps;
        qualityLevel = other.qualityLevel;
        rateControl = other.rateControl;
        targetFps = other.targetFps;
        splitAudioTracks = other.splitAudioTracks;
        recordMic = other.recordMic;
        micDeviceId = other.micDeviceId;
        micDeviceName = other.micDeviceName;
        systemVolume = other.systemVolume;
        micVolume = other.micVolume;
        showNotification = other.showNotification;
        customSaveFolder = other.customSaveFolder;
        customTempFolder = other.customTempFolder;
        return *this;
    }

    // Returns a thread-safe value copy of the current configuration.
    ReplayConfig snapshot() const {
        return ReplayConfig(*this);
    }

    std::string getCustomCodec() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return customCodec;
    }
    void setCustomCodec(const std::string& val) {
        std::lock_guard<std::mutex> lock(m_mutex);
        customCodec = val;
    }

    std::string getMicDeviceId() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return micDeviceId;
    }
    std::string getMicDeviceName() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return micDeviceName;
    }
    void setMicDevice(const std::string& id, const std::string& name) {
        std::lock_guard<std::mutex> lock(m_mutex);
        micDeviceId = id;
        micDeviceName = name;
    }

    std::string getCustomSaveFolder() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return customSaveFolder;
    }
    void setCustomSaveFolder(const std::string& val) {
        std::lock_guard<std::mutex> lock(m_mutex);
        customSaveFolder = val;
    }

    std::string getCustomTempFolder() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return customTempFolder;
    }
    void setCustomTempFolder(const std::string& val) {
        std::lock_guard<std::mutex> lock(m_mutex);
        customTempFolder = val;
    }

    // Thread-safe: reads only atomic state, never touches CCEGLView on worker threads (#18).
    double getResolutionMultiplier() const noexcept {
        switch (resolution.load()) {
            case ResolutionType::HD_720P:   return 0.50;
            case ResolutionType::FHD_1080P: return 1.00;
            case ResolutionType::QHD_1440P: return 1.60;
            case ResolutionType::UHD_4K:    return 2.40;
            case ResolutionType::NATIVE: {
                int frameH = s_nativeFrameHeight.load(std::memory_order_relaxed);
                if (frameH > 0) {
                    double factor = static_cast<double>(frameH) / 1080.0;
                    return std::clamp(factor, 0.50, 2.50);
                }
                return 1.00;
            }
            default: return 1.00;
        }
    }

    double getFpsMultiplier() const noexcept {
        int fps = targetFps.load();
        if (fps <= 60) return 1.00;
        if (fps == 120) return 1.40;
        if (fps >= 240) return 1.95;
        return 1.00 + (fps - 60) * 0.0053;
    }

    double getCodecMultiplier() const noexcept {
        switch (codec.load()) {
            case CodecType::AV1: return 0.70;  // AV1: ~30% more efficient than H.264
            case CodecType::HEVC: return 0.85; // HEVC: ~15% more efficient than H.264
            case CodecType::H264: return 1.10; // H.264: standard baseline + packet overhead
            default: return 1.00;
        }
    }

    int getEffectiveBitrateMbps() const noexcept {
        RateControlMode rc = rateControl.load();
        int br = bitrateMbps.load();
        if (rc == RateControlMode::CONSTANT_CBR) {
            return std::clamp(br, 5, 250);
        }
        double mult = getResolutionMultiplier() * getFpsMultiplier() * getCodecMultiplier();
        if (rc == RateControlMode::CONSTANT_QUALITY) {
            double qMult = static_cast<double>(std::clamp(qualityLevel.load(), 20, 100)) / 80.0;
            int val = static_cast<int>(br * mult * qMult + 0.5);
            return std::clamp(val, 5, 250);
        }
        int val = static_cast<int>(br * mult + 0.5);
        return std::clamp(val, 5, 250);
    }

    int getEffectiveMaxBitrateMbps() const noexcept {
        if (rateControl.load() == RateControlMode::CONSTANT_CBR) {
            return getEffectiveBitrateMbps();
        }
        double mult = getResolutionMultiplier() * getFpsMultiplier() * getCodecMultiplier();
        int val = static_cast<int>(maxBitrateMbps.load() * mult + 0.5);
        return std::clamp(val, 10, 300);
    }

    // Cached GPU vendor string (populated on the main/GL thread).
    static const std::string& getGpuAccelName() {
        static std::string s_cached;
        if (!s_cached.empty()) {
            return s_cached;
        }
        const char* vendor = reinterpret_cast<const char*>(glGetString(GL_VENDOR));
        const char* renderer = reinterpret_cast<const char*>(glGetString(GL_RENDERER));
        if (!vendor && !renderer) {
            static const std::string kFallback = "GPU";
            return kFallback;
        }
        std::string v = vendor ? vendor : "";
        std::string r = renderer ? renderer : "";
        std::transform(v.begin(), v.end(), v.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        std::transform(r.begin(), r.end(), r.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

        if (v.find("nvidia") != std::string::npos || r.find("nvidia") != std::string::npos || r.find("geforce") != std::string::npos) {
            s_cached = "NVENC";
        } else if (v.find("amd") != std::string::npos || v.find("ati") != std::string::npos || r.find("radeon") != std::string::npos) {
            s_cached = "AMF";
        } else if (v.find("intel") != std::string::npos || r.find("intel") != std::string::npos) {
            s_cached = "QSV";
        } else {
            s_cached = "GPU";
        }
        return s_cached;
    }

    std::string getCodecDisplayName() const {
        CodecType c = codec.load();
        if (c == CodecType::CUSTOM) {
            std::string cc = getCustomCodec();
            return cc.empty() ? "Custom Codec" : ("Custom (" + cc + ")");
        }
        std::string name;
        switch (c) {
            case CodecType::AV1: name = "AV1"; break;
            case CodecType::HEVC: name = "HEVC"; break;
            case CodecType::H264:
            default: name = "H.264"; break;
        }

        if (hardwareAcceleration.load()) {
            return name + " (" + getGpuAccelName() + ")";
        }
        return name + " (Software)";
    }

    double estimateSizeMBForDuration(double durSeconds) const noexcept {
        double videoMbps = static_cast<double>(getEffectiveBitrateMbps());
        double audioMbps = 0.192; // Game audio AAC
        if (splitAudioTracks.load() && recordMic.load()) {
            audioMbps += 0.128; // Mic audio AAC
        }
        return ((videoMbps + audioMbps) * durSeconds) / 8.0;
    }

    double getEstimatedFileSizeMB() const noexcept {
        return estimateSizeMBForDuration(static_cast<double>(durationSeconds.load()));
    }

    // Rolling segment buffer in TEMP keeps up to duration + 60s of completed 30s segments
    // plus the active 30s segment being recorded before the next purge (duration + 90s peak).
    double getEstimatedTempBufferSizeMB() const noexcept {
        return estimateSizeMBForDuration(static_cast<double>(durationSeconds.load()) + 90.0);
    }

    static std::string formatSizeMB(double totalMB) {
        char buf[48];
        if (totalMB >= 1024.0) {
            snprintf(buf, sizeof(buf), "~%.2f GB", totalMB / 1024.0);
        } else {
            snprintf(buf, sizeof(buf), "~%d MB", static_cast<int>(totalMB + 0.5));
        }
        return std::string(buf);
    }

    std::string getEstimatedFileSizeText() const {
        return "Est. Size: " + formatSizeMB(getEstimatedFileSizeMB());
    }

    std::string getEstimatedFileSizeDetailedText() const {
        int dur = durationSeconds.load();
        int m = dur / 60;
        int s = dur % 60;
        const char* rName = resolutionDisplayName(resolution.load());
        std::string sizeStr = formatSizeMB(getEstimatedFileSizeMB());

        char buf[160];
        snprintf(buf, sizeof(buf), "Estimated File Size: %s (%s @ %d FPS, %d:%02d)",
                 sizeStr.c_str(), rName, targetFps.load(), m, s);
        return std::string(buf);
    }

    // Blocking filesystem query: call ONLY from worker / async tasks, never on the UI thread (#16).
    static uint64_t queryAvailableSpaceForPath(std::filesystem::path p) {
        std::error_code ec;
        while (!p.empty() && !std::filesystem::exists(p, ec)) {
            auto parent = p.parent_path();
            if (parent == p) break;
            p = std::move(parent);
        }
        if (p.empty() || !std::filesystem::exists(p, ec)) return 0;
        auto space = std::filesystem::space(p, ec);
        return ec ? 0 : space.available;
    }

    bool isLowDiskSpace(uint64_t* outFreeMB = nullptr, uint64_t* outNeededMB = nullptr,
                        bool* outIsTemp = nullptr, bool includeTemp = true) const {
        constexpr uint64_t kMinFloorBytes = 350ULL * 1024 * 1024;

        auto saveDir = resolveRecordingsDirectory(false, true);
        uint64_t saveAvail = queryAvailableSpaceForPath(saveDir);
        uint64_t saveNeeded = static_cast<uint64_t>(getEstimatedFileSizeMB() * 1024.0 * 1024.0);
        bool saveLow = (saveAvail > 0) && (saveAvail < saveNeeded || saveAvail < kMinFloorBytes);

        if (includeTemp) {
            auto tempDir = resolveTempDirectory(false, true);
            uint64_t tempAvail = queryAvailableSpaceForPath(tempDir);
            uint64_t tempNeeded = static_cast<uint64_t>(getEstimatedTempBufferSizeMB() * 1024.0 * 1024.0);
            bool tempLow = (tempAvail > 0) && (tempAvail < tempNeeded || tempAvail < kMinFloorBytes);
            if (tempLow) {
                if (outFreeMB) *outFreeMB = tempAvail / (1024 * 1024);
                if (outNeededMB) *outNeededMB = std::max<uint64_t>(350, tempNeeded / (1024 * 1024));
                if (outIsTemp) *outIsTemp = true;
                return true;
            }
        }
        if (saveLow) {
            if (outFreeMB) *outFreeMB = saveAvail / (1024 * 1024);
            if (outNeededMB) *outNeededMB = std::max<uint64_t>(350, saveNeeded / (1024 * 1024));
            if (outIsTemp) *outIsTemp = false;
            return true;
        }
        return false;
    }

    static bool isH264Conflict(ResolutionType res, int fps) noexcept {
        return (res == ResolutionType::UHD_4K && fps >= 240);
    }

    bool sanitizeCodecSettings() noexcept {
        if (codec.load() == CodecType::H264 && isH264Conflict(resolution.load(), targetFps.load())) {
            targetFps = 120;
            return true;
        }
        return false;
    }

    static inline std::atomic<bool> s_isSavingSettings{false};

    static ReplayConfig& get() {
        static ReplayConfig instance;
        return instance;
    }

    void loadFromSettings() {
        auto mod = geode::Mod::get();
        if (!mod) return;

        enabled = mod->getSettingValue<bool>("enabled");
        durationSeconds = std::clamp(static_cast<int>(mod->getSettingValue<int64_t>("duration")), 5, 3600);

        std::string codecStr = mod->getSettingValue<std::string>("codec");
        std::string loadedCustomCodec = mod->getSettingValue<std::string>("custom-codec");
        std::string loadedSaveFolder = mod->getSettingValue<std::string>("save-folder");
        std::string loadedTempFolder = mod->getSettingValue<std::string>("temp-folder");

        hardwareAcceleration = mod->getSettingValue<bool>("hardware-accel");
        codec = codecFromString(codecStr);
        resolution = resolutionFromString(mod->getSettingValue<std::string>("resolution"));
        rateControl = rateControlFromString(mod->getSettingValue<std::string>("rate-control"));

        bitrateMbps = std::clamp(static_cast<int>(mod->getSettingValue<int64_t>("bitrate")), 5, 250);
        maxBitrateMbps = std::clamp(static_cast<int>(mod->getSettingValue<int64_t>("max-bitrate")), 10, 300);
        qualityLevel = std::clamp(static_cast<int>(mod->getSettingValue<int64_t>("quality-level")), 20, 100);
        targetFps = std::clamp(static_cast<int>(mod->getSettingValue<int64_t>("fps")), 30, 240);

        bool sanitized = sanitizeCodecSettings();

        splitAudioTracks = mod->getSettingValue<bool>("split-audio");
        recordMic = mod->getSettingValue<bool>("record-mic");

        std::string loadedMicId = mod->getSettingValue<std::string>("mic-device-id");
        std::string loadedMicName = mod->getSettingValue<std::string>("mic-device-name");
        if (loadedMicId.empty()) loadedMicId = "default";
        if (loadedMicName.empty()) loadedMicName = "Default System Device";

        {
            std::lock_guard<std::mutex> lock(m_mutex);
            customCodec = std::move(loadedCustomCodec);
            customSaveFolder = std::move(loadedSaveFolder);
            customTempFolder = std::move(loadedTempFolder);
            micDeviceId = std::move(loadedMicId);
            micDeviceName = std::move(loadedMicName);
        }

        systemVolume = std::clamp(static_cast<float>(mod->getSettingValue<double>("system-volume")), 0.0f, 2.0f);
        micVolume = std::clamp(static_cast<float>(mod->getSettingValue<double>("mic-volume")), 0.0f, 2.0f);
        showNotification = mod->getSettingValue<bool>("show-notification");

        if (sanitized) {
            s_isSavingSettings.store(true);
            setClampedSetting<geode::IntSettingV3>(mod, "fps", static_cast<int64_t>(targetFps.load()));
            s_isSavingSettings.store(false);
        }
    }

    template <typename SettingT, typename ValT>
    static void setClampedSetting(geode::Mod* mod, std::string_view key, ValT val) {
        if (auto sett = mod->getSetting(key)) {
            if (auto typed = geode::cast::typeinfo_pointer_cast<SettingT>(sett)) {
                if (auto minV = typed->getMinValue()) val = std::max<ValT>(val, *minV);
                if (auto maxV = typed->getMaxValue()) val = std::min<ValT>(val, *maxV);
            }
        }
        (void)mod->setSettingValue(key, val);
    }

    // Updates only the volume settings in memory without rewriting all 20 settings or blocking on disk I/O.
    void saveVolumeSettings() {
        auto mod = geode::Mod::get();
        if (!mod) return;
        s_isSavingSettings.store(true);
        setClampedSetting<geode::FloatSettingV3>(mod, "system-volume", static_cast<double>(systemVolume.load()));
        setClampedSetting<geode::FloatSettingV3>(mod, "mic-volume", static_cast<double>(micVolume.load()));
        s_isSavingSettings.store(false);
    }

    void saveToSettings(bool flushToDisk = false) {
        auto mod = geode::Mod::get();
        if (!mod) return;

        s_isSavingSettings.store(true);

        std::string curCustomCodec;
        std::string curSaveFolder;
        std::string curTempFolder;
        std::string curMicId;
        std::string curMicName;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            curCustomCodec = customCodec;
            curSaveFolder = customSaveFolder;
            curTempFolder = customTempFolder;
            curMicId = micDeviceId;
            curMicName = micDeviceName;
        }

        (void)mod->setSettingValue("enabled", enabled.load());
        setClampedSetting<geode::IntSettingV3>(mod, "duration", static_cast<int64_t>(durationSeconds.load()));
        (void)mod->setSettingValue("hardware-accel", hardwareAcceleration.load());
        (void)mod->setSettingValue("codec", std::string(codecToString(codec.load())));
        (void)mod->setSettingValue("custom-codec", curCustomCodec);
        (void)mod->setSettingValue("save-folder", curSaveFolder);
        (void)mod->setSettingValue("temp-folder", curTempFolder);
        (void)mod->setSettingValue("resolution", std::string(resolutionToString(resolution.load())));
        (void)mod->setSettingValue("rate-control", std::string(rateControlToString(rateControl.load())));

        setClampedSetting<geode::IntSettingV3>(mod, "bitrate", static_cast<int64_t>(bitrateMbps.load()));
        setClampedSetting<geode::IntSettingV3>(mod, "max-bitrate", static_cast<int64_t>(maxBitrateMbps.load()));
        setClampedSetting<geode::IntSettingV3>(mod, "quality-level", static_cast<int64_t>(qualityLevel.load()));
        setClampedSetting<geode::IntSettingV3>(mod, "fps", static_cast<int64_t>(targetFps.load()));
        (void)mod->setSettingValue("split-audio", splitAudioTracks.load());
        (void)mod->setSettingValue("record-mic", recordMic.load());
        (void)mod->setSettingValue("mic-device-id", curMicId);
        (void)mod->setSettingValue("mic-device-name", curMicName);
        setClampedSetting<geode::FloatSettingV3>(mod, "system-volume", static_cast<double>(systemVolume.load()));
        setClampedSetting<geode::FloatSettingV3>(mod, "mic-volume", static_cast<double>(micVolume.load()));
        (void)mod->setSettingValue("show-notification", showNotification.load());

        if (flushToDisk) {
            (void)mod->saveData();
        }

        s_isSavingSettings.store(false);
    }

    // Returns the encoded video size for a given game framebuffer size.
    // Presets define the output HEIGHT; width follows the window aspect ratio
    // (no stretching on 16:10 / 21:9 monitors). Always even (NV12 requirement).
    std::pair<int, int> getResolutionDimensions(int nativeW, int nativeH) const noexcept {
        if (nativeW <= 0 || nativeH <= 0) return {1920, 1080};

        int targetH = 0;
        switch (resolution.load()) {
            case ResolutionType::UHD_4K:    targetH = 2160; break;
            case ResolutionType::QHD_1440P: targetH = 1440; break;
            case ResolutionType::FHD_1080P: targetH = 1080; break;
            case ResolutionType::HD_720P:   targetH = 720;  break;
            case ResolutionType::NATIVE:
            default:                        targetH = nativeH; break;
        }

        int w = nativeW;
        int h = nativeH;
        if (targetH != nativeH) {
            h = targetH;
            w = static_cast<int>(static_cast<double>(nativeW) * targetH / nativeH + 0.5);
        }
        w &= ~1;
        h &= ~1;
        return {std::max(64, w), std::max(64, h)};
    }

    // Pure lexical normalization: strips surrounding whitespace/quotes, validates syntax,
    // removes trailing separators so `.filename()` is never empty on `"D:\\Temp\\"` (#23).
    static std::filesystem::path normalizeCustomDirPath(const std::string& rawUtf8) {
        size_t first = 0;
        size_t last = rawUtf8.size();
        while (first < last && (std::isspace(static_cast<unsigned char>(rawUtf8[first])) || rawUtf8[first] == '"')) {
            ++first;
        }
        while (last > first && (std::isspace(static_cast<unsigned char>(rawUtf8[last - 1])) || rawUtf8[last - 1] == '"')) {
            --last;
        }
        if (first >= last) return {};

        std::wstring wide = geode::utils::string::utf8ToWide(rawUtf8.substr(first, last - first));
        if (wide.empty()) return {};

        std::filesystem::path p(wide);
        if (!p.is_absolute() || !p.has_root_name() || !p.has_root_directory()) {
            return {};
        }

        p = p.lexically_normal();
        while (!p.has_filename() && p.parent_path() != p) {
            p = p.parent_path();
        }

        // Validate non-root path components against forbidden Win32 filename characters
        std::filesystem::path rel = p.relative_path();
        for (const auto& comp : rel) {
            const auto& ws = comp.native();
            if (ws.empty()) continue;
            for (wchar_t wc : ws) {
                if (wc < 32 || wc == L'<' || wc == L'>' || wc == L':' ||
                    wc == L'"' || wc == L'|' || wc == L'?' || wc == L'*') {
                    return {};
                }
            }
        }
        return p;
    }

    // When createIfMissing == false, performs zero disk I/O so UI callers never block (#16).
    std::filesystem::path getRecordingsDirectory(bool createIfMissing = false) const {
        return resolveRecordingsDirectory(createIfMissing, createIfMissing);
    }

    std::filesystem::path getTempDirectory(bool createIfMissing = false) const {
        return resolveTempDirectory(createIfMissing, createIfMissing);
    }

    // Finds the nearest existing directory on disk for folder-picker initial path without creating folders (#11).
    std::filesystem::path getExistingRecordingsPickerPath() const {
        std::filesystem::path p = resolveRecordingsDirectory(false, true);
        std::error_code ec;
        while (!p.empty() && !std::filesystem::is_directory(p, ec)) {
            auto parent = p.parent_path();
            if (parent == p) break;
            p = std::move(parent);
        }
        return p;
    }

private:
    static const std::filesystem::path& cachedDefaultVideosDir() {
        static const std::filesystem::path s_videosDir = []() -> std::filesystem::path {
            PWSTR pszVideos = nullptr;
            HRESULT hr = SHGetKnownFolderPath(FOLDERID_Videos, 0, nullptr, &pszVideos);
            std::filesystem::path result;
            if (SUCCEEDED(hr) && pszVideos) {
                result = std::filesystem::path(pszVideos) / L"Geometry Dash";
            }
            if (pszVideos) {
                CoTaskMemFree(pszVideos);
            }
            return result;
        }();
        return s_videosDir;
    }

    static const std::filesystem::path& cachedDefaultTempDir() {
        static const std::filesystem::path s_tempDir = []() -> std::filesystem::path {
            wchar_t tempBuf[MAX_PATH + 1] = {};
            DWORD len = GetTempPathW(MAX_PATH, tempBuf);
            if (len > 0 && len <= MAX_PATH) {
                return std::filesystem::path(tempBuf) / L"GeometryDash_InstantReplay";
            }
            return {};
        }();
        return s_tempDir;
    }

    static bool isPathCreatableOrExistingDir(const std::filesystem::path& dir, bool createIfMissing) {
        if (dir.empty()) return false;
        std::error_code ec;
        if (std::filesystem::is_directory(dir, ec)) {
            return true;
        }
        if (std::filesystem::exists(dir, ec)) {
            // Exists on disk, but is NOT a directory (e.g. regular file) (#11, #22)
            return false;
        }
        if (createIfMissing) {
            std::filesystem::create_directories(dir, ec);
            return !ec && std::filesystem::is_directory(dir, ec);
        }
        // Verify root drive exists and nearest existing ancestor is a directory, not a file
        auto cur = dir;
        while (!cur.empty() && !std::filesystem::exists(cur, ec)) {
            auto parent = cur.parent_path();
            if (parent == cur) return false; // Root volume itself does not exist (e.g. Z:\)
            cur = std::move(parent);
        }
        return !cur.empty() && std::filesystem::is_directory(cur, ec);
    }

    std::filesystem::path resolveRecordingsDirectory(bool createIfMissing, bool verifyOnDisk) const {
        std::filesystem::path customPath = normalizeCustomDirPath(getCustomSaveFolder());
        if (!customPath.empty()) {
            if (!verifyOnDisk || isPathCreatableOrExistingDir(customPath, createIfMissing)) {
                return customPath;
            }
        }

        const auto& videosDir = cachedDefaultVideosDir();
        if (!videosDir.empty()) {
            if (!verifyOnDisk || isPathCreatableOrExistingDir(videosDir, createIfMissing)) {
                return videosDir;
            }
        }

        std::filesystem::path fallback = geode::dirs::getGameDir() / L"Replays";
        if (createIfMissing) {
            std::error_code ec;
            std::filesystem::create_directories(fallback, ec);
        }
        return fallback;
    }

    std::filesystem::path resolveTempDirectory(bool createIfMissing, bool verifyOnDisk) const {
        std::filesystem::path customPath = normalizeCustomDirPath(getCustomTempFolder());
        if (!customPath.empty()) {
            auto fname = customPath.filename().wstring();
            if (fname != L"GeometryDash_InstantReplay" && fname != L"InstantReplay") {
                customPath /= L"GeometryDash_InstantReplay";
            }
            if (!verifyOnDisk || isPathCreatableOrExistingDir(customPath, createIfMissing)) {
                return customPath;
            }
        }

        const auto& sysTempDir = cachedDefaultTempDir();
        if (!sysTempDir.empty()) {
            if (!verifyOnDisk || isPathCreatableOrExistingDir(sysTempDir, createIfMissing)) {
                return sysTempDir;
            }
        }

        std::filesystem::path fallback = geode::dirs::getTempDir() / L"InstantReplay";
        if (createIfMissing) {
            std::error_code ec;
            std::filesystem::create_directories(fallback, ec);
        }
        return fallback;
    }

    mutable std::mutex m_mutex;
};

} // namespace replay

#endif // GEODE_IS_WINDOWS
