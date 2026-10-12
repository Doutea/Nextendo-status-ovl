// Nextendo NAT check overlay
//
// A standalone overlay that reports the network environment type (A/B/C/D/F)
// and NAT behaviour, using the same RFC 4787/5780 classification the console's
// own connection test uses. Also shows the public IP.
//
// Stores the result in sdmc:/switch/nextendo-status/nat.txt so the next open
// shows it immediately.

#define TESLA_INIT_IMPL
#include <tesla.hpp>
#include <switch.h>
#include <curl/curl.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <poll.h>
#include <atomic>
#include <vector>
#include <string>
#include <algorithm>
#include <cstring>
#include <cstdio>
#include <cstdlib>

// ---------------------------------------------------------------------------
// Language
// ---------------------------------------------------------------------------

static bool g_useChinese = false;

void nextendoUseChinese(bool chinese) {
    g_useChinese = chinese;
}

extern "C" bool nextendoIsChinese() {
    return g_useChinese;
}

static bool detectChineseLanguage() {
    u64 code = 0;
    if (R_FAILED(appletGetDesiredLanguage(&code))) return false;
    SetLanguage lang{};
    if (R_FAILED(setMakeLanguage(code, &lang))) return false;
    return lang == SetLanguage_ZHCN || lang == SetLanguage_ZHHANS
        || lang == SetLanguage_ZHTW || lang == SetLanguage_ZHHANT;
}

struct UiText { const char* zh; const char* en; };

static const char* T(const UiText& t) {
    return g_useChinese ? t.zh : t.en;
}

static const UiText kTitle       = {"\u7F51\u7EDC\u73AF\u5883\u68C0\u6D4B", "NAT Check"};
static const UiText kSubtitle    = {"\u8FDE\u63A5\u4E0E NAT", "link & NAT"};
static const UiText kTesting     = {"\u68C0\u6D4B\u4E2D\u2026", "Testing\u2026"};
static const UiText kUntested    = {"\u672A\u68C0\u6D4B", "Not tested"};
static const UiText kRetest      = {"\u91CD\u65B0\u68C0\u6D4B", "Test again"};
static const UiText kResult      = {"\u68C0\u6D4B\u7ED3\u679C", "Result"};
static const UiText kNatType     = {"NAT \u7C7B\u578B", "NAT type"};
static const UiText kNatPeer     = {"\u5BF9\u7B49\u7F51\u7EDC", "Peer to peer"};
static const UiText kNatMapping  = {"\u6620\u5C04\u884C\u4E3A", "Mapping"};
static const UiText kNatPublic   = {"\u516C\u7F51\u5730\u5740", "Public address"};
static const UiText kUnknown     = {"\u672A\u77E5", "Unknown"};
static const UiText kTypeA       = {"\u7C7B\u578B A", "Type A"};
static const UiText kTypeB       = {"\u7C7B\u578B B", "Type B"};
static const UiText kTypeC       = {"\u7C7B\u578B C", "Type C"};
static const UiText kTypeD       = {"\u7C7B\u578B D", "Type D"};
static const UiText kTypeF       = {"\u7C7B\u578B F", "Type F"};
static const UiText kExcellent   = {"\u6781\u4F73", "Excellent"};
static const UiText kGood        = {"\u826F\u597D", "Good"};
static const UiText kFair        = {"\u4E00\u822C", "Fair"};
static const UiText kPoor        = {"\u8F83\u5DEE", "Poor"};
static const UiText kBlocked     = {"\u5DF2\u963B\u65AD", "Blocked"};
static const UiText kEi          = {"\u7AEF\u70B9\u65E0\u5173", "Endpoint-independent"};
static const UiText kApd         = {"\u5730\u5740\u7AEF\u53E3\u76F8\u5173", "Address-and-port-dependent"};

// ---------------------------------------------------------------------------
// Cache helpers (reuse the fs-API path, which is the only one that works in
// overlay processes on this console).
// ---------------------------------------------------------------------------

static std::string cacheEscape(const std::string& t) {
    std::string o; o.reserve(t.size());
    for (char c : t) { switch(c) { case '\\': o+="\\\\"; break; case '\t': o+="\\t"; break; case '\n': o+="\\n"; break; case '\r': o+="\\r"; break; default: o+=c; break; } }
    return o;
}
static std::string cacheUnescape(const std::string& t) {
    std::string o; o.reserve(t.size());
    for (size_t i=0; i<t.size(); ++i) {
        if (t[i]=='\\' && i+1<t.size()) { switch(t[++i]) { case 't': o+='\t'; break; case 'n': o+='\n'; break; case 'r': o+='\r'; break; default: o+=t[i]; break; } }
        else o+=t[i];
    }
    return o;
}

