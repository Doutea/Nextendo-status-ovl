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
static const UiText kNatLabel    = {"\u7F51\u7EDC\u73AF\u5883", "Network"};
static const UiText kNatTitle    = {"\u7F51\u7EDC\u73AF\u5883", "Network"};
static const UiText kNatSubtitle = {"\u8FDE\u63A5\u4E0E NAT", "link and NAT"};
static const UiText kNatTesting  = {"\u68C0\u6D4B\u4E2D\u2026", "Testing\u2026"};
static const UiText kNatUntested = {"\u672A\u68C0\u6D4B", "Not tested"};
static const UiText kNatRetest   = {"\u91CD\u65B0\u68C0\u6D4B", "Test again"};
static const UiText kNatResult   = {"\u68C0\u6D4B\u7ED3\u679C", "Result"};
static const UiText kNatGrade    = {"NAT \u7C7B\u578B", "NAT type"};
static const UiText kNatMapping  = {"\u6620\u5C04\u884C\u4E3A", "Mapping"};
static const UiText kNatPort     = {"\u7AEF\u53E3\u4FDD\u6301", "Port preserved"};
static const UiText kNatCgnat    = {"\u8FD0\u8425\u5546 NAT", "Carrier NAT"};
static const UiText kNatPublic   = {"\u516C\u7F51\u5730\u5740", "Public address"};
static const UiText kNatLocal    = {"\u672C\u5730\u7F51\u7EDC", "Local network"};
static const UiText kNatLocalIp  = {"\u672C\u5730\u5730\u5740", "Local address"};
static const UiText kNatSubnet   = {"\u5B50\u7F51\u63A9\u7801", "Subnet mask"};
static const UiText kNatGateway  = {"\u7F51\u5173", "Gateway"};
static const UiText kNatDns      = {"DNS", "DNS"};
static const UiText kNatLink     = {"\u8FDE\u63A5\u65B9\u5F0F", "Link"};
static const UiText kNatWired    = {"\u6709\u7EBF", "Ethernet"};
static const UiText kNatWifi     = {"Wi-Fi", "Wi-Fi"};
static const UiText kNatUnknown  = {"\u672A\u77E5", "Unknown"};
static const UiText kNatYes      = {"\u662F", "Yes"};
static const UiText kNatNo       = {"\u5426", "No"};
static const UiText kNatGood     = {"\u826F\u597D", "Good"};
static const UiText kNatPoor     = {"\u4E00\u822C", "Fair"};
static const UiText kNatBad      = {"\u8F83\u5DEE", "Poor"};
static const UiText kNatFailing  = {"\u4E0D\u53EF\u7528", "Unavailable"};
static const UiText kNatEi       = {"\u7AEF\u70B9\u65E0\u5173", "Endpoint-independent"};
static const UiText kNatAd       = {"\u5730\u5740\u76F8\u5173", "Address-dependent"};
static const UiText kNatApd      = {"\u5730\u5740\u7AEF\u53E3\u76F8\u5173", "Address-and-port-dependent"};
static const UiText kNatFilter      = {"\u8FC7\u6EE4\u884C\u4E3A", "Filtering"};
static const UiText kNatNotMeasured = {"\u672A\u6D4B\u5B9A", "Not measured"};

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
static Mutex g_namesMutex;
// When the last attempt was made, so a game that simply has no translation yet
// does not cause a request on every poll.
static std::atomic<u64> g_namesFetchedTick{0};

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
        text += std::to_string(entry.players) + "\t" + cacheEscape(entry.name) + "\n";
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
        entry.name = cacheUnescape(line.substr(tab + 1));
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

// ---------------------------------------------------------------------------
// NAT environment detection
//
// The console's own "NAT type" is worked out by Nintendo's conntest servers and
// is not exposed to homebrew - libnx has no API for it at all - and this overlay
// is used on a private server anyway. What matters in practice is whether the
// network lets two players reach each other, so the behaviour is measured
// directly instead.
//
// Nintendo's A/B/C/D labels are the RFC 4787 mapping and filtering combinations:
//
//   A  mapping endpoint-independent, filtering endpoint-independent or
//      address-dependent
//   B  mapping endpoint-independent, filtering address-and-port-dependent
//   C  mapping not endpoint-independent, but the port is predictable
//   D  everything else
//   F  no UDP at all
//
// The mapping half is measured with STUN, and C is inferred from how predictable
// the mapped port is. The filtering half needs a server that replies from a
// different address, which no public STUN server does (28 were tried), so A and
// B cannot be told apart here and are reported together.
// ---------------------------------------------------------------------------

enum class NatMapping { Unknown, EndpointIndependent, AddressDependent, AddressPortDependent };
enum class NatGrade { Unknown, Good, Poor, Bad, Failing };

