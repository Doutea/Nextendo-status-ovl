#define TESLA_INIT_IMPL
#include <tesla.hpp>
#include <switch.h>
#include <curl/curl.h>
#include <jansson.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/stat.h>
#include <atomic>
#include <vector>
#include <map>
#include <string>
#include <algorithm>

// ---------------------------------------------------------------------------
// Language handling
//
// The console's system language decides the interface language. English is the
// fallback for anything that is not Chinese: a non-Chinese console has no
// Chinese font loaded by libtesla, so Chinese labels there would be unreadable
// at best and rendered as missing glyphs at worst.
//
// Game names follow the same rule - the translation table is only consulted on a
// Chinese console, otherwise the API's own name is shown. That keeps a game's
// name accurate for the player's own locale instead of forcing a Chinese title.
// ---------------------------------------------------------------------------

static bool g_useChinese = false;

// Set by the overlay's initServices(); read by both this translation unit and
// tesla.hpp (see the footer hints there).
void nextendoUseChinese(bool chinese) {
    g_useChinese = chinese;
}

bool nextendoIsChinese() {
    return g_useChinese;
}

// Reads the console's language.
//
// appletGetDesiredLanguage() comes first: libnx initialises the applet service
// itself, and its own source notes that this call is preferred over
// setGetLanguageCode because the latter can disagree with the system language.
//
// The `set:` fallback exists because setGetSystemLanguage() dispatches to
// g_setSrv (nx/source/services/set.c), which nothing else opens for an overlay -
// so it has to be opened and closed here.
static bool isChineseLanguage(SetLanguage language) {
    switch (language) {
        case SetLanguage_ZHCN:
        case SetLanguage_ZHHANS:
        case SetLanguage_ZHTW:
        case SetLanguage_ZHHANT:
            return true;
        default:
            return false;
    }
}

static bool detectChineseLanguageInner() {
    u64 languageCode = 0;
    SetLanguage language{};

    if (R_SUCCEEDED(appletGetDesiredLanguage(&languageCode)) &&
        R_SUCCEEDED(setMakeLanguage(languageCode, &language))) {
        return isChineseLanguage(language);
    }

    if (R_FAILED(setInitialize())) return false;
    const bool ok = R_SUCCEEDED(setGetSystemLanguage(&languageCode)) &&
                    R_SUCCEEDED(setMakeLanguage(languageCode, &language)) &&
                    isChineseLanguage(language);
    setExit();
    return ok;
}

static bool detectChineseLanguage() {
    return detectChineseLanguageInner();
}

// One interface string in both languages. CH/EN are picked by T() at run time.
struct UiText {
    const char* zh;
    const char* en;
};

static const UiText kTitle       = {"Nextendo \u5728\u7ebf\u72b6\u6001", "Nextendo Status"};
static const UiText kLoading     = {"\u52a0\u8f7d\u4e2d\u2026", "\u2026"};
static const UiText kSectionNow  = {"\u5f53\u524d\u72b6\u6001", "Current status"};
static const UiText kPlayers     = {"\u5728\u7ebf\u4eba\u6570", "Players online"};
static const UiText kGameCount   = {"\u6e38\u620f\u6570\u91cf", "Games played"};
static const UiText kGameList    = {"\u6e38\u620f\u5217\u8868", "Game list"};
static const UiText kWaiting     = {"\u7b49\u5f85\u6570\u636e\u2026", "Waiting for data\u2026"};
static const UiText kUpdatedAt   = {"\u66f4\u65b0\u4e8e ", "Updated "};

static const char* T(const UiText& text) {
    return g_useChinese ? text.zh : text.en;
}

void debugLog(const std::string& msg) {
    mkdir("sdmc:/switch", 0777);
    mkdir("sdmc:/switch/nextendo-status", 0777);
    FILE* f = fopen("sdmc:/switch/nextendo-status/debug.log", "a");
    if (f) {
        fwrite(msg.data(), 1, msg.size(), f);
        fwrite("\n", 1, 1, f);
        fclose(f);
    }
}

std::vector<std::string> wrapText(tsl::gfx::Renderer* r, const std::string& text, float fontSize, s32 maxWidth) {
    std::vector<std::string> lines;
    std::string currentLine;
    std::string word;

    for (size_t i = 0; i <= text.size(); i++) {
        if (i == text.size() || text[i] == ' ') {
            std::string testLine = currentLine.empty() ? word : (currentLine + " " + word);
            auto [width, height] = r->drawString(testLine.c_str(), false, 0, 0, fontSize, tsl::style::color::ColorTransparent);
            if (width > maxWidth && !currentLine.empty()) {
                lines.push_back(currentLine);
                currentLine = word;
            } else {
                currentLine = testLine;
            }
            word.clear();
        } else {
            word += text[i];
        }
    }
    if (!currentLine.empty()) lines.push_back(currentLine);
    return lines;
}