// ---------------------------------------------------------------------------
// Network helpers (resolveHostToIp, fetchUrl)
// ---------------------------------------------------------------------------

static std::string resolveHostToIp(const std::string& hostname) {
    struct addrinfo hints{}; hints.ai_family=AF_INET; hints.ai_socktype=SOCK_STREAM;
    struct addrinfo* res=nullptr;
    if (getaddrinfo(hostname.c_str(),nullptr,&hints,&res)!=0||!res) return "";
    char buf[16]; inet_ntop(AF_INET, &reinterpret_cast<struct sockaddr_in*>(res->ai_addr)->sin_addr, buf, sizeof(buf));
    freeaddrinfo(res);
    return buf;
}

static size_t WriteCallback(void* c, size_t s, size_t n, void* p) {
    size_t total=s*n; static_cast<std::string*>(p)->append(static_cast<char*>(c),total); return total;
}

// Simple fetch for plain HTTP endpoints (checkip.amazonaws.com). No TLS needed.
static std::string fetchUrl(const std::string& url, std::string& errorOut) {
    std::string buf;
    CURL* c = curl_easy_init();
    if (!c) { errorOut="curl_easy_init failed"; return ""; }
    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, WriteCallback);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &buf);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 15L);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    CURLcode res = curl_easy_perform(c);
    if (res!=CURLE_OK) { errorOut=curl_easy_strerror(res); curl_easy_cleanup(c); return ""; }
    curl_easy_cleanup(c);
    return buf;
}

static const SocketInitConfig socketConfig = {
    .tcp_tx_buf_size = 0x800,
    .tcp_rx_buf_size = 0x1000,
    .tcp_tx_buf_max_size = 0x10000,
    .tcp_rx_buf_max_size = 0x10000,
    .udp_tx_buf_size = 0x2400,
    .udp_rx_buf_size = 0xA500,
    .sb_efficiency = 4,
};

// ---------------------------------------------------------------------------
// NAT detection engine
// ---------------------------------------------------------------------------

// Colours: must be defined here (not in libtesla) since we don't patch that
// header. The naming matches what the reference table expects.
static constexpr tsl::Color ColorGreen      = {0x5, 0xC, 0x7, 0xF};
static constexpr tsl::Color ColorLightGreen = {0x8, 0xE, 0x8, 0xF};
static constexpr tsl::Color ColorAmber      = {0xD, 0x9, 0x3, 0xF};
static constexpr tsl::Color ColorOrange     = {0xE, 0x8, 0x4, 0xF};
static constexpr tsl::Color ColorRed        = {0xE, 0x6, 0x6, 0xF};

enum class NatMapping  { Unknown, EndpointIndependent, AddressPortDependent };
enum class NatFiltering{ Unknown, EndpointIndependent, AddressDependent, AddressPortDependent };
enum class NatType     { Unknown, A, B, C, D, F };
enum class NatQuality  { Unknown, Excellent, Good, Fair, Poor };
enum class NatPeer     { Unknown, Excellent, Fair, Poor, Blocked };

struct NatStatus {
    bool tested = false;
    bool udpWorking = false;
    NatType type = NatType::Unknown;
    NatQuality quality = NatQuality::Unknown;
    NatPeer peer = NatPeer::Unknown;
    NatMapping mapping = NatMapping::Unknown;
    NatFiltering filtering = NatFiltering::Unknown;
    std::string publicIp;
};

static NatStatus g_nat;
static Mutex g_natMutex;
static std::atomic<bool> g_natRequested{false};
static std::atomic<bool> g_natTesting{false};
static std::atomic<bool> g_natDirty{false};
static std::atomic<bool> g_natThreadRunning{false};
static Thread g_natThread;
alignas(0x1000) static u8 g_natThreadStack[0x40000];
static std::atomic<u64> g_natTestedSeconds{0};

static const char* kNatCachePath = "/switch/nextendo-status/nat.txt";

// ---------------------------------------------------------------------------
// STUN
// ---------------------------------------------------------------------------

struct StunServer { const char* host; u16 port; const char* ipFallback; };

static const StunServer kStunServers[] = {
    {"stun.miwifi.com",    3478,  "111.206.174.2"},
    {"stun.dus.net",       3478,  "83.125.8.45"},
    {"stun.pure-ip.com",   3478,  "34.195.177.19"},
    {"stun.tel.lu",        3478,  "116.79.127.49"},
    {"stun.vo.lu",         3478,  "116.79.127.49"},
    {"stun.siptrunk.com",  3478,  "23.140.243.250"},
    {"stun.voipxs.nl",     3478,  "122.197.160.201"},
    {"stun.nextcloud.com", 3478,  "46.225.95.169"},
    {"stun.l.google.com",  19302, "74.125.250.129"},
    {"stun.cloudflare.com",3478,  "162.159.207.0"},
};
static const int kStunServerCount = sizeof(kStunServers)/sizeof(kStunServers[0]);

