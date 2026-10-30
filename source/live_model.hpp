#pragma once
#include <jansson.h>
#include <string>
#include <vector>
#include <algorithm>

namespace nextendo {
// Limits are shared by the parser and the UI; avatars are deliberately omitted.
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
    if (!json_is_array(json_object_get(obj, "players")) || !json_is_array(json_object_get(obj, "lobbies"))) return false;
    game.roomsAvailable = !json_is_false(json_object_get(obj, "roomsAvailable"));
    game.players = parsePlayers(json_object_get(obj, "players"));
    json_t* rooms = json_object_get(obj, "lobbies");
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
inline bool parseLive(const std::string& body, bool single, std::vector<LiveGame>& out) {
    if (body.size() > kMaxBody) return false;
    json_error_t err{};
    json_t* root = json_loadb(body.data(), body.size(), JSON_REJECT_DUPLICATES, &err);
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
        if (ok) for (size_t i = 0; i < std::min<size_t>(json_array_size(games), 64); ++i) {
            LiveGame game;
            if (parseGame(json_array_get(games, i), game)) parsed.push_back(std::move(game));
            else { ok = false; break; }
        }
    }
    json_decref(root);
    if (ok) out = std::move(parsed);
    return ok;
}
inline bool matches(const LiveGame& game, const std::vector<std::string>& ids) {
    for (const auto& id : ids)
        if (std::find(game.titles.begin(), game.titles.end(), id) != game.titles.end()) return true;
    return false;
}
inline bool validToken(const std::string& token) {
    if (token.empty() || token.size() > 8192) return false;
    return std::all_of(token.begin(), token.end(), [](unsigned char c) { return c > 32 && c < 127; });
}
} // namespace nextendo
