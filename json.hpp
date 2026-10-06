// json.hpp - Minimal, dependency-free JSON parser for the Nextendo overlay.
//
// Why hand-rolled: the overlay must not depend on nlohmann/json or any other
// header-only library the devkitPro portlibs do not ship by default, and the
// payload we consume is small and well known.
//
// The parser is intentionally forgiving in one direction: an unknown or
// unexpected value never aborts the whole parse, so a server-side addition to
// the JSON cannot break the overlay. It is strict about malformed syntax.
//
// This file has no Switch-specific dependency so it can be unit-tested on the
// host (see tests/).

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace nextendo {

// One entry of the "jeux" array: a game and how many players are on it.
struct GameEntry {
    std::string name;  // "nom"
    int players = 0;   // "joueurs"
};

// Everything the overlay needs out of GET /api/online-counts.
struct OnlineCounts {
    // Sum of jeux[].joueurs. This is the number the website displays.
    // It is deliberately NOT the sum of "counts": a game with several
    // regional title ids (e.g. Pokemon Scarlet/Violet) repeats the same
    // player count once per title id, so summing "counts" double-counts.
    int total = 0;

    // Per-game breakdown, de-duplicated and already useful for display.
    std::vector<GameEntry> games;

    // Number of title ids seen in "counts" / names seen in "noms".
    // Kept only for diagnostics; not used for display.
    std::size_t title_count = 0;
    std::size_t name_count = 0;

    // True once at least one of the expected keys was found.
    bool plausible = false;
};

enum class ParseStatus {
    Ok,
    Truncated,  // input does not end where the value should end (HTTP body cut off)
    Syntax,     // genuinely malformed JSON
};

// Parses a JSON document (already fully read into memory; no NUL required).
ParseStatus parse_online_counts(const char* data, std::size_t len, OnlineCounts& out);

// Convenience overload for a std::string body.
inline ParseStatus parse_online_counts(const std::string& body, OnlineCounts& out) {
    return parse_online_counts(body.data(), body.size(), out);
}

// Copies the best-effort UTF-8 prefix of `src` into `dst` without exceeding
// `max_bytes`, never splitting a multi-byte UTF-8 sequence. Game names on the
// API are UTF-8 (e.g. "Legendes Pokemon : Z-A" with accents), so truncating
// naively would render a broken glyph.
std::string utf8_truncate(const std::string& src, std::size_t max_bytes);

}  // namespace nextendo
