#include "json.hpp"

#include <cstdlib>
#include <cstring>

namespace nextendo {
namespace {

// A JSON scanner. Every accessor is bounds-checked and reports the first
// structural problem it sees, rather than reading past the buffer.
class Scanner {
public:
    Scanner(const char* data, std::size_t len) : p_(data), end_(data + len) {}

    bool eof() const { return p_ >= end_; }
    std::size_t remaining() const { return static_cast<std::size_t>(end_ - p_); }
    char peek() const { return eof() ? '\0' : *p_; }

    void skip_ws() {
        while (!eof()) {
            const char c = *p_;
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                ++p_;
            } else {
                break;
            }
        }
    }

    bool literal(const char* text) {
        const std::size_t n = std::strlen(text);
        if (remaining() < n || std::memcmp(p_, text, n) != 0) return false;
        p_ += n;
        return true;
    }

    bool take(char expected) {
        if (!eof() && *p_ == expected) {
            ++p_;
            return true;
        }
        return false;
    }

    // Reads a JSON string into `out` as UTF-8. Handles the standard escapes
    // plus \uXXXX, including surrogate pairs.
    bool string(std::string& out) {
        out.clear();
        if (!take('"')) return false;
        while (true) {
            if (eof()) return false;
            const unsigned char c = static_cast<unsigned char>(*p_);
            if (c == '"') {
                ++p_;
                return true;
            }
            if (c == '\\') {
                ++p_;
                if (eof()) return false;
                const char esc = *p_++;
                switch (esc) {
                    case '"': out.push_back('"'); break;
                    case '\\': out.push_back('\\'); break;
                    case '/': out.push_back('/'); break;
                    case 'b': out.push_back('\b'); break;
                    case 'f': out.push_back('\f'); break;
                    case 'n': out.push_back('\n'); break;
                    case 'r': out.push_back('\r'); break;
                    case 't': out.push_back('\t'); break;
                    case 'u': {
                        std::uint32_t cp = 0;
                        if (!hex4(cp)) return false;
                        // Combine a UTF-16 surrogate pair when present.
                        if (cp >= 0xD800 && cp <= 0xDBFF && remaining() >= 6 &&
                            p_[0] == '\\' && p_[1] == 'u') {
                            const char* save = p_;
                            p_ += 2;
                            std::uint32_t low = 0;
                            if (hex4(low) && low >= 0xDC00 && low <= 0xDFFF) {
                                cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                            } else {
                                p_ = save;  // lone high surrogate: emit as-is
                            }
                        }
                        append_utf8(out, cp);
                        break;
                    }
                    default:
                        return false;
                }
                continue;
            }
            // Raw byte (ASCII or UTF-8 continuation): copy through.
            out.push_back(static_cast<char>(c));
            ++p_;
        }
    }

    // Reads an integer. Floats are accepted and truncated toward zero so a
    // server switching to 12.0 does not break the overlay.
    bool integer(long long& out) {
        const char* start = p_;
        if (!eof() && (*p_ == '-' || *p_ == '+')) ++p_;
        std::size_t digits = 0;
        while (!eof() && *p_ >= '0' && *p_ <= '9') {
            ++p_;
            ++digits;
        }
        bool is_float = false;
        if (!eof() && *p_ == '.') {
            is_float = true;
            ++p_;
            while (!eof() && *p_ >= '0' && *p_ <= '9') {
                ++p_;
                ++digits;
            }
        }
        if (digits == 0) {
            p_ = start;
            return false;
        }
        // Skip an exponent if present, then parse the leading integer part.
        if (!eof() && (*p_ == 'e' || *p_ == 'E')) {
            is_float = true;
            ++p_;
            if (!eof() && (*p_ == '-' || *p_ == '+')) ++p_;
            while (!eof() && *p_ >= '0' && *p_ <= '9') ++p_;
        }
        (void)is_float;
        out = std::strtoll(std::string(start, static_cast<std::size_t>(p_ - start)).c_str(),
                           nullptr, 10);
        return true;
    }

    // Skips any JSON value without interpreting it. Used for the parts of the
    // payload the overlay does not care about.
    bool skip_value(int depth = 0) {
        if (depth > 64) return false;  // absurd nesting: give up rather than recurse
        skip_ws();
        if (eof()) return false;
        const char c = *p_;
        if (c == '"') {
            std::string scratch;
            return string(scratch);
        }
        if (c == '{' || c == '[') {
            const char close = (c == '{') ? '}' : ']';
            ++p_;
            skip_ws();
            if (take(close)) return true;
            while (true) {
                skip_ws();
                if (c == '{') {
                    std::string key;
                    if (!string(key)) return false;
                    skip_ws();
                    if (!take(':')) return false;
                }
                if (!skip_value(depth + 1)) return false;
                skip_ws();
                if (take(close)) return true;
                if (!take(',')) return false;
            }
        }
        if (literal("true") || literal("false") || literal("null")) return true;
        long long ignored = 0;
        return integer(ignored);
    }

    bool at_end() {
        skip_ws();
        return eof();
    }

private:
    bool hex4(std::uint32_t& out) {
        if (remaining() < 4) return false;
        out = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = *p_++;
            out <<= 4;
            if (c >= '0' && c <= '9') {
                out |= static_cast<std::uint32_t>(c - '0');
            } else if (c >= 'a' && c <= 'f') {
                out |= static_cast<std::uint32_t>(c - 'a' + 10);
            } else if (c >= 'A' && c <= 'F') {
                out |= static_cast<std::uint32_t>(c - 'A' + 10);
            } else {
                return false;
            }
        }
        return true;
    }

