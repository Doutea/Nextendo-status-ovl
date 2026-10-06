// debug_log.hpp - a tiny step logger for diagnosing exit-time crashes.
//
// The overlay crashes while closing, and the fatal screen only shows zeroed
// registers, so the failing step is invisible. Recording each step to a file on
// the SD card makes the last written line identify it.
//
// The file is deliberately small and flushed per line: a crash mid-write must
// still leave the earlier steps readable.

#pragma once

#include <cstdio>
#include <cstring>

namespace nextendo {

// Leaves at most this many bytes in the log: enough for the last few steps.
constexpr long kLogMaxBytes = 2048;

inline void debug_log(const char* step) {
    // fopen/fwrite are available because libnx's __appInit already mounted sdmc.
    FILE* file = std::fopen("sdmc:/nextendo-ovl.log", "a");
    if (file == nullptr) return;

    // Rotate: if the file grew past the cap, start over so the newest steps are
    // always present.
    std::fseek(file, 0, SEEK_END);
    if (std::ftell(file) > kLogMaxBytes) {
        std::fclose(file);
        file = std::fopen("sdmc:/nextendo-ovl.log", "w");
        if (file == nullptr) return;
    }

    std::fputs(step, file);
    std::fputc('\n', file);
    std::fflush(file);
    std::fclose(file);
}

}  // namespace nextendo
