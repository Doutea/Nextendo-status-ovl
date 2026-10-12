#define TESLA_INIT_IMPL
#include <tesla.hpp>
#include <switch.h>
#include <curl/curl.h>
#include <jansson.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/stat.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <atomic>
#include <vector>
#include <map>
#include <string>
#include <algorithm>
#include <cstdlib>
#include <mutex>
#include <cstring>

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

// extern "C" so this overrides the weak fallback declared in tesla.hpp.
extern "C" bool nextendoIsChinese() {
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
static const UiText kGameList    = {"\u6e38\u620f\u5217\u8868", "Game list"};
static const UiText kRefreshing  = {"\u5237\u65b0\u4e2d\u2026", "Refreshing\u2026"};
static const UiText kWaiting     = {"\u7b49\u5f85\u6570\u636e\u2026", "Waiting for data\u2026"};
static const UiText kUpdatedAt   = {"\u66f4\u65b0\u4e8e ", "Updated "};

static const char* T(const UiText& text) {
    return g_useChinese ? text.zh : text.en;
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

struct JeuEntry {
    std::string name;
    int players = 0;
    // Title ids this game reports under, from the API's "titres" field.
    std::vector<std::string> titleIds;
};

static Mutex g_mutex;
static std::map<std::string, int> g_counts;
// Per-game breakdown from the API's "jeux" array, and the running total.
static std::vector<JeuEntry> g_jeux;
static std::string g_lastError;
static std::atomic<bool> g_dirty{false};
static std::atomic<u64> g_lastSuccessTick{0};
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

// The address that last worked, reused so later requests skip the lookup.
// Last address that worked, per host.
//
// A single global was wrong once a second host was fetched: the address that
// served nextendo.network would be pinned for raw.githubusercontent.com too,
// where it cannot work.
static std::map<std::string, std::string> g_cachedIpByHost;
// Entries fetched from the repository, layered over the compiled-in table.
static std::map<std::string, std::string> g_namesZhOverride;
// The same entries keyed by normalised name; see normaliseName().
static std::map<std::string, std::string> g_namesZhByNorm;
static Mutex g_namesMutex;
// When the last attempt was made, so a game that simply has no translation yet
// does not cause a request on every poll.
static std::atomic<u64> g_namesFetchedTick{0};
// Set once the first table fetch of this loader session has been made.
static std::atomic<bool> g_namesFetchAttempted{false};

// Fetches a URL, pinning the address curl connects to.
//
// curl's own name resolution does not work in this process - letting it resolve
// produced "Could not resolve host name" on a console where the applet browser
// and every other network client is fine. libnx's resolver does work, so the
// address is looked up with getaddrinfo() and handed to curl through
// CURLOPT_RESOLVE instead.
//
// The URL, SNI and certificate checks all still use the hostname, so pinning the
// address changes nothing about TLS.
// ---------------------------------------------------------------------------
// Last-good-result cache
//
// Measured on the console (timing.log): a single successful request takes
// 8-20s, and about half of them time out. Waiting for one before showing
// anything therefore means a blank panel for a long time, so the last result is
// written to the SD card and read back during initialisation. The panel then
// opens with numbers already on it and the background refresh updates them.
//
// The raw file API is used rather than fopen(), which fails in an overlay
// process.
// ---------------------------------------------------------------------------

static const char* kCountsCachePath = "/switch/nextendo-status/last_counts.txt";

// Escapes the characters that would break the tiny line-based format.
static std::string cacheEscape(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    for (char c : text) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '\t': out += "\\t"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            default: out += c; break;
        }
    }
    return out;
}

static std::string cacheUnescape(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\\' && i + 1 < text.size()) {
            const char next = text[++i];
            switch (next) {
                case 't': out += '\t'; break;
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                default: out += next; break;
            }
        } else {
            out += text[i];
        }
    }
    return out;
}

// Written after every successful fetch. Line format:
//
//   fetched=<seconds since boot>
//   <players>\t<escaped game name>
static void saveCountsCache(const std::vector<JeuEntry>& jeux, u64 successTick) {
    std::string text;
    text += "fetched=" + std::to_string(armTicksToNs(successTick) / 1000000000ULL) + "\n";
    for (const auto& entry : jeux) {
        text += std::to_string(entry.players) + "\t" + cacheEscape(entry.name);
        for (const auto& id : entry.titleIds) text += "\t" + cacheEscape(id);
        text += "\n";
    }

    FsFileSystem fs{};
    if (R_FAILED(fsOpenSdCardFileSystem(&fs))) return;

    // Remove any previous file so a shorter list cannot leave stale rows behind.
    fsFsDeleteFile(&fs, kCountsCachePath);
    if (R_SUCCEEDED(fsFsCreateFile(&fs, kCountsCachePath, text.size(), 0))) {
        FsFile file{};
        if (R_SUCCEEDED(fsFsOpenFile(&fs, kCountsCachePath, FsOpenMode_Write, &file))) {
            fsFileWrite(&file, 0, text.data(), text.size(), FsWriteOption_Flush);
            fsFileClose(&file);
        }
    }
    fsFsClose(&fs);
}

