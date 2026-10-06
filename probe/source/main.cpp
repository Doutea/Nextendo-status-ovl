// Diagnostic probe overlay: the smallest thing that shows up in the menu and
// draws a screen.
//
// It initialises no services at all. If this launches on the console while the
// real overlay crashes, the fault is in the real overlay's code (its
// initServices or network layer), not in the libtesla/loader integration.

#define TESLA_INIT_IMPL
#include <tesla.hpp>

class ProbeGui : public tsl::Gui {
public:
    virtual tsl::elm::Element* createUI() override {
        auto* frame = new tsl::elm::OverlayFrame("Nextendo Probe", "diagnostic build");

        auto* list = new tsl::elm::List();
        list->addItem(new tsl::elm::CategoryHeader("Probe status"));
        list->addItem(new tsl::elm::ListItem("libtesla overlay launches", "OK"));
        list->addItem(new tsl::elm::ListItem("Services initialised", "none, by design"));
        list->addItem(new tsl::elm::CategoryHeader("What this means"));
        list->addItem(new tsl::elm::ListItem("If you see this screen", "the real overlay's own code is at fault"));

        frame->setContent(list);
        return frame;
    }
};

class ProbeOverlay : public tsl::Overlay {
public:
    // Intentionally empty: that is the whole point of the probe.
    virtual void initServices() override {}
    virtual void exitServices() override {}

    virtual std::unique_ptr<tsl::Gui> loadInitialGui() override {
        return initially<ProbeGui>();
    }
};

int main(int argc, char** argv) {
    return tsl::loop<ProbeOverlay>(argc, argv);
}
