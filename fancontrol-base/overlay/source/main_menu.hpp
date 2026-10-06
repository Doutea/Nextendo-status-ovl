#pragma once

#include <tesla.hpp>

#include "nextendo.hpp"

// The panel.
//
// Same shape as the template's MainMenu: a tsl::Gui with a createUI() override.
// createUI() is where the request happens, because libtesla calls it after
// initScreen() and before anything is drawn - so the numbers are already fetched
// when the rows are built, and no rebuild or polling is needed.
class MainMenu : public tsl::Gui
{
public:
    MainMenu() = default;

    virtual tsl::elm::Element* createUI() override;
};
