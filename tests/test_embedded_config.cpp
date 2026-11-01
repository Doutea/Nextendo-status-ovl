// Exercise the production loader with deterministic SD-card results.
#include <cassert>
#include <cstring>
#include <iostream>
#include <string>
using s64 = long long;
using u64 = unsigned long long;
struct FsFileSystem {};
struct FsFile {};
constexpr int FsOpenMode_Read = 1, FsReadOption_None = 0;
#define R_SUCCEEDED(rc) ((rc) == 0)
#define R_FAILED(rc) ((rc) != 0)
static bool mounted = true, exists = false, readable = true;
static std::string contents;
static int fsOpenSdCardFileSystem(FsFileSystem*) { return mounted ? 0 : 1; }
static int fsFsOpenFile(FsFileSystem*, const char*, int, FsFile*) { return exists ? 0 : 1; }
static int fsFileGetSize(FsFile*, s64* size) { *size = contents.size(); return 0; }
static int fsFileRead(FsFile*, int, void* out, size_t n, int, u64* read) {
    if (!readable) return 1;
    std::memcpy(out, contents.data(), n); *read = n; return 0;
}
static void fsFileClose(FsFile*) {}
static void fsFsClose(FsFileSystem*) {}
#define NEXTENDO_EMBEDDED_RELAY_CONFIG "relay_url=https://relay.example.com\nrelay_ip=203.0.113.1\nrelay_ip_backup=203.0.113.2\nrelay_key=aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\n"
#include "../source/relay_config.hpp"
int main() {
    loadRelayRoute();
    assert(g_relayRoute.enabled() && g_relayRoute.host == "relay.example.com");
    assert(g_relayRoute.ip == "203.0.113.1" && g_relayRoute.backupIp == "203.0.113.2");
    mounted = false; loadRelayRoute();
    assert(g_relayRoute.enabled());
    mounted = true; exists = true;
    contents = "relay_url=https://other.example.com\nrelay_key=bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb\n";
    loadRelayRoute();
    assert(g_relayRoute.enabled() && g_relayRoute.host == "other.example.com");
    contents.clear(); loadRelayRoute();
    assert(!g_relayRoute.enabled() && g_relayRoute.error.empty());
    contents = "bad_key=value"; loadRelayRoute();
    assert(!g_relayRoute.enabled() && !g_relayRoute.error.empty());
    contents.assign(4097, 'x'); loadRelayRoute();
    assert(!g_relayRoute.enabled() && !g_relayRoute.error.empty());
    contents = "test"; readable = false; loadRelayRoute();
    assert(!g_relayRoute.enabled() && !g_relayRoute.error.empty());
    exists = false; loadRelayRoute();
    assert(g_relayRoute.enabled() && g_relayRoute.host == "relay.example.com");
    std::cout << "Embedded defaults and SD override checks passed\n";
}
