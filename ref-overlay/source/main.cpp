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

static std::vector<GameEntry> g_games;

static Mutex g_mutex;
static std::map<std::string, int> g_counts;
static std::string g_lastError;
static std::atomic<bool> g_dirty{false};
static std::atomic<bool> g_overlayVisible{false};
static std::atomic<bool> g_threadRunning{false};

static Thread g_pollThread;
alignas(0x1000) static u8 g_threadStack[0x4000];

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
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 8L);
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
static void pollThreadFunc(void*) {
    while (g_threadRunning.load()) {
        if (g_overlayVisible.load()) {
            std::string error;
            std::string raw = fetchOnlineCounts(error);
            auto newCounts = parseOnlineCounts(raw);

            mutexLock(&g_mutex);
            g_counts = newCounts;
            g_lastError = error;
            mutexUnlock(&g_mutex);

            g_dirty = true;
        }

        for (int i = 0; i < 15 && g_threadRunning.load(); i++) {
            svcSleepThread(1'000'000'000LL);
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
                if (!combined.empty()) combined += "  •  ";
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

class GuiTest : public tsl::Gui {
public:
    GuiTest() {
        std::string error;
        std::string raw = fetchOnlineCounts(error);
        mutexLock(&g_mutex);
        g_counts = parseOnlineCounts(raw);
        g_lastError = error;
        mutexUnlock(&g_mutex);
        g_dirty = true;
    }

    virtual tsl::elm::Element* createUI() override {
        m_frame = new tsl::elm::OverlayFrame("Nextendo Status", "--:--:--");
        auto list = new tsl::elm::List();

        m_summaryHeader = new tsl::elm::CategoryHeader("Jugadores en línea: --");
        list->addItem(m_summaryHeader);

        for (const auto& game : g_games) {
            auto item = new tsl::elm::ListItem(game.name);
            item->setClickListener([game](u64 keys) {
                if (keys & HidNpadButton_A) {
                    tsl::changeTo<GuiGameDetail>(game);
                    return true;
                }
                return false;
            });
            list->addItem(item);
            m_items[game.name] = item;
        }

        m_frame->setContent(list);
        refreshUI();
        return m_frame;
    }

    virtual void update() override {
        if (g_dirty.exchange(false)) {
            refreshUI();
        }
    }

    virtual bool handleInput(u64 keysDown, u64 keysHeld, const HidTouchState &touchPos, HidAnalogStickState joyStickPosLeft, HidAnalogStickState joyStickPosRight) override {
        return false;
    }

private:
    void refreshUI() {
        mutexLock(&g_mutex);
        auto countsCopy = g_counts;
        std::string errCopy = g_lastError;
        mutexUnlock(&g_mutex);

        u64 timestamp;
        timeGetCurrentTime(TimeType_LocalSystemClock, &timestamp);
        TimeCalendarTime caltime;
        TimeCalendarAdditionalInfo addinfo;
        timeToCalendarTimeWithMyRule(timestamp, &caltime, &addinfo);
        char timeBuf[32];
        snprintf(timeBuf, sizeof(timeBuf), "Última act.: %02d:%02d:%02d", caltime.hour, caltime.minute, caltime.second);
        m_frame->setSubtitle(timeBuf);

        int totalOnline = 0;
        for (const auto& game : g_games) {
            int maxCount = 0;
            bool anyOnline = false;
            for (const auto& id : game.ids) {
                auto it = countsCopy.find(id);
                if (it != countsCopy.end()) {
                    anyOnline = true;
                    maxCount = std::max(maxCount, it->second);
                }
            }
            if (anyOnline) {
                totalOnline += maxCount;
                m_items[game.name]->setValue(std::to_string(maxCount) + " jugadores", false);
            } else {
                m_items[game.name]->setValue("Offline", true);
            }
        }

        char summaryBuf[48];
        if (countsCopy.empty()) {
            snprintf(summaryBuf, sizeof(summaryBuf), "%s", errCopy.empty() ? "Sin datos" : errCopy.c_str());
        } else {
            snprintf(summaryBuf, sizeof(summaryBuf), "Jugadores en línea: %d", totalOnline);
        }
        m_summaryHeader->setText(summaryBuf);
    }

    tsl::elm::OverlayFrame* m_frame;
    tsl::elm::CategoryHeader* m_summaryHeader;
    std::map<std::string, tsl::elm::ListItem*> m_items;
};

class OverlayTest : public tsl::Overlay {
public:
    virtual void initServices() override {
        fsdevMountSdmc();

        timeInitialize();
        nifmInitialize(NifmServiceType_User);
        socketInitialize(&socketConfig);
        curl_global_init(CURL_GLOBAL_DEFAULT);
        mutexInit(&g_mutex);

        loadGamesConfig();

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