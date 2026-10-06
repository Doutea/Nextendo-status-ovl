#include "gui.hpp"

#include <algorithm>

namespace nextendo {
namespace {

// The panel scrolls, but there is no point listing every title the network
// tracks; the website's status page shows a comparable subset.
constexpr std::size_t kMaxGameRows = 40;
constexpr const char* kSpinner[] = {"|", "/", "-", "\\"};

// Index of the status row inside the list built by rebuild(). Kept as a named
// constant so the in-place spinner update cannot silently drift out of sync with
// the row order there.
constexpr u32 kStatusRowIndex = 2;

// libtesla picks its font from the console's system language: when that is
// Chinese (simplified or traditional) it loads Nintendo's Chinese shared font
// and uses it for any glyph the standard font lacks. So the strings below render
// as long as the console's language is set to Chinese; on a console set to
// another language Chinese glyphs would come out blank.
constexpr const char* kTitle = "Nextendo \u7f51\u7edc";           // Nextendo 网络
constexpr const char* kSubtitle = "\u5728\u7ebf\u4eba\u6570";     // 在线人数
constexpr const char* kHeaderNow = "\u5f53\u524d\u5728\u7ebf";    // 当前在线

constexpr const char* kTotalLabel = "\u5728\u7ebf\u4eba\u6570";   // 在线人数
constexpr const char* kStatusLabel = "\u72b6\u6001";              // 状态
constexpr const char* kApiLabel = "Nextendo API";

// Value texts for the status row.
constexpr const char* kStatusWaiting = "\u7b49\u5f85\u6570\u636e";      // 等待数据
constexpr const char* kStatusUpToDate = "\u5df2\u66f4\u65b0";            // 已更新
constexpr const char* kStatusBadData = "\u6570\u636e\u5f02\u5e38";       // 数据异常
constexpr const char* kStatusNoService = "\u7f51\u7edc\u670d\u52a1\u4e0d\u53ef\u7528";  // 网络服务不可用
constexpr const char* kStatusRequestFailed = "\u8bf7\u6c42\u5931\u8d25";  // 请求失败
constexpr const char* kStatusChecking = "\u68c0\u67e5\u4e2d";            // 检查中

constexpr const char* kApiOk = "\u6b63\u5e38";        // 正常
constexpr const char* kApiDegraded = "\u5f02\u5e38";  // 异常
constexpr const char* kApiChecking = "\u68c0\u67e5\u4e2d";  // 检查中

constexpr const char* kGamesPrefix = "\u6e38\u620f";  // 游戏
constexpr const char* kWaitingData = "\u7b49\u5f85\u6570\u636e\u2026";      // 等待数据…
constexpr const char* kNoNetworking = "\u65e0\u6cd5\u542f\u52a8\u7f51\u7edc"; // 无法启动网络
constexpr const char* kNobodyPlaying = "\u5f53\u524d\u65e0\u4eba\u5728\u7ebf"; // 当前无人在线

}  // namespace

GuiMain::GuiMain(FetchJob* job) : job_(job) {}

GuiMain::~GuiMain() = default;

tsl::elm::Element* GuiMain::createUI() {
    frame_ = new tsl::elm::OverlayFrame(kTitle, kSubtitle);

    // rebuild() creates the List itself and paints the initial state right away,
    // so the panel is never shown empty.
    rebuild(nullptr, job_ == nullptr ? kStatusNoService : kStatusChecking, job_ == nullptr);

    return frame_;
}

void GuiMain::rebuild(const FetchOutcome* outcome, const std::string& status_text,
                      bool status_faint) {
    // The focused element may be inside the list about to be deleted, so drop
    // the focus first (libtesla requires this before removing items).
    this->removeFocus();

    auto* list = new tsl::elm::List();

    list->addItem(new tsl::elm::CategoryHeader(kHeaderNow));

    const OnlineCounts* counts = (outcome != nullptr) ? &outcome->counts : nullptr;

    list->addItem(new tsl::elm::ListItem(
        kTotalLabel, counts != nullptr ? std::to_string(counts->total) : "--"));

    // ListItem's constructor takes only (text, value); the "faint" style is set
    // afterwards with setValue, so it cannot be passed here.
    auto* statusItem = new tsl::elm::ListItem(kStatusLabel, status_text);
    statusItem->setValue(status_text, status_faint);
    // Selecting this row and pressing A refreshes, same as the X button.
    // A listener must always be set: Element::onClick() calls m_clickListener
    // unconditionally, and an empty std::function would abort.
    statusItem->setClickListener([this](u64 keys) {
        if ((keys & HidNpadButton_A) != 0) {
            begin_fetch();
            return true;
        }
        return false;
    });
    list->addItem(statusItem);

    if (outcome != nullptr) {
        list->addItem(new tsl::elm::ListItem(kApiLabel,
                                             outcome->health_ok ? kApiOk : kApiDegraded));
    } else if (job_ != nullptr) {
        list->addItem(new tsl::elm::ListItem(kApiLabel, kApiChecking));
    }

    if (counts != nullptr) {
        std::vector<GameEntry> games = counts->games;
        // The API already sorts by player count, but sorting here keeps the
        // display correct if that ever changes.
        std::sort(games.begin(), games.end(), [](const GameEntry& a, const GameEntry& b) {
            if (a.players != b.players) return a.players > b.players;
            return a.name < b.name;
        });

        list->addItem(new tsl::elm::CategoryHeader(
            std::string(kGamesPrefix) + " (" + std::to_string(games.size()) + ")"));

        const std::size_t shown = std::min(games.size(), kMaxGameRows);
        for (std::size_t i = 0; i < shown; ++i) {
            list->addItem(new tsl::elm::ListItem(games[i].name, std::to_string(games[i].players)));
        }
        if (games.size() > shown) {
            list->addItem(new tsl::elm::ListItem(
                "+" + std::to_string(games.size() - shown) + " \u9879"));  // 项
        }
        if (games.empty()) {
            list->addItem(new tsl::elm::ListItem(kNobodyPlaying));
        }
    } else {
        list->addItem(new tsl::elm::CategoryHeader(kGamesPrefix));
        list->addItem(new tsl::elm::ListItem(
            job_ == nullptr ? kNoNetworking : kWaitingData));
    }

    // No trailing "Refresh" row: refreshing is bound to X and to the status row,
    // so a row that only documents the key would just take up space.

    // setContent deletes the previous content element and its children.
    frame_->setContent(list);
    list_ = list;
}

void GuiMain::begin_fetch() {
    if (job_ == nullptr) return;

    // start() joins any previous worker before launching a new one, so repeated
    // presses cannot accumulate threads.
    job_->start();
    shownState_ = FetchState::Idle;
}

void GuiMain::update() {
    ++tick_;

    if (job_ == nullptr) return;

    const FetchState state = job_->state();

    if (state == FetchState::Running) {
        const std::string spinner =
            std::string(kStatusChecking) + " " + kSpinner[(tick_ / 15) % 4];
        if (shownState_ != FetchState::Running) {
            shownState_ = FetchState::Running;
            rebuild(nullptr, spinner, false);
        } else {
            // Animate the spinner in place. ListItem's value has a setter, so a
            // full rebuild is not needed for this. The cast is safe (RTTI is on)
            // and the index is bounds-checked by the list itself.
            if (auto* item = dynamic_cast<tsl::elm::ListItem*>(list_->getItemAtIndex(kStatusRowIndex));
                item != nullptr) {
                item->setValue(spinner);
            }
        }
        return;
    }

    if (state == FetchState::Idle) return;

    const FetchOutcome outcome = job_->result();

    if (state == FetchState::Failed) {
        const std::string error =
            outcome.error.empty() ? kStatusRequestFailed : outcome.error;
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
        rebuild(&outcome, outcome.counts.plausible ? kStatusUpToDate : kStatusBadData,
                !outcome.counts.plausible);
    }
}

bool GuiMain::handleInput(u64 keysDown, u64, const HidTouchState&, HidAnalogStickState,
                          HidAnalogStickState) {
    // X refreshes from anywhere; the status row also refreshes when clicked.
    if ((keysDown & HidNpadButton_X) != 0) {
        begin_fetch();
        return true;
    }
    return false;
}

}  // namespace nextendo
