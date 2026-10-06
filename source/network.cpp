#include "network.hpp"

#include <curl/curl.h>

#include <netdb.h>
#include <sys/socket.h>

#include <cstdio>
#include <cstring>

namespace nextendo {
namespace {

constexpr const char* kCountsUrl = "https://nextendo.network/api/online-counts";
constexpr const char* kHealthUrl = "https://nextendo.network/api/health";

// The payload is a few kilobytes. Capping the body keeps a hostile or broken
// response from eating the overlay's 4 MB heap.
constexpr std::size_t kMaxBodyBytes = 96 * 1024;

// Everything curl needs to hand us one response body.
struct BodySink {
    std::string data;
    bool overflowed = false;
};

// libnx's last bsd:/sfdnsres result, which pinpoints why name resolution or a
// transfer failed. curl only reports the generic "Couldn't resolve host name",
// which is not enough to tell a DNS refusal from a stalled transfer.
std::string last_socket_result_text() {
    const Result rc = socketGetLastResult();
    if (R_SUCCEEDED(rc)) return {};
    char buf[32];
    std::snprintf(buf, sizeof(buf), " (bsd 0x%08X)", static_cast<unsigned>(rc));
    return buf;
}

std::size_t write_callback(char* ptr, std::size_t size, std::size_t nmemb, void* userdata) {
    auto* sink = static_cast<BodySink*>(userdata);
    const std::size_t bytes = size * nmemb;

    if (sink->data.size() + bytes > kMaxBodyBytes) {
        sink->overflowed = true;
        return 0;  // returning short aborts the transfer with CURLE_WRITE_ERROR
    }

    sink->data.append(ptr, bytes);
    return bytes;
}

// Aborts a transfer early when the user closed the overlay while it was in
// flight, instead of making them wait out the timeout.
int progress_callback(void* clientp, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
    const auto* abort_flag = static_cast<const std::atomic<bool>*>(clientp);
    return abort_flag->load(std::memory_order_relaxed) ? 1 : 0;
}

bool g_services_up = false;
int g_service_users = 0;
// A GET tuned for a small JSON document on a console that may be on flaky
// Wi-Fi. The timeouts are deliberately short: the overlay is modal, so a long
// hang is worse than an error the user can retry.
CURLcode get(const char* url, BodySink& sink, long& http_status,
             const std::atomic<bool>& abort_flag, std::string& error) {
    CURL* curl = curl_easy_init();
    if (curl == nullptr) {
        error = "curl init failed";
        return CURLE_FAILED_INIT;
    }

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &sink);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 3L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 6L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 12L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 32L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 6L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "nextendo-ovl/1.0");
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
    // switch-curl is built without IPv6 support, so resolve to IPv4 explicitly
    // rather than relying on the resolver ordering.
    curl_easy_setopt(curl, CURLOPT_IPRESOLVE, CURL_IPRESOLVE_V4);
    // The libnx TLS backend implements TLS 1.0-1.2 only; cap at 1.2 so the
    // client does not advertise something the service cannot negotiate.
    curl_easy_setopt(curl, CURLOPT_SSLVERSION, CURL_SSLVERSION_TLSv1_2);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, progress_callback);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &abort_flag);

    const CURLcode rc = curl_easy_perform(curl);
    http_status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_status);
    curl_easy_cleanup(curl);

    if (rc != CURLE_OK) {
        if (sink.overflowed) {
            error = "response too large";
        } else if (abort_flag.load(std::memory_order_relaxed)) {
            error = "cancelled";
        } else {
            error = curl_easy_strerror(rc);
            // Append libnx's own result code when there is one: it separates a
            // DNS failure from a stalled or refused connection.
            error += last_socket_result_text();
        }
    }
    return rc;
}

}  // namespace

