#include "main_menu.hpp"

#include <algorithm>

// libtesla loads Nintendo's Chinese shared font when the console's system
// language is Chinese, and uses it for any glyph the standard font lacks, so
// these strings render as long as the console is set to Chinese.
static const char* kTitle = "Nextendo \u7f51\u7edc";           // Nextendo 网络
static const char* kSubtitle = "\u5728\u7ebf\u4eba\u6570";     // 在线人数

static const char* kHeaderNow = "\u5f53\u524d\u5728\u7ebf";    // 当前在线
static const char* kTotalLabel = "\u5728\u7ebf\u4eba\u6570";   // 在线人数
static const char* kStatusLabel = "\u72b6\u6001";              // 状态
static const char* kApiLabel = "Nextendo API";
static const char* kGamesPrefix = "\u6e38\u620f";              // 游戏

static const char* kOkText = "\u5df2\u66f4\u65b0";             // 已更新
static const char* kBadData = "\u6570\u636e\u5f02\u5e38";      // 数据异常
static const char* kFailed = "\u8bf7\u6c42\u5931\u8d25";       // 请求失败
static const char* kApiOk = "\u6b63\u5e38";                    // 正常
static const char* kApiBad = "\u5f02\u5e38";                   // 异常
static const char* kNobody = "\u5f53\u524d\u65e0\u4eba\u5728\u7ebf";  // 当前无人在线

tsl::elm::Element* MainMenu::createUI()
{
    // The request happens here: after initScreen(), before the panel is drawn.
    nextendo::fetch_and_store();

    auto* frame = new tsl::elm::OverlayFrame(kTitle, kSubtitle);
    auto* list = new tsl::elm::List();

    const nextendo::Outcome& result = nextendo::outcome();

    list->addItem(new tsl::elm::CategoryHeader(kHeaderNow));

    const bool have_counts = result.ok;
    list->addItem(new tsl::elm::ListItem(
        kTotalLabel,
        have_counts ? std::to_string(result.counts.total) : std::string("--")));

    std::string status_text;
    bool status_faint = false;
    if (!result.ok) {
        status_text = result.error.empty() ? kFailed : result.error;
        status_faint = true;
    } else if (!result.counts.plausible) {
        status_text = kBadData;
        status_faint = true;
    } else {
        status_text = kOkText;
    }

    // ListItem takes (text, value) only; the faint style is applied afterwards.
    auto* status_item = new tsl::elm::ListItem(kStatusLabel, status_text);
    status_item->setValue(status_text, status_faint);
    // Selecting this row and pressing A refreshes: the request is redone and the
    // Gui is replaced with a freshly built one. A listener must always be set,
    // because Element::onClick() calls it unconditionally and an empty
    // std::function would abort.
    status_item->setClickListener([](u64 keys) {
        if ((keys & HidNpadButton_A) != 0) {
            tsl::changeTo<MainMenu>();
            return true;
        }
        return false;
    });
    list->addItem(status_item);

    list->addItem(new tsl::elm::ListItem(
        kApiLabel, result.api_ok ? kApiOk : kApiBad));

    if (!have_counts) {
        list->addItem(new tsl::elm::CategoryHeader(kGamesPrefix));
        list->addItem(new tsl::elm::ListItem(kFailed));
        frame->setContent(list);
        return frame;
    }

    std::vector<nextendo::GameEntry> games = result.counts.games;
    std::sort(games.begin(), games.end(), [](const nextendo::GameEntry& a,
                                            const nextendo::GameEntry& b) {
        if (a.players != b.players) return a.players > b.players;
        return a.name < b.name;
    });

    list->addItem(new tsl::elm::CategoryHeader(
        std::string(kGamesPrefix) + " (" + std::to_string(games.size()) + ")"));

    const std::size_t shown = std::min<std::size_t>(games.size(), 40);
    for (std::size_t i = 0; i < shown; ++i) {
        list->addItem(new tsl::elm::ListItem(
            nextendo::utf8_truncate(games[i].name, 30),
            std::to_string(games[i].players)));
    }
    if (games.empty()) {
        list->addItem(new tsl::elm::ListItem(kNobody));
    }

    frame->setContent(list);
    return frame;
}
