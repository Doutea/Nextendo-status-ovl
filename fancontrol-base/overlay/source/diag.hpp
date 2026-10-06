// diag.hpp - step logging to the SD card, using the raw fs API.
//
// WHY NOT stdio: on this console fopen() fails for every path with errno 88
// (ENOSYS) inside the overlay process - the "sdmc:" devoptab device is simply
// not registered. Measured with the write probe:
//
//     fopen sdmc:/nextendo.log        -> null, errno 88
//     fopen sdmc:/switch/nextendo.log -> null, errno 88
//     fopen /nextendo.log             -> null, errno 88
//     fopen nextendo.log              -> null, errno 88
//     fsOpenSdCardFileSystem          -> ok
//     fsFsOpenFile(write)             -> ok
//     fsFileWrite                     -> ok
//
// That is why every earlier attempt at a log produced no file. The raw fs API
// works, so the log goes through that instead.
//
// The file is rewritten each time: read the existing text back, append the new
// line, write it out. Only exit-time steps are logged, so this stays tiny.

#pragma once

#include <switch.h>

#include <cstdio>
#include <cstring>
#include <string>

namespace nextendo {

constexpr const char* kLogPath = "/nextendo.log";
constexpr s64 kLogMaxBytes = 4096;

inline void diag(const char* step) {
    FsFileSystem sdmc;
    if (R_FAILED(fsOpenSdCardFileSystem(&sdmc))) return;

    // Read what is already there (if anything).
    std::string content;
    FsFile file;
    if (R_SUCCEEDED(fsFsOpenFile(&sdmc, kLogPath, FsOpenMode_Read, &file))) {
        s64 size = 0;
        if (R_SUCCEEDED(fsFileGetSize(&file, &size)) && size > 0) {
            if (size > kLogMaxBytes) size = kLogMaxBytes;
            content.resize(static_cast<std::size_t>(size));
            u64 got = 0;
            fsFileRead(&file, 0, &content[0], static_cast<u64>(size),
                       FsReadOption_None, &got);
            content.resize(static_cast<std::size_t>(got));
        }
        fsFileClose(&file);
    }

    content += step;
    content += '\n';

    // Create if missing (PathAlreadyExists is fine and expected on later runs).
    fsFsCreateFile(&sdmc, kLogPath, 0, 0);

    if (R_SUCCEEDED(fsFsOpenFile(&sdmc, kLogPath,
                                 FsOpenMode_Write | FsOpenMode_Append, &file))) {
        // FsOpenMode_Append advances to the end, so the offset argument is not
        // used; the whole buffer is written in one call.
        fsFileWrite(&file, 0, content.data(), content.size(), FsWriteOption_Flush);
        fsFileClose(&file);
    }

    fsFsClose(&sdmc);
}

}  // namespace nextendo
