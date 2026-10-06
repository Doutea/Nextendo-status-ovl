#define TESLA_INIT_IMPL  // libtesla needs its implementation in exactly one TU
#include <tesla.hpp>

#include "gui.hpp"
#include "network.hpp"

namespace {

// One job object for the overlay's lifetime.
nextendo::FetchJob g_fetchJob;

bool g_servicesOk = false;

// Deliberately mirrors the structure of overlays that are known to work on the
// target console: services are brought up in initServices(), torn down in
// exitServices(), and main() simply returns loop()'s result.
//
// An earlier revision added an atexit() hook that called std::_Exit() after
// envSetNextLoad(). That is NOT how working overlays exit, and it is removed:
// exitServices() plus a normal return is what the reference implementations do.
class NextendoOverlay : public tsl::Overlay {
public:
    void initServices() override {
        // libtesla has already initialized fs, hid, pl, pmdmnt, hidsys and
        // setsys by the time this runs; we only add what networking needs.
        g_servicesOk = nextendo::services_init();
    }

    void exitServices() override {
        // Only the job state is touched here. The service teardown lives in
        // services_exit(), which is currently a no-op; see the note there.
        g_fetchJob.cancel();
        nextendo::services_exit();
    }

    std::unique_ptr<tsl::Gui> loadInitialGui() override {
        // A null job means the networking never came up; the GUI shows that
        // instead of waiting for data that will never arrive.
        return initially<nextendo::GuiMain>(g_servicesOk ? &g_fetchJob : nullptr);
    }
};

}  // namespace

int main(int argc, char** argv) {
    // No exception handling: the project is built with -fno-exceptions, and the
    // loader unmaps this NRO as soon as main returns.
    return tsl::loop<NextendoOverlay>(argc, argv);
}
