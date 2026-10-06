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

    // The socket stack is deliberately left alone.
    //
    // socketInitialize() returns LibnxError_AlreadyInitialized (0xE401) in this
    // process - measured on hardware - so the stack is already up before this
    // overlay runs. Both previous approaches therefore only differed in how they
    // reacted to that: treating it as success and continuing on to nifm/curl
    // produced a working request, while treating it as failure and returning
    // early produced no data at all. Neither touched the socket, and the build
    // that continued is the one that loaded data - so the data needs nifm and
    // curl, not a socket initialisation of ours.
    //
    // Every attempt to initialise the socket here (library defaults, and several
    // hand-tuned buffer sizes) ended with nx-ovlloader failing right after the
    // overlay closed, with Atmosphère fatal 2347-0004 (Module_HomebrewLoader, 4):
    // its own fsFileRead of the next NRO returning an error. Leaving a stack we
    // do not own untouched removes that entirely.
    nifmInitialize(NifmServiceType_User);

    g_services_up = true;
    return true;
}

void services_exit() {
    if (g_service_users > 0) --g_service_users;
    if (g_service_users > 0 || !g_services_up) return;

    // Deliberately does NOT tear the network stack down.
    //
    // Closing the overlay ran curl_global_cleanup(), nifmExit() and socketExit()
    // here, and doing so made the loader fail immediately afterwards with
    // an Atmosphère fatal 2347-0004 (Module_HomebrewLoader, 4) - that is
    // nx-ovlloader's own `fsFileRead` of the next NRO returning an error or zero
    // bytes. The overlay is unmapped straight after this returns, so nothing it
    // leaves behind can be observed, and overlays that are known to work on this
    // console (NX-FanControl, FPSLocker, Status-Monitor) likewise just let the
    // process clean up.
    //
    // Set NEXTENDO_TEARDOWN_NETWORK to re-enable the teardown for experiments.
#ifdef NEXTENDO_TEARDOWN_NETWORK
    curl_global_cleanup();
    nifmExit();
    socketExit();
#endif
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
    // (an Atmosphère fatal 2347-0004, PC=0) while the probe that performs the same
    // network work without a thread closed cleanly, which pointed at
    // threadCreate/threadWaitForExit/threadClose as the trigger. Running the
    // request inline removes that whole code path; the cost is that the panel
    // cannot animate while a request is in flight, which the short timeouts in
    // get() keep to a few seconds at worst.
    run();
}

void FetchJob::run() {
    FetchOutcome outcome;

    // Open `sm:` only around the transfer.
    //
    // The resolver needs the session while it is used, and holding it open for
    // the whole overlay lifetime was tried and made the loader crash right after
    // the overlay closed. Scoping it to the request keeps the session alive
    // exactly when the resolver touches it, and gone before teardown.
    smInitialize();

    // curl global state is set up here, not in services_init(), so the
    // diagnostic build that skips network initialisation skips it too.
    curl_global_init(CURL_GLOBAL_DEFAULT);

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

    smExit();

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