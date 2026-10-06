// Nextendo player-count overlay.
//
// This file is NX-FanControl's main.cpp with its two fan-control calls swapped
// for the networking this overlay needs. The shape is unchanged on purpose:
// same class, same three virtuals, same initServices/exitServices pairing, same
// `return tsl::loop<...>(argc, argv)`.
//
//   NX-FanControl                       this overlay
//   ------------------------------      ------------------------------------
//   fsdevMountSdmc()                    (not needed; nothing is read from disk)
//   pmshellInitialize()                 nextendo::fetch_and_store()
//   fsdevUnmountAll()                   fsdevUnmountAll()      [kept verbatim]
//   pmshellExit()                       (nothing to undo)

#define TESLA_INIT_IMPL
#include <tesla.hpp>

#include <curl/curl.h>

#include "main_menu.hpp"
#include "nextendo.hpp"

class NextendoOverlay : public tsl::Overlay {
public:
    virtual void initServices() override
    {
        // Runs inside libtesla's doWithSmSession, so `sm:` is already open here.
        //
        // An EXTRA reference is taken and held for the overlay's whole lifetime,
        // released in exitServices(). Name resolution is why: libnx's sfdnsres
        // resolver initialises lazily on the first getaddrinfo and needs `sm:` at
        // that moment, but the request runs from createUI(), outside libtesla's
        // session. Opening a session around the request instead was tried and
        // every such build failed with "Couldn't resolve host name"; the build
        // that resolved names successfully held the session for the lifetime.
        // Service guards are reference counted, so this stays balanced.
        smInitialize();

        curl_global_init(CURL_GLOBAL_DEFAULT);
    }

    virtual void exitServices() override
    {
        // Deliberately does NOT close anything.
        //
        // smExit() was here to balance the smInitialize() above, but closing that
        // session on the way out is what crashes the loader, so the reference is
        // simply left open. The overlay is unmapped the moment main() returns and
        // libnx's own exit path closes sm: anyway, so nothing leaks and there is
        // nothing to balance.
        //
        // socketExit() is left out for the same reason: it made nx-ovlloader fail
        // right afterwards with an Atmosphère fatal 2347-0004 (its own read of the
        // next NRO returning an error).
        fsdevUnmountAll();
    }

    virtual std::unique_ptr<tsl::Gui> loadInitialGui() override {
        return initially<MainMenu>();
    }
};

int main(int argc, char **argv) {
    return tsl::loop<NextendoOverlay>(argc, argv);
}