bool services_init() {
    ++g_service_users;
    if (g_services_up) return true;

    // Socket buffer tuning. Both extremes fail, so the values below are chosen
    // against libnx's actual memory formula.
    //
    // libnx computes the TransferMemory it hands to bsd: as
    //     sb_efficiency * page_align(tcp_tx_buf_max + tcp_rx_buf_max
    //                                + udp_tx_buf + udp_rx_buf)
    // and documents (nx/source/services/bsd.c) that if that memory is too small
    // "the BSD sockets service would only send ZeroWindow packets (for TCP),
    // resulting in a transfer rate not exceeding 1 byte/s".
    //
    //   defaults : 4 * page_align(0x40000+0x40000+0x2400+0xA500) = ~2.20 MB
    //               -> exhausted the 4 MB overlay heap, the overlay died on
    //                  launch with Atmosph闂佺粯姘ㄩ獮宸?fatal 2345-0002.
    //   tiny     : 1 * page_align(0x8000+0x8000+0x800+0x1000)    = ~70 KB
    //               -> below the threshold, so every transfer stalled.
    //
    // The sizes here give 2 * page_align(0x2000+0x10000+0x1000+0x4000) ~= 184 KB.
    SocketInitConfig socket_config = *socketGetDefaultInitConfig();
    socket_config.tcp_tx_buf_size = 0x2000;
    socket_config.tcp_rx_buf_size = 0x4000;
    socket_config.tcp_tx_buf_max_size = 0x2000;
    socket_config.tcp_rx_buf_max_size = 0x10000;
    socket_config.udp_tx_buf_size = 0x1000;
    socket_config.udp_rx_buf_size = 0x4000;
    socket_config.sb_efficiency = 2;

    // LibnxError_AlreadyInitialized means the socket stack is already up in this
    // process, which is success - not a reason to give up. Treating it as a
    // failure was a bug: it silently disabled networking.
    //
    // It must not be "cleaned up" either. libnx's own socketInitialize() calls
    // socketExit() when its initialisation fails, so a half-finished attempt
    // could otherwise tear down a working socket stack.
    //
    // The comparison uses R_DESCRIPTION: a Result packs the module in the low 9
    // bits, so a plain R_VALUE() of the whole Result would never equal the bare
    // error number.
    const Result socket_rc = socketInitialize(&socket_config);
    if (R_FAILED(socket_rc) &&
        R_DESCRIPTION(socket_rc) != LibnxError_AlreadyInitialized) {
        --g_service_users;
        return false;
    }

    // Optional: name resolution goes through sfdnsres, not nifm.
    nifmInitialize(NifmServiceType_User);

    // Warm up name resolution here, while `sm:` is guaranteed to be open.
    //
    // libtesla wraps initServices() in doWithSmSession, but the fetch itself
    // runs later from onShow(), outside that session. libnx's sfdnsres resolver
    // initialises lazily on the first getaddrinfo and needs `sm:` at that moment
    // to fetch its service handle; without it the call fails inside the SM
    // module, which libnx maps to errno = EAGAIN (11) and curl reports only as
    // "Couldn't resolve host name".
    //
    // Doing one throwaway lookup now initialises the resolver while the session
    // is available, so later lookups need no session at all. This avoids holding
    // `sm:` open for the overlay's whole lifetime, which matters because the
    // loader can reopen an overlay repeatedly inside one process.
    {
        addrinfo hints{};
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        addrinfo* probe = nullptr;
        if (getaddrinfo("nextendo.network", "443", &hints, &probe) == 0 && probe != nullptr) {
            freeaddrinfo(probe);
        }
    }

    curl_global_init(CURL_GLOBAL_DEFAULT);
    g_services_up = true;
    return true;
}

void services_exit() {
    if (g_service_users > 0) --g_service_users;
    if (g_service_users > 0 || !g_services_up) return;

    curl_global_cleanup();
    nifmExit();
    // Mirrors the network probe, which is known to close cleanly on this
    // console: it calls socketInitialize() on the way in and socketExit() on the
    // way out. socketExit() is reference counted, so calling it only undoes the
    // initialisation this overlay performed and leaves a pre-existing stack
    // alone.
    socketExit();
    g_services_up = false;
}

FetchJob::~FetchJob() {
    cancel();
}

void FetchJob::start() {
    if (state_.load(std::memory_order_acquire) == FetchState::Running) {
        return;
    }

    abort_.store(false, std::memory_order_relaxed);
    published_ = FetchOutcome{};
    state_.store(FetchState::Running, std::memory_order_release);

    // Synchronous on purpose.
    //
    // The worker-thread version crashed the loader process on close
    // (Atmosph閻氱幎e fatal 2347-0004, PC=0) while the probe that performs the same
    // network work without a thread closed cleanly, which pointed at
    // threadCreate/threadWaitForExit/threadClose as the trigger. Running the
    // request inline removes that whole code path; the cost is that the panel
    // cannot animate while a request is in flight, which the short timeouts in
    // get() keep to a few seconds at worst.
    run();
}

void FetchJob::run() {
    FetchOutcome outcome;

    // Health first: it is tiny, and a failure here is the clearest signal
    // that the network (not the stats endpoint) is the problem.
    BodySink health;
    long health_status = 0;
    std::string health_error;
    const CURLcode health_rc =
        get(kHealthUrl, health, health_status, abort_, health_error);
    outcome.health_ok = (health_rc == CURLE_OK && health_status == 200 &&
                         health.data.find("true") != std::string::npos);

    BodySink body;
    long http_status = 0;
    std::string error;
    const CURLcode rc = get(kCountsUrl, body, http_status, abort_, error);

    outcome.http_status = static_cast<int>(http_status);

    if (abort_.load(std::memory_order_relaxed)) {
        outcome.state = FetchState::Failed;
        outcome.error = "cancelled";
    } else if (rc != CURLE_OK) {
        outcome.state = FetchState::Failed;
        outcome.error = error.empty() ? "network error" : error;
    } else if (http_status != 200) {
        outcome.state = FetchState::Failed;
        outcome.error = "server returned HTTP " + std::to_string(http_status);
    } else {
        // Use the std::string overload: passing (data, size) here matched the
        // inline overload whose own parameter is also named `body`, which
        // shadowed the local variable and made the call fail to resolve.
        switch (parse_online_counts(body.data, outcome.counts)) {
            case ParseStatus::Ok:
                outcome.state = FetchState::Done;
                break;
            case ParseStatus::Truncated:
                outcome.state = FetchState::Failed;
                outcome.error = "truncated response";
                break;
            case ParseStatus::Syntax:
                outcome.state = FetchState::Failed;
                outcome.error = "unexpected response format";
                break;
        }
    }

    published_ = outcome;
    // Release: the store makes every write above visible to a reader that
    // observes Done/Failed with an acquire load.
    state_.store(outcome.state, std::memory_order_release);
}

FetchOutcome FetchJob::result() const {
    return published_;
}

void FetchJob::cancel() {
    // With the request running inline there is no thread to join. The abort flag
    // is still honoured: it is read by curl's progress callback, so closing the
    // overlay mid-transfer stops it at the next callback.
    abort_.store(true, std::memory_order_relaxed);
}

}  // namespace nextendo