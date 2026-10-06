#include "gui.hpp"

#include <algorithm>

namespace nextendo {
namespace {

// The panel scrolls, but there is no point listing every title the network
// tracks; the website's status page shows a comparable subset.
constexpr std::size_t kMaxGameRows = 40;

// libtesla picks its font from the console's system language: when that is
// Chinese (simplified or traditional) it loads Nintendo's Chinese shared font
// and uses it for any glyph the standard font lacks. So the strings below render
// as long as the console's language is set to Chinese.
constexpr const char* kTitle = "Nextendo \u7f51\u7edc";           // Nextendo 网络
constexpr const char* kSubtitle = "\u5728\u7ebf\u4eba\u6570";     // 在线人数
constexpr const char* kHeaderNow = "\u5f53\u524d\u5728\u7ebf";    // 当前在线

constexpr const char* kTotalLabel = "\u5728\u7ebf\u4eba\u6570";   // 在线人数
constexpr const char* kStatusLabel = "\u72b6\u6001";              // 状态
constexpr const char* kApiLabel = "Nextendo API";

constexpr const char* kStatusWaiting = "\u7b49\u5f85\u6570\u636e";      // 等待数据
constexpr const char* kStatusUpToDate = "\u5df2\u66f4\u65b0";            // 已更新
constexpr const char* kStatusBadData = "\u6570\u636e\u5f02\u5e38";       // 数据异常
constexpr const char* kStatusNoService = "\u7f51\u7edc\u670d\u52a1\u4e0d\u53ef\u7528";  // 网络服务不可用
constexpr const char* kStatusFailed = "\u8bf7\u6c42\u5931\u8d25";        // 请求失败

constexpr const char* kApiOk = "\u6b63\u5e38";        // 正常
constexpr const char* kApiDegraded = "\u5f02\u5e38";  // 异常
constexpr const char* kApiChecking = "\u68c0\u67e5\u4e2d";  // 检查中

constexpr const char* kGamesPrefix = "\u6e38\u620f";  // 游戏
constexpr const char* kNobodyPlaying = "\u5f53\u524d\u65e0\u4eba\u5728\u7ebf";  // 当前无人在线

}  // namespace

GuiMain::GuiMain(FetchJob* job) : job_(job) {}

GuiMain::~GuiMain() = default;

tsl::elm::Element* GuiMain::createUI() {
    auto* frame = new tsl::elm::OverlayFrame(kTitle, kSubtitle);
    auto* list = new tsl::elm::List();

    if (job_ == nullptr) {
        // Networking never came up; say so instead of waiting for data.
        list->addItem(new tsl::elm::CategoryHeader(kHeaderNow));
        list->addItem(new tsl::elm::ListItem(kStatusLabel, kStatusNoService));
        frame->setContent(list);
        return frame;
    }

    // The transfer happens HERE, before anything is drawn.
    //
    // libtesla calls changeTo() -> createUI() after initScreen(), so the
    // renderer is ready, and the panel is not on screen yet. Running the request
    // at this point means the outcome is already in hand when the rows are
    // built, with no dependence on which callback runs when.
    //
    // Fetching from onShow() instead was tried and the panel came up empty: the
    // data arrived after createUI() had already read the job.
    job_->start();

    const FetchOutcome outcome = job_->result();
    const bool failed = (job_->state() == FetchState::Failed);
    const OnlineCounts* counts = failed ? nullptr : &outcome.counts;

    list->addItem(new tsl::elm::CategoryHeader(kHeaderNow));
    list->addItem(new tsl::elm::ListItem(
        kTotalLabel, counts != nullptr ? std::to_string(counts->total) : "--"));

    // ListItem's constructor takes only (text, value); the "faint" style is set
    // afterwards with setValue, so it cannot be passed here.
    std::string status_text;
    bool status_faint = false;
    if (failed) {
        status_text = outcome.error.empty() ? kStatusFailed : outcome.error;
        status_faint = true;
    } else if (counts == nullptr || job_->state() == FetchState::Idle) {
        status_text = kStatusWaiting;
        status_faint = true;
    } else if (counts->plausible) {
        status_text = kStatusUpToDate;
    } else {
        status_text = kStatusBadData;
        status_faint = true;
    }
    auto* statusItem = new tsl::elm::ListItem(kStatusLabel, status_text);
    statusItem->setValue(status_text, status_faint);
    // Selecting this row and pressing A reloads the panel, same as X.
    //
    // The refresh touches only the long-lived FetchJob, never this Gui, so
    // calling tsl::changeTo() from here is safe: changeTo() replaces the current
    // Gui, and nothing inside this object is read afterwards. A listener must
    // always be set because Element::onClick() calls m_clickListener
    // unconditionally and an empty std::function would abort.
    statusItem->setClickListener([job = job_](u64 keys) {
        if ((keys & HidNpadButton_A) != 0) {
            // start() does nothing while a transfer is already running, and the
            // replacement Gui renders whatever the job published.
            if (job != nullptr) job->start();
            // Rebuild the panel from scratch rather than mutating the live list.
            tsl::changeTo<GuiMain>(job);
            return true;
        }
        return false;
    });
    list->addItem(statusItem);

    list->addItem(new tsl::elm::ListItem(
        kApiLabel,
        counts == nullptr ? kApiChecking : (outcome.health_ok ? kApiOk : kApiDegraded)));

    if (counts == nullptr) {
        list->addItem(new tsl::elm::CategoryHeader(kGamesPrefix));
        list->addItem(new tsl::elm::ListItem(kStatusWaiting));
        frame->setContent(list);
        return frame;
    }

    std::vector<GameEntry> games = counts->games;
    // The API already sorts by player count, but sorting here keeps the display
    // correct if that ever changes.
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

    frame->setContent(list);
    return frame;
}

}  // namespace nextendo
