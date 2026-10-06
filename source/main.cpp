#define TESLA_INIT_IMPL  // libtesla needs its implementation in exactly one TU
#include <tesla.hpp>

#include "gui.hpp"
#include "network.hpp"

namespace {

// One job object for the overlay's lifetime. It is created in initServices (when
// the bsd/ssl services are available) and torn down in exitServices, which
// happens before libtesla's __appExit closes our handles.
nextendo::FetchJob g_fetchJob;

bool g_servicesOk = false;

class NextendoOverlay : public tsl::Overlay {
public:
    void initServices() override {
        // libtesla has already initialized fs, hid, pl, pmdmnt, hidsys and
        // setsys by the time this runs; we only add what networking needs.
        g_servicesOk = nextendo::services_init();
    }

    void exitServices() override {
        // Order matters: stop the worker (which may be mid-transfer) before
        // taking the socket and ssl services down underneath it.
        g_fetchJob.cancel();
        nextendo::services_exit();
    }

    void onShow() override {
        // Fetch once per open. The job ignores the call if a transfer is already
        // running, so re-showing cannot start a second one.
        if (g_servicesOk) g_fetchJob.start();
    }

    void onHide() override {
        // Nothing should keep running while the overlay is invisible.
        g_fetchJob.cancel();
    }

    std::unique_ptr<tsl::Gui> loadInitialGui() override {
        // A null job means the socket services never came up; the GUI renders a
        // clear message instead of waiting forever for data.
        return std::make_unique<nextendo::GuiMain>(g_servicesOk ? &g_fetchJob : nullptr);
    }
};

}  // namespace

int main(int argc, char** argv) {
    // No exception handling here on purpose: the project is built with
    // -fno-exceptions, and the loader unmaps this NRO as soon as main returns.
    return tsl::loop<NextendoOverlay>(argc, argv);
}