static const u32 kStunMagic = 0x2112A442u;
static const u16 kStunBindingRequest  = 0x0001;
static const u16 kStunBindingResponse = 0x0101;
static const u16 kStunAttrChangeRequest = 0x0003;
static const u16 kStunAttrMapped     = 0x0001;
static const u16 kStunAttrXorMapped  = 0x0020;
static const u16 kStunAttrResponseOrigin = 0x802B;
static const u16 kStunAttrOtherAddress   = 0x802C;

static u64 uptimeMs()      { return armTicksToNs(armGetSystemTick()) / 1000000ULL; }
static u64 uptimeSeconds() { return armTicksToNs(armGetSystemTick()) / 1000000000ULL; }

static bool isPrivateOrCgnat(const std::string& ip) {
    unsigned a=0,b=0,c=0,d=0;
    if (sscanf(ip.c_str(),"%u.%u.%u.%u",&a,&b,&c,&d)!=4) return false;
    return a==10 || (a==172 && b>=16 && b<=31) || (a==192 && b==168)
        || (a==100 && b>=64 && b<=127) || (a==169 && b==254) || a==127 || a==0;
}

static std::string ipv4ToString(u32 addr) {
    char buf[20];
    snprintf(buf,sizeof(buf),"%u.%u.%u.%u",
             addr&0xFF,(addr>>8)&0xFF,(addr>>16)&0xFF,(addr>>24)&0xFF);
    return buf;
}

static size_t stunBuildRequest(u8* out, u32 changeFlags) {
    u8 txn[12]; u64 r1=randomGet64(), r2=randomGet64();
    memcpy(txn,&r1,8); memcpy(txn+8,&r2,4);
    size_t pos=0; u16 attrLen=changeFlags?8:0;
    out[pos++]=kStunBindingRequest>>8; out[pos++]=kStunBindingRequest&0xFF;
    out[pos++]=attrLen>>8; out[pos++]=attrLen&0xFF;
    out[pos++]=0x21; out[pos++]=0x12; out[pos++]=0xA4; out[pos++]=0x42;
    memcpy(out+pos,txn,12); pos+=12;
    if (changeFlags) {
        out[pos++]=0x00; out[pos++]=0x03; out[pos++]=0x00; out[pos++]=0x04;
        out[pos++]=(changeFlags>>24)&0xFF; out[pos++]=(changeFlags>>16)&0xFF;
        out[pos++]=(changeFlags>>8)&0xFF; out[pos++]=changeFlags&0xFF;
    }
    return pos;
}

struct StunAttributes { std::string mapped, other, origin; };

static std::string stunAddress(const u8* v, u16 len, bool xored) {
    if (len<8 || v[1]!=0x01) return "";
    u16 port = (v[2]<<8)|v[3]; u32 addr = (v[4]<<24)|(v[5]<<16)|(v[6]<<8)|v[7];
    if (xored) { port^=kStunMagic>>16; addr^=kStunMagic; }
    char buf[24]; snprintf(buf,sizeof(buf),"%u.%u.%u.%u:%u",
        (addr>>24)&0xFF,(addr>>16)&0xFF,(addr>>8)&0xFF,addr&0xFF,(unsigned)port);
    return buf;
}

static bool stunParse(const u8* data, size_t len, StunAttributes& out) {
    if (len<20) return false;
    u16 mt=(data[0]<<8)|data[1], alen=(data[2]<<8)|data[3];
    if (mt!=kStunBindingResponse) return false;
    if (data[4]!=0x21||data[5]!=0x12||data[6]!=0xA4||data[7]!=0x42) return false;
    bool saw=false; size_t pos=20, end=20+alen;
    while (pos+4<=len && pos+4<=end) {
        u16 t=(data[pos]<<8)|data[pos+1], vl=(data[pos+2]<<8)|data[pos+3];
        const u8* val=data+pos+4; if (pos+4+vl>len) break;
        if      (t==kStunAttrXorMapped)     { out.mapped=stunAddress(val,vl,true);  saw=true; }
        else if (t==kStunAttrMapped)         { out.mapped=stunAddress(val,vl,false); saw=true; }
        else if (t==kStunAttrOtherAddress)   { out.other=stunAddress(val,vl,false); }
        else if (t==kStunAttrResponseOrigin) { out.origin=stunAddress(val,vl,false); }
        pos += 4+vl+((4-(vl%4))%4);
    }
    return saw;
}

