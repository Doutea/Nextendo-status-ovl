#define TESLA_INIT_IMPL  // libtesla needs its implementation in exactly one TU
#include <tesla.hpp>

#include <cstdlib>

#include "debug_log.hpp"
#include "gui.hpp"
#include "network.hpp"

namespace {

// One job object for the overlay's lifetime.
nextendo::FetchJob g_fetchJob;

bool g_servicesOk = false;

// Runs when the process is torn down, after libtesla's loop() has returned.
//
// The overlay is chainloaded by nx-ovlloader, which shares one process with the
// menu and the game. libnx's exit path de-initialises sm:/hid:/applet on the way
// out, which leaves that shared process unable to handle the menu's swipe
// gesture afterwards. Setting the next load path and stopping the process right
// here skips that teardown, and the loader's trampoline continues from the
// hand-off exactly as it does for a normal chainload.
void exit_cleanup() {
    nextendo::debug_log("atexit: begin");
    g_fetchJob.cancel();
    nextendo::debug_log("atexit: worker joined");
    nextendo::services_exit();
    nextendo::debug_log("atexit: services down");

    // Hand the process back to the menu without running libnx's __appExit().
    envSetNextLoad("sdmc:/switch/.overlays/ovlmenu.ovl", "sdmc:/switch/.overlays/ovlmenu.ovl");
    nextendo::debug_log("atexit: next load set");
    std::_Exit(0);
}

bool g_exitHookInstalled = false;

class NextendoOverlay : public tsl::Overlay {
public:
    void initServices() override {
        nextendo::debug_log("initServices: begin");
        if (!g_exitHookInstalled) {
            std::atexit(exit_cleanup);
            g_exitHookInstalled = true;
        }
        g_servicesOk = nextendo::services_init();
        nextendo::debug_log(g_servicesOk ? "initServices: services up" : "initServices: services FAILED");
    }

    void exitServices() override {
        nextendo::debug_log("exitServices: begin");
        g_fetchJob.cancel();
        nextendo::debug_log("exitServices: worker joined");
        nextendo::services_exit();
        nextendo::debug_log("exitServices: done");
    }

    void onShow() override {
        nextendo::debug_log("onShow");
        if (g_servicesOk) g_fetchJob.start();
    }

    void onHide() override {
        nextendo::debug_log("onHide: begin");
        g_fetchJob.cancel();
        nextendo::debug_log("onHide: done");
    }

    std::unique_ptr<tsl::Gui> loadInitialGui() override {
        nextendo::debug_log("loadInitialGui");
        return std::make_unique<nextendo::GuiMain>(g_servicesOk ? &g_fetchJob : nullptr);
    }
};

}  // namespace

int main(int argc, char** argv) {
    // No exception handling here on purpose: the project is built with
    // -fno-exceptions, and the loader unmaps this NRO as soon as main returns.
    nextendo::debug_log("--- overlay start ---");
    const int rc = tsl::loop<NextendoOverlay>(argc, argv);
    nextendo::debug_log("main: loop returned");
    return rc;
}