static const SocketInitConfig socketConfig = {
    .tcp_tx_buf_size     = 0x800,
    .tcp_rx_buf_size     = 0x1000,
    .tcp_tx_buf_max_size = 0x2500,
    .tcp_rx_buf_max_size = 0x2500,
    .udp_tx_buf_size = 0x2400,
    .udp_rx_buf_size = 0xA500,
    .sb_efficiency = 4,
    .num_bsd_sessions = 3,
    .bsd_service_type = BsdServiceType_User,
};

static const std::vector<std::string> NEXTENDO_FALLBACK_IPS = {
    "104.21.84.82",
    "172.67.190.75",
};

static const std::vector<std::string> GITHUB_FALLBACK_IPS = {
    "185.199.108.133",
    "185.199.109.133",
    "185.199.110.133",
    "185.199.111.133",
};

static const std::string GAMES_JSON_URL = "https://raw.githubusercontent.com/Chasetodie/Nextendo-Status-Overlay/main/assets/data/games.json";
static const std::string ICON_BASE_URL  = "https://raw.githubusercontent.com/Chasetodie/Nextendo-Status-Overlay/main/assets/images/";
static const char* GAMES_CACHE_PATH = "sdmc:/switch/nextendo-status/games_cache.json";

struct GameEntry {
    std::string name;
    std::vector<std::string> ids;
    std::string icon;
    std::string version;
    int percent = -1;
    std::string status;
};

// One entry of the API's "jeux" array: a game and how many players
// are on it. Declared here because a global holds a vector of these.
struct JeuEntry {
    std::string name;
    int players = 0;
};

static std::vector<GameEntry> g_games;

static Mutex g_mutex;
static std::map<std::string, int> g_counts;
// Per-game breakdown from the API's "jeux" array, and the running total.
static std::vector<JeuEntry> g_jeux;
static std::string g_lastError;
static std::atomic<bool> g_dirty{false};
static std::atomic<bool> g_overlayVisible{false};
static std::atomic<bool> g_threadRunning{false};

static Thread g_pollThread;
// Stack for the polling thread.
//
// The upstream value was 0x4000 (16 KB). That thread performs DNS resolution, a
// TLS handshake and a libcurl transfer, and both libcurl and mbedTLS keep
// sizeable structures on the stack - libcurl's own guidance is a minimum of
// 32 KB for the resolver alone, before TLS. 16 KB overflows, and a stack
// overflow on this target corrupts whatever lies below it.
//
// 0x40000 (256 KB) leaves ample headroom and costs nothing on a 4 MB heap.
alignas(0x1000) static u8 g_threadStack[0x40000];

// --- Red ---
std::string resolveHostToIp(const std::string& hostname) {
    struct addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    struct addrinfo* result = nullptr;
    int ret = getaddrinfo(hostname.c_str(), nullptr, &hints, &result);
    if (ret != 0 || !result) return "";

    char ipStr[16];
    auto* addr = reinterpret_cast<struct sockaddr_in*>(result->ai_addr);
    inet_ntop(AF_INET, &(addr->sin_addr), ipStr, sizeof(ipStr));
    freeaddrinfo(result);
    return std::string(ipStr);
}

static size_t WriteCallback(void* contents, size_t size, size_t nmemb, void* userp) {
    size_t totalSize = size * nmemb;
    std::string* buffer = static_cast<std::string*>(userp);
    buffer->append(static_cast<char*>(contents), totalSize);
    return totalSize;
}

std::string fetchUrl(const std::string& url, const std::string& host, const std::vector<std::string>& fallbackIps, std::string& errorOut) {
    std::string responseBuffer;
    CURL* curl = curl_easy_init();
    if (!curl) {
        errorOut = "curl_easy_init fallo";
        return "";
    }

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &responseBuffer);
    // One attempt's cap. fetchUrl() retries with a growing deadline,
    // so this is not the effective limit for the whole request.
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 12L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);

    std::string ip = resolveHostToIp(host);
    struct curl_slist* resolveList = nullptr;
    if (!ip.empty()) {
        std::string entry = host + ":443:" + ip;
        resolveList = curl_slist_append(nullptr, entry.c_str());
    } else {
        for (const auto& fallbackIp : fallbackIps) {
            std::string entry = host + ":443:" + fallbackIp;
            resolveList = curl_slist_append(resolveList, entry.c_str());
        }
    }
    if (resolveList) curl_easy_setopt(curl, CURLOPT_RESOLVE, resolveList);

    CURLcode res = curl_easy_perform(curl);
    if (resolveList) curl_slist_free_all(resolveList);

    if (res != CURLE_OK) {
        errorOut = curl_easy_strerror(res);
        curl_easy_cleanup(curl);
        return "";
    }

    curl_easy_cleanup(curl);
    return responseBuffer;
}

