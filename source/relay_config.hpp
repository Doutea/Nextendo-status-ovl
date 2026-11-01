#pragma once
#include "relay_route.hpp"
// Set once before the worker starts. Never embed a private relay key in source.
static nextendo::RelayRoute g_relayRoute;
static void loadRelayRoute() {
    FsFileSystem fs{};
    if (R_FAILED(fsOpenSdCardFileSystem(&fs))) return;
    FsFile file{};
    if (R_SUCCEEDED(fsFsOpenFile(&fs, "/switch/nextendo-status/network.ini", FsOpenMode_Read, &file))) {
        s64 size = 0;
        if (R_FAILED(fsFileGetSize(&file, &size)) || size < 0 || size > 4096) {
            g_relayRoute.error = "network.ini is unreadable or too large";
        } else if (size == 0) g_relayRoute = {};
        else {
            std::string text(static_cast<size_t>(size), '\0');
            u64 read = 0;
            if (R_SUCCEEDED(fsFileRead(&file, 0, text.data(), text.size(), FsReadOption_None, &read)) && read == text.size())
                g_relayRoute = nextendo::parseRelayRoute(text);
            else g_relayRoute.error = "could not read network.ini";
        }
        fsFileClose(&file);
    }
    fsFsClose(&fs);
}