struct StunSocket {
    int fd=-1; u16 localPort=0;
    bool open() {
        fd=socket(AF_INET,SOCK_DGRAM,0); if (fd<0) return false;
        sockaddr_in local{}; local.sin_family=AF_INET; local.sin_addr.s_addr=INADDR_ANY;
        if (bind(fd,(sockaddr*)&local,sizeof(local))<0) { ::close(fd); fd=-1; return false; }
        sockaddr_in bound{}; socklen_t bl=sizeof(bound);
        if (getsockname(fd,(sockaddr*)&bound,&bl)==0) localPort=ntohs(bound.sin_port);
        return true;
    }
    void close() { if (fd>=0) ::close(fd); fd=-1; }
    bool query(const std::string& ip, u16 port, u32 changeFlags, int timeoutMs,
               StunAttributes& out, std::string& src) {
        if (fd<0) return false;
        sockaddr_in to{}; to.sin_family=AF_INET; to.sin_port=htons(port);
        to.sin_addr.s_addr=inet_addr(ip.c_str());
        u8 req[32]; size_t len=stunBuildRequest(req,changeFlags);
        if (sendto(fd,req,len,0,(sockaddr*)&to,sizeof(to))<0) return false;
        u64 dl=uptimeMs()+timeoutMs;
        while (uptimeMs()<dl) {
            pollfd p{}; p.fd=fd; p.events=POLLIN;
            if (poll(&p,1,200)<=0) continue;
            u8 resp[512]; sockaddr_in from{}; socklen_t fl=sizeof(from);
            int n=recvfrom(fd,resp,sizeof(resp),0,(sockaddr*)&from,&fl);
            if (n<=0) continue;
            if (!stunParse(resp,n,out)) continue;
            char s[32]={0}; inet_ntop(AF_INET,&from.sin_addr,s,sizeof(s));
            src=std::string(s)+":"+std::to_string(ntohs(from.sin_port));
            return true;
        }
        return false;
    }
};

static std::string stunServerAddress(const StunServer& s) {
    std::string r=resolveHostToIp(s.host); return r.empty()?s.ipFallback:r;
}

static std::string addressPart(const std::string& a) { size_t c=a.find(':'); return c==std::string::npos?a:a.substr(0,c); }
static u16 portPart(const std::string& a) { size_t c=a.find(':'); return c==std::string::npos?0:(u16)atoi(a.c_str()+c+1); }

static void readLocalIp(std::string& out) {
    u32 addr=0,mask=0,gw=0,d1=0,d2=0;
    if (R_SUCCEEDED(nifmGetCurrentIpConfigInfo(&addr,&mask,&gw,&d1,&d2))) out=ipv4ToString(addr);
}

static std::string publicIpOverHttp() {
    std::string err;
    std::string body = fetchUrl("http://checkip.amazonaws.com", err);
    if (!err.empty() || body.empty()) return "";
    size_t f=body.find_first_not_of(" \t\r\n"); if (f==std::string::npos) return "";
    size_t l=body.find_last_not_of(" \t\r\n"); std::string ip=body.substr(f,l-f+1);
    unsigned a=0,b=0,c=0,d=0;
    return (sscanf(ip.c_str(),"%u.%u.%u.%u",&a,&b,&c,&d)==4) ? ip : "";
}

// ---------------------------------------------------------------------------
// The behaviour test
// ---------------------------------------------------------------------------

