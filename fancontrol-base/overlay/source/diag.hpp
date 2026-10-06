// diag.hpp - step logging to the SD card.
//
// The overlay crashes the loader while closing, and Atmosphère's fatal screen
// only shows zeroed registers, so the failing step is invisible. Appending each
// step to a file makes the last written line identify it.
//
// Writes only happen from initServices()/exitServices()/createUI(), where libnx
// has already brought fs up and has not yet taken it down. Earlier attempts at
// this failed because they wrote from a constructor (before fs init) and from an
// atexit hook (after fs uninit).

#pragma once

#include <cstdio>

namespace nextendo {

inline void diag(const char* step) {
    FILE* file = std::fopen("sdmc:/nextendo.log", "a");
    if (file == nullptr) return;
    std::fputs(step, file);
    std::fputc('\n', file);
    std::fclose(file);
}

}  // namespace nextendo
