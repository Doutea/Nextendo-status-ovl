// Run the actual account worker with deterministic HTTP/filesystem doubles.
#define CURL_STATICLIB
#include <curl/curl.h>
#include <atomic>
#include <cassert>
#include <cstring>
#include <deque>
#include <functional>
#include <iostream>
#include <map>
#include <mutex>
#include <string>
#include <vector>
#include <cstdarg>
using u64 = unsigned long long;
using s64 = long long;
using Result = int;
using Mutex = std::mutex;
struct FsFileSystem {};
struct FsFile { std::string name; };
constexpr int FsOpenMode_Read = 1, FsOpenMode_Write = 2, FsWriteOption_Flush = 1, FsReadOption_None = 0;
#define R_FAILED(rc) ((rc) != 0)
#define R_SUCCEEDED(rc) ((rc) == 0)
static std::map<std::string, std::string> files;
static bool failFs = false, g_useChinese = false;
static Mutex g_mutex;
static std::atomic<bool> g_threadRunning{true};
static const std::vector<std::string> NEXTENDO_FALLBACK_IPS;
static void mutexLock(Mutex* m) { m->lock(); }
static void mutexUnlock(Mutex* m) { m->unlock(); }
static u64 armGetSystemTick() { static u64 tick = 0; return ++tick; }
static Result fsOpenSdCardFileSystem(FsFileSystem*) { return failFs ? 1 : 0; }
static Result fsFsDeleteFile(FsFileSystem*, const char* p) { return files.erase(p) ? 0 : 1; }
static Result fsFsCreateFile(FsFileSystem*, const char* p, size_t n, int) { files[p] = std::string(n, '\0'); return 0; }
static Result fsFsOpenFile(FsFileSystem*, const char* p, int, FsFile* f) { f->name = p; return files.count(p) ? 0 : 1; }
static Result fsFileWrite(FsFile* f, int, const void* p, size_t n, int) { files[f->name].assign(static_cast<const char*>(p), n); return 0; }
static Result fsFileGetSize(FsFile* f, s64* n) { *n = files[f->name].size(); return 0; }
static Result fsFileRead(FsFile* f, int, void* p, size_t n, int, u64* read) { std::memcpy(p, files[f->name].data(), n); *read = n; return 0; }
static Result fsFsRenameFile(FsFileSystem*, const char* a, const char* b) { files[b] = files.at(a); files.erase(a); return 0; }
static void fsFileClose(FsFile*) {}
static void fsFsClose(FsFileSystem*) {}
struct FakeCurl { long status = 0; const char* post = nullptr; curl_slist* headers = nullptr; };
extern "C" CURL* curl_easy_init() { return reinterpret_cast<CURL*>(new FakeCurl); }
extern "C" void curl_easy_cleanup(CURL* c) { delete reinterpret_cast<FakeCurl*>(c); }
extern "C" CURLcode curl_easy_setopt(CURL* c, CURLoption option, ...) {
    auto* f = reinterpret_cast<FakeCurl*>(c);
    va_list args; va_start(args, option);
    if (option == CURLOPT_HTTPHEADER) f->headers = va_arg(args, curl_slist*);
    else if (option == CURLOPT_POSTFIELDS) f->post = va_arg(args, const char*);
    va_end(args); return CURLE_OK;
}
extern "C" CURLcode curl_easy_getinfo(CURL* c, CURLINFO info, ...) {
    assert(info == CURLINFO_RESPONSE_CODE);
    va_list args; va_start(args, info);
    *va_arg(args, long*) = reinterpret_cast<FakeCurl*>(c)->status;
    va_end(args); return CURLE_OK;
}
extern "C" curl_slist* curl_slist_append(curl_slist* list, const char* text) {
    auto* node = new curl_slist{new char[std::strlen(text) + 1], nullptr};
    std::strcpy(node->data, text);
    if (!list) return node;
    auto* tail = list; while (tail->next) tail = tail->next;
    tail->next = node; return list;
}
extern "C" void curl_slist_free_all(curl_slist* list) {
    while (list) { auto* next = list->next; delete[] list->data; delete list; list = next; }
}
static bool performOnHandle(CURL*, const std::string&, const std::string&, const std::vector<std::string>&,
                            std::string&, std::string&, bool, long, long, bool);
#include "../source/relay_route.hpp"
static nextendo::RelayRoute g_relayRoute;
static u64 armTicksToNs(u64 t) { return t * 1000; }
#include "../source/account.hpp"
struct Reply { std::string path; long status; std::string body; bool transport = true; };
static std::deque<Reply> replies;
static std::function<void()> onRequest;
static bool performOnHandle(CURL* c, const std::string& url, const std::string& host,
                            const std::vector<std::string>&, std::string& body, std::string&, bool bounded, long, long, bool) {
    assert(bounded && host == "nextendo.network" && !replies.empty());
    auto* f = reinterpret_cast<FakeCurl*>(c);
    Reply reply = replies.front(); replies.pop_front();
    assert(url == "https://nextendo.network" + reply.path);
    bool client = false, bearer = false;
    for (auto* h = f->headers; h; h = h->next) {
        client |= std::string(h->data) == "X-Nextendo-Client-Id: nxc_hvGUwjpszrDT";
        bearer |= std::string(h->data).find("Authorization: Bearer ") == 0;
    }
    assert(client);
    if (reply.path == "/api/login") {
        assert(!bearer && f->post);
        json_error_t error{}; json_t* post = json_loads(f->post, 0, &error);
        assert(post && std::string(json_string_value(json_object_get(post, "password"))) == "p\"a\\ss");
        json_decref(post);
    } else assert(bearer && !f->post);
    if (onRequest) { auto fn = std::move(onRequest); onRequest = {}; fn(); }
    f->status = reply.status; body = reply.body;
    return reply.transport;
}
static void queueLogin(bool remember = true) {
    g_account.login = "test@example.com";
    g_account.password = "p\"a\\ss";
    g_account.remember = remember;
    g_account.busy = true;
    g_account.job = AccountJob::Login;
    ++g_account.epoch;
}