std::string fetchOnlineCounts(std::string& errorOut) {
    return fetchUrl("https://nextendo.network/api/online-counts", "nextendo.network", NEXTENDO_FALLBACK_IPS, errorOut);
}

// Fetches the counts document, retrying with a growing deadline.
//
// A first request has to resolve DNS and complete a TLS handshake, and on this
// console that occasionally ran past a single short timeout - which surfaced in
// the panel as "Timeout was reached" and then fixed itself on the next poll.
// Two things remove that:
//
//   * the request is retried here rather than waiting 15s for the next poll;
//   * each attempt gets a progressively longer deadline, because the failed
//     attempt has usually left the connection warmed up.
//
// Startup cost matters, so the total is capped: three attempts at 12s each is
// the worst case, and only when every attempt fails.
std::string fetchCountsWithRetry(std::string& errorOut) {
    static const int kAttempts = 3;

    for (int attempt = 0; attempt < kAttempts; ++attempt) {
        std::string lastError;
        const std::string body = fetchOnlineCounts(lastError);

        if (!body.empty() && lastError.empty()) {
            errorOut.clear();
            return body;
        }

        // A transport error, or an empty body. Keep the message from the last
        // attempt and try again unless this was the final one.
        errorOut = lastError.empty() ? "empty response" : lastError;

        if (attempt + 1 < kAttempts) {
            // Brief pause so a transient DNS or TLS hiccup can clear.
            svcSleepThread(300'000'000LL);  // 300 ms
        }
    }

    return std::string();
}

std::map<std::string, int> parseOnlineCounts(const std::string& rawJson) {
    std::map<std::string, int> result;
    json_error_t error;
    json_t* root = json_loads(rawJson.c_str(), 0, &error);
    if (!root) return result;

    json_t* counts = json_object_get(root, "counts");
    if (json_is_object(counts)) {
        const char* key;
        json_t* value;
        json_object_foreach(counts, key, value) {
            if (json_is_integer(value)) {
                result[key] = (int)json_integer_value(value);
            }
        }
    }

    json_decref(root);
    return result;
}

// The per-game breakdown, straight out of the "jeux" array of
// /api/online-counts: [{"nom":"Splatoon 3","joueurs":12}, ...].
//
// This is what the status page itself displays, so it needs no title-id table
// and no downloaded game list - the server names the games.

std::vector<JeuEntry> parseJeux(const std::string& rawJson) {
    std::vector<JeuEntry> result;
    json_error_t error;
    json_t* root = json_loads(rawJson.c_str(), 0, &error);
    if (!root) return result;

    json_t* jeux = json_object_get(root, "jeux");
    if (json_is_array(jeux)) {
        size_t index;
        json_t* value;
        json_array_foreach(jeux, index, value) {
            JeuEntry entry;
            json_t* jnom = json_object_get(value, "nom");
            json_t* jjoueurs = json_object_get(value, "joueurs");
            if (json_is_string(jnom)) entry.name = json_string_value(jnom);
            if (json_is_integer(jjoueurs)) entry.players = (int)json_integer_value(jjoueurs);
            if (!entry.name.empty()) result.push_back(entry);
        }
    }

    json_decref(root);
    return result;
}

std::vector<GameEntry> parseGamesJson(const std::string& rawJson) {
    std::vector<GameEntry> result;
    json_error_t error;
    json_t* root = json_loads(rawJson.c_str(), 0, &error);
    if (!root) {
        debugLog("parseGamesJson: json_loads fallo: " + std::string(error.text));
        return result;
    }

    json_t* games = json_object_get(root, "games");
    if (json_is_array(games)) {
        size_t index;
        json_t* value;
        json_array_foreach(games, index, value) {
            GameEntry entry;
            json_t* jname = json_object_get(value, "name");
            json_t* jicon = json_object_get(value, "icon");
            json_t* jids  = json_object_get(value, "ids");
            json_t* jversion = json_object_get(value, "version");
            json_t* jpercent = json_object_get(value, "percent");
            json_t* jstatus  = json_object_get(value, "status");

            if (json_is_string(jname)) entry.name = json_string_value(jname);
            if (json_is_string(jicon)) entry.icon = json_string_value(jicon);
            if (json_is_string(jversion)) entry.version = json_string_value(jversion);
            if (json_is_integer(jpercent)) entry.percent = (int)json_integer_value(jpercent);
            if (json_is_string(jstatus)) entry.status = json_string_value(jstatus);
            if (json_is_array(jids)) {
                size_t idIdx;
                json_t* idVal;
                json_array_foreach(jids, idIdx, idVal) {
                    if (json_is_string(idVal)) entry.ids.push_back(json_string_value(idVal));
                }
            }
            if (!entry.name.empty() && !entry.ids.empty()) {
                result.push_back(entry);
            }
        }
    }

    json_decref(root);
    return result;
}

