#pragma once

#include <Geode/Geode.hpp>
#include <string>
#include <filesystem>
#include <chrono>

namespace replay {

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

struct ReplayConfig {
    bool enabled = true;
    int durationSeconds = 120; // 2 minutes
    CodecType codec = CodecType::AV1;
    bool hardwareAcceleration = true; // Auto GPU hardware acceleration (NVENC, AMF, QSV)
    std::string customCodec = ""; // e.g. "av1_nvenc", "amf_av1", "qsv_av1", "hevc"
    ResolutionType resolution = ResolutionType::NATIVE;
    int bitrateMbps = 35;
    int maxBitrateMbps = 60;
    int qualityLevel = 80;
    RateControlMode rateControl = RateControlMode::ADAPTIVE_VBR;
    int targetFps = 60;
    
    // Audio settings
    bool splitAudioTracks = true; // Separate track 1 (Game) & track 2 (Mic)
    bool recordMic = true;
    std::string micDeviceId = "default";
    std::string micDeviceName = "Default System Device";
    float systemVolume = 1.0f;
    float micVolume = 1.0f;
    
    // UI settings
    bool showNotification = true;
    bool showOverlayIcon = true;
    std::string customSaveFolder = "";

    double getResolutionMultiplier() const {
        switch (resolution) {
            case ResolutionType::HD_720P:   return 0.50;
            case ResolutionType::FHD_1080P: return 1.00;
            case ResolutionType::QHD_1440P: return 1.60;
            case ResolutionType::UHD_4K:    return 2.40;
            case ResolutionType::NATIVE: {
                if (auto director = cocos2d::CCDirector::sharedDirector()) {
                    auto winSize = director->getWinSizeInPixels();
                    if (winSize.height > 0.0f) {
                        double factor = static_cast<double>(winSize.height) / 1080.0;
                        return std::clamp(factor, 0.50, 2.50);
                    }
                }
                return 1.00;
            }
            default: return 1.00;
        }
    }

    double getFpsMultiplier() const {
        if (targetFps <= 60) return 1.00;
        if (targetFps == 120) return 1.40;
        if (targetFps >= 240) return 1.95;
        return 1.00 + (targetFps - 60) * 0.0053;
    }

    double getCodecMultiplier() const {
        switch (codec) {
            case CodecType::AV1: return 0.70;  // AV1: ~30% more efficient than H.264
            case CodecType::HEVC: return 0.85; // HEVC: ~15% more efficient than H.264
            case CodecType::H264: return 1.10; // H.264: standard baseline + packet overhead
            default: return 1.00;
        }
    }

    int getEffectiveBitrateMbps() const {
        if (rateControl == RateControlMode::CONSTANT_CBR) {
            return std::clamp(bitrateMbps, 2, 250);
        }
        double mult = getResolutionMultiplier() * getFpsMultiplier() * getCodecMultiplier();
        int val = static_cast<int>(bitrateMbps * mult + 0.5);
        return std::clamp(val, 5, 250);
    }

    int getEffectiveMaxBitrateMbps() const {
        if (rateControl == RateControlMode::CONSTANT_CBR) {
            return getEffectiveBitrateMbps();
        }
        double mult = getResolutionMultiplier() * getFpsMultiplier() * getCodecMultiplier();
        int val = static_cast<int>(maxBitrateMbps * mult + 0.5);
        return std::clamp(val, 10, 300);
    }

    static std::string getGpuAccelName() {
        const char* vendor = reinterpret_cast<const char*>(glGetString(GL_VENDOR));
        const char* renderer = reinterpret_cast<const char*>(glGetString(GL_RENDERER));
        std::string v = vendor ? vendor : "";
        std::string r = renderer ? renderer : "";
        std::transform(v.begin(), v.end(), v.begin(), ::tolower);
        std::transform(r.begin(), r.end(), r.begin(), ::tolower);

        if (v.find("nvidia") != std::string::npos || r.find("nvidia") != std::string::npos || r.find("geforce") != std::string::npos) {
            return "NVENC";
        }
        if (v.find("amd") != std::string::npos || v.find("ati") != std::string::npos || r.find("radeon") != std::string::npos) {
            return "AMF";
        }
        if (v.find("intel") != std::string::npos || r.find("intel") != std::string::npos) {
            return "QSV";
        }
        return "GPU";
    }

    std::string getCodecDisplayName() const {
        if (codec == CodecType::CUSTOM) {
            return customCodec.empty() ? "Custom Codec" : ("Custom (" + customCodec + ")");
        }
        std::string name;
        switch (codec) {
            case CodecType::AV1: name = "AV1"; break;
            case CodecType::HEVC: name = "HEVC"; break;
            case CodecType::H264:
            default: name = "H.264"; break;
        }

        if (hardwareAcceleration) {
            return name + " (" + getGpuAccelName() + ")";
        } else {
            return name + " (Software)";
        }
    }