static void runNatDetection() {
    NatStatus result;
    std::string localIp; readLocalIp(localIp);

    // --- mapping ---------------------------------------------------------
    std::vector<std::string> behaviourTargets;
    StunSocket probe;
    if (probe.open()) {
        std::vector<std::string> dests; std::vector<u16> ports;
        for (int i=0; i<kStunServerCount && dests.size()<3; ++i) {
            const std::string addr=stunServerAddress(kStunServers[i]);
            if (addr.empty() || std::find(dests.begin(),dests.end(),addr)!=dests.end()) continue;
            StunAttributes a; std::string src;
            if (!probe.query(addr,kStunServers[i].port,0,1500,a,src)) continue;
            dests.push_back(addr); ports.push_back(portPart(a.mapped));
            if (!a.other.empty()) behaviourTargets.push_back(addr+":"+std::to_string(kStunServers[i].port));
            if (result.publicIp.empty()) { result.publicIp=addressPart(a.mapped); result.udpWorking=true; }
        }
        if (dests.size()>=2) {
            bool same=std::all_of(ports.begin(),ports.end(),[&](u16 p){return p==ports[0];});
            result.mapping=same?NatMapping::EndpointIndependent:NatMapping::AddressPortDependent;
        }
    }
    probe.close();

    // --- port predictability (C vs D) ------------------------------------
    bool predictable=false;
    if (result.mapping==NatMapping::AddressPortDependent) {
        std::vector<std::pair<u16,u16>> pairs;
        for (int i=0; i<3; ++i) {
            StunSocket s; if (!s.open()) continue;
            for (int k=0; k<kStunServerCount; ++k) {
                std::string addr=stunServerAddress(kStunServers[k]); if (addr.empty()) continue;
                StunAttributes a; std::string src;
                if (s.query(addr,kStunServers[k].port,0,1500,a,src)) { pairs.emplace_back(s.localPort,portPart(a.mapped)); break; }
            }
            s.close();
        }
        if (pairs.size()>=2) {
            int ld=(int)pairs[1].first-(int)pairs[0].first;
            int md=(int)pairs[1].second-(int)pairs[0].second;
            predictable = ld!=0 && ld==md;
        }
    }

    // --- filtering -------------------------------------------------------
    if (result.mapping==NatMapping::EndpointIndependent) {
        int usable=0, honBoth=0, honPort=0;
        for (const auto& tgt : behaviourTargets) {
            if (honBoth>0 || (usable>=2&&honPort>0) || usable>=3) break;
            const std::string addr=addressPart(tgt); const u16 port=portPart(tgt); ++usable;
            StunSocket t; if (!t.open()) continue;
            StunAttributes a; std::string src;
            if (t.query(addr,port,0x00000006,1200,a,src) && addressPart(src)!=addr) { ++honBoth; t.close(); break; }
            if (t.query(addr,port,0x00000004,1200,a,src) &&
                (addressPart(src)!=addr || portPart(src)!=port)) {
                if (addressPart(src)!=addr) ++honBoth; else ++honPort;
            }
            t.close();
        }
        if      (honBoth>0) result.filtering=NatFiltering::EndpointIndependent;
        else if (honPort>0) result.filtering=NatFiltering::AddressDependent;
        else if (usable>0)  result.filtering=NatFiltering::AddressPortDependent;
    }

    // --- public address fallback / carrier NAT ---------------------------
    if (result.publicIp.empty()) result.publicIp = publicIpOverHttp();
    bool cgnat = !result.publicIp.empty() && isPrivateOrCgnat(result.publicIp);
    bool openNet = !result.publicIp.empty() && !localIp.empty() &&
                   result.publicIp==localIp && !isPrivateOrCgnat(result.publicIp);

    // --- classification -------------------------------------------------
    if (!result.udpWorking) {
        result.type = result.publicIp.empty()?NatType::F:NatType::Unknown;
        result.quality=NatQuality::Poor; result.peer=NatPeer::Blocked;
    } else if (openNet) {
        result.type=NatType::A; result.quality=NatQuality::Excellent; result.peer=NatPeer::Excellent;
    } else if (result.mapping==NatMapping::AddressPortDependent) {
        result.type=predictable?NatType::C:NatType::D;
        result.quality=NatQuality::Poor; result.peer=NatPeer::Blocked;
    } else if (result.mapping==NatMapping::Unknown) {
        result.type=NatType::Unknown; result.quality=NatQuality::Unknown; result.peer=NatPeer::Unknown;
    } else {
        switch (result.filtering) {
            case NatFiltering::EndpointIndependent:
                result.type=NatType::A; result.quality=NatQuality::Excellent; result.peer=NatPeer::Excellent; break;
            case NatFiltering::AddressDependent:
                result.type=NatType::A; result.quality=NatQuality::Good; result.peer=NatPeer::Fair; break;
            default:
                result.type=NatType::B; result.quality=NatQuality::Fair; result.peer=NatPeer::Poor; break;
        }
    }
    if (cgnat) { result.type=NatType::D; result.quality=NatQuality::Poor; result.peer=NatPeer::Blocked; }

    result.tested=true;
    g_natTestedSeconds = uptimeSeconds();
    mutexLock(&g_natMutex); g_nat=result; mutexUnlock(&g_natMutex);
    saveNatCache(result);
    g_natDirty=true;
}

// ---------------------------------------------------------------------------
// Presentation
// ---------------------------------------------------------------------------

