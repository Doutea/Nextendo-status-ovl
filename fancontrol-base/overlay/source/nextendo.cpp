// The request itself.
//
// Modeled on the parts of this repository's earlier network.cpp that were proven
// on hardware, reduced to what is actually needed:
//
//   * the `sm:` session is opened around the request only;
//   * the socket stack is never touched - socketInitialize() already reports
//     LibnxError_AlreadyInitialized in this process, so it belongs to somebody
//     else, and calling socketExit() on it made nx-ovlloader fail right after
//     the overlay closed.
//
// Two endpoints are queried, matching the website's status page: the counts, and
// a small health check so the panel can say whether the backend is up.

#include "nextendo.hpp"

#include <curl/curl.h>
#include <switch.h>

#include <cstring>

namespace nextendo {
namespace {

constexpr const char* kCountsUrl = "https://nextendo.network/api/online-counts";
constexpr const char* kHealthUrl = "https://nextendo.network/api/health";

// The payload is a few kilobytes. Capping the body keeps a hostile or broken
// response from eating the overlay's heap.
constexpr std::size_t kMaxBodyBytes = 96 * 1024;

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

CURLcode http_get(const char* url, BodySink& sink, long& http_status, std::string& error) {
    CURL* curl = curl_easy_init();
    if (curl == nullptr) {
        error = "curl init failed";
        return CURLE_FAILED_INIT;
    }

    http_status = 0;
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &sink);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "nextendo-ovl/1.0");
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 6L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    // devkitPro's curl is built with libnx's `ssl` service as its TLS backend.
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    curl_easy_setopt(curl, CURLOPT_IPRESOLVE, CURL_IPRESOLVE_V4);
    curl_easy_setopt(curl, CURLOPT_SSLVERSION, CURL_SSLVERSION_TLSv1_2);

    const CURLcode rc = curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_status);
    curl_easy_cleanup(curl);

    if (rc != CURLE_OK) {
        error = sink.overflowed ? "response too large" : curl_easy_strerror(rc);
    }
    return rc;
}

Outcome g_outcome;

}  // namespace

const Outcome& outcome() {
    return g_outcome;
}

void fetch_and_store() {
    Outcome result;
    result.attempted = true;

    // The resolver (sfdnsres) initialises on first use and needs `sm:` at that
    // moment. Fetching from createUI() runs outside libtesla's doWithSmSession,
    // so the session is opened here for the duration of the request.
    smInitialize();

    BodySink health;
    long health_status = 0;
    std::string health_error;
    if (http_get(kHealthUrl, health, health_status, health_error) == CURLE_OK &&
        health_status == 200) {
        result.api_ok = health.data.find("true") != std::string::npos;
    }

    BodySink body;
    long http_status = 0;
    std::string error;
    const CURLcode rc = http_get(kCountsUrl, body, http_status, error);

    smExit();

    if (rc != CURLE_OK) {
        result.error = error.empty() ? "request failed" : error;
    } else if (http_status != 200) {
        result.error = "HTTP " + std::to_string(http_status);
    } else {
        switch (parse_online_counts(body.data, result.counts)) {
            case ParseStatus::Ok:
                result.ok = true;
                // A response with none of the expected keys parses fine but
                // carries nothing usable.
                if (!result.counts.plausible) {
                    result.ok = false;
                    result.error = "unexpected data";
                }
                break;
            case ParseStatus::Truncated:
                result.error = "truncated response";
                break;
            case ParseStatus::Syntax:
                result.error = "unexpected response format";
                break;
        }
    }

    g_outcome = result;
}

}  // namespace nextendo