    static void append_utf8(std::string& out, std::uint32_t cp) {
        if (cp < 0x80) {
            out.push_back(static_cast<char>(cp));
        } else if (cp < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }

    const char* p_;
    const char* end_;
};

// Parses the "jeux" array: [{ "nom": str, "joueurs": int, "titres": [str] }, ...]
bool parse_jeux(Scanner& s, OnlineCounts& out) {
    if (!s.take('[')) return false;
    s.skip_ws();
    if (s.take(']')) return true;

    while (true) {
        s.skip_ws();
        if (!s.take('{')) return false;

        GameEntry entry;
        bool has_players = false;

        s.skip_ws();
        if (!s.take('}')) {
            while (true) {
                s.skip_ws();
                std::string key;
                if (!s.string(key)) return false;
                s.skip_ws();
                if (!s.take(':')) return false;
                s.skip_ws();

                if (key == "nom") {
                    if (!s.string(entry.name)) return false;
                } else if (key == "joueurs") {
                    long long v = 0;
                    if (!s.integer(v)) return false;
                    if (v < 0) v = 0;  // never display a negative count
                    if (v > 1000000) v = 1000000;  // clamp nonsense to avoid overflow
                    entry.players = static_cast<int>(v);
                    has_players = true;
                } else {
                    if (!s.skip_value()) return false;
                }

                s.skip_ws();
                if (s.take(',')) continue;
                if (s.take('}')) break;
                return false;
            }
        }

        // Only count entries we can actually label and quantify.
        if (has_players && !entry.name.empty()) {
            out.games.push_back(entry);
            out.total += entry.players;
        } else if (has_players) {
            out.total += entry.players;  // unlabelled game still counts toward the total
        }

        s.skip_ws();
        if (s.take(',')) continue;
        if (s.take(']')) return true;
        return false;
    }
}

// Counts the members of a flat JSON object without keeping any of it.
bool count_object_members(Scanner& s, std::size_t& count) {
    if (!s.take('{')) return false;
    count = 0;
    s.skip_ws();
    if (s.take('}')) return true;
    while (true) {
        s.skip_ws();
        std::string key;
        if (!s.string(key)) return false;
        s.skip_ws();
        if (!s.take(':')) return false;
        if (!s.skip_value()) return false;
        ++count;
        s.skip_ws();
        if (s.take(',')) continue;
        if (s.take('}')) return true;
        return false;
    }
}

}  // namespace

ParseStatus parse_online_counts(const char* data, std::size_t len, OnlineCounts& out) {
    // Parse into a temporary and only publish it on success: a truncated body
    // must never leave a half-counted total in the caller's struct. On any
    // non-Ok status `out` is empty, which the UI relies on to show an error
    // instead of a misleading number.
    OnlineCounts result;
    const ParseStatus status = [&]() -> ParseStatus {
        Scanner s(data, len);

        s.skip_ws();
        if (!s.take('{')) {
            // Distinguish "empty body" from "malformed body": an empty or
            // whitespace-only body is a truncated read, not a syntax error.
            Scanner probe(data, len);
            probe.skip_ws();
            return probe.eof() ? ParseStatus::Truncated : ParseStatus::Syntax;
        }

        s.skip_ws();
        if (!s.take('}')) {
            while (true) {
                s.skip_ws();
                std::string key;
                if (!s.string(key)) {
                    return s.eof() ? ParseStatus::Truncated : ParseStatus::Syntax;
                }
                s.skip_ws();
                if (!s.take(':')) return ParseStatus::Syntax;
                s.skip_ws();

                if (key == "jeux") {
                    // parse_jeux consumes the opening '[' itself.
                    if (!parse_jeux(s, result)) {
                        return s.eof() ? ParseStatus::Truncated : ParseStatus::Syntax;
                    }
                    result.plausible = true;
                } else if (key == "counts") {
                    if (!count_object_members(s, result.title_count)) return ParseStatus::Syntax;
                    result.plausible = true;
                } else if (key == "noms") {
                    if (!count_object_members(s, result.name_count)) return ParseStatus::Syntax;
                    result.plausible = true;
                } else {
                    if (!s.skip_value()) {
                        return s.eof() ? ParseStatus::Truncated : ParseStatus::Syntax;
                    }
                }

                s.skip_ws();
                if (s.take(',')) continue;
                if (s.take('}')) break;
                return s.eof() ? ParseStatus::Truncated : ParseStatus::Syntax;
            }
        }

        // A total of 0 is legitimate (nobody online), so "plausible" tracks
        // whether we recognised the document at all, not whether anyone is on.
        return ParseStatus::Ok;
    }();

    out = (status == ParseStatus::Ok) ? result : OnlineCounts{};
    return status;
}

std::string utf8_truncate(const std::string& src, std::size_t max_bytes) {
    if (src.size() <= max_bytes) return src;

    std::size_t cut = max_bytes;
    // Walk back off any continuation byte (10xxxxxx) so we never split a glyph.
    while (cut > 0) {
        const unsigned char c = static_cast<unsigned char>(src[cut]);
        if ((c & 0xC0) != 0x80) break;
        --cut;
    }
    return src.substr(0, cut);
}

}  // namespace nextendo
