#include "gui.hpp"

#include <algorithm>
#include <cstdio>

namespace nextendo {
namespace {

// The panel scrolls, but there is no point listing every title the network
// tracks; the website's status page shows a comparable subset.
constexpr std::size_t kMaxGameRows = 40;
constexpr const char* kSpinner[] = {"|", "/", "-", "\\"};

// Index of the "Status" row inside the list built by rebuild(). Kept as a named
// constant so the in-place spinner update cannot silently drift out of sync with
// the row order there.
constexpr u32 kStatusRowIndex = 2;

}  // namespace

GuiMain::GuiMain(FetchJob* job) : job_(job) {}

GuiMain::~GuiMain() = default;

tsl::elm::Element* GuiMain::createUI() {
    frame_ = new tsl::elm::OverlayFrame("Nextendo Network", "Players online");

    // rebuild() creates the List itself and paints the initial state right away,
    // so the panel is never shown empty.
    rebuild(nullptr, job_ == nullptr ? "network services unavailable" : "Starting...",
            job_ == nullptr);

    return frame_;
}

void GuiMain::rebuild(const FetchOutcome* outcome, const std::string& status_text,
                      bool status_faint) {
    // The focused element may be inside the list about to be deleted, so drop
    // the focus first (libtesla requires this before removing items).
    this->removeFocus();

    auto* list = new tsl::elm::List();

    list->addItem(new tsl::elm::CategoryHeader("Now playing"));

    const OnlineCounts* counts = (outcome != nullptr) ? &outcome->counts : nullptr;

    list->addItem(new tsl::elm::ListItem(
        "Players online",
        counts != nullptr ? std::to_string(counts->total) : "--"));

    list->addItem(new tsl::elm::ListItem("Status", status_text, status_faint));

    if (outcome != nullptr) {
        list->addItem(new tsl::elm::ListItem("Nextendo API", outcome->health_ok ? "ok" : "degraded"));
    } else if (job_ != nullptr) {
        list->addItem(new tsl::elm::ListItem("Nextendo API", "checking"));
    }

    if (counts != nullptr) {
        std::vector<GameEntry> games = counts->games;
        // The API already sorts by player count, but sorting here keeps the
        // display correct if that ever changes.
        std::sort(games.begin(), games.end(), [](const GameEntry& a, const GameEntry& b) {
            if (a.players != b.players) return a.players > b.players;
            return a.name < b.name;
        });

        list->addItem(new tsl::elm::CategoryHeader("Games (" + std::to_string(games.size()) + ")"));

        const std::size_t shown = std::min(games.size(), kMaxGameRows);
        for (std::size_t i = 0; i < shown; ++i) {
            list->addItem(new tsl::elm::ListItem(games[i].name, std::to_string(games[i].players)));
        }
        if (games.size() > shown) {
            list->addItem(new tsl::elm::ListItem(
                "+" + std::to_string(games.size() - shown) + " more games"));
        }
        if (games.empty()) {
            list->addItem(new tsl::elm::ListItem("Nobody is playing right now"));
        }
    } else {
        list->addItem(new tsl::elm::CategoryHeader("Games"));
        list->addItem(new tsl::elm::ListItem(
            job_ == nullptr ? "Networking could not be started" : "Waiting for data..."));
    }

    list->addItem(new tsl::elm::CategoryHeader(""));
    list->addItem(new tsl::elm::ListItem("Refresh", "X"));

    // setContent deletes the previous content element and its children.
    frame_->setContent(list);
    list_ = list;
}

void GuiMain::update() {
    ++tick_;

    if (job_ == nullptr) return;

    const FetchState state = job_->state();

    if (state == FetchState::Running) {
        if (shownState_ != FetchState::Running) {
            shownState_ = FetchState::Running;
            rebuild(nullptr, std::string("Checking ") + kSpinner[(tick_ / 15) % 4], false);
        } else {
            // Animate the spinner in place. ListItem's value has a setter, so a
            // full rebuild is not needed for this. The cast is safe (RTTI is on)
            // and the index is bounds-checked by the list itself.
            if (auto* item = dynamic_cast<tsl::elm::ListItem*>(list_->getItemAtIndex(kStatusRowIndex));
                item != nullptr) {
                item->setValue(std::string("Checking ") + kSpinner[(tick_ / 15) % 4]);
            }
        }
        return;
    }

    if (state == FetchState::Idle) return;

    const FetchOutcome outcome = job_->result();

    if (state == FetchState::Failed) {
        const std::string error = outcome.error.empty() ? "request failed" : outcome.error;
        if (shownState_ != FetchState::Failed || shownErrorText_ != error) {
            shownState_ = FetchState::Failed;
            shownErrorText_ = error;
            rebuild(nullptr, error, true);
        }
        return;
    }

    if (shownState_ != FetchState::Done) {
        shownState_ = FetchState::Done;
        shownErrorText_.clear();
        rebuild(&outcome, outcome.counts.plausible ? "up to date" : "unexpected data",
                !outcome.counts.plausible);
    }
}

bool GuiMain::handleInput(u64 keysDown, u64, const HidTouchState&, HidAnalogStickState,
                          HidAnalogStickState) {
    if ((keysDown & HidNpadButton_X) != 0 && job_ != nullptr) {
        // Manual refresh. start() joins the previous worker first, so pressing X
        // repeatedly cannot accumulate threads.
        job_->start();
        shownState_ = FetchState::Idle;
        return true;
    }
    return false;
}

}  // namespace nextendo
