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
        // Intentionally empty, and deliberately so.
        //
        // NX-FanControl unmounts here because its initServices() mounted
        // (fsdevMountSdmc). This overlay never mounts anything - it reads nothing
        // from disk - so unmounting would be an unmatched call that tears down the
        // loader's own view of the SD card. That matches the failure this overlay
        // was hitting: nx-ovlloader dying right after the overlay closed with an
        // Atmosphère fatal 2347-0004, which is its own fsFileRead of the next NRO
        // failing. libnx's exit path already calls fsdevUnmountAll() itself
        // (nx/source/runtime/init.c).
        //
        // Nothing else is closed here either. sm: and the socket stack stay up:
        // the overlay is unmapped the moment main() returns, so nothing left
        // behind is reachable, and closing either one on the way out is what
        // broke the loader.
    }

    virtual std::unique_ptr<tsl::Gui> loadInitialGui() override {
        return initially<MainMenu>();
    }
};

int main(int argc, char **argv) {
    return tsl::loop<NextendoOverlay>(argc, argv);
}