// Read during initialisation, before the panel is built.
static void loadCountsCache() {
    FsFileSystem fs{};
    if (R_FAILED(fsOpenSdCardFileSystem(&fs))) return;

    FsFile file{};
    if (R_FAILED(fsFsOpenFile(&fs, kCountsCachePath, FsOpenMode_Read, &file))) {
        fsFsClose(&fs);
        return;
    }

    s64 size = 0;
    fsFileGetSize(&file, &size);
    if (size <= 0 || size > 256 * 1024) {
        fsFileClose(&file);
        fsFsClose(&fs);
        return;
    }

    std::string content(static_cast<size_t>(size), '\0');
    u64 bytesRead = 0;
    const Result readResult = fsFileRead(&file, 0, content.data(), content.size(),
                                         FsReadOption_None, &bytesRead);
    fsFileClose(&file);
    fsFsClose(&fs);
    if (R_FAILED(readResult)) return;
    content.resize(static_cast<size_t>(bytesRead));

    std::vector<JeuEntry> jeux;
    u64 fetchedSeconds = 0;

    size_t pos = 0;
    while (pos <= content.size()) {
        const size_t eol = content.find('\n', pos);
        const std::string line =
            content.substr(pos, (eol == std::string::npos ? content.size() : eol) - pos);
        pos = (eol == std::string::npos) ? content.size() + 1 : eol + 1;
        if (line.empty()) continue;

        if (line.compare(0, 8, "fetched=") == 0) {
            fetchedSeconds = strtoull(line.c_str() + 8, nullptr, 10);
            continue;
        }

        const size_t tab = line.find('\t');
        if (tab == std::string::npos) continue;
        JeuEntry entry;
        entry.players = static_cast<int>(strtol(line.c_str(), nullptr, 10));

        // Tab-separated after the count: the name, then the game's title ids.
        const std::string rest = line.substr(tab + 1);
        size_t fieldStart = 0;
        while (fieldStart <= rest.size()) {
            const size_t nextTab = rest.find('\t', fieldStart);
            const std::string field = rest.substr(
                fieldStart,
                (nextTab == std::string::npos ? rest.size() : nextTab) - fieldStart);
            if (entry.name.empty()) {
                entry.name = cacheUnescape(field);
            } else if (!field.empty()) {
                entry.titleIds.push_back(cacheUnescape(field));
            }
            if (nextTab == std::string::npos) break;
            fieldStart = nextTab + 1;
        }

        if (!entry.name.empty()) jeux.push_back(std::move(entry));
    }

    if (jeux.empty()) return;

    mutexLock(&g_mutex);
    g_jeux = std::move(jeux);
    g_lastError.clear();
    mutexUnlock(&g_mutex);

    // Rebuild a tick for the stored moment so the panel can show when the data
    // is from. The API only needs seconds, so this is accurate enough for a
    // label.
    const u64 nowTick = armGetSystemTick();
    const u64 uptimeSeconds = armTicksToNs(nowTick) / 1000000000ULL;
    if (fetchedSeconds > 0 && fetchedSeconds <= uptimeSeconds) {
        const u64 ageSeconds = uptimeSeconds - fetchedSeconds;
        g_lastSuccessTick = nowTick - (ageSeconds * 19200000ULL);
    }
}

// The game list, with every title id a game can report under.
//
// The API's "jeux" array is capped at 30 entries, so games outside the top
// 30 never appear there and the names it uses are inconsistent. This is the
// same list the status page keeps, which is what makes all games visible.
struct SiteGame {
    const char* name;
    const char* ids[3];
};

static const SiteGame kSiteGames[] = {
    {"Mario Kart 8 Deluxe", {"0100152000022000", nullptr, nullptr}},
    {"Splatoon 2", {"0100f8f0000a2000", "01003bc0000a0000", "01003c700009c800"}},
    {"Splatoon 3", {"0100c2500fc20000", nullptr, nullptr}},
    {"Super Smash Bros. Ultimate", {"01006a800016e000", nullptr, nullptr}},
    {"Animal Crossing: New Horizons", {"01006f8002326000", nullptr, nullptr}},
    {"Luigi's Mansion 3", {"0100dca0064a6000", nullptr, nullptr}},
    {"Minecraft: Nintendo Switch Edition", {"01006bd001e06000", nullptr, nullptr}},
    {"ARMS", {"01009b500007c000", nullptr, nullptr}},
    {"Mario Tennis Aces", {"0100bde00862a000", nullptr, nullptr}},
    {"Super Mario Maker 2", {"01009b90006dc000", nullptr, nullptr}},
    {"PAC-MAN 99", {"0100ad9012510000", nullptr, nullptr}},
    {"Tetris 99", {"010040600c5ce000", nullptr, nullptr}},
    {"Super Mario Bros. 35", {"0100277011f1a000", nullptr, nullptr}},
    {"Super Mario Bros. Wonder", {"010015100b514000", nullptr, nullptr}},
    {"Clubhouse Games: 51 Worldwide Classics", {"010047700d540000", nullptr, nullptr}},
    {"Monster Hunter Generations Ultimate", {"0100770008dd8000", nullptr, nullptr}},
    {"Crash Team Racing Nitro-Fueled", {"0100f9f00c696000", nullptr, nullptr}},
    {"Diablo III: Eternal Collection", {"01001b300b9be000", nullptr, nullptr}},
    {"Mario Party Superstars", {"01006fe013472000", nullptr, nullptr}},
    {"Super Mario Party Jamboree", {"0100965017338000", nullptr, nullptr}},
    {"Overcooked! 2", {"01006fd0080b2000", nullptr, nullptr}},
    {"Pok\u00E9mon Scarlet and Violet", {"0100a3d008c5c000", "01008f6008c5e000", nullptr}},
    {"Nintendo 64 - Nintendo Classics", {"0100c9a00ece6000", nullptr, nullptr}},
    {"Pok\u00E9mon Legends: Z-A", {"0100f43008c44000", nullptr, nullptr}},
    {"Mario Golf: Super Rush", {"0100c9c00e25c000", nullptr, nullptr}},
    {"Super Mario 3D World + Bowser's Fury", {"010028600ebda000", nullptr, nullptr}},
    {"Super Mario Odyssey", {"0100000000010000", nullptr, nullptr}},
    {"METAL GEAR SOLID: Peace Walker", {"0100c6f01c4f8000", nullptr, nullptr}},
    {"Mario Strikers: Battle League", {"010019401051c000", nullptr, nullptr}},
    {"Minecraft Dungeons II", {"0100a7c01b792000", nullptr, nullptr}},
    {"Pokk\u00E9n Tournament DX", {"0100b3f000be2000", nullptr, nullptr}},
    {"Saints Row: The Third - The Full Package", {"0100de600beee000", nullptr, nullptr}},
    {"Ultimate Chicken Horse", {"0100fcf002a58000", nullptr, nullptr}},
    {"Nintendo Switch Sports", {"0100d2f00d5c0000", nullptr, nullptr}},
    {"Monopoly for Nintendo Switch", {"01007430037f6000", nullptr, nullptr}},
};