    double getEstimatedFileSizeMB() const {
        double dur = static_cast<double>(durationSeconds);
        double videoMbps = static_cast<double>(getEffectiveBitrateMbps());
        double audioMbps = 0.192; // Game audio AAC
        if (splitAudioTracks && recordMic) {
            audioMbps += 0.128; // Mic audio AAC
        }
        double totalMbps = videoMbps + audioMbps;
        return (totalMbps * dur) / 8.0;
    }

    uint64_t getAvailableDiskSpaceBytes() const {
        std::error_code ec;
        auto space = std::filesystem::space(getRecordingsDirectory(), ec);
        return ec ? 0 : space.available;
    }

    bool isLowDiskSpace() const {
        uint64_t avail = getAvailableDiskSpaceBytes();
        if (avail == 0) return false;
        double estMB = getEstimatedFileSizeMB();
        uint64_t estBytes = static_cast<uint64_t>(estMB * 1024.0 * 1024.0);
        return (avail < estBytes) || (avail < (350ULL * 1024 * 1024));
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
        int m = durationSeconds / 60;
        int s = durationSeconds % 60;

        const char* rName = "Native";
        if (resolution == ResolutionType::UHD_4K) rName = "4K";
        else if (resolution == ResolutionType::QHD_1440P) rName = "1440p";
        else if (resolution == ResolutionType::FHD_1080P) rName = "1080p";
        else if (resolution == ResolutionType::HD_720P) rName = "720p";

        char buf[160];
        if (totalMB >= 1024.0) {
            snprintf(buf, sizeof(buf), "Estimated File Size: ~%.2f GB (%s @ %d FPS, %d:%02d)", totalMB / 1024.0, rName, targetFps, m, s);
        } else {
            snprintf(buf, sizeof(buf), "Estimated File Size: ~%d MB (%s @ %d FPS, %d:%02d)", static_cast<int>(totalMB + 0.5), rName, targetFps, m, s);
        }
        return std::string(buf);
    }

    static bool isH264Conflict(ResolutionType res, int fps) {
        return (res == ResolutionType::UHD_4K && fps >= 240);
    }

    void sanitizeCodecSettings() {
        if (codec == CodecType::H264) {
            if (isH264Conflict(resolution, targetFps)) {
                targetFps = 120;
            }
        }
    }

    static inline bool s_isSavingSettings = false;

    static ReplayConfig& get() {
        static ReplayConfig instance;
        return instance;
    }

    void loadFromSettings() {
        auto mod = geode::Mod::get();
        if (!mod) return;

        enabled = mod->getSettingValue<bool>("enabled");
        durationSeconds = static_cast<int>(mod->getSettingValue<int64_t>("duration"));
        
        std::string codecStr = mod->getSettingValue<std::string>("codec");
        customCodec = mod->getSettingValue<std::string>("custom-codec");
        customSaveFolder = mod->getSettingValue<std::string>("save-folder");

        if (mod->hasSetting("hardware-accel")) {
            hardwareAcceleration = mod->getSettingValue<bool>("hardware-accel");
        }

        if (codecStr == "av1" || codecStr == "av1_nvenc" || codecStr == "auto_hw") codec = CodecType::AV1;
        else if (codecStr == "hevc" || codecStr == "hevc_nvenc") codec = CodecType::HEVC;
        else if (codecStr == "software_h264") {
            codec = CodecType::H264;
            hardwareAcceleration = false;
        }
        else if (codecStr == "custom") codec = CodecType::CUSTOM;
        else codec = CodecType::H264;

        std::string resStr = mod->getSettingValue<std::string>("resolution");
        if (resStr == "2160p") resolution = ResolutionType::UHD_4K;
        else if (resStr == "1440p") resolution = ResolutionType::QHD_1440P;
        else if (resStr == "1080p") resolution = ResolutionType::FHD_1080P;
        else if (resStr == "720p") resolution = ResolutionType::HD_720P;
        else resolution = ResolutionType::NATIVE;

        std::string rcStr = mod->getSettingValue<std::string>("rate-control");
        if (rcStr == "constant_cbr") rateControl = RateControlMode::CONSTANT_CBR;
        else rateControl = RateControlMode::ADAPTIVE_VBR;

        bitrateMbps = static_cast<int>(mod->getSettingValue<int64_t>("bitrate"));
        maxBitrateMbps = static_cast<int>(mod->getSettingValue<int64_t>("max-bitrate"));
        qualityLevel = static_cast<int>(mod->getSettingValue<int64_t>("quality-level"));
        targetFps = static_cast<int>(mod->getSettingValue<int64_t>("fps"));

        sanitizeCodecSettings();

        splitAudioTracks = mod->getSettingValue<bool>("split-audio");
        recordMic = mod->getSettingValue<bool>("record-mic");

        if (mod->hasSavedValue("split-audio")) {
            splitAudioTracks = mod->getSavedValue<bool>("split-audio");
        }
        if (mod->hasSavedValue("record-mic")) {
            recordMic = mod->getSavedValue<bool>("record-mic");
        }

        micDeviceId = mod->getSettingValue<std::string>("mic-device-id");
        micDeviceName = mod->getSettingValue<std::string>("mic-device-name");
        if (micDeviceId.empty()) micDeviceId = "default";
        if (micDeviceName.empty()) micDeviceName = "Default System Device";
        systemVolume = static_cast<float>(mod->getSettingValue<double>("system-volume"));
        micVolume = static_cast<float>(mod->getSettingValue<double>("mic-volume"));

        showNotification = mod->getSettingValue<bool>("show-notification");
    }