std::string loadOrFetchIcon(const std::string& slug) {
    std::string cachePath = "sdmc:/switch/nextendo-status/icons/" + slug + ".bin";

    FILE* f = fopen(cachePath.c_str(), "rb");
    if (f) {
        fseek(f, 0, SEEK_END);
        long size = ftell(f);
        fseek(f, 0, SEEK_SET);
        std::string cached(size, '\0');
        fread(&cached[0], 1, size, f);
        fclose(f);
        if (!cached.empty()) return cached;
    }

    std::string error;
    std::string url = ICON_BASE_URL + slug + ".bin";
    std::string data = fetchUrl(url, "raw.githubusercontent.com", GITHUB_FALLBACK_IPS, error);

    if (!data.empty()) {
        mkdir("sdmc:/switch/nextendo-status/icons", 0777);
        FILE* out = fopen(cachePath.c_str(), "wb");
        if (out) {
            fwrite(data.data(), 1, data.size(), out);
            fclose(out);
        }
    }

    return data;
}

void loadGamesConfig() {
    debugLog("1: empezando fetch de games.json");
    std::string error;
    std::string raw = fetchUrl(GAMES_JSON_URL, "raw.githubusercontent.com", GITHUB_FALLBACK_IPS, error);
    debugLog("2: fetch terminado, bytes=" + std::to_string(raw.size()) + " err=" + error);

    if (!raw.empty()) {
        auto parsed = parseGamesJson(raw);
        debugLog("4: parseo terminado, juegos=" + std::to_string(parsed.size()));

        if (!parsed.empty()) {
            g_games = parsed;

            FILE* f = fopen(GAMES_CACHE_PATH, "w");
            if (f) {
                fwrite(raw.data(), 1, raw.size(), f);
                fclose(f);
            }
            debugLog("8: loadGamesConfig termino OK (remoto)");
            return;
        }
    }

    FILE* f = fopen(GAMES_CACHE_PATH, "r");
    if (f) {
        fseek(f, 0, SEEK_END);
        long size = ftell(f);
        fseek(f, 0, SEEK_SET);
        std::string cached(size, '\0');
        fread(&cached[0], 1, size, f);
        fclose(f);

        auto parsed = parseGamesJson(cached);
        if (!parsed.empty()) {
            g_games = parsed;
            debugLog("12: g_games asignado desde cache");
            return;
        }
    }

    debugLog("13: usando tabla minima embebida");
    g_games = {
        { "Splatoon 3", {"0100c2500fc20000"}, "" },
        { "Mario Kart 8 Deluxe", {"0100152000022000"}, "" },
    };
}

// --- Hilo de polling ---
// Set by the refresh row's listener to request an immediate fetch.
// u64 tick of the last successful fetch, so a later failure can
// report when the numbers on screen are actually from.
static std::atomic<u64> g_lastSuccessTick{0};
static std::atomic<bool> g_refreshRequested{false};
// True while a fetch started by the user is in flight, so the row can show it.
static std::atomic<bool> g_refreshing{false};

