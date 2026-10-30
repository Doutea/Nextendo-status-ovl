#pragma once
#include <jansson.h>
#include <string>
#include <vector>
#include <algorithm>

namespace nextendo {
// Limits are shared by the parser and the UI; avatars are deliberately omitted.
// accountWrite() aborts a transfer that grows past this, and the response string
// also has to reallocate to hold it - doubling transiently. The live document
// measures about 24 KB, so 96 KB is four times the headroom; going higher pushed
// the overlay heap far enough to make libcurl fail with "out of memory".
constexpr size_t kMaxBody = 96 * 1024;
struct Player {
    std::string name, activity;
    bool you = false, friendPlayer = false;
};
struct Lobby {
    std::string mode, state;
    int count = 0, capacity = 0;
    std::vector<Player> players;
};
struct LiveGame {
    std::vector<std::string> titles;
    std::vector<Player> players;
    std::vector<Lobby> lobbies;
    bool roomsAvailable = true;
};
inline std::string stringField(json_t* obj, const char* key, size_t limit = 128) {
    json_t* v = json_object_get(obj, key);
    if (!json_is_string(v)) return {};
    std::string s(json_string_value(v), json_string_length(v));
    // Keep UTF-8 code points intact when applying the display limit.
    if (s.size() > limit) {
        size_t end = limit;
        while (end && (static_cast<unsigned char>(s[end]) & 0xc0) == 0x80) --end;
        s.resize(end);
    }
    for (char& c : s) if (static_cast<unsigned char>(c) < 32 || c == 127) c = ' ';
    return s;
}
inline int numberField(json_t* obj, const char* key) {
    json_t* v = json_object_get(obj, key);
    return json_is_integer(v) ? static_cast<int>(std::clamp<json_int_t>(json_integer_value(v), 0, 9999)) : 0;
}
inline std::vector<Player> parsePlayers(json_t* array) {
    std::vector<Player> players;
    if (!json_is_array(array)) return players;
    for (size_t i = 0; i < std::min<size_t>(json_array_size(array), 128); ++i) {
        json_t* p = json_array_get(array, i);
        if (!json_is_object(p)) continue;
        players.push_back({stringField(p, "name"), stringField(p, "activity"),
                           json_is_true(json_object_get(p, "you")),
                           json_is_true(json_object_get(p, "friend"))});
    }
    return players;
}
inline bool parseGame(json_t* obj, LiveGame& game) {
    if (!json_is_object(obj)) return false;
    json_t* titles = json_object_get(obj, "titles");
    if (!json_is_array(titles)) return false;
    for (size_t i = 0; i < std::min<size_t>(json_array_size(titles), 16); ++i) {
        json_t* t = json_array_get(titles, i);
        if (!json_is_string(t) || json_string_length(t) != 16) continue;
        std::string id(json_string_value(t));
        if (id.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos) continue;
        std::transform(id.begin(), id.end(), id.begin(), [](unsigned char c) { return c >= 'A' && c <= 'F' ? c + 32 : c; });
        game.titles.push_back(id);
    }
    if (game.titles.empty()) return false;
    // Both fields are optional. The server omits them for titles that publish no
    // roster, and treating that as an error used to discard the whole document.
    json_t* playersArray = json_object_get(obj, "players");
    json_t* rooms = json_object_get(obj, "lobbies");

    // "nolist" is the flag the status page reads; "roomsAvailable" is kept as
    // well in case the API sends it, but on its own it is never present.
    const bool noList = json_is_true(json_object_get(obj, "nolist"));
    game.roomsAvailable = !json_is_false(json_object_get(obj, "roomsAvailable")) && !noList;
    game.players = parsePlayers(playersArray);
    if (json_is_array(rooms)) {
        for (size_t i = 0; i < std::min<size_t>(json_array_size(rooms), 64); ++i) {
            json_t* r = json_array_get(rooms, i);
            if (!json_is_object(r)) continue;
            game.lobbies.push_back({stringField(r, "mode"), stringField(r, "state"),
                numberField(r, "count"), numberField(r, "max"), parsePlayers(json_object_get(r, "players"))});
        }
    }
    return true;
}
inline bool matches(const LiveGame& game, const std::vector<std::string>& ids) {
    for (const auto& id : ids)
        if (std::find(game.titles.begin(), game.titles.end(), id) != game.titles.end()) return true;
    return false;
}

// `only`, when given, keeps just the games whose titles it lists. The endpoint
// sends the whole roster; retaining 46 games' worth of names and lobbies when one
// game is on screen is what the overlay cannot afford.
inline bool parseLive(const std::string& body, bool single, std::vector<LiveGame>& out,
                      const std::vector<std::string>* only = nullptr) {
    if (body.size() > kMaxBody) return false;
    json_error_t err{};
    // No JSON_REJECT_DUPLICATES: a repeated key made the whole document fail to
    // load, and jansson keeps the last value anyway. Nothing else in the overlay
    // passes that flag.
    json_t* root = json_loadb(body.data(), body.size(), 0, &err);
    if (!json_is_object(root)) { if (root) json_decref(root); return false; }
    std::vector<LiveGame> parsed;
    bool ok = true;
    if (single) {
        LiveGame game;
        ok = parseGame(root, game);
        if (ok) parsed.push_back(std::move(game));
    } else {
        json_t* games = json_object_get(root, "games");
        ok = json_is_array(games);
        // Skip an entry that cannot be read rather than abandoning the document:
        // one malformed row used to blank the entire roster.
        if (ok) for (size_t i = 0; i < std::min<size_t>(json_array_size(games), 64); ++i) {
            LiveGame game;
            if (!parseGame(json_array_get(games, i), game)) continue;
            if (only != nullptr && !only->empty() && !matches(game, *only)) continue;
            parsed.push_back(std::move(game));
        }
    }
    json_decref(root);
    if (ok) out = std::move(parsed);
    return ok;
}
// Does the buffer hold a closed top-level JSON object?
//
// Needed because the live endpoint sends the entire document and then leaves the
// connection open: curl reports a timeout although every byte arrived. Treating a
// complete body as success is what makes the roster readable at all.
inline bool completeJson(const std::string& body) {
    int depth = 0;
    bool inString = false;
    bool escaped = false;
    bool started = false;
    for (const char ch : body) {
        if (inString) {
            if (escaped) escaped = false;
            else if (ch == '\\') escaped = true;
            else if (ch == '"') inString = false;
            continue;
        }
        if (ch == '"') inString = true;
        else if (ch == '{') { ++depth; started = true; }
        else if (ch == '}') {
            --depth;
            if (started && depth == 0) return true;
        }
    }
    return false;
}

inline bool validToken(const std::string& token) {
    if (token.empty() || token.size() > 8192) return false;
    return std::all_of(token.begin(), token.end(), [](unsigned char c) { return c > 32 && c < 127; });
}
} // namespace nextendo
