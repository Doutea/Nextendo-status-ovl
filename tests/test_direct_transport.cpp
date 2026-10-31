#define CURL_STATICLIB
#include <curl/curl.h>
#include <atomic>
#include <cassert>
#include <cstdarg>
#include <cstring>
#include <deque>
#include <iostream>
#include <map>
#include "../source/live_model.hpp"
#include "../source/transport_policy.hpp"
using u64 = unsigned long long;
using s64 = long long;
using Result = int;
struct FsFileSystem {};
struct FsFile {};
constexpr int FsOpenMode_Write = 2, FsWriteOption_Flush = 1;
#define R_FAILED(x) ((x) != 0)
#define R_SUCCEEDED(x) ((x) == 0)
static std::string diagnostic;
static Result fsOpenSdCardFileSystem(FsFileSystem*) { return 0; }
static Result fsFsDeleteFile(FsFileSystem*, const char*) { return 0; }
static Result fsFsCreateFile(FsFileSystem*, const char*, size_t, int) { return 0; }
static Result fsFsOpenFile(FsFileSystem*, const char*, int, FsFile*) { return 0; }
static Result fsFileWrite(FsFile*, int, const void* data, size_t n, int) { diagnostic.assign(static_cast<const char*>(data), n); return 0; }
static void fsFileClose(FsFile*) {}
static void fsFsClose(FsFileSystem*) {}
static std::atomic<bool> g_threadRunning{true};
static std::map<std::string, std::string> g_cachedIpByHost;
static u64 clockMs = 0;
static u64 armGetSystemTick() { return clockMs; }
static u64 armTicksToNs(u64 tick) { return tick * 1000000; }
static std::vector<std::string> resolveHostIps(const std::string&) { return {"104.21.84.82", "172.67.190.75"}; }
static int shutdownProgress(void*, curl_off_t, curl_off_t, curl_off_t, curl_off_t) { return g_threadRunning ? 0 : 1; }
using WriteFn = size_t (*)(void*, size_t, size_t, void*);
struct Reply {
    CURLcode code; long status; std::string body;
    long requestBytes = 0; curl_off_t uploaded = 0;
    u64 elapsed = 100;
};
struct FakeCurl {
    std::string url, resolve, ip, encoding;
    WriteFn write = nullptr;
    void* sink = nullptr;
    curl_xferinfo_callback progress = nullptr;
    void* progressData = nullptr;
    std::map<CURLoption, long> options;
    Reply current{CURLE_OK, 0, ""};
};
static std::deque<Reply> replies;
static std::vector<std::string> attempts;
extern "C" CURLcode curl_easy_setopt(CURL* ptr, CURLoption option, ...) {
    auto& c = *reinterpret_cast<FakeCurl*>(ptr);
    va_list args; va_start(args, option);
    switch (option) {
        case CURLOPT_URL: c.url = va_arg(args, const char*); break;
        case CURLOPT_ACCEPT_ENCODING: c.encoding = va_arg(args, const char*); break;
        case CURLOPT_WRITEFUNCTION: c.write = va_arg(args, WriteFn); break;
        case CURLOPT_WRITEDATA: c.sink = va_arg(args, void*); break;
        case CURLOPT_XFERINFOFUNCTION: c.progress = va_arg(args, curl_xferinfo_callback); break;
        case CURLOPT_XFERINFODATA: c.progressData = va_arg(args, void*); break;
        case CURLOPT_RESOLVE: {
            const auto* list = va_arg(args, curl_slist*);
            if (list) { assert(!list->next); c.resolve = list->data; }
            else c.resolve.clear();
            break;
        }
        default: c.options[option] = va_arg(args, long); break;
    }
    va_end(args); return CURLE_OK;
}
extern "C" CURLcode curl_easy_getinfo(CURL* ptr, CURLINFO info, ...) {
    auto& c = *reinterpret_cast<FakeCurl*>(ptr);
    va_list args; va_start(args, info);
    switch (info) {
        case CURLINFO_RESPONSE_CODE: *va_arg(args, long*) = c.current.status; break;
        case CURLINFO_REQUEST_SIZE: *va_arg(args, long*) = c.current.requestBytes; break;
        case CURLINFO_SIZE_UPLOAD_T: *va_arg(args, curl_off_t*) = c.current.uploaded; break;
        case CURLINFO_PRIMARY_IP: *va_arg(args, char**) = c.ip.data(); break;
        case CURLINFO_TOTAL_TIME: *va_arg(args, double*) = c.current.elapsed / 1000.0; break;
        default: *va_arg(args, double*) = c.current.status ? 0.1 : 0.0; break;
    }
    va_end(args); return CURLE_OK;
}
extern "C" curl_slist* curl_slist_append(curl_slist* list, const char* value) {
    assert(!list);
    auto* item = new curl_slist{new char[std::strlen(value) + 1], nullptr};
    std::strcpy(item->data, value); return item;
}
extern "C" void curl_slist_free_all(curl_slist* item) { if (item) { delete[] item->data; delete item; } }
extern "C" const char* curl_easy_strerror(CURLcode) { return "Simulated transport failure"; }
extern "C" CURLcode curl_easy_perform(CURL* ptr) {
    auto& c = *reinterpret_cast<FakeCurl*>(ptr);
    assert(!replies.empty() && c.write && c.sink);
    assert(c.options[CURLOPT_SSL_VERIFYPEER] == 1 && c.options[CURLOPT_SSL_VERIFYHOST] == 2);
    assert(c.options[CURLOPT_HTTP_VERSION] == CURL_HTTP_VERSION_1_1);
    assert(c.options[CURLOPT_SSLVERSION] == (CURL_SSLVERSION_TLSv1_2 | CURL_SSLVERSION_MAX_TLSv1_2));
    assert(c.options[CURLOPT_LOW_SPEED_TIME] == 0 && c.encoding == "gzip, deflate");
    assert(c.resolve.rfind("nextendo.network:443:", 0) == 0);
    c.ip = c.resolve.substr(std::string("nextendo.network:443:").size());
    attempts.push_back(c.ip);
    c.current = replies.front(); replies.pop_front(); clockMs += c.current.elapsed;
    for (char& ch : c.current.body)
        if (c.write(&ch, 1, 1, c.sink) != 1) return CURLE_WRITE_ERROR;
    return c.current.code;
}
#include "../source/http_transport.hpp"
static int accountCallback(void*, curl_off_t, curl_off_t, curl_off_t, curl_off_t) { return 0; }
static void reset() { replies.clear(); attempts.clear(); g_cachedIpByHost.clear(); clockMs = 0; g_threadRunning = true; }
int main() {
    using namespace nextendo;
    assert((ipCandidates("b", {"a", "b"}, {"b", "c", "d", "e"}) == std::vector<std::string>{"b", "a", "c", "d"}));
    assert(retryTransport(CURLE_COULDNT_CONNECT, 0, false, 0, 0));
    assert(!retryTransport(CURLE_ABORTED_BY_CALLBACK, 0, false, 0, 0));
    assert(!retryTransport(CURLE_WRITE_ERROR, 200, false, 0, 0));
    for (long status : {401L, 403L, 429L, 503L}) assert(!retryTransport(CURLE_OPERATION_TIMEDOUT, status, false, 0, 0));
    assert(!retryTransport(CURLE_OPERATION_TIMEDOUT, 0, true, 250, 30));
    assert(retryTransport(CURLE_SSL_CONNECT_ERROR, 0, true, 0, 0));
    assert(std::string(transportStage(0, 0, 0, 0)) == "connect");
    assert(std::string(transportStage(.1, 0, 0, 0)) == "TLS");
    assert(std::string(transportStage(.1, .2, 0, 0)) == "response");
    assert(std::string(transportStage(0, 0, 200, 0)) == "headers");
    for (const auto& invalid : {"<html>", "{]", "{\"x\":\"a\n\"}", "{}junk"}) {
        JsonFrame frame; frame.push(invalid, std::strlen(invalid)); assert(!frame.complete());
    }
    const std::string utf8 = R"({"name":"甲乙 } \" ","games":[{"x":[]}]} )";
    JsonFrame frame;
    for (char ch : utf8) frame.push(&ch, 1);
    assert(frame.complete());
    JsonFrame deep; const std::string depth(129, '['); deep.push(depth.data(), depth.size()); assert(deep.invalid);

    FakeCurl c; int epoch = 1; c.progress = accountCallback; c.progressData = &epoch;
    CURL* curl = reinterpret_cast<CURL*>(&c);
    std::string body, error;
    auto run = [&](bool post = false) { return performOnHandle(curl, "https://nextendo.network/api/status/live", "nextendo.network", {}, body, error, true, 20, 10, post); };
    reset();
    replies.push_back({CURLE_COULDNT_CONNECT, 0, "", 0, 0, 5000});
    replies.push_back({CURLE_OK, 200, R"({"games":[]})"});
    assert(run() && body == R"({"games":[]})" && error.empty());
    assert((attempts == std::vector<std::string>{"104.21.84.82", "172.67.190.75"}));
    assert(g_cachedIpByHost["nextendo.network"] == "172.67.190.75");
    assert(c.progress == accountCallback && c.progressData == &epoch);
    assert(c.sink == nullptr && c.resolve.empty());
    attempts.clear(); replies.push_back({CURLE_OK, 401, R"({"error":"expired"})"});
    assert(run() && attempts.size() == 1 && attempts[0] == "172.67.190.75" && c.current.status == 401);
    assert(diagnostic.find("json_done=1") != std::string::npos);

    reset(); replies.push_back({CURLE_OPERATION_TIMEDOUT, 0, "", 300, 20});
    assert(!run(true) && attempts.size() == 1); // login transmitted: never replay
    reset(); replies.push_back({CURLE_SSL_CONNECT_ERROR, 0, ""});
    replies.push_back({CURLE_OK, 200, R"({"token":"not-logged"})"});
    assert(run(true) && attempts.size() == 2);
    assert(diagnostic.find("not-logged") == std::string::npos);
    reset(); replies.push_back({CURLE_PARTIAL_FILE, 200, R"({"games":[)"});
    replies.push_back({CURLE_OK, 200, R"({"games":[]})"});
    assert(run() && attempts.size() == 2 && body == R"({"games":[]})");
    reset(); replies.push_back({CURLE_ABORTED_BY_CALLBACK, 0, ""});
    assert(!run() && attempts.size() == 1);
    reset(); replies.push_back({CURLE_OK, 200, "{\"x\":\"" + std::string(kMaxBody, 'a') + "\"}"});
    assert(!run() && attempts.size() == 1 && error.find("96 KB") != std::string::npos);
    reset(); replies.push_back({CURLE_OPERATION_TIMEDOUT, 0, "", 0, 0, 20000});
    assert(!run() && attempts.size() == 1); // total budget, not per-IP full timeouts
    assert(replies.empty());
    std::cout << "Direct transport checks passed\n";
}
