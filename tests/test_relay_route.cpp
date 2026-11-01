#include "../source/relay_route.hpp"
#include <cassert>
#include <iostream>
int main() {
    using namespace nextendo;
    const std::string key(64, 'a');
    auto direct = parseRelayRoute("# No relay configured\nrelay_url=\n");
    assert(direct.error.empty() && !direct.enabled() && !direct.tcpEnabled());
    auto relay = parseRelayRoute("relay_url=https://Relay.Example.com:443/\nrelay_ip=203.0.113.1\nrelay_key=" + key);
    assert(relay.error.empty() && relay.host == "relay.example.com" && relay.enabled());
    auto route = selectRoute(relay, "https://nextendo.network/api/me", "nextendo.network", {"104.21.84.82"});
    assert(route.url == "https://relay.example.com/api/me" && route.host == relay.host);
    assert(route.fallback.size() == 1 && route.fallback[0] == "203.0.113.1" && route.relayed && route.connectTo.empty());
    auto github = selectRoute(relay, "https://raw.githubusercontent.com/file", "raw.githubusercontent.com", {"185.199.108.133"});
    assert(!github.relayed && github.host == "raw.githubusercontent.com" && github.fallback[0] == "185.199.108.133");
    assert(!selectRoute(relay, "https://nextendo.network.evil.example/api/me", "nextendo.network", {}).relayed);
    for (const auto& url : {"http://relay.example.com", "https://user@relay.example.com", "https://relay.example.com/path",
        "https://relay.example.com?url=x", "https://relay.example.com:8443", "https://-bad.example.com", "https://a..example.com",
        "https://a.example.com/#x", "https://a.example.com\r\nInjected:yes", "https://a.example.com/../"}) {
        assert(!parseRelayRoute(std::string("relay_url=") + url + "\nrelay_key=" + key).error.empty());
    }
    assert(!parseRelayRoute("relay_url=https://a.example.com\nrelay_key=short").error.empty());
    assert(!parseRelayRoute("relay_url=\nrelay_url=\n").error.empty());
    assert(!parseRelayRoute("unexpected=yes").error.empty());
    assert(!parseRelayRoute(std::string(4097, 'x')).error.empty());
    assert(!parseRelayRoute("relay_ip=203.0.113.1").error.empty());
    assert(relayLivePath(relay, "/api/status/live", {"0100152000022000"}) == "/api/status/live?titles=0100152000022000");
    assert(relayLivePath(direct, "/api/status/live", {"0100152000022000"}) == "/api/status/live");
    assert(relayLivePath(relay, "/api/status/live", {"bad&x=y"}) == "/api/status/live");
    auto tcp = parseRelayRoute("tcp_relay=192.168.31.167:18443\n");
    assert(tcp.tcpEnabled() && !tcp.enabled() && tcp.error.empty());
    auto tunnel = selectRoute(tcp, "https://nextendo.network/api/status/live", "nextendo.network", {"172.67.190.75"});
    assert(tunnel.url == "https://nextendo.network/api/status/live" && tunnel.host == "nextendo.network");
    assert(tunnel.connectTo == "nextendo.network:443:192.168.31.167:18443" && tunnel.fallback.empty());
    assert(selectRoute(tcp, "https://raw.githubusercontent.com/x", "raw.githubusercontent.com", {}).connectTo.empty());
    for (const auto& config : {"tcp_relay=192.168.1.2:0", "tcp_relay=192.168.1.2:65536", "tcp_relay=192.168.1.2:abc",
        "tcp_relay=999.168.1.2:443", "tcp_relay=192.168.01.2:443", "tcp_relay=host.example:443",
        "tcp_relay=192.168.1.2:443\nrelay_url=https://relay.example.com"}) assert(!parseRelayRoute(config).error.empty());
    assert(routeIpv4("127.0.0.1") && !routeIpv4("127.0.0.1.2") && !routeIpv4("1.2.3"));
    auto dual = parseRelayRoute("relay_url=https://relay.example.com\nrelay_ip=104.21.62.41\nrelay_ip_backup=172.67.219.244\nrelay_key=" + key);
    auto two = selectRoute(dual, "https://nextendo.network/api/me", "nextendo.network", {});
    assert((two.fallback == std::vector<std::string>{"104.21.62.41", "172.67.219.244"}));
    assert(!routeNeedsDns(two, false) && !routeNeedsDns(two, true));
    two.fallback.clear();
    assert(routeNeedsDns(two, false) && !routeNeedsDns(two, true));
    assert(!routeNeedsDns(tunnel, false));
    assert(!parseRelayRoute("relay_url=https://relay.example.com\nrelay_ip_backup=172.67.219.244\nrelay_key=" + key).error.empty());
    assert(!parseRelayRoute("relay_url=https://relay.example.com\nrelay_ip=104.21.62.41\nrelay_ip_backup=999.1.1.1\nrelay_key=" + key).error.empty());
    std::cout << "Relay routing checks passed\n";
}
