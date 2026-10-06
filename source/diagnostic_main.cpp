// diagnostic_main.cpp - minimal libultrahand overlay used to isolate a launch
// crash.
//
// It draws two rows and initialises NOTHING (no socket, no nifm, no custom
// services). If this launches but the real overlay does not, the fault is in
// the real overlay's initServices()/network code rather than in how the overlay
// integrates with libultrahand.
//
// Built separately with:  make OVL_MAIN=source/diagnostic_main.cpp
// The real main.cpp guards its own definition so only one main() exists.

#define TESLA_INIT_IMPL
#include <tesla.hpp>

namespace {

class ProbeGui : public tsl::Gui {
public:
    tsl::elm::Element* createUI() override {
        auto* frame = new tsl::elm::OverlayFrame("Probe", "diagnostic build");
        auto* list = new tsl::elm::List();
        list->addItem(new tsl::elm::CategoryHeader("If you can read this"));
        list->addItem(new tsl::elm::ListItem("libultrahand overlay launches", "OK"));
        list->addItem(new tsl::elm::ListItem("No services initialised", "as intended"));
        list->addItem(new tsl::elm::CategoryHeader("Next step"));
        list->addItem(new tsl::elm::ListItem("Report this screen back", ""));
        frame->setContent(list);
        return frame;
    }
};

class ProbeOverlay : public tsl::Overlay {
public:
    // Deliberately empty: this is the whole point of the probe.
    void initServices() override {}
    void exitServices() override {}

    std::unique_ptr<tsl::Gui> loadInitialGui() override {
        return std::make_unique<ProbeGui>();
    }
};

}  // namespace

int main(int argc, char** argv) {
    return tsl::loop<ProbeOverlay>(argc, argv);
}
