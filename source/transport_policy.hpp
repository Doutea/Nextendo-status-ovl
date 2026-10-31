#pragma once
#include <curl/curl.h>
#include <array>
#include <algorithm>
#include <string>
#include <vector>

namespace nextendo {
inline std::vector<std::string> ipCandidates(const std::string& cached,
        const std::vector<std::string>& resolved, const std::vector<std::string>& fallback) {
    std::vector<std::string> out;
    auto add = [&](const std::string& ip) {
        if (!ip.empty() && out.size() < 4 && std::find(out.begin(), out.end(), ip) == out.end()) out.push_back(ip);
    };
    add(cached);
    for (const auto& ip : resolved) add(ip);
    for (const auto& ip : fallback) add(ip);
    return out;
}
inline bool retryTransport(CURLcode code, long status, bool post, long requestBytes, double uploaded) {
    // Auth/error replies, cancellation and buffer limits must not cause replay.
    // An incomplete successful GET may retry; a transmitted login must not.
    if ((status != 0 && status != 200) || (post && (status != 0 || requestBytes != 0 || uploaded != 0))) return false;
    switch (code) {
        case CURLE_COULDNT_RESOLVE_HOST: case CURLE_COULDNT_CONNECT:
        case CURLE_SSL_CONNECT_ERROR: case CURLE_OPERATION_TIMEDOUT:
        case CURLE_SEND_ERROR: case CURLE_RECV_ERROR: case CURLE_GOT_NOTHING:
        case CURLE_PARTIAL_FILE:
            return true;
        default: return false;
    }
}
// Incremental framing only; the existing Jansson parser still validates JSON.
// This avoids waiting for a broken upstream to terminate an already complete
// response. Quotes, escapes and matching nested arrays/objects are respected.
struct JsonFrame {
    std::array<char, 128> stack{};
    size_t depth = 0;
    bool quoted = false, escaped = false, started = false, done = false, invalid = false;
    void push(const char* bytes, size_t count) {
        for (size_t i = 0; i < count && !invalid; ++i) {
            const unsigned char ch = static_cast<unsigned char>(bytes[i]);
            if (done) { if (ch != ' ' && ch != '\t' && ch != '\r' && ch != '\n') invalid = true; continue; }
            if (quoted) {
                if (ch < 32) { invalid = true; continue; }
                if (escaped) escaped = false;
                else if (ch == '\\') escaped = true;
                else if (ch == '"') quoted = false;
                continue;
            }
            if (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n') continue;
            if (!started && ch != '{' && ch != '[') { invalid = true; continue; }
            if (ch == '"') quoted = true;
            else if (ch == '{' || ch == '[') {
                started = true;
                if (depth == stack.size()) invalid = true;
                else stack[depth++] = static_cast<char>(ch);
            } else if (ch == '}' || ch == ']') {
                if (!depth || (ch == '}' ? stack[depth - 1] != '{' : stack[depth - 1] != '[')) invalid = true;
                else if (--depth == 0) done = true;
            }
        }
    }
    bool complete() const { return done && !invalid; }
};
inline const char* transportStage(double connected, double tls, long status, size_t bytes) {
    if (bytes) return "body";
    if (status > 0) return "headers";
    if (tls > 0) return "response";
    if (connected > 0) return "TLS";
    return "connect";
}
} // namespace nextendo
