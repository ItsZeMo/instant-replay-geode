#pragma once

#include <cstdint>
#include <cstdio>
#include <string>

#include "ConcatResult.hpp"

#ifdef GEODE_IS_WINDOWS
#include <windows.h>
#endif

namespace replay {

// Detailed result of encoder initialization or sample write operations.
struct EncoderOpResult {
    bool ok = false;
#ifdef GEODE_IS_WINDOWS
    HRESULT hr = S_OK;
#else
    int64_t code = 0;
#endif
    std::string message;
    bool fatalStorage = false;
    bool retryable = false;

    explicit operator bool() const noexcept { return ok; }

    std::string detailString() const {
        if (!message.empty()) return message;
#ifdef GEODE_IS_WINDOWS
        char buf[32];
        snprintf(buf, sizeof(buf), "HRESULT 0x%08X", static_cast<unsigned>(hr));
        return buf;
#else
        return "error " + std::to_string(code);
#endif
    }
};

} // namespace replay