static const char* natTypeText(NatType t) {
    switch(t) { case NatType::A:return T(kTypeA); case NatType::B:return T(kTypeB);
                case NatType::C:return T(kTypeC); case NatType::D:return T(kTypeD);
                case NatType::F:return T(kTypeF); default: return T(kUnknown); }
}
static tsl::Color natTypeColour(NatType t) {
    switch(t) { case NatType::A:return ColorGreen; case NatType::B:return ColorLightGreen;
                case NatType::C:return ColorAmber; case NatType::D:return ColorOrange;
                case NatType::F:return ColorRed; default: return tsl::style::color::ColorText; }
}
static const char* natQualityText(NatQuality q) {
    switch(q) { case NatQuality::Excellent:return T(kExcellent); case NatQuality::Good:return T(kGood);
                case NatQuality::Fair:return T(kFair); case NatQuality::Poor:return T(kPoor);
                default: return T(kUnknown); }
}
static const char* natPeerText(NatPeer p) {
    switch(p) { case NatPeer::Excellent:return T(kExcellent); case NatPeer::Fair:return T(kFair);
                case NatPeer::Poor:return T(kPoor); case NatPeer::Blocked:return T(kBlocked);
                default: return T(kUnknown); }
}
static const char* natMappingText(NatMapping m) {
    switch(m) { case NatMapping::EndpointIndependent:return T(kEi);
                case NatMapping::AddressPortDependent:return T(kApd); default: return T(kUnknown); }
}

static std::string natSummaryText() {
    if (g_natTesting.load()) return T(kTesting);
    mutexLock(&g_natMutex); bool ok=g_nat.tested; NatQuality q=g_nat.quality; mutexUnlock(&g_natMutex);
    return ok ? natQualityText(q) : T(kUntested);
}
static tsl::Color natSummaryColour() {
    mutexLock(&g_natMutex); NatType t=g_nat.type; mutexUnlock(&g_natMutex);
    return natTypeColour(t);
}

// ---------------------------------------------------------------------------
// Cache
// ---------------------------------------------------------------------------

static void saveNatCache(const NatStatus& s) {
    std::string txt;
    txt += "tested=" + std::to_string(s.tested?1:0) + "\n";
    txt += "at=" + std::to_string(uptimeSeconds()) + "\n";
    txt += "udp=" + std::to_string(s.udpWorking?1:0) + "\n";
    txt += "type=" + std::to_string((int)s.type) + "\n";
    txt += "quality=" + std::to_string((int)s.quality) + "\n";
    txt += "peer=" + std::to_string((int)s.peer) + "\n";
    txt += "mapping=" + std::to_string((int)s.mapping) + "\n";
    txt += "filtering=" + std::to_string((int)s.filtering) + "\n";
    txt += "publicip=" + cacheEscape(s.publicIp) + "\n";

    FsFileSystem fs{}; if (R_FAILED(fsOpenSdCardFileSystem(&fs))) return;
    fsFsDeleteFile(&fs, kNatCachePath);
    if (R_SUCCEEDED(fsFsCreateFile(&fs, kNatCachePath, txt.size(), 0))) {
        FsFile f{};
        if (R_SUCCEEDED(fsFsOpenFile(&fs, kNatCachePath, FsOpenMode_Write, &f))) {
            fsFileWrite(&f, 0, txt.data(), txt.size(), FsWriteOption_Flush); fsFileClose(&f);
        }
    }
    fsFsClose(&fs);
}

static void loadNatCache() {
    FsFileSystem fs{}; if (R_FAILED(fsOpenSdCardFileSystem(&fs))) return;
    FsFile f{};
    if (R_FAILED(fsFsOpenFile(&fs, kNatCachePath, FsOpenMode_Read, &f))) { fsFsClose(&fs); return; }
    s64 sz=0; fsFileGetSize(&f, &sz);
    if (sz<=0 || sz>64*1024) { fsFileClose(&f); fsFsClose(&fs); return; }
    std::string buf((size_t)sz,'\0'); u64 rd=0;
    if (R_FAILED(fsFileRead(&f,0,buf.data(),buf.size(),FsReadOption_None,&rd)) || rd==0)
        { fsFileClose(&f); fsFsClose(&fs); return; }
    buf.resize((size_t)rd); fsFileClose(&f); fsFsClose(&fs);

    NatStatus ld; size_t pos=0;
    while (pos<=buf.size()) {
        size_t eol=buf.find('\n',pos);
        std::string ln=buf.substr(pos,(eol==std::string::npos?buf.size():eol)-pos);
        pos=(eol==std::string::npos)?buf.size()+1:eol+1;
        size_t eq=ln.find('='); if (eq==std::string::npos) continue;
        std::string k=ln.substr(0,eq), v=cacheUnescape(ln.substr(eq+1));
        if      (k=="tested")    ld.tested=(v=="1");
        else if (k=="at")        g_natTestedSeconds=strtoull(v.c_str(),nullptr,10);
        else if (k=="udp")       ld.udpWorking=(v=="1");
        else if (k=="type")      ld.type=(NatType)atoi(v.c_str());
        else if (k=="quality")   ld.quality=(NatQuality)atoi(v.c_str());
        else if (k=="peer")      ld.peer=(NatPeer)atoi(v.c_str());
        else if (k=="mapping")   ld.mapping=(NatMapping)atoi(v.c_str());
        else if (k=="filtering") ld.filtering=(NatFiltering)atoi(v.c_str());
        else if (k=="publicip")  ld.publicIp=v;
    }
    if (!ld.tested) return;
    mutexLock(&g_natMutex); g_nat=ld; mutexUnlock(&g_natMutex);
}

