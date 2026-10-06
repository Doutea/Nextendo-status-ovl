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
#include "main_menu.hpp"
#include "nextendo.hpp"

class NextendoOverlay : public tsl::Overlay {
public:
    virtual void initServices() override
    {
        // Runs inside libtesla's doWithSmSession, so `sm:` is open here. The
        // request itself is made in MainMenu::createUI(), right before the list
        // is built, so the numbers are already in hand when the rows are made.
        curl_global_init(CURL_GLOBAL_DEFAULT);
    }

    virtual void exitServices() override
    {
        // fsdevUnmountAll() is kept from the template. The socket stack is
        // deliberately left alone: socketInitialize() reports
        // LibnxError_AlreadyInitialized in this process, so it is not ours to
        // close, and closing it made nx-ovlloader fail right afterwards with an
        // Atmosphère fatal 2347-0004 (its own read of the next NRO).
        fsdevUnmountAll();
    }

    virtual std::unique_ptr<tsl::Gui> loadInitialGui() override {
        return initially<MainMenu>();
    }
};

int main(int argc, char **argv) {
    return tsl::loop<NextendoOverlay>(argc, argv);
}
