#include "network.hpp"

#include <curl/curl.h>

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
        }
    }
    return rc;
}

}  // namespace

bool services_init() {
    ++g_service_users;
    if (g_services_up) return true;

    // Do NOT use socketInitializeDefault() here.
    //
    // The library defaults are sized for a general-purpose application:
    //   tcp_tx_buf_max 0x40000 + tcp_rx_buf_max 0x40000 + udp_tx 0x2400 +
    //   udp_rx 0xA500, multiplied by sb_efficiency 4
    // which reserves roughly 1.33 MB of TransferMemory. The overlay heap is
    // capped at 4 MB by nx-ovlloader, and this overlay's own image is already
    // ~1.8 MB, so the default reservation exhausted the heap and the overlay
    // died on launch (Atmosphère fatal 2345-0002, PC=0).
    //
    // A single short HTTPS GET with a few-kilobyte response needs far less.
    // Starting from the defaults keeps every other field correct if libnx adds
    // any; only the memory numbers are overridden.
    SocketInitConfig socket_config = *socketGetDefaultInitConfig();
    socket_config.tcp_tx_buf_size = 0x2000;
    socket_config.tcp_rx_buf_size = 0x2000;
    socket_config.tcp_tx_buf_max_size = 0x8000;
    socket_config.tcp_rx_buf_max_size = 0x8000;
    socket_config.udp_tx_buf_size = 0x800;
    socket_config.udp_rx_buf_size = 0x1000;
    socket_config.sb_efficiency = 1;

    if (R_FAILED(socketInitialize(&socket_config))) {
        --g_service_users;
        return false;
    }

    // Not required for DNS (the resolver uses sfdnsres), but it lets us fail
    // fast and gives curl the proxy settings from the console's profile.
    nifmInitialize(NifmServiceType_User);

    curl_global_init(CURL_GLOBAL_DEFAULT);
    g_services_up = true;
    return true;
}

void services_exit() {
    if (g_service_users > 0) --g_service_users;
    if (g_service_users > 0 || !g_services_up) return;

    curl_global_cleanup();
    nifmExit();
    socketExit();
    g_services_up = false;
}

FetchJob::~FetchJob() {
    cancel();
}

void FetchJob::thread_entry(void* self) {
    static_cast<FetchJob*>(self)->run();
}

void FetchJob::start() {
    if (state_.load(std::memory_order_acquire) == FetchState::Running) return;
    cancel();  // join any finished worker before reusing the object

    abort_.store(false, std::memory_order_relaxed);
    published_ = FetchOutcome{};
    state_.store(FetchState::Running, std::memory_order_release);

    // Priority 0x2C and core -2 (any core) match what libtesla uses for its own
    // background poller, so this can never starve the render thread.
    const Result rc = threadCreate(&thread_, &FetchJob::thread_entry, this, nullptr,
                                   kStackSize, 0x2C, -2);
    if (R_FAILED(rc)) {
        FetchOutcome failed;
        failed.state = FetchState::Failed;
        failed.error = "could not start worker thread";
        published_ = failed;
        state_.store(FetchState::Failed, std::memory_order_release);
        return;
    }

    thread_created_ = true;

    if (R_FAILED(threadStart(&thread_))) {
        threadClose(&thread_);
        thread_created_ = false;

        FetchOutcome failed;
        failed.state = FetchState::Failed;
        failed.error = "could not start worker thread";
        published_ = failed;
        state_.store(FetchState::Failed, std::memory_order_release);
    }
}

void FetchJob::run() {
    FetchOutcome outcome;

    // Health first: it is tiny, and a failure here is the clearest signal that
    // the network (not the stats endpoint) is the problem.
    BodySink health;
    long health_status = 0;
    std::string health_error;
    const CURLcode health_rc =
        get(kHealthUrl, health, health_status, abort_, health_error);
    outcome.health_ok =
        (health_rc == CURLE_OK && health_status == 200 && health.data.find("true") != std::string::npos);

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
        // Use the std::string overload: passing (data, size) here was matching
        // the inline overload whose own parameter is also named `body`, which
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
    abort_.store(true, std::memory_order_relaxed);
    if (!thread_created_) return;

    // The worker checks the abort flag through curl's progress callback, so the
    // join returns promptly instead of waiting out the transfer timeout.
    threadWaitForExit(&thread_);
    threadClose(&thread_);
    thread_created_ = false;
}

}  // namespace nextendo
