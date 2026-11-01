#pragma once
#include <cstdio>
// Included by main.cpp after its resolver/cache declarations. The existing
// network worker remains the only caller; no GUI or account data is logged.
struct HttpSink {
    std::string* body;
    bool stopAtJson;
    nextendo::JsonFrame frame;
    bool stopped = false, tooLarge = false;
};
static size_t httpWrite(void* bytes, size_t size, size_t count, void* user) {
    auto& sink = *static_cast<HttpSink*>(user);
    if (size && count > nextendo::kMaxBody / size) { sink.tooLarge = true; return 0; }
    const size_t n = size * count;
    if (n > nextendo::kMaxBody - sink.body->size()) { sink.tooLarge = true; return 0; }
    sink.body->append(static_cast<const char*>(bytes), n);
    if (sink.stopAtJson) {
        sink.frame.push(static_cast<const char*>(bytes), n);
        if (sink.frame.complete()) { sink.stopped = true; return 0; }
    }
    return n;
}
static void recordNetworkTrace(const std::string& line) {
    static std::vector<std::string> recent;
    recent.push_back(line.substr(0, 384));
    if (recent.size() > 24) recent.erase(recent.begin());
    std::string text = "Nextendo transport v1.5.12; times in ms; no credentials or response bodies\n";
    for (const auto& entry : recent) text += entry + "\n";
    FsFileSystem fs{};
    if (R_FAILED(fsOpenSdCardFileSystem(&fs))) return;
    const char* path = "/switch/nextendo-status/network-debug.txt";
    fsFsDeleteFile(&fs, path);
    if (R_SUCCEEDED(fsFsCreateFile(&fs, path, text.size(), 0))) {
        FsFile file{};
        if (R_SUCCEEDED(fsFsOpenFile(&fs, path, FsOpenMode_Write, &file))) {
            fsFileWrite(&file, 0, text.data(), text.size(), FsWriteOption_Flush);
            fsFileClose(&file);
        }
    }
    fsFsClose(&fs);
}
static bool performOnHandle(CURL* curl, const std::string& originUrl, const std::string& originHost,
        const std::vector<std::string>& originFallback, std::string& body, std::string& error,
        bool bounded = false, long timeoutSeconds = 12L, long connectSeconds = 8L, bool post = false) {
    body.clear(); error.clear();
    if (originHost == "nextendo.network" && !g_relayRoute.error.empty()) {
        error = "Relay config: " + g_relayRoute.error;
        return false;
    }
    const auto route = nextendo::selectRoute(g_relayRoute, originUrl, originHost, originFallback);
    const auto& host = route.host;
    const auto& url = route.url;
    const u64 start = armGetSystemTick();
    const auto cached = g_cachedIpByHost.find(host);
    auto resolved = route.connectTo.empty() ? resolveHostIps(host) : std::vector<std::string>{};
    // relay_ip is a preferred address, not a permanent lock: if it fails, try
    // current DNS answers. A previously successful cached address comes first.
    if (route.relayed && !route.fallback.empty()) resolved.insert(resolved.begin(), route.fallback.front());
    const auto ips = !route.connectTo.empty() ? std::vector<std::string>{g_relayRoute.tcpIp} :
        nextendo::ipCandidates(cached == g_cachedIpByHost.end() ? "" : cached->second, resolved, route.fallback);
    const long dnsMs = static_cast<long>(armTicksToNs(armGetSystemTick() - start) / 1000000ULL);
    if (ips.empty()) { error = "DNS failed"; recordNetworkTrace(host + " DNS failed"); return false; }
    std::string path = url.substr(std::min(url.find('/', 8), url.size()));
    const size_t query = path.find('?');
    if (query != std::string::npos) path.resize(query); // never log query values
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    if (route.relayed) curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);
    curl_easy_setopt(curl, CURLOPT_IPRESOLVE, CURL_IPRESOLVE_V4);
    curl_easy_setopt(curl, CURLOPT_SSLVERSION, static_cast<long>(CURL_SSLVERSION_TLSv1_2 | CURL_SSLVERSION_MAX_TLSv1_2));
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "gzip, deflate");
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, httpWrite);
    // Three seconds was shorter than observed direct-route first-byte delays.
    // Complete JSON now terminates the authenticated transfer immediately.
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 0L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 0L);
    if (!bounded) {
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, shutdownProgress);
        curl_easy_setopt(curl, CURLOPT_XFERINFODATA, static_cast<void*>(nullptr));
    } // Otherwise preserve accountProgress and its session-generation context.

    for (size_t attempt = 0; attempt < ips.size() && g_threadRunning.load(); ++attempt) {
        const long elapsed = static_cast<long>(armTicksToNs(armGetSystemTick() - start) / 1000000ULL);
        const long remaining = timeoutSeconds * 1000L - elapsed;
        if (remaining <= 0) { if (error.empty()) error = "Request time budget exhausted"; break; }
        const long slots = static_cast<long>(ips.size() - attempt);
        // A transmitted login is never replayed, so reserve its full response
        // budget. Failed pre-send connections still switch to another address.
        const long attemptMs = post ? remaining : std::max(1L, remaining / slots);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, attemptMs);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, std::min({connectSeconds * 1000L, attemptMs, 6000L}));
        curl_easy_setopt(curl, CURLOPT_FRESH_CONNECT, attempt == 0 ? 0L : 1L);
        body.clear();
        HttpSink sink{&body, bounded, {}, false, false};
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &sink);
        const std::string entry = host + ":443:" + ips[attempt];
        auto* resolve = route.connectTo.empty() ? curl_slist_append(nullptr, entry.c_str()) : nullptr;
        auto* connect = route.connectTo.empty() ? nullptr : curl_slist_append(nullptr, route.connectTo.c_str());
        auto* relayHeaders = route.relayed && !bounded ? curl_slist_append(nullptr,
            ("X-Nextendo-Relay-Key: " + g_relayRoute.key).c_str()) : nullptr;
        if ((route.connectTo.empty() && !resolve) || (!route.connectTo.empty() && !connect) ||
            (route.relayed && !bounded && !relayHeaders)) {
            curl_slist_free_all(resolve); curl_slist_free_all(connect); curl_slist_free_all(relayHeaders);
            error = "Could not allocate request options"; break;
        }
        curl_easy_setopt(curl, CURLOPT_RESOLVE, resolve);
        curl_easy_setopt(curl, CURLOPT_CONNECT_TO, connect);
        if (relayHeaders) curl_easy_setopt(curl, CURLOPT_HTTPHEADER, relayHeaders);
        const CURLcode code = curl_easy_perform(curl);
        long status = 0, requestBytes = 0;
        curl_off_t uploaded = 0;
        double connected = 0, tls = 0, firstByte = 0, total = 0;
        char* primary = nullptr;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
        curl_easy_getinfo(curl, CURLINFO_PRIMARY_IP, &primary);
        const std::string usedIp = primary && *primary ? primary : ips[attempt];
        curl_easy_getinfo(curl, CURLINFO_REQUEST_SIZE, &requestBytes);
        curl_easy_getinfo(curl, CURLINFO_SIZE_UPLOAD_T, &uploaded);
        curl_easy_getinfo(curl, CURLINFO_CONNECT_TIME, &connected);
        curl_easy_getinfo(curl, CURLINFO_APPCONNECT_TIME, &tls);
        curl_easy_getinfo(curl, CURLINFO_STARTTRANSFER_TIME, &firstByte);
        curl_easy_getinfo(curl, CURLINFO_TOTAL_TIME, &total);
        curl_easy_setopt(curl, CURLOPT_RESOLVE, static_cast<struct curl_slist*>(nullptr));
        curl_slist_free_all(resolve);
        curl_easy_setopt(curl, CURLOPT_CONNECT_TO, static_cast<struct curl_slist*>(nullptr));
        curl_slist_free_all(connect);
        if (relayHeaders) {
            curl_easy_setopt(curl, CURLOPT_HTTPHEADER, static_cast<struct curl_slist*>(nullptr));
            curl_slist_free_all(relayHeaders);
        }
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, static_cast<void*>(nullptr));
        const bool complete = sink.stopped && sink.frame.complete() && !sink.tooLarge;
        const bool ok = code == CURLE_OK || (complete && status >= 200 &&
            (code == CURLE_WRITE_ERROR || code == CURLE_OPERATION_TIMEDOUT || code == CURLE_PARTIAL_FILE));
        const char* stage = nextendo::transportStage(connected, tls, status, body.size());
        if (bounded || !ok) {
            char metrics[224];
            snprintf(metrics, sizeof(metrics), " try=%u HTTP=%ld curl=%d ok=%d json_done=%d bytes=%u dns=%ld tcp=%ld tls=%ld first=%ld total=%ld stage=%s",
                static_cast<unsigned>(attempt + 1), status, static_cast<int>(code), ok, complete,
                static_cast<unsigned>(body.size()), dnsMs, static_cast<long>(connected * 1000),
                static_cast<long>(tls * 1000), static_cast<long>(firstByte * 1000), static_cast<long>(total * 1000), stage);
            recordNetworkTrace(host + path + " ip=" + usedIp + metrics);
        }
        if (ok) {
            if (status != 0) g_cachedIpByHost[host] = usedIp;
            error.clear();
            curl_easy_setopt(curl, CURLOPT_FRESH_CONNECT, 0L);
            return true;
        }
        const auto good = g_cachedIpByHost.find(host);
        if (good != g_cachedIpByHost.end() && good->second == ips[attempt]) g_cachedIpByHost.erase(good);
        error = sink.tooLarge ? "Response exceeds 96 KB" : curl_easy_strerror(code);
        error += std::string(" [") + stage + "; IP " + usedIp + "; HTTP " + std::to_string(status) + "]";
        if (!nextendo::retryTransport(code, status, post, requestBytes, static_cast<double>(uploaded))) break;
    }
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, static_cast<void*>(nullptr));
    curl_easy_setopt(curl, CURLOPT_FRESH_CONNECT, 0L);
    return false;
}
