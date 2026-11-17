#pragma once

#include <string>

namespace replay {

// Result of merging rolling segments into the final MP4 (platform independent, Geode-free).
struct ConcatResult {
    bool success = false;
    std::string error;            // Human readable reason when success == false
    double durationSeconds = 0.0; // Duration actually written to the output file
};

} // namespace replay