static const std::vector<std::string> mario = {"0100152000022000"};
static const std::vector<std::string> smash = {"01006a800016e000"};
static void resetState() {
    g_account = {}; g_threadRunning = true; g_liveVisible = false;
    g_accountWake = false; files.clear(); replies.clear(); onRequest = {};
}
static void liveReply(const std::string& id, const std::string& name) {
    replies.push_back({"/api/status/live", 200,
        "{\"games\":[{\"titles\":[\"" + id + "\"],\"players\":[{\"name\":\"" + name + "\"}],\"lobbies\":[]}]}"});
}
int main() {
    resetState();
    queueLogin();
    replies.push_back({"/api/login", 200, R"({"token":"a.b.c"})"});
    replies.push_back({"/api/me", 200, R"({"account":{"name":"Tester"}})"});
    assert(serviceAccount() && g_account.token == "a.b.c" && !g_account.busy);
    assert(g_account.userName == "Tester" && files.at(kSessionPath) == "a.b.c");

    // Opening a room while the startup identity request is pending must queue
    // its fetch, instead of silently losing the only page-open request.
    g_account = {};
    loadSession();
    assert(g_account.job == AccountJob::Identity && g_account.busy);
    requestLive(mario);
    assert(g_account.livePending && g_account.job == AccountJob::Identity);
    replies.push_back({"/api/me", 200, R"({"account":{"name":"Tester"}})"});
    assert(serviceAccount() && g_account.job == AccountJob::Refresh && g_account.busy);
    liveReply(mario[0], "Alice");
    assert(serviceAccount() && !g_account.busy && g_account.games[0].players[0].name == "Alice");

    // B from a loading page cancels its result without deleting the account.
    requestLive(mario);
    liveReply(mario[0], "late");
    onRequest = cancelLive;
    assert(serviceAccount() && g_account.games.empty() && !g_account.busy && g_account.token == "a.b.c");
    assert(g_account.fetchedTick == 0 && g_account.job == AccountJob::None);

    // Changing games while an old refresh is in flight invalidates that result
    // and leaves the new request pending after the old worker has unwound.
    requestLive(mario);
    liveReply(mario[0], "old");
    onRequest = [] { requestLive(smash); };
    assert(serviceAccount() && g_account.games.empty());
    assert(g_account.job == AccountJob::Refresh && g_account.busy && g_account.wanted == smash);
    liveReply(smash[0], "Bob");
    assert(serviceAccount() && !g_account.busy && g_account.games[0].players[0].name == "Bob");

    requestLive(mario); cancelLive();
    assert(!serviceAccount() && !g_account.busy); // cancelled before starting

    requestLive(mario);
    replies.push_back({"/api/status/live", 401, R"({"error":"expired"})"});
    assert(serviceAccount() && g_account.token.empty() && !g_account.busy && !files.count(kSessionPath));

    // A page closed during identity restore must not cause a later live fetch.
    resetState(); files[kSessionPath] = "a.b.c"; loadSession(); requestLive(mario);
    cancelLive();
    replies.push_back({"/api/me", 200, R"({"account":{"name":"Tester"}})"});
    assert(serviceAccount() && !g_account.busy && g_account.job == AccountJob::None);

    // Loading a room during an already executing restore also retains it.
    g_account.job = AccountJob::Identity; g_account.busy = true;
    replies.push_back({"/api/me", 200, R"({"account":{"name":"Tester"}})"});
    onRequest = [] { requestLive(mario); };
    assert(serviceAccount() && g_account.job == AccountJob::Refresh);
    liveReply(mario[0], "Ready");
    assert(serviceAccount() && g_account.games[0].players[0].name == "Ready");

    // Shutdown during a transfer never publishes data or schedules another job.
    requestLive(mario); liveReply(mario[0], "after-close");
    onRequest = [] { g_threadRunning = false; cancelLive(); };
    assert(serviceAccount() && !g_account.busy && !g_account.livePending && g_account.games.empty());
    assert(g_account.activeJob == AccountJob::None && g_account.job == AccountJob::None);

    resetState(); queueLogin();
    replies.push_back({"/api/login", 200, R"({"token":"late.login"})"});
    onRequest = logoutAccount;
    assert(serviceAccount() && g_account.token.empty() && !files.count(kSessionPath));
    assert(replies.empty());
    std::cout << "Account restore, cancellation and shutdown checks passed\n";
}
