// Nextendo player-count overlay - minimal build.
//
// Written from scratch against probe-ovl, which is the only overlay in this
// repository that has never crashed on the target console. Its defining
// properties are kept exactly:
//
//   * the Gui builds a static list once, in createUI();
//   * no worker thread, no atexit hook, no list rebuilding;
//   * the same compiler and linker flags as probe-ovl (no -mtp=soft, no
//     -Wl,--gc-sections), because that is the configuration proven to run here.
//
// The one addition is the fetch, which happens in createUI() - after
// initScreen() and before anything is drawn, so the numbers are already in hand
// when the rows are built.
//
// Services are reintroduced one at a time. Version 2.0.0 called none of them and
// did not crash on exit, which confirmed that the exit crash came from those
// calls; it showed no data, so name resolution needs at least one. This build
// adds back only an `sm:` session, held for the overlay's whole lifetime - the
// one arrangement in which name resolution has ever worked here.

#define TESLA_INIT_IMPL
#include <tesla.hpp>

#include <curl/curl.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

constexpr const char* kCountsUrl = "https://nextendo.network/api/online-counts";
constexpr std::size_t kMaxBodyBytes = 96 * 1024;

struct Game {
    std::string name;
    int players = 0;
};

struct Counts {
    bool ok = false;
    int total = 0;
    std::string error;
    std::vector<Game> games;
};

std::size_t write_cb(char* ptr, std::size_t size, std::size_t nmemb, void* userdata) {
    auto* body = static_cast<std::string*>(userdata);
    const std::size_t bytes = size * nmemb;
    if (body->size() + bytes > kMaxBodyBytes) return 0;
    body->append(ptr, bytes);
    return bytes;
}

Counts fetch() {
    Counts result;

    CURL* curl = curl_easy_init();
    if (curl == nullptr) {
        result.error = "curl init failed";
        return result;
    }

    std::string body;
    curl_easy_setopt(curl, CURLOPT_URL, kCountsUrl);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "nextendo-ovl/2.0");
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 8L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 20L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    curl_easy_setopt(curl, CURLOPT_IPRESOLVE, CURL_IPRESOLVE_V4);

    const CURLcode rc = curl_easy_perform(curl);
    long http = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http);
    curl_easy_cleanup(curl);

    if (rc != CURLE_OK) {
        result.error = curl_easy_strerror(rc);
        return result;
    }
    if (http != 200) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "HTTP %ld", http);
        result.error = buf;
        return result;
    }

    // The payload is
    //   {"jeux":[{"nom":"...","joueurs":N},...],"counts":{...},"noms":{...}}
    //
    // The total comes from sum(jeux[].joueurs), which is what the website shows.
    // It is deliberately not sum(counts): a title that exists in several regions
    // repeats its player count once per regional title id, so that would
    // double-count.
    //
    // Scanned rather than parsed with a JSON library: the shape is fixed and this
    // keeps the build free of extra dependencies.
    std::size_t pos = body.find("\"jeux\"");
    if (pos == std::string::npos) {
        result.error = "unexpected response";
        return result;
    }

    while (true) {
        const std::size_t name_key = body.find("\"nom\"", pos);
        if (name_key == std::string::npos) break;

        const std::size_t value_start = body.find('"', name_key + 5);
        if (value_start == std::string::npos) break;
        const std::size_t value_end = body.find('"', value_start + 1);
        if (value_end == std::string::npos) break;

        const std::size_t players_key = body.find("\"joueurs\"", value_end);
        if (players_key == std::string::npos) break;
        const std::size_t colon = body.find(':', players_key);
        if (colon == std::string::npos) break;

        Game game;
        game.name = body.substr(value_start + 1, value_end - value_start - 1);
        game.players = std::atoi(body.c_str() + colon + 1);

        if (!game.name.empty()) {
            result.total += game.players;
            result.games.push_back(game);
        }
        pos = colon + 1;
    }

    if (result.games.empty()) {
        result.error = "unexpected response";
        return result;
    }

    result.ok = true;
    return result;
}

// Shortens a game name without splitting a multi-byte UTF-8 character, which
// would render as a broken glyph.
std::string utf8_prefix(const std::string& src, std::size_t max_bytes) {
    if (src.size() <= max_bytes) return src;
    std::size_t end = max_bytes;
    while (end > 0 && (static_cast<unsigned char>(src[end]) & 0xC0) == 0x80) --end;
    return src.substr(0, end);
}

class MainGui : public tsl::Gui {
public:
    virtual tsl::elm::Element* createUI() override {
        const Counts counts = fetch();

        auto* frame = new tsl::elm::OverlayFrame(
            "Nextendo \u7f51\u7edc",      // Nextendo 网络
            "\u5728\u7ebf\u4eba\u6570");  // 在线人数
        auto* list = new tsl::elm::List();

        list->addItem(new tsl::elm::CategoryHeader("\u5f53\u524d\u5728\u7ebf"));  // 当前在线
        list->addItem(new tsl::elm::ListItem(
            "\u5728\u7ebf\u4eba\u6570",   // 在线人数
            counts.ok ? std::to_string(counts.total) : std::string("--")));

        // ListItem takes (text, value) only; the faint style is applied after.
        // The error text is kept in a local so setValue does not need a getter.
        std::string status_text;
        if (counts.ok) {
            status_text = "\u5df2\u66f4\u65b0";  // 已更新
        } else if (counts.error.empty()) {
            status_text = "\u5931\u8d25";        // 失败
        } else {
            status_text = counts.error;
        }
        auto* status = new tsl::elm::ListItem("\u72b6\u6001", status_text);  // 状态
        status->setValue(status_text, !counts.ok);
        list->addItem(status);

        if (counts.ok) {
            std::vector<Game> games = counts.games;
            std::sort(games.begin(), games.end(), [](const Game& a, const Game& b) {
                if (a.players != b.players) return a.players > b.players;
                return a.name < b.name;
            });

            list->addItem(new tsl::elm::CategoryHeader(
                std::string("\u6e38\u620f (") + std::to_string(games.size()) + ")"));  // 游戏

            const std::size_t shown = std::min<std::size_t>(games.size(), 40);
            for (std::size_t i = 0; i < shown; ++i) {
                list->addItem(new tsl::elm::ListItem(
                    utf8_prefix(games[i].name, 30),
                    std::to_string(games[i].players)));
            }
        }

        frame->setContent(list);
        return frame;
    }
};

class NextendoOverlay : public tsl::Overlay {
public:
    virtual void initServices() override
    {
        // 1. curl's global state has to be set up before an easy handle is used.
        //    That is library state rather than a system service.
        curl_global_init(CURL_GLOBAL_DEFAULT);

        // 2. An `sm:` session held for the overlay's whole lifetime.
        //
        //    This is the first system service to come back, because it is the
        //    only arrangement in which name resolution has ever worked here:
        //    libnx's sfdnsres resolver initialises lazily on the first
        //    getaddrinfo and needs `sm:` at that moment. Opening a session only
        //    around the request was tried repeatedly and always failed with
        //    "Couldn't resolve host name".
        //
        //    initServices() runs inside libtesla's doWithSmSession, so this takes
        //    a second reference on an already-open session; service guards are
        //    reference counted, so it is additive. It is deliberately never
        //    closed - exitServices() stays empty.
        smInitialize();
    }

    // Empty, exactly as in probe-ovl.
    virtual void exitServices() override {}

    virtual std::unique_ptr<tsl::Gui> loadInitialGui() override {
        return initially<MainGui>();
    }
};

}  // namespace

int main(int argc, char** argv) {
    return tsl::loop<NextendoOverlay>(argc, argv);
}