// Highest count across a set of title ids.
//
// A game reporting under several regional ids must take the highest, not the sum,
// or it would be counted more than once. This is what the status page does, so
// the numbers agree with it.
static int bestCountFor(const std::vector<std::string>& titles,
                        const std::map<std::string, int>& counts) {
    int best = 0;
    for (const auto& id : titles) {
        auto it = counts.find(id);
        if (it != counts.end() && it->second > best) best = it->second;
    }
    return best;
}

// Builds the display list, with the API as the source of truth.
//
// This used to be built from kSiteGames alone, so a game the API started
// reporting after that table was compiled could never show up - the list was only
// ever as new as the table. The API's "jeux" array now carries the "titres" ids,
// which was the only thing the table provided, so a new game appears as soon as
// the API reports it, with no new build needed.
//
// kSiteGames is still consulted, but only to add games the response does not
// mention at all. A game the API does mention is taken as given, even at zero
// players: the API decides what to list, and filtering its entries would hide
// titles the status page shows.
static std::vector<JeuEntry> buildGameList(const std::vector<JeuEntry>& apiJeux,
                                           const std::map<std::string, int>& counts) {
    std::vector<JeuEntry> out;
    out.reserve(apiJeux.size() + 8);

    std::map<std::string, bool> seenIds;
    for (const auto& game : apiJeux) {
        out.push_back(game);
        for (const auto& id : game.titleIds) seenIds[id] = true;
    }

    for (const auto& game : kSiteGames) {
        bool present = false;
        for (const char* id : game.ids) {
            if (id == nullptr) break;
            if (seenIds.count(id) != 0) {
                present = true;
                break;
            }
        }
        if (present) continue;

        std::vector<std::string> ids;
        for (const char* id : game.ids) {
            if (id == nullptr) break;
            ids.push_back(id);
        }

        JeuEntry entry;
        entry.name = game.name;
        entry.players = bestCountFor(ids, counts);
        entry.titleIds = std::move(ids);
        out.push_back(std::move(entry));
    }

    return out;
}

std::string fetchUrl(const std::string& url, const std::string& host,
                     const std::vector<std::string>& fallbackIps, std::string& errorOut) {
    std::string responseBuffer;
    CURL* curl = curl_easy_init();
    if (!curl) {
        errorOut = "curl_easy_init fallo";
        return "";
    }

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &responseBuffer);
    // Measured on the console, a successful request takes 4-6s, so this leaves
    // room for a slow handshake without letting a dead network hold the thread.
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 12L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 8L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

    struct curl_slist* resolveList = nullptr;

    const std::string cachedIp = g_cachedIpByHost.count(host)
                                     ? g_cachedIpByHost[host]
                                     : std::string();

    // Best candidate first: the address that last served this host.
    if (!cachedIp.empty()) {
        const std::string entry = host + ":443:" + cachedIp;
        resolveList = curl_slist_append(resolveList, entry.c_str());
    }

    // Then whatever libnx's resolver reports for the name.
    const std::string resolved = resolveHostToIp(host);
    if (!resolved.empty() && resolved != cachedIp) {
        const std::string entry = host + ":443:" + resolved;
        resolveList = curl_slist_append(resolveList, entry.c_str());
    }

    // Last resort: the addresses compiled in, for when the lookup itself fails.
    for (const auto& ip : fallbackIps) {
        if (ip == cachedIp || ip == resolved) continue;
        const std::string entry = host + ":443:" + ip;
        resolveList = curl_slist_append(resolveList, entry.c_str());
    }

    if (resolveList) curl_easy_setopt(curl, CURLOPT_RESOLVE, resolveList);

    const CURLcode res = curl_easy_perform(curl);

    if (res == CURLE_OK) {
        char* usedIp = nullptr;
        if (curl_easy_getinfo(curl, CURLINFO_PRIMARY_IP, &usedIp) == CURLE_OK && usedIp) {
            g_cachedIpByHost[host] = usedIp;
        }
    }

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

// Fetches the counts document, retrying a couple of times.
//
// The retry is for a transient failure, so the panel does not have to wait a
// whole 15s poll interval for the next chance. Each attempt is short: the
// address is already pinned by fetchUrl(), so there is no DNS stall to ride out,
// and a long deadline would only keep a dead network on screen longer.
std::string fetchCountsWithRetry(std::string& errorOut) {
    static const int kAttempts = 3;

    for (int attempt = 0; attempt < kAttempts; ++attempt) {
        // Give up at once if the overlay is closing: exitServices() waits for
        // this thread, and two more attempts could otherwise hold it for ~30s.
        if (!g_threadRunning.load()) {
            errorOut = "closing";
            return std::string();
        }

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
            // Brief pause so a transient hiccup can clear.
            svcSleepThread(250'000'000LL);  // 250 ms
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

            // "titres" lists the title ids this game reports under. Reading it is
            // what lets the list be driven by the API instead of a compiled table.
            json_t* jtitres = json_object_get(value, "titres");
            if (json_is_array(jtitres)) {
                size_t ti;
                json_t* tv;
                json_array_foreach(jtitres, ti, tv) {
                    if (json_is_string(tv)) entry.titleIds.emplace_back(json_string_value(tv));
                }
            }

            if (!entry.name.empty()) result.push_back(entry);
        }
    }

    json_decref(root);
    return result;
}

static std::atomic<bool> g_refreshRequested{false};
// True while a fetch started by the user is in flight, so the row can show it.
static std::atomic<bool> g_refreshing{false};
// Set the moment A is pressed and cleared when the fetch finishes.
//
// It exists so the row can react to the key press immediately: the polling
// thread only notices g_refreshRequested when its 1s step comes round, and for
// that second nothing would otherwise change on screen, which makes the press
// look ignored.
static std::atomic<bool> g_manualRefresh{false};

static void maybeRefreshNames(const std::vector<JeuEntry>& jeux);

