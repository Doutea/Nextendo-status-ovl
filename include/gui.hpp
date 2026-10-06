#pragma once

#include <tesla.hpp>

#include <string>

#include "network.hpp"

namespace nextendo {

// The panel.
//
// This is deliberately a SINGLE-SHOT Gui: createUI() builds the list once and
// nothing rebuilds it afterwards.
//
// Two facts make that sufficient:
//   * libtesla calls onShow() before loadInitialGui(), and FetchJob::start()
//     runs the request inline, so by the time createUI() runs the result is
//     already published;
//   * pressing X replaces this Gui with a fresh one via tsl::changeTo(), which
//     reuses the createUI() path instead of mutating a live list.
//
// An earlier revision rebuilt the list in place on every state change, which
// meant calling frame_->setContent() and removeFocus() while the framework still
// held references into the tree being replaced. That whole mechanism is gone.
class GuiMain : public tsl::Gui {
public:
    explicit GuiMain(FetchJob* job);
    ~GuiMain() override;

    tsl::elm::Element* createUI() override;

private:
    FetchJob* job_ = nullptr;
};

}  // namespace nextendo