struct NatStatus {
    bool tested = false;
    bool udpWorking = false;
    NatGrade grade = NatGrade::Unknown;
    NatMapping mapping = NatMapping::Unknown;
    bool portPreserved = false;
    bool portPredictable = false;
    bool cgnat = false;
    bool cgnatKnown = false;
    std::string publicIp;
    int publicPort = 0;
    std::string localIp;
    std::string subnetMask;
    std::string gateway;
    std::string dns;
    bool linkKnown = false;
    bool wired = false;
    int wifiStrength = 0;
};

static NatStatus g_nat;
static Mutex g_natMutex;
static std::atomic<bool> g_natRequested{false};
static std::atomic<bool> g_natTesting{false};
static std::atomic<bool> g_natDirty{false};
static std::atomic<bool> g_natThreadRunning{false};
static Thread g_natThread;
// The detection thread does DNS and socket work, so it needs the same headroom
// as the polling thread.
alignas(0x1000) static u8 g_natThreadStack[0x40000];
// When the cached result was produced, so a stale one can be re-tested.
static std::atomic<u64> g_natTestedSeconds{0};

static const char* kNatCachePath = "/switch/nextendo-status/nat.txt";

// Addresses that answered when this was written. DNS is tried first, but this
// console's resolver is unreliable, so a fallback keeps the test working.
struct StunServer {
    const char* host;
    u16 port;
    const char* ipFallback;
};

static const StunServer kStunServers[] = {
    // One entry per distinct address: two hostnames that share one address would
    // make the mapping test compare a destination with itself.
    {"stun.l.google.com", 19302, "74.125.250.129"},
    {"stun.cloudflare.com", 3478, "162.159.207.0"},
    {"stun.miwifi.com", 3478, "111.206.174.2"},
    {"stun.hot-chilli.net", 3478, "144.76.136.87"},
    {"stun.nextcloud.com", 3478, "46.225.95.169"},
    {"stun.telnyx.com", 3478, "192.76.120.66"},
    {"stun.dus.net", 3478, "83.125.8.45"},
    {"stun.sipthor.net", 3478, "108.163.134.186"},
};
static const int kStunServerCount = static_cast<int>(sizeof(kStunServers) / sizeof(kStunServers[0]));

static const u32 kStunMagic = 0x2112A442u;
static const u16 kStunBindingRequest = 0x0001;
static const u16 kStunBindingResponse = 0x0101;
static const u16 kStunAttrXorMapped = 0x0020;
static const u16 kStunAttrMapped = 0x0001;

static u64 uptimeMs() {
    return armTicksToNs(armGetSystemTick()) / 1000000ULL;
}

static u64 uptimeSeconds() {
    return armTicksToNs(armGetSystemTick()) / 1000000000ULL;
}

// RFC 1918, RFC 6598 and the other ranges that mean "not reachable from the
// internet". Seeing one of these as the public address is the clearest sign that
// peer-to-peer hosting will not work.
static bool isPrivateOrCgnat(const std::string& ip) {
    unsigned a = 0, b = 0, c = 0, d = 0;
    if (sscanf(ip.c_str(), "%u.%u.%u.%u", &a, &b, &c, &d) != 4) return false;
    if (a == 10) return true;
    if (a == 172 && b >= 16 && b <= 31) return true;
    if (a == 192 && b == 168) return true;
    if (a == 100 && b >= 64 && b <= 127) return true;
    if (a == 169 && b == 254) return true;
    if (a == 127 || a == 0) return true;
    return false;
}

static std::string ipv4ToString(u32 address) {
    char buf[20];
    snprintf(buf, sizeof(buf), "%u.%u.%u.%u",
             static_cast<unsigned>(address & 0xFF),
             static_cast<unsigned>((address >> 8) & 0xFF),
             static_cast<unsigned>((address >> 16) & 0xFF),
             static_cast<unsigned>((address >> 24) & 0xFF));
    return buf;
}

// ---------------------------------------------------------------------------
// Cache, so the panel shows the last result immediately and offline.
// ---------------------------------------------------------------------------