static void pollThreadFunc(void*) {
    // No game-config download here any more. It fed the per-title detail view,
    // which has no entry point since the panel was reworked, so the request was
    // pure overhead: a GitHub fetch and an SD-card write on every open.

    while (g_threadRunning.load()) {
        const bool requested = g_refreshRequested.exchange(false);

        if (g_overlayVisible.load() || requested) {
            if (requested) g_refreshing = true;

            std::string error;
            std::string raw = fetchCountsWithRetry(error);
            auto newCounts = parseOnlineCounts(raw);
            auto apiJeux = parseJeux(raw);

            // Only build the list when the fetch returned something.
            //
            // buildGameList() always produces one entry per known game, so on a
            // failed fetch it would hand back ~35 zeroes. That is non-empty, which
            // would defeat the "keep the last good numbers" check below and blank
            // the panel on every timeout.
            std::vector<JeuEntry> newJeux;
            if (!apiJeux.empty()) {
                newJeux = buildGameList(apiJeux, newCounts);
            }

            if (!newJeux.empty()) {
                g_lastSuccessTick = armGetSystemTick();
                // Persist immediately so the next open has something to show.
                saveCountsCache(newJeux, g_lastSuccessTick.load());
            }

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

            // A game with no translation is the signal to pull the current table
            // from the repository.
            maybeRefreshNames(newJeux);

            g_refreshing = false;
            g_manualRefresh = false;
            g_dirty = true;
        }

        for (int i = 0; i < 15 && g_threadRunning.load(); i++) {
            svcSleepThread(1'000'000'000LL);
            if (g_refreshRequested.load()) break;
        }
    }
}

static const std::map<std::string, std::string> g_gameNamesZh = {
    {"Mario Kart 8 Deluxe",
     "\u9A6C\u529B\u6B27\u8D5B\u8F66 8 \u8C6A\u534E\u7248"},
    {"Splatoon 2",
     "\u65AF\u666E\u62C9\u9041 2"},
    {"Splatoon 3",
     "\u65AF\u666E\u62C9\u9041 3"},
    {"Super Smash Bros. Ultimate",
     "\u4EFB\u5929\u5802\u5168\u660E\u661F\u5927\u4E71\u6597"},
    {"Animal Crossing: New Horizons",
     "\u52A8\u7269\u68EE\u53CB\u4F1A"},
    {"Luigi's Mansion 3",
     "\u8DEF\u6613\u5409\u9B3C\u5C4B 3"},
    {"Minecraft: Nintendo Switch Edition",
     "\u6211\u7684\u4E16\u754C\uFF1ASwitch \u7248"},
    {"ARMS",
     "ARMS"},
    {"Mario Tennis Aces",
     "\u9A6C\u529B\u6B27\u7F51\u7403 Aces"},
    {"Super Mario Maker 2",
     "\u8D85\u7EA7\u9A6C\u529B\u6B27\u5236\u9020 2"},
    {"PAC-MAN 99",
     "\u5403\u8C46\u4EBA 99"},
    {"Tetris 99",
     "\u4FC4\u7F57\u65AF\u65B9\u5757 99"},
    {"Super Mario Bros. 35",
     "\u8D85\u7EA7\u9A6C\u529B\u6B27\u5144\u5F1F 35"},
    {"Super Mario Bros. Wonder",
     "\u8D85\u7EA7\u9A6C\u529B\u6B27\u5144\u5F1F \u60CA\u5947"},
    {"Clubhouse Games: 51 Worldwide Classics",
     "\u4E16\u754C\u6E38\u620F\u5927\u5168 51"},
    {"Monster Hunter Generations Ultimate",
     "\u602A\u7269\u730E\u4EBA XX"},
    {"Crash Team Racing Nitro-Fueled",
     "\u53E4\u60D1\u72FC\u8D5B\u8F66\u91CD\u5236\u7248"},
    {"Diablo III: Eternal Collection",
     "\u6697\u9ED1\u7834\u574F\u795E III\uFF1A\u6C38\u6052\u4E4B\u6218"},
    {"Mario Party Superstars",
     "\u9A6C\u529B\u6B27\u6D3E\u5BF9 \u8D85\u7EA7\u5DE8\u661F"},
    {"Super Mario Party Jamboree",
     "\u8D85\u7EA7\u9A6C\u529B\u6B27\u6D3E\u5BF9 \u7A7A\u524D\u76DB\u4F1A"},
    {"Overcooked! 2",
     "\u80E1\u95F9\u53A8\u623F 2"},
    {"Pok\u00E9mon Scarlet and Violet",
     "\u5B9D\u53EF\u68A6 \u6731\u7D2B"},
    {"Nintendo 64 - Nintendo Classics",
     "Nintendo 64 \u7ECF\u5178\u5408\u96C6"},
    {"Pok\u00E9mon Legends: Z-A",
     "\u5B9D\u53EF\u68A6\u4F20\u8BF4 Z-A"},
    {"Mario Golf: Super Rush",
     "\u9A6C\u529B\u6B27\u9AD8\u5C14\u592B\uFF1A\u8D85\u7EA7\u51B2\u523A"},
    {"Super Mario 3D World + Bowser's Fury",
     "\u8D85\u7EA7\u9A6C\u529B\u6B27 3D \u4E16\u754C"},
    {"Super Mario Odyssey",
     "\u8D85\u7EA7\u9A6C\u529B\u6B27 \u5965\u5FB7\u8D5B"},
    {"METAL GEAR SOLID: Peace Walker",
     "\u5408\u91D1\u88C5\u5907\uFF1A\u548C\u5E73\u884C\u8005"},
    {"Mario Strikers: Battle League",
     "\u9A6C\u529B\u6B27\u8DB3\u7403\uFF1A\u8D85\u7EA7\u6D32\u9645\u8054\u8D5B"},
    {"Minecraft Dungeons II",
     "\u6211\u7684\u4E16\u754C\uFF1A\u5730\u7262 II"},
    {"Pokk\u00E9n Tournament DX",
     "\u5B9D\u53EF\u62F3 DX"},
    {"Saints Row: The Third - The Full Package",
     "\u9ED1\u9053\u5723\u5F92 3\uFF1A\u5B8C\u6574\u7248"},
    {"Ultimate Chicken Horse",
     "\u8D85\u7EA7\u9E21\u9A6C"},
    {"Nintendo Switch Sports",
     "Nintendo Switch \u8FD0\u52A8"},
    {"Monopoly for Nintendo Switch",
     "\u5730\u4EA7\u5927\u4EA8"},
    {"Pok\u00E9mon Scarlet",
     "\u5B9D\u53EF\u68A6 \u6731\u7D2B"},
    {"L\u00E9gendes Pok\u00E9mon : Z-A",
     "\u5B9D\u53EF\u68A6\u4F20\u8BF4 Z-A"},
    {"DRAGON BALL FighterZ",
     "\u9F99\u73E0 \u6597\u58EBZ"},
    {"Just Shapes & Beats",
     "\u5F62\u72B6\u4E0E\u97F3\u7B26"},
};

// Lowercases and drops accents and punctuation so two spellings of one title
// compare equal. "MONSTER HUNTER GENERATIONS ULTIMATE" and "Monster Hunter
// Generations Ultimate" are the same game, as are the en dash and hyphen forms of
// "Nintendo 64 - Nintendo Classics".
static std::string normaliseName(const std::string& text) {
    std::string out;
    out.reserve(text.size());

    for (size_t i = 0; i < text.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(text[i]);

        // Accented letters used in these titles, folded onto their base letter.
        // Matched as the two-byte UTF-8 sequences they are.
        if (c == 0xC3 && i + 1 < text.size()) {
            const unsigned char d = static_cast<unsigned char>(text[i + 1]);
            char folded = 0;
            switch (d) {
                case 0xA9: case 0xA8: case 0xAA: folded = 'e'; break;  // e-acute, e-grave, e-circumflex
                case 0xA1: case 0xA0:            folded = 'a'; break;  // a-acute, a-grave
                case 0xAD:                       folded = 'i'; break;  // i-acute
                case 0xB3: case 0xB4:            folded = 'o'; break;  // o-acute, o-circumflex
                case 0xBA: case 0xB9:            folded = 'u'; break;  // u-acute, u-grave
                default: break;
            }
            if (folded != 0) {
                out += folded;
                ++i;
                continue;
            }
        }

        if (c >= 'A' && c <= 'Z') {
            out += static_cast<char>(c - 'A' + 'a');
        } else if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
            out += static_cast<char>(c);
        }
        // Anything else - spaces, dashes, colons, ampersands, other non-ASCII -
        // contributes nothing.
    }

    return out;
}

