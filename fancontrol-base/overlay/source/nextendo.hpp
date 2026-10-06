// The one piece of state this overlay keeps: the last fetched counts.
//
// Deliberately a single struct rather than a job object with state transitions:
// the request is made synchronously from MainMenu::createUI() right before the
// rows are built, so there is nothing to poll and nothing to synchronise.

#pragma once

#include <string>

#include "json.hpp"

namespace nextendo {

struct Outcome {
    bool ok = false;        // the counts request succeeded and parsed
    bool api_ok = false;    // /api/health reported the backend as up
    bool attempted = false; // at least one request has been made
    std::string error;      // populated when ok is false
    OnlineCounts counts;
};

// Performs the request and stores the result. Safe to call again to refresh.
void fetch_and_store();

// The last stored result. Never null.
const Outcome& outcome();

}  // namespace nextendo
