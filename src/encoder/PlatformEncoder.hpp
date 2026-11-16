#pragma once

#include "VideoEncoderTypes.hpp"

#ifdef GEODE_IS_WINDOWS
#include "MediaFoundationEncoder.hpp"
namespace replay {
using PlatformEncoder = MediaFoundationEncoder;
}
#elif defined(GEODE_IS_MACOS)
#include "VideoToolboxEncoder.hpp"
namespace replay {
using PlatformEncoder = VideoToolboxEncoder;
}
#endif
