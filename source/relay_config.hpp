#pragma once
#include "relay_route.hpp"
#include "relay_defaults.hpp"
// Set once before the worker starts. Build-time credentials stay out of git.
static nextendo::RelayRoute g_relayRoute;
static void loadRelayRoute() {
    // Missing SD override uses the compiled default. An existing empty file
    // explicitly selects direct mode; malformed overrides remain visible errors.
    g_relayRoute = nextendo::embeddedRelayRoute();
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
