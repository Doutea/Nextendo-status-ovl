// gui.hpp - the overlay's user interface.
//
// Layout is a single OverlayFrame holding a List:
//   total players (the headline number the website shows)
//   service status rows
//   one row per game, highest player count first
//
// The list is rebuilt whenever the fetch state changes, rather than mutated in
// place: libtesla's List applies addItem/removeItem lazily on the next layout
// pass, and ListItem's left-hand text has no setter, so rebuilding a fresh List
// and handing it to OverlayFrame::setContent (which deletes the old one) is both
// simpler and free of dangling references.

#pragma once

#include <tesla.hpp>

#include <memory>
#include <string>

#include "network.hpp"

namespace nextendo {

class GuiMain : public tsl::Gui {
public:
    // `job` may be null, which means networking could not be started; the GUI
    // then shows that instead of waiting for data that will never arrive.
    explicit GuiMain(FetchJob* job);
    ~GuiMain() override;

    tsl::elm::Element* createUI() override;
    void update() override;
    bool handleInput(u64 keysDown, u64 keysHeld, const HidTouchState& touchPos,
                     HidAnalogStickState leftStick, HidAnalogStickState rightStick) override;

private:
    // Rebuilds the whole panel from the current state.
    void rebuild(const FetchOutcome* outcome, const std::string& status_text,
                 bool status_faint);

    FetchJob* job_ = nullptr;

    tsl::elm::OverlayFrame* frame_ = nullptr;
    tsl::elm::List* list_ = nullptr;

    // Render state last painted, used to decide when a rebuild is needed.
    FetchState shownState_ = FetchState::Idle;
    std::string shownErrorText_;
    u32 tick_ = 0;
};

}  // namespace nextendo