// ---------------------------------------------------------------------------
// Worker thread
// ---------------------------------------------------------------------------

static void natThreadFunc(void*) {
    while (g_natThreadRunning.load()) {
        if (g_natRequested.exchange(false)) {
            g_natTesting=true;  g_natDirty=true;
            runNatDetection();
            g_natTesting=false; g_natDirty=true;
        }
        svcSleepThread(200'000'000LL);
    }
}

// ---------------------------------------------------------------------------
// Detail screen
// ---------------------------------------------------------------------------

static void setValue(tsl::elm::ListItem* item, const std::string& v, bool faint) {
    if (item) item->setValue(v, faint);
}

class NatDetailGui : public tsl::Gui {
public:
    tsl::elm::OverlayFrame* m_frame=nullptr;
    tsl::elm::ListItem* m_retest=nullptr;
    tsl::elm::ListItem* m_type=nullptr;
    tsl::elm::ListItem* m_peer=nullptr;
    tsl::elm::ListItem* m_mapping=nullptr;
    tsl::elm::ListItem* m_public=nullptr;

    virtual tsl::elm::Element* createUI() override {
        m_frame = new tsl::elm::OverlayFrame(T(kTitle), T(kSubtitle));
        auto* list = new tsl::elm::List();

        m_retest = new tsl::elm::ListItem(T(kRetest));
        m_retest->setClickListener([](u64 keys){
            if (keys & HidNpadButton_A) { g_natRequested=true; return true; } return false;
        });
        list->addItem(m_retest);

        list->addItem(new tsl::elm::CategoryHeader(T(kResult)));
        m_type    = new tsl::elm::ListItem(T(kNatType));    list->addItem(m_type);
        m_peer    = new tsl::elm::ListItem(T(kNatPeer));    list->addItem(m_peer);
        m_mapping = new tsl::elm::ListItem(T(kNatMapping)); list->addItem(m_mapping);
        m_public  = new tsl::elm::ListItem(T(kNatPublic));  list->addItem(m_public);

        m_frame->setContent(list);
        refresh();
        return m_frame;
    }

    virtual void update() override {
        if (g_natDirty.exchange(false)) refresh();
    }

    void refresh() {
        NatStatus s; mutexLock(&g_natMutex); s=g_nat; mutexUnlock(&g_natMutex);
        bool testing=g_natTesting.load();

        if (testing)      m_frame->setSubtitle(T(kTesting));
        else              m_frame->setSubtitle(s.tested ? T(kSubtitle) : T(kUntested));
        if (m_retest)     m_retest->setValue(testing ? T(kTesting) : "", true);

        if (!s.tested) { setValue(m_type,T(kUnknown),true); setValue(m_peer,T(kUnknown),true);
                          setValue(m_mapping,T(kUnknown),true); setValue(m_public,T(kUnknown),true); return; }

        if (m_type) { m_type->setValue(natTypeText(s.type)); m_type->setValueColor(natTypeColour(s.type)); }
        setValue(m_peer, natPeerText(s.peer), false);
        setValue(m_mapping, natMappingText(s.mapping), false);
        setValue(m_public, s.publicIp.empty()?T(kUnknown):s.publicIp, s.publicIp.empty());
    }
};

// ---------------------------------------------------------------------------
// Main panel
// ---------------------------------------------------------------------------

class GuiTest : public tsl::Gui {
public:
    tsl::elm::OverlayFrame* m_frame=nullptr;
    tsl::elm::ListItem* m_statusItem=nullptr;
    tsl::elm::ListItem* m_retestItem=nullptr;
    tsl::elm::ListItem* m_typeItem=nullptr;
    tsl::elm::ListItem* m_peerItem=nullptr;
    tsl::elm::ListItem* m_mappingItem=nullptr;
    tsl::elm::ListItem* m_publicItem=nullptr;