static void saveNatCache(const NatStatus& status) {
    std::string text;
    text += "tested=" + std::to_string(status.tested ? 1 : 0) + "\n";
    text += "at=" + std::to_string(uptimeSeconds()) + "\n";
    text += "udp=" + std::to_string(status.udpWorking ? 1 : 0) + "\n";
    text += "grade=" + std::to_string(static_cast<int>(status.grade)) + "\n";
    text += "mapping=" + std::to_string(static_cast<int>(status.mapping)) + "\n";
    text += "portpreserved=" + std::to_string(status.portPreserved ? 1 : 0) + "\n";
    text += "portpredictable=" + std::to_string(status.portPredictable ? 1 : 0) + "\n";
    text += "cgnat=" + std::to_string(status.cgnat ? 1 : 0) + "\n";
    text += "cgnatknown=" + std::to_string(status.cgnatKnown ? 1 : 0) + "\n";
    text += "publicip=" + cacheEscape(status.publicIp) + "\n";
    text += "publicport=" + std::to_string(status.publicPort) + "\n";
    text += "localip=" + cacheEscape(status.localIp) + "\n";
    text += "subnet=" + cacheEscape(status.subnetMask) + "\n";
    text += "gateway=" + cacheEscape(status.gateway) + "\n";
    text += "dns=" + cacheEscape(status.dns) + "\n";
    text += "linkknown=" + std::to_string(status.linkKnown ? 1 : 0) + "\n";
    text += "wired=" + std::to_string(status.wired ? 1 : 0) + "\n";
    text += "wifi=" + std::to_string(status.wifiStrength) + "\n";

    FsFileSystem fs{};
    if (R_FAILED(fsOpenSdCardFileSystem(&fs))) return;
    fsFsDeleteFile(&fs, kNatCachePath);
    if (R_SUCCEEDED(fsFsCreateFile(&fs, kNatCachePath, text.size(), 0))) {
        FsFile file{};
        if (R_SUCCEEDED(fsFsOpenFile(&fs, kNatCachePath, FsOpenMode_Write, &file))) {
            fsFileWrite(&file, 0, text.data(), text.size(), FsWriteOption_Flush);
            fsFileClose(&file);
        }
    }
    fsFsClose(&fs);
}

