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
// Usage: start() once, then poll state() from the per-frame update until it is
// Done or Failed. The job owns its thread and joins it in its destructor (or in
// cancel()), so no thread outlives the overlay — the loader unmaps this NRO as
// soon as main() returns.
//
// The thread is a libnx Thread rather than std::thread on purpose: the overlay is
// built with -fno-exceptions, and with libstdc++'s gthread detection unreliable
// on this target std::thread's error path would call std::terminate.
class FetchJob {
public:
    FetchJob() = default;
    ~FetchJob();

    FetchJob(const FetchJob&) = delete;
    FetchJob& operator=(const FetchJob&) = delete;

    // Starts the transfer. Does nothing if one is already running.
    void start();

    // Current state; cheap and safe to call every frame.
    FetchState state() const { return state_.load(std::memory_order_acquire); }

    // Copies the published outcome. Only meaningful once state() != Running.
    FetchOutcome result() const;

    // Asks the worker to stop after the current transfer and joins it.
    // Called on overlay hide/exit so nothing keeps running in the background.
    void cancel();

private:
    static void thread_entry(void* self);
    void run();

    // A libcurl easy handle plus the TLS handshake fit comfortably in this; the
    // stack lives in the overlay's 4 MB heap while a fetch is in flight.
    static constexpr std::size_t kStackSize = 64 * 1024;

    Thread thread_{};
    bool thread_created_ = false;
    std::atomic<FetchState> state_{FetchState::Idle};
    std::atomic<bool> abort_{false};

    // Written by the worker before the state store that releases them, read
    // after the acquire load of state_, so no extra locking is needed.
    FetchOutcome published_;
};

}  // namespace nextendo
