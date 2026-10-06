// Nextendo player-count overlay.
//
// This file is NX-FanControl's main.cpp with its two fan-control calls swapped
// for the networking this overlay needs. The shape is unchanged on purpose:
// same class, same three virtuals, same initServices/exitServices pairing, same
// `return tsl::loop<...>(argc, argv)`.
//
//   NX-FanControl                       this overlay
//   ------------------------------      ------------------------------------
//   fsdevUnmountAll()                   (not mounted here; libnx unmounts itself)
//   pmshellInitialize()                 smInitialize() + curl_global_init()
//   pmshellExit()                       (nothing to undo)
//
// fs: is deliberately NOT mounted here. The write probe measured fopen() failing
// with errno 88 (ENOSYS) for every path, which means the "sdmc:" stdio device is
// not registered in this process - the overlay is launched by nx-ovlloader, not
// by hbmenu, and nothing has mounted sdmc for it. Mounting it is therefore an
// option if disk access is ever needed; the log below goes through the raw fs
// API instead, which the probe showed works.

#define TESLA_INIT_IMPL
#include <tesla.hpp>

#include <curl/curl.h>

#include "diag.hpp"
#include "main_menu.hpp"
#include "nextendo.hpp"

class NextendoOverlay : public tsl::Overlay {
public:
    virtual void initServices() override
    {
        // Runs inside libtesla's doWithSmSession, so `sm:` is already open here.
        //
        // An EXTRA reference is taken and held for the overlay's whole lifetime.
        // Name resolution is why: libnx's sfdnsres resolver initialises lazily on
        // the first getaddrinfo and needs `sm:` at that moment, but the request
        // runs from createUI(), outside libtesla's session. Opening a session
        // around the request instead was tried and every such build failed with
        // "Couldn't resolve host name"; the build that resolved names
        // successfully held the session for the lifetime. Service guards are
        // reference counted, so this is additive rather than a reset.
        nextendo::diag("init: enter");
        smInitialize();
        nextendo::diag("init: sm up");
        curl_global_init(CURL_GLOBAL_DEFAULT);
        nextendo::diag("init: curl up");
    }

    virtual void exitServices() override
    {
        // Intentionally empty, and deliberately so.
        //
        // NX-FanControl unmounts here because its initServices() mounted; this
        // overlay never mounts. Closing sm: here was also tried and is what
        // appeared to break the loader, so the reference is left open - the
        // overlay is unmapped the moment main() returns, and libnx's own exit
        // path closes sm: anyway.
        //
        // These log lines are the whole point of this build: they are the last
        // thing written before the loader runs, so whichever line is missing
        // from sdmc:/nextendo.log identifies where the crash happens.
        nextendo::diag("exit: enter");
        nextendo::diag("exit: leave");
    }

    virtual std::unique_ptr<tsl::Gui> loadInitialGui() override {
        return initially<MainMenu>();
    }
};

int main(int argc, char **argv) {
    return tsl::loop<NextendoOverlay>(argc, argv);
}