    virtual tsl::elm::Element* createUI() override {
        m_frame = new tsl::elm::OverlayFrame(T(kTitle), T(kUntested));
        auto* list = new tsl::elm::List();

        list->addItem(new tsl::elm::CategoryHeader(T(kResult)));

        // Status summary row — opens the detail screen when selected
        m_statusItem = new tsl::elm::ListItem(natSummaryText());
        m_statusItem->setClickListener([](u64 keys){
            if (keys & HidNpadButton_A) { tsl::changeTo<NatDetailGui>(); return true; } return false;
        });
        list->addItem(m_statusItem);

        // Re-test button
        m_retestItem = new tsl::elm::ListItem(T(kRetest));
        m_retestItem->setClickListener([](u64 keys){
            if (keys & HidNpadButton_A) { g_natRequested=true; return true; } return false;
        });
        list->addItem(m_retestItem);

        list->addItem(new tsl::elm::CategoryHeader(T(kResult)));
        m_typeItem    = new tsl::elm::ListItem(T(kNatType));    list->addItem(m_typeItem);
        m_peerItem    = new tsl::elm::ListItem(T(kNatPeer));    list->addItem(m_peerItem);
        m_mappingItem = new tsl::elm::ListItem(T(kNatMapping)); list->addItem(m_mappingItem);
        m_publicItem  = new tsl::elm::ListItem(T(kNatPublic));  list->addItem(m_publicItem);

        m_frame->setContent(list);
        refresh();
        return m_frame;
    }

    virtual void update() override {
        if (g_natDirty.exchange(false)) refresh();
    }

    virtual bool handleInput(u64 keysDown, u64 keysHeld, const HidTouchState& touchPos,
                             HidAnalogStickState joyStickPosLeft,
                             HidAnalogStickState joyStickPosRight) override { return false; }

private:
    void refresh() {
        NatStatus s; mutexLock(&g_natMutex); s=g_nat; mutexUnlock(&g_natMutex);
        bool testing = g_natTesting.load();

        // Subtitle
        if (testing)          m_frame->setSubtitle(T(kTesting));
        else if (s.tested)    m_frame->setSubtitle(natSummaryText());
        else                  m_frame->setSubtitle(T(kUntested));

        // Summary row
        if (m_statusItem) { m_statusItem->setValue(natSummaryText()); m_statusItem->setValueColor(natSummaryColour()); }
        // Re-test
        if (m_retestItem) m_retestItem->setValue(testing ? T(kTesting) : "", true);

        if (!s.tested) {
            setValue(m_typeItem, T(kUnknown), true);
            setValue(m_peerItem, T(kUnknown), true);
            setValue(m_mappingItem, T(kUnknown), true);
            setValue(m_publicItem, T(kUnknown), true);
            return;
        }

        if (m_typeItem) { m_typeItem->setValue(natTypeText(s.type)); m_typeItem->setValueColor(natTypeColour(s.type)); }
        setValue(m_peerItem,    natPeerText(s.peer),    false);
        setValue(m_mappingItem, natMappingText(s.mapping), false);
        setValue(m_publicItem,  s.publicIp.empty()?T(kUnknown):s.publicIp, s.publicIp.empty());
    }
};

// ---------------------------------------------------------------------------
// Overlay entry
// ---------------------------------------------------------------------------

class NatCheckOverlay : public tsl::Overlay {
public:
    virtual void initServices() override {
        fsdevMountSdmc();
        timeInitialize();
        nifmInitialize(NifmServiceType_User);
        socketInitialize(&socketConfig);
        curl_global_init(CURL_GLOBAL_DEFAULT);

        nextendoUseChinese(detectChineseLanguage());
        mutexInit(&g_natMutex);
        loadNatCache();

        g_natThreadRunning=true;
        threadCreate(&g_natThread, natThreadFunc, nullptr, g_natThreadStack,
                      sizeof(g_natThreadStack), 0x2C, -2);
        threadStart(&g_natThread);

        // Auto-test if no result exists or it is older than 30 min.
        bool needTest=false;
        { mutexLock(&g_natMutex); needTest=!g_nat.tested; mutexUnlock(&g_natMutex); }
        if (!needTest) {
            u64 last=g_natTestedSeconds.load(), now=uptimeSeconds();
            if (last==0 || now<last || (now-last)>1800) needTest=true;
        }
        if (needTest) g_natRequested=true;
    }

    virtual void exitServices() override {
        g_natThreadRunning=false;
        threadWaitForExit(&g_natThread);
        threadClose(&g_natThread);
        curl_global_cleanup();
        socketExit();
        nifmExit();
        timeExit();
        fsdevUnmountDevice("sdmc");
    }

    virtual void onShow() override {}
    virtual void onHide() override {}
    virtual std::unique_ptr<tsl::Gui> loadInitialGui() override {
        return initially<GuiTest>();
    }
};

int main(int argc, char** argv) {
    return tsl::loop<NatCheckOverlay>(argc, argv);
}