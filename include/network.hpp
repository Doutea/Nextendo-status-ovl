// network.hpp - HTTPS retrieval for the Nextendo overlay.
//
// The console's `ssl` service backs libcurl here (devkitPro builds switch-curl
// with --with-default-ssl-backend=libnx), so HTTPS needs no bundled TLS library
// and no CA bundle of our own: verification uses Nintendo's built-in trust
// store.
//
// A blocking request must never run on the render thread, because that freezes
// input and drawing. FetchJob therefore runs the transfer on a worker thread and
// publishes its result through atomics that the GUI polls each frame.

#pragma once

#include <switch.h>

#include <atomic>
#include <string>

#include "json.hpp"

namespace nextendo {

// Brings up bsd:u and nifm. Safe to call more than once.
bool services_init();
void services_exit();

enum class FetchState {
    Idle,
    Running,
    Done,
    Failed,
};

struct FetchOutcome {
    FetchState state = FetchState::Idle;
    int http_status = 0;
    std::string error;       // human-readable reason, shown when state == Failed
    OnlineCounts counts;     // valid when state == Done
    bool health_ok = false;  // /api/health answered {"ok":true}
};

// A single background GET of the Nextendo API.
//
// Usage: start() once, then read state()/result().
//
// The request runs INLINE, on the calling thread. An earlier version used a
// libnx worker thread (threadCreate/threadWaitForExit/threadClose); with that,
// closing the overlay crashed the shared loader process, while a diagnostic
// build performing the same network work without a thread closed cleanly. Going
// synchronous removes that entire code path.
//
// The tradeoff is that the panel cannot animate during a request. The timeouts
// in get() bound that to a few seconds at worst.
class FetchJob {
public:
    FetchJob() = default;
    ~FetchJob();

    FetchJob(const FetchJob&) = delete;
    FetchJob& operator=(const FetchJob&) = delete;

    // Performs the transfer on the calling thread and returns once state() has
    // settled on Done or Failed.
    void start();

    // Current state.
    FetchState state() const { return state_.load(std::memory_order_acquire); }

    // Copies the published outcome. Only meaningful once state() != Running.
    FetchOutcome result() const;

    // Requests that an in-flight transfer stop at the next curl callback.
    void cancel();

private:
    void run();

    std::atomic<FetchState> state_{FetchState::Idle};
    std::atomic<bool> abort_{false};

    FetchOutcome published_;
};

}  // namespace nextendo
