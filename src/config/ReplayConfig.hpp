#pragma once

#include <Geode/Geode.hpp>

#ifdef GEODE_IS_WINDOWS

#include <Geode/loader/SettingV3.hpp>
#include <Geode/utils/string.hpp>
#include <windows.h>
#include <shlobj.h>
#include <string>
#include <filesystem>
#include <chrono>
#include <atomic>
#include <mutex>
#include <algorithm>

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

    double getResolutionMultiplier() const {
        switch (resolution.load()) {
            case ResolutionType::HD_720P:   return 0.50;
            case ResolutionType::FHD_1080P: return 1.00;
            case ResolutionType::QHD_1440P: return 1.60;
            case ResolutionType::UHD_4K:    return 2.40;
            case ResolutionType::NATIVE: {
                int frameH = 0;
                if (auto view = cocos2d::CCEGLView::get()) {
                    auto fs = view->getFrameSize();
                    if (fs.height > 0.0f) {
                        frameH = static_cast<int>(fs.height + 0.5f);
                        s_nativeFrameHeight.store(frameH, std::memory_order_relaxed);
                    }
                }
                if (frameH <= 0) {
                    frameH = s_nativeFrameHeight.load(std::memory_order_relaxed);
                }
                if (frameH > 0) {
                    double factor = static_cast<double>(frameH) / 1080.0;
                    return std::clamp(factor, 0.50, 2.50);
                }
                return 1.00;
            }
            default: return 1.00;
        }
    }

    double getFpsMultiplier() const {
        int fps = targetFps.load();
        if (fps <= 60) return 1.00;
        if (fps == 120) return 1.40;
        if (fps >= 240) return 1.95;
        return 1.00 + (fps - 60) * 0.0053;
    }

    double getCodecMultiplier() const {
        switch (codec.load()) {
            case CodecType::AV1: return 0.70;  // AV1: ~30% more efficient than H.264
            case CodecType::HEVC: return 0.85; // HEVC: ~15% more efficient than H.264
            case CodecType::H264: return 1.10; // H.264: standard baseline + packet overhead
            default: return 1.00;
        }
    }

    int getEffectiveBitrateMbps() const {
        RateControlMode rc = rateControl.load();
        int br = bitrateMbps.load();
        if (rc == RateControlMode::CONSTANT_CBR) {
            return std::clamp(br, 2, 250);
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

    int getEffectiveMaxBitrateMbps() const {
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
        } else {
            return name + " (Software)";
        }
    }

    double getEstimatedFileSizeMB() const {
        double dur = static_cast<double>(durationSeconds.load());
        double videoMbps = static_cast<double>(getEffectiveBitrateMbps());
        double audioMbps = 0.192; // Game audio AAC
        if (splitAudioTracks.load() && recordMic.load()) {
            audioMbps += 0.128; // Mic audio AAC
        }
        double totalMbps = videoMbps + audioMbps;
        return (totalMbps * dur) / 8.0;
    }

    // Rolling segment buffer in TEMP keeps up to duration + 60s of completed 30s segments
    // plus the active 30s segment being recorded before the next purge (duration + 90s peak).
    double getEstimatedTempBufferSizeMB() const {
        double dur = static_cast<double>(durationSeconds.load()) + 90.0;
        double videoMbps = static_cast<double>(getEffectiveBitrateMbps());
        double audioMbps = 0.192;
        if (splitAudioTracks.load() && recordMic.load()) {
            audioMbps += 0.128;
        }
        return ((videoMbps + audioMbps) * dur) / 8.0;
    }

    static uint64_t queryAvailableSpaceForPath(std::filesystem::path p) {
        std::error_code ec;
        while (!p.empty() && !std::filesystem::exists(p, ec)) {
            auto parent = p.parent_path();
            if (parent == p) break;
            p = std::move(parent);
        }
        if (p.empty()) return 0;
        auto space = std::filesystem::space(p, ec);
        return ec ? 0 : space.available;
    }

    uint64_t getAvailableSaveDiskSpaceBytes() const {
        return queryAvailableSpaceForPath(getRecordingsDirectory(false));
    }

    uint64_t getAvailableTempDiskSpaceBytes() const {
        return queryAvailableSpaceForPath(getTempDirectory(false));
    }

    bool isLowDiskSpace(uint64_t* outFreeMB = nullptr, uint64_t* outNeededMB = nullptr, bool* outIsTemp = nullptr) const {
        constexpr uint64_t kMinFloorBytes = 350ULL * 1024 * 1024;

        uint64_t saveAvail = getAvailableSaveDiskSpaceBytes();
        uint64_t saveNeeded = static_cast<uint64_t>(getEstimatedFileSizeMB() * 1024.0 * 1024.0);
        bool saveLow = (saveAvail > 0) && (saveAvail < saveNeeded || saveAvail < kMinFloorBytes);

        uint64_t tempAvail = getAvailableTempDiskSpaceBytes();
        uint64_t tempNeeded = static_cast<uint64_t>(getEstimatedTempBufferSizeMB() * 1024.0 * 1024.0);
        bool tempLow = (tempAvail > 0) && (tempAvail < tempNeeded || tempAvail < kMinFloorBytes);

        if (tempLow) {
            if (outFreeMB) *outFreeMB = tempAvail / (1024 * 1024);
            if (outNeededMB) *outNeededMB = std::max<uint64_t>(350, tempNeeded / (1024 * 1024));
            if (outIsTemp) *outIsTemp = true;
            return true;
        }
        if (saveLow) {
            if (outFreeMB) *outFreeMB = saveAvail / (1024 * 1024);
            if (outNeededMB) *outNeededMB = std::max<uint64_t>(350, saveNeeded / (1024 * 1024));
            if (outIsTemp) *outIsTemp = false;
            return true;
        }
        return false;
    }

    std::string getEstimatedFileSizeText() const {
        double totalMB = getEstimatedFileSizeMB();
        char buf[80];
        if (totalMB >= 1024.0) {
            snprintf(buf, sizeof(buf), "Est. Size: ~%.2f GB", totalMB / 1024.0);
        } else {
            snprintf(buf, sizeof(buf), "Est. Size: ~%d MB", static_cast<int>(totalMB + 0.5));
        }
        return std::string(buf);
    }

    std::string getEstimatedFileSizeDetailedText() const {
        double totalMB = getEstimatedFileSizeMB();
        int dur = durationSeconds.load();
        int m = dur / 60;
        int s = dur % 60;

        const char* rName = resolutionDisplayName(resolution.load());

        char buf[160];
        if (totalMB >= 1024.0) {
            snprintf(buf, sizeof(buf), "Estimated File Size: ~%.2f GB (%s @ %d FPS, %d:%02d)", totalMB / 1024.0, rName, targetFps.load(), m, s);
        } else {
            snprintf(buf, sizeof(buf), "Estimated File Size: ~%d MB (%s @ %d FPS, %d:%02d)", static_cast<int>(totalMB + 0.5), rName, targetFps.load(), m, s);
        }
        return std::string(buf);
    }

    static bool isH264Conflict(ResolutionType res, int fps) {
        return (res == ResolutionType::UHD_4K && fps >= 240);
    }

    void sanitizeCodecSettings() {
        if (codec.load() == CodecType::H264) {
            if (isH264Conflict(resolution.load(), targetFps.load())) {
                targetFps = 120;
            }
        }
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
        std::string loadedTempFolder = mod->hasSetting("temp-folder")
            ? mod->getSettingValue<std::string>("temp-folder")
            : "";

        if (mod->hasSetting("hardware-accel")) {
            hardwareAcceleration = mod->getSettingValue<bool>("hardware-accel");
        }

        codec = codecFromString(codecStr);
        resolution = resolutionFromString(mod->getSettingValue<std::string>("resolution"));
        rateControl = rateControlFromString(mod->getSettingValue<std::string>("rate-control"));

        bitrateMbps = std::clamp(static_cast<int>(mod->getSettingValue<int64_t>("bitrate")), 5, 250);
        maxBitrateMbps = std::clamp(static_cast<int>(mod->getSettingValue<int64_t>("max-bitrate")), 10, 300);
        qualityLevel = std::clamp(static_cast<int>(mod->getSettingValue<int64_t>("quality-level")), 20, 100);
        targetFps = std::clamp(static_cast<int>(mod->getSettingValue<int64_t>("fps")), 30, 240);

        sanitizeCodecSettings();

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
    }

    static void setClampedInt(geode::Mod* mod, std::string_view key, int64_t val) {
        if (auto sett = mod->getSetting(key)) {
            if (auto intSett = geode::cast::typeinfo_pointer_cast<geode::IntSettingV3>(sett)) {
                if (auto minV = intSett->getMinValue()) val = std::max(val, *minV);
                if (auto maxV = intSett->getMaxValue()) val = std::min(val, *maxV);
            }
        }
        (void)mod->setSettingValue(key, val);
    }

    static void setClampedFloat(geode::Mod* mod, std::string_view key, double val) {
        if (auto sett = mod->getSetting(key)) {
            if (auto fltSett = geode::cast::typeinfo_pointer_cast<geode::FloatSettingV3>(sett)) {
                if (auto minV = fltSett->getMinValue()) val = std::max(val, *minV);
                if (auto maxV = fltSett->getMaxValue()) val = std::min(val, *maxV);
            }
        }
        (void)mod->setSettingValue(key, val);
    }

    // Updates only the volume settings in memory without rewriting all 20 settings or blocking on disk I/O.
    void saveVolumeSettings() {
        auto mod = geode::Mod::get();
        if (!mod) return;
        s_isSavingSettings.store(true);
        setClampedFloat(mod, "system-volume", static_cast<double>(systemVolume.load()));
        setClampedFloat(mod, "mic-volume", static_cast<double>(micVolume.load()));
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
        setClampedInt(mod, "duration", static_cast<int64_t>(durationSeconds.load()));
        (void)mod->setSettingValue("hardware-accel", hardwareAcceleration.load());
        (void)mod->setSettingValue("codec", std::string(codecToString(codec.load())));
        (void)mod->setSettingValue("custom-codec", curCustomCodec);
        (void)mod->setSettingValue("save-folder", curSaveFolder);
        if (mod->hasSetting("temp-folder")) {
            (void)mod->setSettingValue("temp-folder", curTempFolder);
        }
        (void)mod->setSettingValue("resolution", std::string(resolutionToString(resolution.load())));
        (void)mod->setSettingValue("rate-control", std::string(rateControlToString(rateControl.load())));

        setClampedInt(mod, "bitrate", static_cast<int64_t>(bitrateMbps.load()));
        setClampedInt(mod, "max-bitrate", static_cast<int64_t>(maxBitrateMbps.load()));
        setClampedInt(mod, "quality-level", static_cast<int64_t>(qualityLevel.load()));
        setClampedInt(mod, "fps", static_cast<int64_t>(targetFps.load()));
        (void)mod->setSettingValue("split-audio", splitAudioTracks.load());
        (void)mod->setSettingValue("record-mic", recordMic.load());
        (void)mod->setSettingValue("mic-device-id", curMicId);
        (void)mod->setSettingValue("mic-device-name", curMicName);
        setClampedFloat(mod, "system-volume", static_cast<double>(systemVolume.load()));
        setClampedFloat(mod, "mic-volume", static_cast<double>(micVolume.load()));
        (void)mod->setSettingValue("show-notification", showNotification.load());

        if (flushToDisk) {
            (void)mod->saveData();
        }

        s_isSavingSettings.store(false);
    }

    // Returns the encoded video size for a given game framebuffer size.
    // Presets define the output HEIGHT; width follows the window aspect ratio
    // (no stretching on 16:10 / 21:9 monitors). Always even (NV12 requirement).
    std::pair<int, int> getResolutionDimensions(int nativeW, int nativeH) const {
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

    std::filesystem::path getRecordingsDirectory(bool createIfMissing = false) const {
        std::string saveFolderStr = getCustomSaveFolder();
        if (!saveFolderStr.empty()) {
            std::error_code ec;
            std::filesystem::path customPath(geode::utils::string::utf8ToWide(saveFolderStr));
            if (customPath.is_absolute()) {
                if (createIfMissing && !std::filesystem::exists(customPath, ec)) {
                    std::filesystem::create_directories(customPath, ec);
                }
                if (!createIfMissing || std::filesystem::exists(customPath, ec)) {
                    return customPath;
                }
            }
        }

        PWSTR pszVideos = nullptr;
        HRESULT hr = SHGetKnownFolderPath(FOLDERID_Videos, 0, nullptr, &pszVideos);
        if (SUCCEEDED(hr) && pszVideos) {
            std::filesystem::path dir = std::filesystem::path(pszVideos) / L"Geometry Dash";
            CoTaskMemFree(pszVideos);
            std::error_code ec;
            if (createIfMissing) {
                std::filesystem::create_directories(dir, ec);
            }
            if (!createIfMissing || (!ec && std::filesystem::exists(dir, ec))) {
                return dir;
            }
        } else if (pszVideos) {
            CoTaskMemFree(pszVideos);
        }

        std::filesystem::path dir = geode::dirs::getGameDir() / L"Replays";
        if (createIfMissing) {
            std::error_code ec;
            std::filesystem::create_directories(dir, ec);
        }
        return dir;
    }

    std::filesystem::path getTempDirectory(bool createIfMissing = false) const {
        std::string tempFolderStr = getCustomTempFolder();
        if (!tempFolderStr.empty()) {
            std::error_code ec;
            std::filesystem::path customPath(geode::utils::string::utf8ToWide(tempFolderStr));
            if (customPath.is_absolute()) {
                auto fname = customPath.filename().wstring();
                if (fname != L"GeometryDash_InstantReplay" && fname != L"InstantReplay") {
                    customPath /= L"GeometryDash_InstantReplay";
                }
                if (createIfMissing && !std::filesystem::exists(customPath, ec)) {
                    std::filesystem::create_directories(customPath, ec);
                }
                if (!createIfMissing || std::filesystem::exists(customPath, ec)) {
                    return customPath;
                }
            }
        }

        wchar_t tempBuf[MAX_PATH + 1] = {};
        DWORD len = GetTempPathW(MAX_PATH, tempBuf);
        if (len > 0 && len <= MAX_PATH) {
            std::filesystem::path dir = std::filesystem::path(tempBuf) / L"GeometryDash_InstantReplay";
            if (createIfMissing) {
                std::error_code ec;
                std::filesystem::create_directories(dir, ec);
            }
            return dir;
        }

        std::filesystem::path fallback = geode::dirs::getTempDir() / L"InstantReplay";
        if (createIfMissing) {
            std::error_code ec;
            std::filesystem::create_directories(fallback, ec);
        }
        return fallback;
    }

private:
    mutable std::mutex m_mutex;
};

} // namespace replay

#endif // GEODE_IS_WINDOWS
