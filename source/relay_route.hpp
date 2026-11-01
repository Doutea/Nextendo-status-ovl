#pragma once
#include <string>
#include <vector>
#include <algorithm>

namespace nextendo {
struct RelayRoute {
    std::string host, ip, key, error, tcpIp, tcpPort, backupIp;
    bool enabled() const { return !host.empty() && error.empty(); }
    bool tcpEnabled() const { return !tcpIp.empty() && error.empty(); }
};
inline std::string trimRoute(std::string s) {
    const auto a = s.find_first_not_of(" \t\r\n");
    return a == std::string::npos ? "" : s.substr(a, s.find_last_not_of(" \t\r\n") - a + 1);
}
inline bool routeIpv4(const std::string& ip) {
    size_t start = 0;
    for (int part = 0; part < 4; ++part) {
        size_t end = ip.find('.', start);
        if (part == 3) { if (end != std::string::npos) return false; end = ip.size(); }
        else if (end == std::string::npos) return false;
        if (end == start || end - start > 3) return false;
        unsigned value = 0;
        for (size_t i = start; i < end; ++i) {
            if (ip[i] < '0' || ip[i] > '9') return false;
            value = value * 10 + ip[i] - '0';
        }
        if (value > 255 || (end - start > 1 && ip[start] == '0')) return false;
        start = end + 1;
    }
    return true;
}
inline bool routeHost(const std::string& host) {
    if (host.empty() || host.size() > 253) return false;
    size_t length = 0;
    char previous = '.';
    for (char c : host) {
        if (c == '.') {
            if (!length || length > 63 || previous == '-') return false;
            length = 0;
        } else {
            if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')) return false;
            if (!length && c == '-') return false;
            ++length;
        }
        previous = c;
    }
    return length && length <= 63 && previous != '-';
}
inline RelayRoute parseRelayRoute(const std::string& text) {
    RelayRoute out;
    auto fail = [&](const char* error) { out.error = error; return out; };
    if (text.size() > 4096 || text.find('\0') != std::string::npos) return fail("network.ini is invalid or too large");
    std::string url, tcp;
    std::vector<std::string> seen;
    size_t pos = text.compare(0, 3, "\xef\xbb\xbf") == 0 ? 3 : 0;
    while (pos < text.size()) {
        const size_t end = text.find('\n', pos);
        const auto line = trimRoute(text.substr(pos, (end == std::string::npos ? text.size() : end) - pos));
        pos = end == std::string::npos ? text.size() : end + 1;
        if (line.empty() || line[0] == '#' || line[0] == ';') continue;
        const size_t eq = line.find('=');
        if (eq == std::string::npos) return fail("expected key=value in network.ini");
        const auto name = trimRoute(line.substr(0, eq)), value = trimRoute(line.substr(eq + 1));
        if (std::find(seen.begin(), seen.end(), name) != seen.end()) return fail("duplicate network.ini key");
        seen.push_back(name);
        if (name == "relay_url") url = value;
        else if (name == "relay_ip") out.ip = value;
        else if (name == "relay_ip_backup") out.backupIp = value;
        else if (name == "relay_key") out.key = value;
        else if (name == "tcp_relay") tcp = value;
        else return fail("unknown network.ini key");
    }
    if (!tcp.empty()) {
        if (!url.empty() || !out.ip.empty() || !out.backupIp.empty() || !out.key.empty()) return fail("tcp_relay cannot be combined with HTTPS relay settings");
        const size_t colon = tcp.find(':');
        if (colon == std::string::npos) return fail("tcp_relay must be IPv4:port");
        out.tcpIp = tcp.substr(0, colon);
        out.tcpPort = tcp.substr(colon + 1);
        if (!routeIpv4(out.tcpIp) || out.tcpPort.empty() || out.tcpPort.size() > 5 ||
            out.tcpPort.find_first_not_of("0123456789") != std::string::npos) return fail("invalid tcp_relay address");
        unsigned port = 0;
        for (char c : out.tcpPort) port = port * 10 + c - '0';
        if (!port || port > 65535) return fail("invalid tcp_relay port");
        return out;
    }
    if (url.empty()) {
        if (!out.ip.empty() || !out.backupIp.empty() || !out.key.empty()) return fail("relay_url is required with relay addresses or key");
        return out; // empty configuration is direct mode
    }
    if (url.compare(0, 8, "https://") != 0) return fail("relay_url must use HTTPS");
    std::string host = url.substr(8);
    if (!host.empty() && host.back() == '/') host.pop_back();
    if (host.size() > 4 && host.compare(host.size() - 4, 4, ":443") == 0) host.resize(host.size() - 4);
    std::transform(host.begin(), host.end(), host.begin(), [](unsigned char c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; });
    if (!routeHost(host)) return fail("relay_url must be a hostname on HTTPS port 443, with no path");
    if (!out.ip.empty() && !routeIpv4(out.ip)) return fail("relay_ip must be an IPv4 address");
    if (!out.backupIp.empty() && (out.ip.empty() || !routeIpv4(out.backupIp))) return fail("relay_ip_backup requires relay_ip and a valid IPv4 address");
    if (out.key.size() < 32 || out.key.size() > 128 || out.key.find_first_not_of(
        "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-") != std::string::npos)
        return fail("relay_key must contain 32-128 letters, digits, dots, underscores or hyphens");
    out.host = host;
    return out;
}
struct RequestRoute { std::string url, host; std::vector<std::string> fallback; bool relayed = false; std::string connectTo; };
inline RequestRoute selectRoute(const RelayRoute& relay, const std::string& url,
                               const std::string& host, const std::vector<std::string>& fallback) {
    const std::string origin = "https://nextendo.network/";
    if (relay.tcpEnabled() && host == "nextendo.network" && url.compare(0, origin.size(), origin) == 0)
        return {url, host, {}, false, "nextendo.network:443:" + relay.tcpIp + ":" + relay.tcpPort};
    if (relay.enabled() && host == "nextendo.network" && url.compare(0, origin.size(), origin) == 0) {
        std::vector<std::string> addresses;
        if (!relay.ip.empty()) addresses.push_back(relay.ip);
        if (!relay.backupIp.empty() && relay.backupIp != relay.ip) addresses.push_back(relay.backupIp);
        return {"https://" + relay.host + "/" + url.substr(origin.size()), relay.host,
                addresses, true, ""};
    }
    return {url, host, fallback, false, ""};
}
inline bool routeNeedsDns(const RequestRoute& route, bool cached) {
    return route.connectTo.empty() && !cached && !(route.relayed && !route.fallback.empty());
}
inline std::string relayLivePath(const RelayRoute& relay, const std::string& path,
                                const std::vector<std::string>& titles) {
    if (!relay.enabled() || titles.empty()) return path;
    std::string ids;
    for (const auto& id : titles) {
        if (id.size() != 16 || id.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos) return path;
        if (!ids.empty()) ids += ',';
        ids += id;
    }
    if (titles.size() > 16) return path;
    return path + "?titles=" + ids;
}
} // namespace nextendo