static void loadNatCache() {
    FsFileSystem fs{};
    if (R_FAILED(fsOpenSdCardFileSystem(&fs))) return;

    FsFile file{};
    if (R_FAILED(fsFsOpenFile(&fs, kNatCachePath, FsOpenMode_Read, &file))) {
        fsFsClose(&fs);
        return;
    }

    s64 size = 0;
    fsFileGetSize(&file, &size);
    if (size <= 0 || size > 64 * 1024) {
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

    NatStatus loaded;
    loaded.tested = false;

    size_t pos = 0;
    while (pos <= content.size()) {
        const size_t eol = content.find('\n', pos);
        const std::string line =
            content.substr(pos, (eol == std::string::npos ? content.size() : eol) - pos);
        pos = (eol == std::string::npos) ? content.size() + 1 : eol + 1;

        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = line.substr(0, eq);
        const std::string value = cacheUnescape(line.substr(eq + 1));

        if (key == "tested")         loaded.tested = (value == "1");
        else if (key == "at")        g_natTestedSeconds = strtoull(value.c_str(), nullptr, 10);
        else if (key == "udp")       loaded.udpWorking = (value == "1");
        else if (key == "grade")     loaded.grade = static_cast<NatGrade>(atoi(value.c_str()));
        else if (key == "mapping")   loaded.mapping = static_cast<NatMapping>(atoi(value.c_str()));
        else if (key == "portpreserved")   loaded.portPreserved = (value == "1");
        else if (key == "portpredictable") loaded.portPredictable = (value == "1");
        else if (key == "cgnat")     loaded.cgnat = (value == "1");
        else if (key == "cgnatknown") loaded.cgnatKnown = (value == "1");
        else if (key == "publicip")  loaded.publicIp = value;
        else if (key == "publicport") loaded.publicPort = atoi(value.c_str());
        else if (key == "localip")   loaded.localIp = value;
        else if (key == "subnet")    loaded.subnetMask = value;
        else if (key == "gateway")   loaded.gateway = value;
        else if (key == "dns")       loaded.dns = value;
        else if (key == "linkknown") loaded.linkKnown = (value == "1");
        else if (key == "wired")     loaded.wired = (value == "1");
        else if (key == "wifi")      loaded.wifiStrength = atoi(value.c_str());
    }

    if (!loaded.tested) return;
    mutexLock(&g_natMutex);
    g_nat = loaded;
    mutexUnlock(&g_natMutex);
}

// ---------------------------------------------------------------------------
// STUN
// ---------------------------------------------------------------------------

static size_t stunBuildRequest(u8* out) {
    u8 txn[12];
    const u64 r1 = randomGet64();
    const u64 r2 = randomGet64();
    memcpy(txn, &r1, 8);
    memcpy(txn + 8, &r2, 4);

    size_t pos = 0;
    out[pos++] = static_cast<u8>(kStunBindingRequest >> 8);
    out[pos++] = static_cast<u8>(kStunBindingRequest & 0xFF);
    out[pos++] = 0x00;               // no attributes
    out[pos++] = 0x00;
    out[pos++] = 0x21; out[pos++] = 0x12; out[pos++] = 0xA4; out[pos++] = 0x42;
    memcpy(out + pos, txn, sizeof(txn));
    return pos + sizeof(txn);
}

static bool stunParseMapped(const u8* data, size_t len, std::string& ipOut, u16& portOut) {
    if (len < 20) return false;
    const u16 msgType = static_cast<u16>((data[0] << 8) | data[1]);
    const u16 attrLen = static_cast<u16>((data[2] << 8) | data[3]);
    if (msgType != kStunBindingResponse) return false;
    if (data[4] != 0x21 || data[5] != 0x12 || data[6] != 0xA4 || data[7] != 0x42) return false;

    size_t pos = 20;
    const size_t end = 20 + attrLen;
    while (pos + 4 <= len && pos + 4 <= end) {
        const u16 type = static_cast<u16>((data[pos] << 8) | data[pos + 1]);
        const u16 alen = static_cast<u16>((data[pos + 2] << 8) | data[pos + 3]);
        const size_t valuePos = pos + 4;
        if (valuePos + alen > len) break;

        if ((type == kStunAttrXorMapped || type == kStunAttrMapped) && alen >= 8 &&
            data[valuePos + 1] == 0x01) {
            u16 port = static_cast<u16>((data[valuePos + 2] << 8) | data[valuePos + 3]);
            u32 addr = (static_cast<u32>(data[valuePos + 4]) << 24) |
                       (static_cast<u32>(data[valuePos + 5]) << 16) |
                       (static_cast<u32>(data[valuePos + 6]) << 8) |
                       static_cast<u32>(data[valuePos + 7]);
            if (type == kStunAttrXorMapped) {
                port ^= static_cast<u16>(kStunMagic >> 16);
                addr ^= kStunMagic;
            }
            char buf[20];
            snprintf(buf, sizeof(buf), "%u.%u.%u.%u",
                     static_cast<unsigned>((addr >> 24) & 0xFF),
                     static_cast<unsigned>((addr >> 16) & 0xFF),
                     static_cast<unsigned>((addr >> 8) & 0xFF),
                     static_cast<unsigned>(addr & 0xFF));
            ipOut = buf;
            portOut = port;
            return true;
        }
        pos = valuePos + alen + ((4 - (alen % 4)) % 4);
    }
    return false;
}

// One UDP socket, so several queries can come from the same local port. That is
// what makes the mapping test meaningful.
struct StunSocket {
    int fd = -1;
    u16 localPort = 0;

    bool open() {
        fd = ::socket(AF_INET, SOCK_DGRAM, 0);
        if (fd < 0) return false;

        sockaddr_in local{};
        local.sin_family = AF_INET;
        local.sin_port = 0;
        local.sin_addr.s_addr = INADDR_ANY;
        if (::bind(fd, reinterpret_cast<sockaddr*>(&local), sizeof(local)) < 0) {
            ::close(fd);
            fd = -1;
            return false;
        }

        sockaddr_in bound{};
        socklen_t boundLen = sizeof(bound);
        if (::getsockname(fd, reinterpret_cast<sockaddr*>(&bound), &boundLen) == 0)
            localPort = ntohs(bound.sin_port);
        return true;
    }

    void close() {
        if (fd >= 0) ::close(fd);
        fd = -1;
    }

    bool query(const std::string& ip, u16 port, int timeoutMs,
               std::string& mappedIp, u16& mappedPort) {
        if (fd < 0) return false;

        sockaddr_in to{};
        to.sin_family = AF_INET;
        to.sin_port = htons(port);
        to.sin_addr.s_addr = inet_addr(ip.c_str());

        u8 request[32];
        const size_t requestLen = stunBuildRequest(request);
        if (::sendto(fd, request, requestLen, 0,
                     reinterpret_cast<sockaddr*>(&to), sizeof(to)) < 0)
            return false;

        const u64 deadline = uptimeMs() + static_cast<u64>(timeoutMs);
        while (uptimeMs() < deadline) {
            pollfd pfd{};
            pfd.fd = fd;
            pfd.events = POLLIN;
            if (::poll(&pfd, 1, 200) <= 0) continue;

            u8 response[512];
            const int n = ::recv(fd, response, sizeof(response), 0);
            if (n <= 0) continue;
            if (stunParseMapped(response, static_cast<size_t>(n), mappedIp, mappedPort))
                return true;
        }
        return false;
    }
};

static std::string stunServerAddress(const StunServer& server) {
    const std::string resolved = resolveHostToIp(server.host);
    if (!resolved.empty()) return resolved;
    return server.ipFallback;
}

static void readLocalNetwork(NatStatus& out) {
    u32 address = 0, mask = 0, gateway = 0, dns1 = 0, dns2 = 0;
    if (R_SUCCEEDED(nifmGetCurrentIpConfigInfo(&address, &mask, &gateway, &dns1, &dns2))) {
        out.localIp = ipv4ToString(address);
        out.subnetMask = ipv4ToString(mask);
        out.gateway = ipv4ToString(gateway);
        out.dns = ipv4ToString(dns1);
    }

    NifmInternetConnectionType type{};
    u32 strength = 0;
    NifmInternetConnectionStatus status{};
    if (R_SUCCEEDED(nifmGetInternetConnectionStatus(&type, &strength, &status))) {
        out.linkKnown = true;
        out.wired = (type == NifmInternetConnectionType_Ethernet);
        out.wifiStrength = static_cast<int>(strength);
    }
}

// The public address, asked of a plain HTTP echo if STUN gave nothing.
static std::string publicIpOverHttp() {
    std::string error;
    const std::string body = fetchUrl("http://checkip.amazonaws.com", "checkip.amazonaws.com",
                                      std::vector<std::string>(), error);
    if (!error.empty()) return std::string();

    const size_t first = body.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return std::string();
    const size_t last = body.find_last_not_of(" \t\r\n");
    const std::string ip = body.substr(first, last - first + 1);

    unsigned a = 0, b = 0, c = 0, d = 0;
    if (sscanf(ip.c_str(), "%u.%u.%u.%u", &a, &b, &c, &d) != 4) return std::string();
    return ip;
}

static void runNatDetection() {
    NatStatus result;
    readLocalNetwork(result);

    // Mapping behaviour: one socket, several servers. An endpoint-independent NAT
    // reuses the same mapping, so every server reports the same port.
    StunSocket probe;
    if (probe.open()) {
        std::vector<std::string> destinations;
        std::vector<std::string> mappedIps;
        std::vector<u16> mappedPorts;

        for (int i = 0; i < kStunServerCount && destinations.size() < 3; ++i) {
            const std::string address = stunServerAddress(kStunServers[i]);
            if (address.empty()) continue;

            // Compare only distinct destinations. A NAT whose mapping depends on
            // the destination still reuses one mapping per destination address,
            // so probing one address repeatedly would look endpoint-independent
            // when it is not.
            if (std::find(destinations.begin(), destinations.end(), address) !=
                destinations.end())
                continue;

            std::string mappedIp;
            u16 mappedPort = 0;
            if (!probe.query(address, kStunServers[i].port, 2500, mappedIp, mappedPort))
                continue;

            destinations.push_back(address);
            mappedIps.push_back(mappedIp);
            mappedPorts.push_back(mappedPort);

            if (result.publicIp.empty()) {
                result.publicIp = mappedIp;
                result.publicPort = mappedPort;
                result.udpWorking = true;
            }
        }

        // Two distinct destinations are the minimum that can say anything about
        // the mapping; with one, the result is left unknown rather than guessed.
        if (destinations.size() >= 2) {
            const bool samePort = std::all_of(mappedPorts.begin(), mappedPorts.end(),
                                              [&](u16 p) { return p == mappedPorts[0]; });
            const bool sameAddress = std::all_of(
                mappedIps.begin(), mappedIps.end(),
                [&](const std::string& ip) { return ip == mappedIps[0]; });
            if (sameAddress) {
                result.mapping = samePort ? NatMapping::EndpointIndependent
                                          : NatMapping::AddressPortDependent;
            }
        }
        if (!mappedPorts.empty())
            result.portPreserved = (mappedPorts[0] == probe.localPort);
    }
    probe.close();

    // Port predictability is what separates Nintendo's C from D: from several
    // local ports, do the mapped ports move in step with them?
    if (result.mapping != NatMapping::EndpointIndependent &&
        result.mapping != NatMapping::Unknown) {
        std::vector<std::pair<u16, u16>> pairs;
        for (int i = 0; i < 3; ++i) {
            StunSocket s;
            if (!s.open()) continue;
            for (int k = 0; k < kStunServerCount; ++k) {
                const std::string address = stunServerAddress(kStunServers[k]);
                if (address.empty()) continue;
                std::string mappedIp;
                u16 mappedPort = 0;
                if (s.query(address, kStunServers[k].port, 2000, mappedIp, mappedPort)) {
                    pairs.emplace_back(s.localPort, mappedPort);
                    break;
                }
            }
            s.close();
        }
        if (pairs.size() >= 2) {
            const int localDelta = static_cast<int>(pairs[1].first) - static_cast<int>(pairs[0].first);
            const int mappedDelta = static_cast<int>(pairs[1].second) - static_cast<int>(pairs[0].second);
            result.portPredictable = (localDelta != 0) && (localDelta == mappedDelta);
        }
    }

    // Without STUN the public address can still come from HTTP, which is enough
    // to spot carrier-grade NAT.
    if (result.publicIp.empty()) {
        result.publicIp = publicIpOverHttp();
        if (!result.publicIp.empty()) result.publicPort = 0;
    }
    if (!result.publicIp.empty()) {
        result.cgnatKnown = true;
        result.cgnat = isPrivateOrCgnat(result.publicIp);
    }

    // Grading, as far as Nintendo's definitions can be measured here.
    if (!result.udpWorking) {
        result.grade = result.publicIp.empty() ? NatGrade::Failing : NatGrade::Unknown;
    } else if (result.mapping == NatMapping::EndpointIndependent) {
        result.grade = result.cgnat ? NatGrade::Bad : NatGrade::Good;
    } else if (result.mapping == NatMapping::Unknown) {
        result.grade = NatGrade::Unknown;
    } else {
        result.grade = result.portPredictable ? NatGrade::Poor : NatGrade::Bad;
    }

    result.tested = true;
    g_natTestedSeconds = uptimeSeconds();
    {
        mutexLock(&g_natMutex);
        g_nat = result;
        mutexUnlock(&g_natMutex);
    }
    saveNatCache(result);
    g_natDirty = true;
}

static void natThreadFunc(void*) {
    while (g_natThreadRunning.load()) {
        if (g_natRequested.exchange(false)) {
            g_natTesting = true;
            g_natDirty = true;              // show "testing" at once
            runNatDetection();
            g_natTesting = false;
            g_natDirty = true;
        }
        svcSleepThread(200'000'000LL);
    }
}

// Short label for the panel row.
static std::string natSummaryText() {
    if (g_natTesting.load()) return T(kNatTesting);

    NatGrade grade = NatGrade::Unknown;
    bool tested = false;
    {
        mutexLock(&g_natMutex);
        tested = g_nat.tested;
        grade = g_nat.grade;
        mutexUnlock(&g_natMutex);
    }
    if (!tested) return T(kNatUntested);

    switch (grade) {
        case NatGrade::Good:    return T(kNatGood);
        case NatGrade::Poor:    return T(kNatPoor);
        case NatGrade::Bad:     return T(kNatBad);
        case NatGrade::Failing: return T(kNatFailing);
        default:                return T(kNatUnknown);
    }
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
            auto newJeux = parseJeux(raw);

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
    {"Nintendo Switch Sports",
     "Nintendo Switch \u8FD0\u52A8"},
    {"Monopoly para Nintendo Switch",
     "\u5730\u4EA7\u5927\u4EA8"},
};

// Chinese name when the console is set to Chinese, otherwise the API's own
// name. A non-Chinese console has no Chinese font loaded, and forcing a Chinese
// title on it would be both unreadable and wrong for that player's locale.
static std::string localisedName(const std::string& apiName) {
    if (!nextendoIsChinese()) return apiName;

    // The fetched table wins, so an edit in the repository takes effect without
    // a new build.
    {
        mutexLock(&g_namesMutex);
        auto fetched = g_namesZhOverride.find(apiName);
        if (fetched != g_namesZhOverride.end()) {
            const std::string name = fetched->second;
            mutexUnlock(&g_namesMutex);
            return name;
        }
        mutexUnlock(&g_namesMutex);
    }

    auto builtin = g_gameNamesZh.find(apiName);
    return (builtin != g_gameNamesZh.end()) ? builtin->second : apiName;
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
static const char* kNamesCachePath = "/switch/nextendo-status/names_zh.txt";

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
    mutexLock(&g_namesMutex);
    g_namesZhOverride = std::move(table);
    mutexUnlock(&g_namesMutex);
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

static void fetchNamesTable() {
    // Recorded before the request so a failure still starts the cooldown.
    g_namesFetchedTick = armGetSystemTick();

    std::string error;
    const std::string raw =
        fetchUrl(NAMES_ZH_URL, "raw.githubusercontent.com", GITHUB_FALLBACK_IPS, error);
    if (!error.empty() || raw.empty()) return;

    auto parsed = parseNamesTable(raw);
    if (parsed.empty()) return;

    applyNamesTable(std::move(parsed));
    saveNamesCache(raw);
    g_dirty = true;   // redraw so the new names appear
}

// Called after a counts fetch. Only asks the repository for the table when some
// game actually lacks a translation, and at most once per cooldown: the network
// on this console is slow, and a game that has no translation yet must not cost
// a request on every poll.
static void maybeRefreshNames(const std::vector<JeuEntry>& jeux) {
    if (!g_useChinese || jeux.empty()) return;

    bool missing = false;
    mutexLock(&g_namesMutex);
    for (const auto& entry : jeux) {
        if (g_namesZhOverride.count(entry.name) == 0 &&
            g_gameNamesZh.count(entry.name) == 0) {
            missing = true;
            break;
        }
    }
    mutexUnlock(&g_namesMutex);
    if (!missing) return;

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

// The NAT detail screen: one row per measurement, plus a row that re-runs the
// test. B goes back, which libtesla handles for any Gui that does not consume it.
class NatDetailGui : public tsl::Gui {
public:
    virtual tsl::elm::Element* createUI() override {
        m_frame = new tsl::elm::OverlayFrame(T(kNatTitle), T(kNatSubtitle));
        auto* list = new tsl::elm::List();

        m_retestItem = new tsl::elm::ListItem(T(kNatRetest));
        m_retestItem->setClickListener([](u64 keys) {
            if ((keys & HidNpadButton_A) != 0) {
                g_natRequested = true;
                return true;
            }
            return false;
        });
        list->addItem(m_retestItem);

        list->addItem(new tsl::elm::CategoryHeader(T(kNatResult)));
        m_gradeItem = new tsl::elm::ListItem(T(kNatGrade));
        list->addItem(m_gradeItem);
        m_mappingItem = new tsl::elm::ListItem(T(kNatMapping));
        list->addItem(m_mappingItem);
        m_portItem = new tsl::elm::ListItem(T(kNatPort));
        list->addItem(m_portItem);
        m_filterItem = new tsl::elm::ListItem(T(kNatFilter));
        list->addItem(m_filterItem);
        m_cgnatItem = new tsl::elm::ListItem(T(kNatCgnat));
        list->addItem(m_cgnatItem);
        m_publicItem = new tsl::elm::ListItem(T(kNatPublic));
        list->addItem(m_publicItem);

        list->addItem(new tsl::elm::CategoryHeader(T(kNatLocal)));
        m_localIpItem = new tsl::elm::ListItem(T(kNatLocalIp));
        list->addItem(m_localIpItem);
        m_subnetItem = new tsl::elm::ListItem(T(kNatSubnet));
        list->addItem(m_subnetItem);
        m_gatewayItem = new tsl::elm::ListItem(T(kNatGateway));
        list->addItem(m_gatewayItem);
        m_dnsItem = new tsl::elm::ListItem(T(kNatDns));
        list->addItem(m_dnsItem);
        m_linkItem = new tsl::elm::ListItem(T(kNatLink));
        list->addItem(m_linkItem);

        m_frame->setContent(list);
        refresh();
        return m_frame;
    }

    virtual void update() override {
        if (g_natDirty.exchange(false)) {
            refresh();
        }
    }

private:
    static const char* mappingText(NatMapping mapping) {
        switch (mapping) {
            case NatMapping::EndpointIndependent:   return T(kNatEi);
            case NatMapping::AddressDependent:      return T(kNatAd);
            case NatMapping::AddressPortDependent:  return T(kNatApd);
            default:                                return T(kNatUnknown);
        }
    }

    static const char* gradeText(NatGrade grade) {
        switch (grade) {
            case NatGrade::Good:    return T(kNatGood);
            case NatGrade::Poor:    return T(kNatPoor);
            case NatGrade::Bad:     return T(kNatBad);
            case NatGrade::Failing: return T(kNatFailing);
            default:                return T(kNatUnknown);
        }
    }

    void setValue(tsl::elm::ListItem* item, const std::string& value, bool faint) {
        if (item != nullptr) item->setValue(value, faint);
    }

    void refresh() {
        NatStatus snapshot;
        {
            mutexLock(&g_natMutex);
            snapshot = g_nat;
            mutexUnlock(&g_natMutex);
        }
        const bool testing = g_natTesting.load();

        if (testing) {
            m_frame->setSubtitle(T(kNatTesting));
        } else if (snapshot.tested) {
            m_frame->setSubtitle(T(kNatSubtitle));
        } else {
            m_frame->setSubtitle(T(kNatUntested));
        }

        if (m_retestItem != nullptr)
            m_retestItem->setValue(testing ? T(kNatTesting) : "", true);

        if (!snapshot.tested) {
            setValue(m_gradeItem, T(kNatUnknown), true);
            setValue(m_mappingItem, T(kNatUnknown), true);
            setValue(m_portItem, T(kNatUnknown), true);
            setValue(m_filterItem, T(kNatUnknown), true);
            setValue(m_cgnatItem, T(kNatUnknown), true);
            setValue(m_publicItem, T(kNatUnknown), true);
        } else {
            setValue(m_gradeItem, gradeText(snapshot.grade), false);
            setValue(m_mappingItem, mappingText(snapshot.mapping), false);
            setValue(m_portItem, snapshot.portPreserved ? T(kNatYes) : T(kNatNo), false);
            // Stated plainly: no public STUN server can drive the filtering
            // test, so the cone subtype is genuinely unknown.
            setValue(m_filterItem, T(kNatNotMeasured), true);

            if (!snapshot.cgnatKnown) {
                setValue(m_cgnatItem, T(kNatUnknown), true);
            } else {
                setValue(m_cgnatItem, snapshot.cgnat ? T(kNatYes) : T(kNatNo), false);
            }

            if (snapshot.publicIp.empty()) {
                setValue(m_publicItem, T(kNatUnknown), true);
            } else if (snapshot.publicPort > 0) {
                setValue(m_publicItem,
                         snapshot.publicIp + ":" + std::to_string(snapshot.publicPort), false);
            } else {
                setValue(m_publicItem, snapshot.publicIp, false);
            }
        }

        // The local half is read from nifm and is available even before a test.
        setValue(m_localIpItem, snapshot.localIp.empty() ? T(kNatUnknown) : snapshot.localIp, false);
        setValue(m_subnetItem, snapshot.subnetMask.empty() ? T(kNatUnknown) : snapshot.subnetMask, false);
        setValue(m_gatewayItem, snapshot.gateway.empty() ? T(kNatUnknown) : snapshot.gateway, false);
        setValue(m_dnsItem, snapshot.dns.empty() ? T(kNatUnknown) : snapshot.dns, false);

        if (!snapshot.linkKnown) {
            setValue(m_linkItem, T(kNatUnknown), true);
        } else if (snapshot.wired) {
            setValue(m_linkItem, T(kNatWired), false);
        } else {
            setValue(m_linkItem,
                     std::string(T(kNatWifi)) + " " + std::to_string(snapshot.wifiStrength) + "/3",
                     false);
        }
    }

    tsl::elm::OverlayFrame* m_frame = nullptr;
    tsl::elm::ListItem* m_retestItem = nullptr;
    tsl::elm::ListItem* m_gradeItem = nullptr;
    tsl::elm::ListItem* m_mappingItem = nullptr;
    tsl::elm::ListItem* m_portItem = nullptr;
    tsl::elm::ListItem* m_filterItem = nullptr;
    tsl::elm::ListItem* m_cgnatItem = nullptr;
    tsl::elm::ListItem* m_publicItem = nullptr;
    tsl::elm::ListItem* m_localIpItem = nullptr;
    tsl::elm::ListItem* m_subnetItem = nullptr;
    tsl::elm::ListItem* m_gatewayItem = nullptr;
    tsl::elm::ListItem* m_dnsItem = nullptr;
    tsl::elm::ListItem* m_linkItem = nullptr;
};

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

        // Network environment. Pressing A opens the detail screen; the test
        // itself runs on its own thread so the panel never waits for it.
        m_natItem = new tsl::elm::ListItem(T(kNatLabel), natSummaryText());
        m_natItem->setClickListener([](u64 keys) {
            if ((keys & HidNpadButton_A) != 0) {
                tsl::changeTo<NatDetailGui>();
                return true;
            }
            return false;
        });
        list->addItem(m_natItem);


        // The list itself. This heading also separates the two blocks.
        m_sectionHeader = new tsl::elm::CategoryHeader(T(kGameList));
        list->addItem(m_sectionHeader);

        m_frame->setContent(list);
        m_list = list;
        refreshUI();
        return m_frame;
    }

    virtual void update() override {
        if (g_natDirty.exchange(false)) {
            refreshUI();
        }
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
        for (const auto& j : jeux) names.push_back(localisedName(j.name));

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

        // Network environment. Pressing A opens the detail screen; the test
        // itself runs on its own thread so the panel never waits for it.
        m_natItem = new tsl::elm::ListItem(T(kNatLabel), natSummaryText());
        m_natItem->setClickListener([](u64 keys) {
            if ((keys & HidNpadButton_A) != 0) {
                tsl::changeTo<NatDetailGui>();
                return true;
            }
            return false;
        });
        list->addItem(m_natItem);


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

        // Busiest first.
        std::sort(jeux.begin(), jeux.end(), [](const JeuEntry& a, const JeuEntry& b) {
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
    tsl::elm::List* m_list = nullptr;
    tsl::elm::ListItem* m_totalItem = nullptr;
    tsl::elm::ListItem* m_natItem = nullptr;
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

        // The mutexes must exist before anything locks them: the cache loaders
        // below take g_mutex.
        mutexInit(&g_mutex);
        mutexInit(&g_natMutex);

        // Put the last known numbers on screen straight away. A fetch on this
        // console can take 8-20s, so without this the panel sits empty.
        loadCountsCache();
        // Same for the name table, so the cached translations are used offline.
        loadNamesCache();
        // And the last NAT result, so the row is filled in immediately.
        loadNatCache();

        // Nothing here performs network I/O any more. The game-config download
        // and the first counts fetch both happen on the polling thread started
        // below, which is what lets the panel appear immediately instead of
        // waiting out a request.

        g_threadRunning = true;
        threadCreate(&g_pollThread, pollThreadFunc, nullptr, g_threadStack, sizeof(g_threadStack), 0x2C, -2);
        threadStart(&g_pollThread);

        // A stale result is re-tested in the background. The address of a home
        // network rarely changes, so this is mainly for the first run.
        {
            bool needTest = false;
            {
                mutexLock(&g_natMutex);
                needTest = !g_nat.tested;
                mutexUnlock(&g_natMutex);
            }
            if (!needTest) {
                const u64 last = g_natTestedSeconds.load();
                const u64 now = uptimeSeconds();
                if (last == 0 || now < last || (now - last) > 1800) needTest = true;
            }
            if (needTest) g_natRequested = true;
        }

        g_natThreadRunning = true;
        threadCreate(&g_natThread, natThreadFunc, nullptr, g_natThreadStack,
                     sizeof(g_natThreadStack), 0x2C, -2);
        threadStart(&g_natThread);
    }
    virtual void exitServices() override {
        g_threadRunning = false;
        threadWaitForExit(&g_pollThread);
        threadClose(&g_pollThread);

        g_natThreadRunning = false;
        threadWaitForExit(&g_natThread);
        threadClose(&g_natThread);

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