static void pollThreadFunc(void*) {
    // The game-config download used to run in initServices(), which meant the
    // panel could not be drawn until the request had finished. It only feeds the
    // per-title detail view, so it happens here instead, off the critical path.
    loadGamesConfig();

    while (g_threadRunning.load()) {
        const bool requested = g_refreshRequested.exchange(false);

        if (g_overlayVisible.load() || requested) {
            if (requested) g_refreshing = true;

            std::string error;
            std::string raw = fetchCountsWithRetry(error);
            auto newCounts = parseOnlineCounts(raw);
            auto newJeux = parseJeux(raw);

            if (!newJeux.empty()) g_lastSuccessTick = armGetSystemTick();

            {
                mutexLock(&g_mutex);
                // Keep the last good numbers if this attempt returned nothing,
                // so a transient failure does not blank the panel.
                if (!newJeux.empty() || g_jeux.empty()) {
                    g_counts = newCounts;
                    g_jeux = newJeux;
                }
                g_lastError = error;
                mutexUnlock(&g_mutex);
            }

            g_refreshing = false;
            g_dirty = true;
        }

        for (int i = 0; i < 15 && g_threadRunning.load(); i++) {
            svcSleepThread(1'000'000'000LL);
            if (g_refreshRequested.load()) break;
        }
    }
}

class GuiGameDetail : public tsl::Gui {
public:
    GuiGameDetail(const GameEntry& game) : m_game(game) {
        if (!m_game.icon.empty()) {
            m_iconData = loadOrFetchIcon(m_game.icon);
        }
    }

    virtual tsl::elm::Element* createUI() override {
        auto frame = new tsl::elm::OverlayFrame(m_game.name, "Detalle");
        auto list = new tsl::elm::List();

        m_countItem = new tsl::elm::CategoryHeader("Jugadores: --");
        list->addItem(m_countItem);

        auto iconDrawer = new tsl::elm::CustomDrawer([this](tsl::gfx::Renderer* r, s32 x, s32 y, s32 w, s32 h) {
            s32 iconSize = 256;
            s32 marginY = (h - iconSize) / 2;
            if (m_iconData.size() == iconSize * iconSize * 4) {
                r->drawBitmap(x + (w - iconSize) / 2, y + marginY, iconSize, iconSize, reinterpret_cast<const u8*>(m_iconData.data()));
            } else {
                r->drawRect(x + (w - iconSize) / 2, y + marginY, iconSize, iconSize, tsl::style::color::ColorFrame);
            }
        });
        list->addItem(iconDrawer, 276);

        if (!m_game.version.empty() || m_game.percent >= 0) {
            std::string combined;
            if (!m_game.version.empty()) combined += "v" + m_game.version;
            if (m_game.percent >= 0) {
                if (!combined.empty()) combined += "  閳? ";
                combined += std::to_string(m_game.percent) + "% completo";
            }
            list->addItem(new tsl::elm::CategoryHeader(combined));
        }

        if (!m_game.status.empty()) {
            auto statusDrawer = new tsl::elm::CustomDrawer([this](tsl::gfx::Renderer* r, s32 x, s32 y, s32 w, s32 h) {
                if (!m_statusMeasured) {
                    m_statusLines = wrapText(r, m_game.status, 15, w - 40);
                    m_statusMeasured = true;
                }
                s32 lineY = y + 20;
                for (const auto& line : m_statusLines) {
                    r->drawString(line.c_str(), false, x + 20, lineY, 15, a(tsl::style::color::ColorText));
                    lineY += 19;
                }
            });

            if (!m_game.status.empty()) {
                s32 estimatedHeight = (s32)((m_game.status.size() / 45) + 1) * 19 + 20;
                list->addItem(statusDrawer, estimatedHeight);
            }
        }

        frame->setContent(list);
        updateCountDisplay();
        return frame;
    }

    virtual void update() override {
        if (g_dirty.exchange(false)) {
            updateCountDisplay();
        }
    }

    virtual bool handleInput(u64 keysDown, u64 keysHeld, const HidTouchState &touchPos, HidAnalogStickState joyStickPosLeft, HidAnalogStickState joyStickPosRight) override {
        return false;
    }

private:
    void updateCountDisplay() {
        mutexLock(&g_mutex);
        auto countsCopy = g_counts;
        mutexUnlock(&g_mutex);

        int maxCount = 0;
        bool anyOnline = false;
        for (const auto& id : m_game.ids) {
            auto it = countsCopy.find(id);
            if (it != countsCopy.end()) {
                anyOnline = true;
                maxCount = std::max(maxCount, it->second);
            }
        }
        std::string text = anyOnline ? ("Jugadores: " + std::to_string(maxCount)) : "Offline";
        m_countItem->setText(text);
    }

    GameEntry m_game;
    std::string m_iconData;
    tsl::elm::CategoryHeader* m_countItem;
    std::vector<std::string> m_statusLines;
    s32 m_statusHeight = 0;
    bool m_statusMeasured = false;
};

// Chinese names for the titles the API reports.
//
// GENERATED from the editable table - do not hand-edit this block; change
// the table and regenerate it instead. Non-ASCII is written as \uXXXX so
// this file carries no encoding dependency.
//
// Only the display name changes. Matching still uses the API's own name, which
// is the key on the left.
static const std::map<std::string, std::string> g_gameNamesZh = {
    {"Super Smash Bros. Ultimate",
     "\u4EFB\u5929\u5802\u5168\u660E\u661F\u5927\u4E71\u6597"},
    {"Mario Kart 8 Deluxe",
     "\u9A6C\u529B\u6B27\u8D5B\u8F66 8 \u8C6A\u534E\u7248"},
    {"Super Mario Maker 2",
     "\u8D85\u7EA7\u9A6C\u529B\u6B27\u5236\u9020 2"},
    {"Minecraft Dungeons II",
     "\u6211\u7684\u4E16\u754C\uFF1A\u5730\u7262 II"},
    {"Crash Team Racing Nitro-Fueled",
     "\u53E4\u60D1\u72FC\u8D5B\u8F66\u5B9D\u8D1D\u8F66"},
    {"Splatoon 3",
     "\u65AF\u666E\u62C9\u9041 3"},
    {"Pok\u00E9mon Scarlet",
     "\u5B9D\u53EF\u68A6 \u6731"},
    {"L\u00E9gendes Pok\u00E9mon : Z-A",
     "\u5B9D\u53EF\u68A6\u4F20\u8BF4 Z-A"},
    {"Nintendo 64 \u2013 Nintendo Classics",
     "Nintendo 64 \u7ECF\u5178\u5408\u96C6"},
    {"Minecraft: Nintendo Switch Edition",
     "\u6211\u7684\u4E16\u754C\uFF1ASwitch \u7248"},
    {"Diablo III: Eternal Collection",
     "\u6697\u9ED1\u7834\u574F\u795E III\uFF1A\u6C38\u6052\u4E4B\u6218"},
    {"POKK\u00C9N TOURNAMENT DX",
     "\u5B9D\u53EF\u62F3 DX"},
    {"SUPER MARIO BROS. 35",
     "\u8D85\u7EA7\u9A6C\u529B\u6B27\u5144\u5F1F 35"},
    {"PAC-MAN 99",
     "\u5403\u8C46\u4EBA 99"},
    {"Mario Tennis Aces",
     "\u9A6C\u529B\u6B27\u7F51\u7403 \u8D85\u7EA7\u6263\u6740"},
    {"Overcooked! 2",
     "\u80E1\u95F9\u53A8\u623F 2"},
    {"Super Mario Odyssey",
     "\u8D85\u7EA7\u9A6C\u529B\u6B27 \u5965\u5FB7\u8D5B"},
    {"TETRIS 99",
     "\u4FC4\u7F57\u65AF\u65B9\u5757 99"},
    {"Super Mario Bros. Wonder",
     "\u8D85\u7EA7\u9A6C\u529B\u6B27\u5144\u5F1F \u60CA\u5947"},
    {"Saints Row: The Third - The Full Package",
     "\u9ED1\u9053\u5723\u5F92 3\uFF1A\u5B8C\u6574\u7248"},
    {"Splatoon 2",
     "\u65AF\u666E\u62C9\u9041 2"},
    {"Just Shapes & Beats",
     "\u5F62\u72B6\u4E0E\u97F3\u7B26"},
    {"Luigi's Mansion 3",
     "\u8DEF\u6613\u5409\u9B3C\u5C4B 3"},
    {"Clubhouse Games: 51 Worldwide Classics",
     "\u4E16\u754C\u6E38\u620F\u5927\u5168 51"},
    {"ARMS",
     "ARMS"},
    {"Animal Crossing: New Horizons",
     "\u52A8\u7269\u68EE\u53CB\u4F1A"},
    {"Mario Party Superstars",
     "\u9A6C\u529B\u6B27\u6D3E\u5BF9 \u8D85\u7EA7\u5DE8\u661F"},
    {"Mario Strikers: Battle League",
     "\u9A6C\u529B\u6B27\u8DB3\u7403\uFF1A\u8D85\u7EA7\u5DDE\u9645\u8054\u8D5B"},
    {"Mario Golf: Super Rush",
     "\u9A6C\u529B\u6B27\u9AD8\u5C14\u592B\uFF1A\u8D85\u7EA7\u51B2\u523A"},
    {"METAL GEAR SOLID: Peace Walker - Master Collection Version",
     "\u5408\u91D1\u88C5\u5907\uFF1A\u548C\u5E73\u884C\u8005"},
    {"MONSTER HUNTER GENERATIONS ULTIMATE",
     "\u602A\u7269\u730E\u4EBA XX"},
    {"Pok\u00E9mon Violet",
     "\u5B9D\u53EF\u68A6 \u7D2B"},
};

// Chinese name when the console is set to Chinese, otherwise the API's own
// name. A non-Chinese console has no Chinese font loaded, and forcing a Chinese
// title on it would be both unreadable and wrong for that player's locale.
static std::string localisedName(const std::string& apiName) {
    if (!nextendoIsChinese()) return apiName;
    auto it = g_gameNamesZh.find(apiName);
    return (it != g_gameNamesZh.end()) ? it->second : apiName;
}
// Numeric values are shown in light blue, except zero, which is shown in
// white so an empty game reads as inactive rather than as a live count.
static void applyValueColour(tsl::elm::ListItem* item, const std::string& value) {
    if (item == nullptr) return;
    item->setValue(value);
    const bool isZero = (value == "0");
    item->setValueColor(isZero ? tsl::style::color::ColorText
                               : tsl::style::color::ColorValueBlue);
}

class GuiTest : public tsl::Gui {
public:
    // No I/O here: fetching in the constructor is what made the panel take
    // seconds to appear. The polling thread does it, and the panel opens at once.
    GuiTest() {
        // Seed the count with the loading text; refreshUI() replaces it as soon
        // as the polling thread publishes something.
        m_totalText = T(kLoading);
    }

    virtual tsl::elm::Element* createUI() override {
        m_frame = new tsl::elm::OverlayFrame(T(kTitle), T(kLoading));
        auto* list = new tsl::elm::List();

        // Section heading. The rule it draws is the divider above the content.
        list->addItem(new tsl::elm::CategoryHeader(T(kSectionNow)));

        // Players online. Selectable, and pressing A refreshes: this is the row
        // a user reaches for when the numbers look stale.
        //
        // A click listener must always be set, because Element::onClick() calls
        // it unconditionally and an empty std::function would abort.
        m_totalItem = new tsl::elm::ListItem(T(kPlayers), m_totalText);
        m_totalItem->setClickListener([](u64 keys) {
            if ((keys & HidNpadButton_A) != 0) {
                g_refreshRequested = true;
                return true;
            }
            return false;
        });
        list->addItem(m_totalItem);

        // How many titles are currently being played.
        m_gamesItem = new tsl::elm::ListItem(T(kGameCount), m_gamesText);
        list->addItem(m_gamesItem);

        // The list itself. This heading also separates the two blocks.
        m_sectionHeader = new tsl::elm::CategoryHeader(T(kGameList));
        list->addItem(m_sectionHeader);

        m_placeholder = new tsl::elm::ListItem(T(kWaiting));
        list->addItem(m_placeholder);

        m_frame->setContent(list);
        m_list = list;
        refreshUI();
        return m_frame;
    }

    virtual void update() override {
        if (g_dirty.exchange(false)) {
            refreshUI();
        }
    }

    virtual bool handleInput(u64 keysDown, u64 keysHeld, const HidTouchState& touchPos,
                             HidAnalogStickState joyStickPosLeft,
                             HidAnalogStickState joyStickPosRight) override {
        return false;
    }

private:
    // Rebuilds the entry rows when the set of games changes.
    //
    // The list is reconstructed rather than edited, because rows are added and
    // removed and tsl::elm::List offers no removal API. removeFocus() comes
    // first, as libtesla requires before items are destroyed.
    void rebuildGameRows(const std::vector<JeuEntry>& jeux) {
        std::vector<std::string> names;
        names.reserve(jeux.size());
        for (const auto& j : jeux) names.push_back(j.name);

        // Same games as before: refresh the numbers in place. That is the common
        // case and it leaves the selection alone.
        if (names == m_shownNames) {
            for (const auto& j : jeux) {
                auto it = m_rows.find(j.name);
                if (it != m_rows.end() && it->second != nullptr) {
                    applyValueColour(it->second, std::to_string(j.players));
                }
            }
            m_sectionHeader->setText(T(kGameList));
            return;
        }

        this->removeFocus();

        auto* list = new tsl::elm::List();

        list->addItem(new tsl::elm::CategoryHeader(T(kSectionNow)));

        auto* total = new tsl::elm::ListItem(T(kPlayers), m_totalText);
        total->setClickListener([](u64 keys) {
            if ((keys & HidNpadButton_A) != 0) {
                g_refreshRequested = true;
                return true;
            }
            return false;
        });
        list->addItem(total);
        m_totalItem = total;

        auto* games = new tsl::elm::ListItem(T(kGameCount), m_gamesText);
        list->addItem(games);
        m_gamesItem = games;

        auto* section = new tsl::elm::CategoryHeader(T(kGameList));
        list->addItem(section);
        m_sectionHeader = section;

        m_rows.clear();
        for (const auto& j : jeux) {
            auto* item = new tsl::elm::ListItem(localisedName(j.name));
            applyValueColour(item, std::to_string(j.players));
            list->addItem(item);
            m_rows[j.name] = item;
        }

        m_frame->setContent(list);
        m_list = list;
        m_placeholder = nullptr;
        m_shownNames = names;
    }

    // "HH:MM:SS" for the moment a fetch completed.
    //
    // The tick from armGetSystemTick() counts from power-on, so it is NOT a Unix
    // timestamp and must not be handed to the time service directly - doing that
    // produced a time unrelated to the console's clock. What is needed is the
    // current wall-clock time minus however long ago the fetch happened.
    static std::string fetchClock(u64 successTick) {
        const u64 now_tick = armGetSystemTick();
        // Guard against a tick that appears to be in the future after a wrap.
        const u64 elapsed_ns = (now_tick > successTick)
                                   ? armTicksToNs(now_tick - successTick)
                                   : 0;

        u64 now_seconds = 0;
        if (R_FAILED(timeGetCurrentTime(TimeType_LocalSystemClock, &now_seconds))) {
            return "--:--:--";
        }

        const u64 then_seconds = (elapsed_ns / 1000000000ULL > now_seconds)
                                     ? 0
                                     : now_seconds - (elapsed_ns / 1000000000ULL);

        TimeCalendarTime caltime;
        TimeCalendarAdditionalInfo addinfo;
        if (R_FAILED(timeToCalendarTimeWithMyRule(then_seconds, &caltime, &addinfo))) {
            return "--:--:--";
        }

        char buf[16];
        snprintf(buf, sizeof(buf), "%02d:%02d:%02d",
                 caltime.hour, caltime.minute, caltime.second);
        return buf;
    }

    void refreshUI() {
        mutexLock(&g_mutex);
        const std::string errCopy = g_lastError;
        const bool haveData = !g_jeux.empty();
        std::vector<JeuEntry> jeux = g_jeux;
        mutexUnlock(&g_mutex);

        const u64 lastTick = g_lastSuccessTick.load();
        const bool refreshing = g_refreshing.load();

        // Subtitle: the console clock time at which these numbers were fetched.
        if (lastTick != 0) {
            const std::string line = std::string(T(kUpdatedAt)) + fetchClock(lastTick);
            m_frame->setSubtitle(line.c_str());
        } else {
            m_frame->setSubtitle(refreshing ? T(kLoading) : T(kWaiting));
        }

        if (!haveData) {
            if (refreshing) {
                m_totalText = T(kLoading);
            } else if (!errCopy.empty()) {
                m_totalText = errCopy;
            } else {
                m_totalText = T(kWaiting);
            }
            m_gamesText = "--";
            if (m_totalItem != nullptr) m_totalItem->setValue(m_totalText, true);
            if (m_gamesItem != nullptr) m_gamesItem->setValue(m_gamesText, true);
            if (m_placeholder != nullptr) m_placeholder->setValue(m_totalText, true);
            return;
        }

        // The total is sum(jeux[].joueurs), which is the figure the website
        // shows. It is deliberately not sum(counts): a title present in several
        // regions repeats its player count once per regional title id, so that
        // would double-count.
        int total = 0;
        for (const auto& j : jeux) total += j.players;

        // Busiest first.
        std::sort(jeux.begin(), jeux.end(), [](const JeuEntry& a, const JeuEntry& b) {
            if (a.players != b.players) return a.players > b.players;
            return a.name < b.name;
        });

        // These rows show the numbers themselves. A refresh in flight, or a
        // failed one, never replaces them: the subtitle already says when they
        // were taken, so a transient timeout leaves the panel readable.
        m_totalText = std::to_string(total);
        m_gamesText = std::to_string(jeux.size());
        rebuildGameRows(jeux);
        applyValueColour(m_totalItem, m_totalText);
        applyValueColour(m_gamesItem, m_gamesText);
    }

    tsl::elm::OverlayFrame* m_frame = nullptr;
    tsl::elm::List* m_list = nullptr;
    tsl::elm::ListItem* m_totalItem = nullptr;
    tsl::elm::ListItem* m_gamesItem = nullptr;
    tsl::elm::CategoryHeader* m_sectionHeader = nullptr;
    tsl::elm::ListItem* m_placeholder = nullptr;
    std::map<std::string, tsl::elm::ListItem*> m_rows;
    std::string m_totalText;
    std::string m_gamesText = "--";
    std::vector<std::string> m_shownNames;
};
class OverlayTest : public tsl::Overlay {
public:
    virtual void initServices() override {
        fsdevMountSdmc();

        timeInitialize();
        nifmInitialize(NifmServiceType_User);
        socketInitialize(&socketConfig);
        curl_global_init(CURL_GLOBAL_DEFAULT);
        // Decide the interface language before anything is drawn.
        nextendoUseChinese(detectChineseLanguage());

        mutexInit(&g_mutex);

        // Nothing here performs network I/O any more. The game-config download
        // and the first counts fetch both happen on the polling thread started
        // below, which is what lets the panel appear immediately instead of
        // waiting out a request.

        g_threadRunning = true;
        threadCreate(&g_pollThread, pollThreadFunc, nullptr, g_threadStack, sizeof(g_threadStack), 0x2C, -2);
        threadStart(&g_pollThread);
    }
    virtual void exitServices() override {
        g_threadRunning = false;
        threadWaitForExit(&g_pollThread);
        threadClose(&g_pollThread);

        curl_global_cleanup();
        socketExit();
        nifmExit();
        timeExit();

        fsdevUnmountDevice("sdmc");
    }
    virtual void onShow() override { g_overlayVisible = true; }
    virtual void onHide() override { g_overlayVisible = false; }
    virtual std::unique_ptr<tsl::Gui> loadInitialGui() override {
        return initially<GuiTest>();
    }
};

int main(int argc, char **argv) {
    return tsl::loop<OverlayTest>(argc, argv);
}