// Chinese names indexed by title id, built from kSiteGames.
//
// kSiteGames knows every title id a game reports under, and its names are the keys
// the table uses, so this is what covers a game the API has renamed or split: the
// API name misses, but its title id does not.
//
// Built outside g_namesMutex: it reads the override table, which the polling
// thread replaces, and taking the mutex here would mean reading it while holding
// it from localisedName().
static std::mutex g_idIndexMutex;
static std::map<std::string, std::string> g_namesZhById;
static bool g_namesZhByIdReady = false;

static void ensureIdNameIndex() {
    std::lock_guard<std::mutex> lock(g_idIndexMutex);
    if (g_namesZhByIdReady) return;

    std::map<std::string, std::string> fetched;
    {
        mutexLock(&g_namesMutex);
        fetched = g_namesZhOverride;
        mutexUnlock(&g_namesMutex);
    }

    std::map<std::string, std::string> byId;
    for (const auto& game : kSiteGames) {
        // A fetched translation wins over the compiled one.
        auto name = fetched.find(game.name);
        if (name != fetched.end()) {
            for (const char* id : game.ids) {
                if (id == nullptr) break;
                byId[id] = name->second;
            }
            continue;
        }

        auto compiled = g_gameNamesZh.find(game.name);
        if (compiled == g_gameNamesZh.end()) continue;
        for (const char* id : game.ids) {
            if (id == nullptr) break;
            byId[id] = compiled->second;
        }
    }

    g_namesZhById = std::move(byId);
    g_namesZhByIdReady = true;
}

static std::string lookupByTitleId(const std::vector<std::string>& titleIds) {
    ensureIdNameIndex();

    std::lock_guard<std::mutex> lock(g_idIndexMutex);
    for (const auto& id : titleIds) {
        auto it = g_namesZhById.find(id);
        if (it != g_namesZhById.end()) return it->second;
    }
    return std::string();
}

// True when a Chinese name exists for this game, by any of the lookups
// localisedName() uses. Separate from localisedName() so the check does not have
// to build the translated string.
static bool hasTranslation(const std::string& apiName,
                           const std::vector<std::string>& titleIds) {
    const std::string wanted = normaliseName(apiName);
    if (wanted.empty()) return false;

    {
        mutexLock(&g_namesMutex);
        const bool found =
            g_namesZhOverride.count(apiName) != 0 || g_namesZhByNorm.count(wanted) != 0;
        mutexUnlock(&g_namesMutex);
        if (found) return true;
    }

    if (g_gameNamesZh.count(apiName) != 0) return true;
    for (const auto& entry : g_gameNamesZh) {
        if (normaliseName(entry.first) == wanted) return true;
    }

    if (!titleIds.empty() && !lookupByTitleId(titleIds).empty()) return true;

    return false;
}

