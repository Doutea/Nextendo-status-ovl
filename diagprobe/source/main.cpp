// Diagnostic probe: proves this overlay can write a log to the SD card.
//
// Every earlier attempt to capture a diagnostic log produced no file, which made
// it useless. Before relying on a log again, this checks the write works - and
// from exactly the call sites the real log will use (initServices and
// exitServices, where libnx's fs is up and has not yet been taken down).
//
// It draws one line so it is visible in the overlay list, then writes on open
// and on close. Afterwards, read sdmc:/nextendo.log.

#define TESLA_INIT_IMPL
#include <tesla.hpp>

#include <cstdio>

#include "diag.hpp"

class ProbeGui : public tsl::Gui {
public:
    virtual tsl::elm::Element* createUI() override {
        auto* frame = new tsl::elm::OverlayFrame("Nextendo Log Probe",
                                                 "\u65e5\u5fd7\u6d4b\u8bd5");  // 鏃ュ織娴嬭瘯
        auto* list = new tsl::elm::List();
        list->addItem(new tsl::elm::CategoryHeader("log probe"));
        list->addItem(new tsl::elm::ListItem(
            "nextendo.log", "written on open/close"));
        list->addItem(new tsl::elm::ListItem(
            "\u8bf7\u6253\u5f00\u540e\u6309 B \u9000\u51fa", "then read sdmc:/nextendo.log"));
        frame->setContent(list);
        return frame;
    }
};

class ProbeOverlay : public tsl::Overlay {
public:
    virtual void initServices() override
    {
        nextendo::diag("probe: initServices enter");

        // A plain write of a known string, so a readable file also proves
        // ordinary file I/O rather than only appends.
        FILE* marker = std::fopen("sdmc:/nextendo-probe.txt", "w");
        if (marker != nullptr) {
            std::fputs("write ok\n", marker);
            std::fclose(marker);
            nextendo::diag("probe: plain write ok");
        } else {
            nextendo::diag("probe: plain write FAILED");
        }

        nextendo::diag("probe: initServices exit");
    }

    virtual void exitServices() override
    {
        nextendo::diag("probe: exitServices enter");
        nextendo::diag("probe: exitServices exit");
    }

    virtual std::unique_ptr<tsl::Gui> loadInitialGui() override {
        return initially<ProbeGui>();
    }
};

int main(int argc, char **argv) {
    return tsl::loop<ProbeOverlay>(argc, argv);
}
