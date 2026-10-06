// Can this overlay write a file to the SD card at all?
//
// Every earlier attempt to get a diagnostic log out of this overlay produced no
// file, which made the logs useless. Before relying on a log again, this proves
// the write works - and proves it from exactly the same call sites the log will
// use (initServices and exitServices, where libnx's fs is up).
//
// Build with -DNEXTENDO_DIAG_PROBE to swap this file's content in for main.cpp.

#define TESLA_INIT_IMPL
#include <tesla.hpp>

#include <curl/curl.h>

#include "diag.hpp"
#include "main_menu.hpp"

class NextendoOverlay : public tsl::Overlay {
public:
    virtual void initServices() override
    {
        diag("probe: initServices enter");

        // A write of a known string to a second file, so a readable log also
        // proves ordinary file I/O rather than only appends.
        FILE* marker = std::fopen("sdmc:/nextendo-probe.txt", "w");
        if (marker != nullptr) {
            std::fputs("write ok\n", marker);
            std::fclose(marker);
        }

        diag("probe: initServices exit");
    }

    virtual void exitServices() override
    {
        diag("probe: exitServices enter");
        diag("probe: exitServices exit");
    }

    virtual std::unique_ptr<tsl::Gui> loadInitialGui() override {
        return initially<MainMenu>();
    }
};

int main(int argc, char **argv) {
    return tsl::loop<NextendoOverlay>(argc, argv);
}