// Chinese name when the console is set to Chinese, otherwise the API's own name.
// A non-Chinese console has no Chinese font loaded, and forcing a Chinese title on
// it would be both unreadable and wrong for that player's locale.
//
// The lookups go from most to least exact, because the API's names do not always
// match the table's keys:
//
//   1. the fetched table, by the name exactly as given;
//   2. the fetched table, by normalised name - this is what covers the API
//      shouting a title or spelling its punctuation differently;
//   3. the compiled table, likewise normalised;
//   4. the title ids, through kSiteGames - this covers games the API has renamed
//      or split, where the table key is the site's older name.
static std::string localisedName(const std::string& apiName,
                                 const std::vector<std::string>& titleIds = {}) {
    if (!nextendoIsChinese()) return apiName;

    const std::string wanted = normaliseName(apiName);

    // The fetched table wins, so an edit in the repository takes effect without a
    // new build.
    {
        mutexLock(&g_namesMutex);

        auto fetched = g_namesZhOverride.find(apiName);
        if (fetched != g_namesZhOverride.end()) {
            const std::string name = fetched->second;
            mutexUnlock(&g_namesMutex);
            return name;
        }

        if (!wanted.empty()) {
            auto byNorm = g_namesZhByNorm.find(wanted);
            if (byNorm != g_namesZhByNorm.end()) {
                const std::string name = byNorm->second;
                mutexUnlock(&g_namesMutex);
                return name;
            }
        }

        mutexUnlock(&g_namesMutex);
    }

    auto builtin = g_gameNamesZh.find(apiName);
    if (builtin != g_gameNamesZh.end()) return builtin->second;

    if (!wanted.empty()) {
        for (const auto& entry : g_gameNamesZh) {
            if (normaliseName(entry.first) == wanted) return entry.second;
        }
    }

    // Renamed or split entries. kSiteGames holds the title ids but its names are
    // not always the table's keys, so the id-to-name mapping is used directly.
    if (!titleIds.empty()) {
        const std::string byId = lookupByTitleId(titleIds);
        if (!byId.empty()) return byId;
    }

    return apiName;
}
// ---------------------------------------------------------------------------
// Chinese name table, updatable without a new release
//
// The table compiled into this file goes stale as soon as the API starts
// reporting a game it does not know. docs/game-names-zh.txt in the project
// repository is the live copy: when a game has no translation the overlay
// fetches that file, caches it on the SD card and uses it from then on, so a new
// game needs an edit in the repository rather than a new build.
//
// The compiled-in table remains the offline fallback, and a partial or malformed
// response never replaces a working table.
// ---------------------------------------------------------------------------

static const std::string NAMES_ZH_URL =
    "https://raw.githubusercontent.com/Doutea/Nextendo-status-ovl/main/docs/game-names-zh.txt";
static const char* kNamesCachePath = "/switch/nextendo-status/games_zh.txt";

static std::string trimSpaces(const std::string& text) {
    const size_t first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return std::string();
    const size_t last = text.find_last_not_of(" \t\r\n");
    return text.substr(first, last - first + 1);
}

// Parses the "English name | Chinese name" format, skipping '#' comments.
static std::map<std::string, std::string> parseNamesTable(const std::string& text) {
    std::map<std::string, std::string> table;
    size_t pos = 0;
    while (pos <= text.size()) {
        const size_t eol = text.find('\n', pos);
        const std::string line =
            text.substr(pos, (eol == std::string::npos ? text.size() : eol) - pos);
        pos = (eol == std::string::npos) ? text.size() + 1 : eol + 1;

        const size_t first = line.find_first_not_of(" \t\r");
        if (first == std::string::npos || line[first] == '#') continue;

        const size_t bar = line.find('|', first);
        if (bar == std::string::npos) continue;

        const std::string key = trimSpaces(line.substr(first, bar - first));
        const std::string value = trimSpaces(line.substr(bar + 1));
        if (!key.empty() && !value.empty()) table[key] = value;
    }
    return table;
}

static void applyNamesTable(std::map<std::string, std::string> table) {
    if (table.empty()) return;

    // Index the same entries by normalised name as well, so a game whose name the
    // API spells differently still finds its translation. Built here rather than
    // in localisedName() because that runs for every row on every refresh.
    std::map<std::string, std::string> byNorm;
    for (const auto& entry : table) {
        const std::string key = normaliseName(entry.first);
        if (!key.empty()) byNorm[key] = entry.second;
    }

    mutexLock(&g_namesMutex);
    g_namesZhOverride = std::move(table);
    g_namesZhByNorm = std::move(byNorm);
    mutexUnlock(&g_namesMutex);

    // The id index folds the fetched table in, so it has to be rebuilt.
    {
        std::lock_guard<std::mutex> lock(g_idIndexMutex);
        g_namesZhByIdReady = false;
    }
}

// Kept verbatim so the next run needs no network request.
static void saveNamesCache(const std::string& text) {
    FsFileSystem fs{};
    if (R_FAILED(fsOpenSdCardFileSystem(&fs))) return;

    fsFsDeleteFile(&fs, kNamesCachePath);
    if (R_SUCCEEDED(fsFsCreateFile(&fs, kNamesCachePath, text.size(), 0))) {
        FsFile file{};
        if (R_SUCCEEDED(fsFsOpenFile(&fs, kNamesCachePath, FsOpenMode_Write, &file))) {
            fsFileWrite(&file, 0, text.data(), text.size(), FsWriteOption_Flush);
            fsFileClose(&file);
        }
    }
    fsFsClose(&fs);
}

// Read during initialisation, before the panel is built.
static void loadNamesCache() {
    FsFileSystem fs{};
    if (R_FAILED(fsOpenSdCardFileSystem(&fs))) return;

    FsFile file{};
    if (R_FAILED(fsFsOpenFile(&fs, kNamesCachePath, FsOpenMode_Read, &file))) {
        fsFsClose(&fs);
        return;
    }

    s64 size = 0;
    fsFileGetSize(&file, &size);
    if (size <= 0 || size > 512 * 1024) {
        fsFileClose(&file);
        fsFsClose(&fs);
        return;
    }

    std::string content(static_cast<size_t>(size), '\0');
    u64 bytesRead = 0;
    const Result readResult = fsFileRead(&file, 0, content.data(), content.size(),
                                         FsReadOption_None, &bytesRead);
    fsFileClose(&file);
    fsFsClose(&fs);
    if (R_FAILED(readResult)) return;
    content.resize(static_cast<size_t>(bytesRead));

    applyNamesTable(parseNamesTable(content));
}