    void saveToSettings() {
        auto mod = geode::Mod::get();
        if (!mod) return;

        s_isSavingSettings = true;

        (void)mod->setSettingValue("enabled", enabled);
        (void)mod->setSettingValue("duration", static_cast<int64_t>(durationSeconds));
        (void)mod->setSettingValue("hardware-accel", hardwareAcceleration);
        
        std::string codecStr = "av1";
        if (codec == CodecType::CUSTOM) codecStr = "custom";
        else if (codec == CodecType::AV1) codecStr = "av1";
        else if (codec == CodecType::HEVC) codecStr = "hevc";
        else if (codec == CodecType::H264) codecStr = "h264";
        (void)mod->setSettingValue("codec", codecStr);
        (void)mod->setSettingValue("custom-codec", customCodec);
        (void)mod->setSettingValue("save-folder", customSaveFolder);

        std::string resStr = "native";
        if (resolution == ResolutionType::UHD_4K) resStr = "2160p";
        else if (resolution == ResolutionType::QHD_1440P) resStr = "1440p";
        else if (resolution == ResolutionType::FHD_1080P) resStr = "1080p";
        else if (resolution == ResolutionType::HD_720P) resStr = "720p";
        (void)mod->setSettingValue("resolution", resStr);

        std::string rcStr = "adaptive_vbr";
        if (rateControl == RateControlMode::CONSTANT_CBR) rcStr = "constant_cbr";
        else if (rateControl == RateControlMode::CONSTANT_QUALITY) rcStr = "constant_quality";
        (void)mod->setSettingValue("rate-control", rcStr);

        (void)mod->setSettingValue("bitrate", static_cast<int64_t>(bitrateMbps));
        (void)mod->setSettingValue("max-bitrate", static_cast<int64_t>(maxBitrateMbps));
        (void)mod->setSettingValue("quality-level", static_cast<int64_t>(qualityLevel));
        (void)mod->setSettingValue("fps", static_cast<int64_t>(targetFps));
        (void)mod->setSettingValue("split-audio", splitAudioTracks);
        (void)mod->setSettingValue("record-mic", recordMic);
        (void)mod->setSettingValue("mic-device-id", micDeviceId);
        (void)mod->setSettingValue("mic-device-name", micDeviceName);
        (void)mod->setSettingValue("system-volume", static_cast<double>(systemVolume));
        (void)mod->setSettingValue("mic-volume", static_cast<double>(micVolume));

        // Persistent save values
        mod->setSavedValue("record-mic", recordMic);
        mod->setSavedValue("split-audio", splitAudioTracks);
        mod->setSavedValue("hardware-accel", hardwareAcceleration);

        // Immediate disk flush so settings survive game termination/closing
        (void)mod->saveData();

        s_isSavingSettings = false;
    }

    // Returns the encoded video size for a given game framebuffer size.
    // Presets define the output HEIGHT; width follows the window aspect ratio
    // (no stretching on 16:10 / 21:9 monitors). Always even (NV12 requirement).
    std::pair<int, int> getResolutionDimensions(int nativeW, int nativeH) const {
        if (nativeW <= 0 || nativeH <= 0) return {1920, 1080};

        int targetH = 0;
        switch (resolution) {
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

    std::filesystem::path getRecordingsDirectory() const {
        if (!customSaveFolder.empty()) {
            std::error_code ec;
            std::filesystem::path customPath(customSaveFolder);
            if (!std::filesystem::exists(customPath, ec)) {
                std::filesystem::create_directories(customPath, ec);
            }
            if (std::filesystem::exists(customPath, ec)) {
                return customPath;
            }
        }

        char* userProfile = nullptr;
        size_t len = 0;
        if (_dupenv_s(&userProfile, &len, "USERPROFILE") == 0 && userProfile) {
            std::filesystem::path dir = std::filesystem::path(userProfile) / "Videos" / "Geometry Dash";
            free(userProfile);
            std::error_code ec;
            std::filesystem::create_directories(dir, ec);
            return dir;
        }

        std::filesystem::path dir = geode::dirs::getGameDir() / "Replays";
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        return dir;
    }
};

} // namespace replay
