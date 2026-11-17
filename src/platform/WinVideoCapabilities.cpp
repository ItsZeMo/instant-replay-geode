#include "WinVideoCapabilities.hpp"

#include <Geode/Geode.hpp>

#ifdef GEODE_IS_WINDOWS

#include "../ui/ReplayPopup.hpp"
#include <Geode/utils/async.hpp>
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <atomic>
#include <mutex>

namespace replay {

namespace {

std::mutex s_capsMutex;
WinVideoEncoderCaps s_caps;
std::atomic<bool> s_probeStarted{false};

bool hasEncoder(const GUID& subtype, bool hardware) {
    MFT_REGISTER_TYPE_INFO outType{MFMediaType_Video, subtype};
    const UINT32 flags = hardware
        ? (MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER)
        : (MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_ASYNCMFT | MFT_ENUM_FLAG_SORTANDFILTER);
    IMFActivate** activates = nullptr;
    UINT32 count = 0;
    HRESULT hr = MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER, flags, nullptr, &outType, &activates, &count);
    if (activates) {
        for (UINT32 i = 0; i < count; ++i) {
            if (activates[i]) activates[i]->Release();
        }
        CoTaskMemFree(activates);
    }
    return SUCCEEDED(hr) && count > 0;
}

} // namespace

void startWinVideoEncoderProbe() {
    if (s_probeStarted.exchange(true)) return;
    geode::async::runtime().spawnBlocking<void>([]() {
        HRESULT coHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        HRESULT mfHr = MFStartup(MF_VERSION);

        WinVideoEncoderCaps caps;
        caps.av1Hardware = hasEncoder(MFVideoFormat_AV1, true);
        caps.av1Software = hasEncoder(MFVideoFormat_AV1, false);
        caps.hevcHardware = hasEncoder(MFVideoFormat_HEVC, true);
        caps.hevcSoftware = hasEncoder(MFVideoFormat_HEVC, false);
        caps.h264Hardware = hasEncoder(MFVideoFormat_H264, true);
        caps.h264Software = hasEncoder(MFVideoFormat_H264, false);
        caps.probed = true;

        if (SUCCEEDED(mfHr)) MFShutdown();
        if (SUCCEEDED(coHr)) CoUninitialize();

        {
            std::lock_guard<std::mutex> lock(s_capsMutex);
            s_caps = caps;
        }
        geode::log::info(
            "Instant Replay: Media Foundation encoders - AV1(HW={},SW={}), HEVC(HW={},SW={}), H.264(HW={},SW={})",
            caps.av1Hardware, caps.av1Software, caps.hevcHardware, caps.hevcSoftware,
            caps.h264Hardware, caps.h264Software);

        geode::queueInMainThread([]() {
            if (auto* popup = ReplayPopup::get()) {
                popup->refreshUI(false);
            }
        });
    });
}

WinVideoEncoderCaps getWinVideoEncoderCaps() noexcept {
    std::lock_guard<std::mutex> lock(s_capsMutex);
    return s_caps;
}

} // namespace replay

#endif // GEODE_IS_WINDOWS