// Renders a table back into the editable "English name | Chinese name" form.
static std::string serialiseNamesTable(const std::map<std::string, std::string>& table) {
    std::string text =
        "# Nextendo \u5728\u7ebf\u72b6\u6001 - \u6e38\u620f\u540d\u79f0\u5bf9\u7167\u8868\n"
        "#\n"
        "# \u6b64\u6587\u4ef6\u7531\u63d2\u4ef6\u81ea\u52a8\u7ef4\u62a4\uff0c\u4e0b\u6b21\u6210\u529f\u8054\u7f51\u65f6\u4f1a\u88ab\u8986\u76d6\u3002\n"
        "# \u8981\u4fee\u6539\u8bd1\u540d\u8bf7\u6539\u4ed3\u5e93\u91cc\u7684 docs/game-names-zh.txt\u3002\n"
        "#\n"
        "# This file is maintained by the overlay and is overwritten on the next\n"
        "# successful download. To change a name, edit docs/game-names-zh.txt in the\n"
        "# project repository instead.\n"
        "\n";
    for (const auto& entry : table) {
        text += entry.first + " | " + entry.second + "\n";
    }
    return text;
}

// True when a cached table is already on the SD card.
static bool namesCacheExists() {
    FsFileSystem fs{};
    if (R_FAILED(fsOpenSdCardFileSystem(&fs))) return false;
    FsFile file{};
    const bool exists = R_SUCCEEDED(fsFsOpenFile(&fs, kNamesCachePath, FsOpenMode_Read, &file));
    if (exists) fsFileClose(&file);
    fsFsClose(&fs);
    return exists;
}

static void fetchNamesTable() {
    // Recorded before the request so a failure still starts the cooldown.
    g_namesFetchedTick = armGetSystemTick();

    std::string error;
    const std::string raw =
        fetchUrl(NAMES_ZH_URL, "raw.githubusercontent.com", GITHUB_FALLBACK_IPS, error);

    if (error.empty() && !raw.empty()) {
        auto parsed = parseNamesTable(raw);
        if (!parsed.empty()) {
            applyNamesTable(std::move(parsed));
            saveNamesCache(raw);
            g_dirty = true;   // redraw so the new names appear
            return;
        }
    }

    // The download failed. Write the compiled-in table out anyway, so the folder
    // always holds a readable copy instead of staying empty.
    if (!namesCacheExists()) {
        saveNamesCache(serialiseNamesTable(g_gameNamesZh));
    }
}

// Called after a counts fetch. One attempt is made per loader session, so an
// edit in the repository reaches this console without waiting for a new game to
// appear; after that a request only happens when some game has no translation at
// all, and never more often than the cooldown. The network on this console is
// slow, so requests are kept rare on purpose.
static void maybeRefreshNames(const std::vector<JeuEntry>& jeux) {
    if (!g_useChinese || jeux.empty()) return;

    bool need = !g_namesFetchAttempted.exchange(true);

    if (!need) {
        for (const auto& entry : jeux) {
            // Same matching localisedName() uses, so a game that is translated by
            // normalised name or by title id does not look untranslated here and
            // trigger a pointless download.
            if (!hasTranslation(entry.name, entry.titleIds)) {
                need = true;
                break;
            }
        }
    }
    if (!need) return;

    const u64 cooldownTicks = 19200000ULL * 60ULL * 30ULL;   // 30 minutes
    const u64 last = g_namesFetchedTick.load();
    const u64 now = armGetSystemTick();
    if (last != 0 && now > last && (now - last) < cooldownTicks) return;

    fetchNamesTable();
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

// Games pinned above the rest, by the API's own name so a pin survives the
// display name being translated. Stored in the order they were pinned, so the
// list keeps a stable, predictable order.
//
// Only touched from the render thread: the polling thread never reads it.
static std::vector<std::string> g_pinnedGames;

// Position in the pinned order, or -1 when not pinned.
static int pinnedRank(const std::string& gameName) {
    for (size_t i = 0; i < g_pinnedGames.size(); i++) {
        if (g_pinnedGames[i] == gameName) return static_cast<int>(i);
    }
    return -1;
}

static const char* kPinnedPath = "/switch/nextendo-status/pinned.txt";

// The name would only ever be a game title, so a line-based file needs no escaping.
static void savePinnedGame() {
    std::string text;
    for (const auto& name : g_pinnedGames) text += name + "\n";

    FsFileSystem fs{};
    if (R_FAILED(fsOpenSdCardFileSystem(&fs))) return;

    fsFsDeleteFile(&fs, kPinnedPath);
    if (R_SUCCEEDED(fsFsCreateFile(&fs, kPinnedPath, text.size(), 0))) {
        FsFile file{};
        if (R_SUCCEEDED(fsFsOpenFile(&fs, kPinnedPath, FsOpenMode_Write, &file))) {
            fsFileWrite(&file, 0, text.data(), text.size(), FsWriteOption_Flush);
            fsFileClose(&file);
        }
    }
    fsFsClose(&fs);
}

// Read during initialisation, before the panel is built.
static void loadPinnedGame() {
    FsFileSystem fs{};
    if (R_FAILED(fsOpenSdCardFileSystem(&fs))) return;

    FsFile file{};
    if (R_FAILED(fsFsOpenFile(&fs, kPinnedPath, FsOpenMode_Read, &file))) {
        fsFsClose(&fs);
        return;
    }

    s64 size = 0;
    fsFileGetSize(&file, &size);
    if (size <= 0 || size > 4096) {
        fsFileClose(&file);
        fsFsClose(&fs);
        return;
    }

    std::string content(static_cast<size_t>(size), '\0');
    u64 bytesRead = 0;
    const Result readResult = fsFileRead(&file, 0, content.data(), content.size(),
                                         FsReadOption_None, &bytesRead);
    fsFileClose(&file);
    fsFsClose(&fs);
    if (R_FAILED(readResult)) return;

    content.resize(static_cast<size_t>(bytesRead));

    // One game name per line.
    size_t pos = 0;
    while (pos <= content.size()) {
        const size_t eol = content.find('\n', pos);
        std::string line =
            content.substr(pos, (eol == std::string::npos ? content.size() : eol) - pos);
        pos = (eol == std::string::npos) ? content.size() + 1 : eol + 1;

        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (!line.empty()) g_pinnedGames.push_back(line);
    }
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
                // Raised here rather than in the thread: this runs on the frame
                // the button was pressed, so the row reacts at once.
                g_manualRefresh = true;
                return true;
            }
            return false;
        });
        list->addItem(m_totalItem);

        // The list itself. This heading also separates the two blocks.
        m_sectionHeader = new tsl::elm::CategoryHeader(T(kGameList));
        list->addItem(m_sectionHeader);

        m_frame->setContent(list);
        refreshUI();
        return m_frame;
    }

    virtual void update() override {
        const bool dirty = g_dirty.exchange(false);
        const bool manual = g_manualRefresh.load();
        const bool active = g_refreshing.load();

        // Redraw when the "refreshing" state changes as well as when new data
        // arrives. g_dirty alone is not enough: it is only raised once a fetch
        // has finished, so the press itself would show nothing.
        static bool lastManual = false;
        static bool lastActive = false;
        const bool stateChanged = (manual != lastManual) || (active != lastActive);
        lastManual = manual;
        lastActive = active;

        if (dirty || stateChanged) {
            refreshUI();
        }
    }

    virtual bool handleInput(u64 keysDown, u64 keysHeld, const HidTouchState& touchPos,
                             HidAnalogStickState joyStickPosLeft,
                             HidAnalogStickState joyStickPosRight) override {
        return false;
    }

private:
    // Pins or unpins a game, then rebuilds the list so it sits above the rest.
    //
    // Rebuilding destroys the rows, so the focus has to be dropped first. libtesla
    // then gives the focus back to the first row rather than to the game that was
    // acted on, which is a limitation of its focus handling, not of this code.
    void applyPin(const std::string& gameName) {
        const int rank = pinnedRank(gameName);
        if (rank >= 0) {
            g_pinnedGames.erase(g_pinnedGames.begin() + rank);
        } else {
            g_pinnedGames.push_back(gameName);
        }
        savePinnedGame();

        this->removeFocus();

        m_shownNames.clear();
        refreshUI();

    }

    // Rebuilds the entry rows when the set of games changes.
    //
    // The list is reconstructed rather than edited, because rows are added and
    // removed and tsl::elm::List offers no removal API. removeFocus() comes
    // first, as libtesla requires before items are destroyed.
    void rebuildGameRows(const std::vector<JeuEntry>& jeux) {
        // The displayed names, not the API names: a translation that changed
        // must count as a change so the rows are rebuilt with the new label.
        std::vector<std::string> names;
        names.reserve(jeux.size());
        for (const auto& j : jeux) names.push_back(localisedName(j.name, j.titleIds));

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
                // Raised here rather than in the thread: this runs on the frame
                // the button was pressed, so the row reacts at once.
                g_manualRefresh = true;
                return true;
            }
            return false;
        });
        list->addItem(total);
        m_totalItem = total;

        auto* section = new tsl::elm::CategoryHeader(T(kGameList));
        list->addItem(section);
        m_sectionHeader = section;

        m_rows.clear();
        for (const auto& j : jeux) {
            // No marker on the name: a pinned game is simply at the top of the
            // list, which is enough to tell it apart.
            auto* item = new tsl::elm::ListItem(localisedName(j.name, j.titleIds));
            applyValueColour(item, std::to_string(j.players));

            // X pins this game to the top, or clears the pin if it is already
            // there. The name is captured by value: the row outlives the loop.
            const std::string gameName = j.name;
            item->setClickListener([this, gameName](u64 keys) {
                if ((keys & HidNpadButton_X) != 0) {
                    applyPin(gameName);
                    return true;
                }
                return false;
            });

            list->addItem(item);
            m_rows[j.name] = item;
        }

        m_frame->setContent(list);
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

        // Set by the A handler and cleared when the fetch finishes, so the row
        // responds on the frame the button was pressed rather than up to a
        // second later when the polling thread picks the request up.
        const bool manualRefresh = g_manualRefresh.load();

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
            if (m_totalItem != nullptr) m_totalItem->setValue(m_totalText, true);
            // The game-list section stays empty here. It used to hold a row
            // repeating the count row's "waiting" text, which read as clutter.
            return;
        }

        // The total is sum(jeux[].joueurs), which is the figure the website
        // shows. It is deliberately not sum(counts): a title present in several
        // regions repeats its player count once per regional title id, so that
        // would double-count.
        int total = 0;
        for (const auto& j : jeux) total += j.players;

        // Pinned first, then busiest first.
        //
        // The pinned game is lifted regardless of its player count, which is the
        // point of pinning: the games someone actually plays are rarely the ones
        // with the most players online.
        std::sort(jeux.begin(), jeux.end(), [](const JeuEntry& a, const JeuEntry& b) {
            const int aRank = pinnedRank(a.name);
            const int bRank = pinnedRank(b.name);
            if (aRank >= 0 || bRank >= 0) {
                if (aRank < 0) return false;
                if (bRank < 0) return true;
                return aRank < bRank;
            }
            if (a.players != b.players) return a.players > b.players;
            return a.name < b.name;
        });

        // The rows keep their last good numbers through an automatic poll, and a
        // failed fetch never blanks them - a transient error leaves the panel
        // readable rather than replacing the counts with a warning.
        //
        // A refresh the user asked for is the exception: the count row shows
        // that the press was registered, then goes back to the number.
        m_totalText = std::to_string(total);
        rebuildGameRows(jeux);

        if (manualRefresh && m_totalItem != nullptr) {
            // Faint, because it is a status rather than a value. The number is
            // restored as soon as the flag clears.
            m_totalItem->setValue(T(kRefreshing), true);
        } else {
            applyValueColour(m_totalItem, m_totalText);
        }
    }

    tsl::elm::OverlayFrame* m_frame = nullptr;
    tsl::elm::ListItem* m_totalItem = nullptr;
    tsl::elm::CategoryHeader* m_sectionHeader = nullptr;
    std::map<std::string, tsl::elm::ListItem*> m_rows;
    std::string m_totalText;
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

        // The mutex must exist before anything locks it: the cache loader below
        // takes g_mutex.
        mutexInit(&g_mutex);

        // Put the last known numbers on screen straight away. A fetch on this
        // console can take 8-20s, so without this the panel sits empty.
        loadCountsCache();
        // So the pinned game is at the top on the first draw.
        loadPinnedGame();
        // Same for the name table, so the cached translations are used offline.
        loadNamesCache();

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