// Network diagnostic probe.
//
// The main overlay reports only "Couldn't resolve host name", which does not
// say whether DNS, the connection, or TLS failed. This probe performs the same
// kind of work step by step and prints the exact libnx result codes, so the
// failure can be located without access to a debugger.
//
// Nothing here is meant to be pretty; every row reports one outcome.

#define TESLA_INIT_IMPL
#include <tesla.hpp>

#include <switch.h>

#include <curl/curl.h>

#include <cstdio>
#include <cstring>
#include <string>

namespace {

std::string hex_result(Result rc) {
    if (R_SUCCEEDED(rc)) return "ok";
    char buf[24];
    std::snprintf(buf, sizeof(buf), "0x%08X", static_cast<unsigned>(rc));
    return buf;
}

std::string errno_name(int err) {
    switch (err) {
        case EAI_AGAIN: return "EAI_AGAIN";
        case EAI_BADFLAGS: return "EAI_BADFLAGS";
        case EAI_FAIL: return "EAI_FAIL";
        case EAI_FAMILY: return "EAI_FAMILY";
        case EAI_MEMORY: return "EAI_MEMORY";
        case EAI_NONAME: return "EAI_NONAME";
        case EAI_SERVICE: return "EAI_SERVICE";
        case EAI_SOCKTYPE: return "EAI_SOCKTYPE";
        default: break;
    }
    char buf[24];
    std::snprintf(buf, sizeof(buf), "errno %d", err);
    return buf;
}

std::size_t sink_cb(char* ptr, std::size_t size, std::size_t nmemb, void* userdata) {
    auto* out = static_cast<std::string*>(userdata);
    if (out->size() < 512) out->append(ptr, size * nmemb);
    return size * nmemb;
}

struct Row {
    std::string label;
    std::string value;
};

// A compact screen of label/value rows, rebuilt as results come in.
class NetProbeGui : public tsl::Gui {
public:
    tsl::elm::Element* createUI() override {
        frame_ = new tsl::elm::OverlayFrame("Net Probe", "diagnostic");
        list_ = new tsl::elm::List();
        frame_->setContent(list_);

        // Socket init, mirroring the overlay's own configuration.
        SocketInitConfig cfg = *socketGetDefaultInitConfig();
        cfg.tcp_tx_buf_size = 0x2000;
        cfg.tcp_rx_buf_size = 0x4000;
        cfg.tcp_tx_buf_max_size = 0x2000;
        cfg.tcp_rx_buf_max_size = 0x10000;
        cfg.udp_tx_buf_size = 0x1000;
        cfg.udp_rx_buf_size = 0x4000;
        cfg.sb_efficiency = 2;

        results_.push_back({"socketInitialize", hex_result(socketInitialize(&cfg))});
        results_.push_back({"nifmInitialize", hex_result(nifmInitialize(NifmServiceType_User))});

        // 1. Plain getaddrinfo on the API host.
        addrinfo hints{};
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        addrinfo* res = nullptr;
        const int gai = getaddrinfo("nextendo.network", "443", &hints, &res);
        results_.push_back({"getaddrinfo(nextendo)", gai == 0 ? "ok" : errno_name(gai)});
        if (gai == 0 && res != nullptr) {
            char ip[64]{};
            if (res->ai_addr != nullptr) {
                auto* in4 = reinterpret_cast<sockaddr_in*>(res->ai_addr);
                std::snprintf(ip, sizeof(ip), "%u.%u.%u.%u",
                              (in4->sin_addr.s_addr >> 0) & 0xFF,
                              (in4->sin_addr.s_addr >> 8) & 0xFF,
                              (in4->sin_addr.s_addr >> 16) & 0xFF,
                              (in4->sin_addr.s_addr >> 24) & 0xFF);
            }
            results_.push_back({"  first address", ip});
            results_.push_back({"  socketGetLastResult", hex_result(socketGetLastResult())});
            freeaddrinfo(res);
        }

        // 2. Same call for a domain that is known to resolve, as a control.
        res = nullptr;
        const int gai2 = getaddrinfo("example.com", "80", &hints, &res);
        results_.push_back({"getaddrinfo(example.com)", gai2 == 0 ? "ok" : errno_name(gai2)});
        if (gai2 == 0 && res != nullptr) freeaddrinfo(res);

        // 3. A literal address, which skips DNS entirely.
        res = nullptr;
        addrinfo lit{};
        lit.ai_family = AF_INET;
        lit.ai_socktype = SOCK_STREAM;
        const int gai3 = getaddrinfo("104.21.84.82", "443", &lit, &res);
        results_.push_back({"getaddrinfo(literal ip)", gai3 == 0 ? "ok" : errno_name(gai3)});
        if (gai3 == 0 && res != nullptr) freeaddrinfo(res);

        // 4. HTTPS by name, then HTTPS by literal address.
        results_.push_back({"https by name", https_get("https://nextendo.network/api/health")});
        results_.push_back({"https by ip", https_get("https://104.21.84.82/api/health")});

        rebuild();
        return frame_;
    }

private:
    std::string https_get(const char* url) {
        CURL* curl = curl_easy_init();
        if (curl == nullptr) return "curl init failed";

        std::string body;
        curl_easy_setopt(curl, CURLOPT_URL, url);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, sink_cb);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 8L);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 12L);
        curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);

        const CURLcode rc = curl_easy_perform(curl);
        long http = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http);
        curl_easy_cleanup(curl);

        char buf[96];
        if (rc != CURLE_OK) {
            std::snprintf(buf, sizeof(buf), "%s (%d)", curl_easy_strerror(rc), static_cast<int>(rc));
        } else {
            std::snprintf(buf, sizeof(buf), "HTTP %ld, %zu bytes", http, body.size());
        }
        return buf;
    }

    void rebuild() {
        list_->clear();
        list_->addItem(new tsl::elm::CategoryHeader("results"));
        for (const auto& row : results_) {
            list_->addItem(new tsl::elm::ListItem(row.label, row.value));
        }
    }

    tsl::elm::OverlayFrame* frame_ = nullptr;
    tsl::elm::List* list_ = nullptr;
    std::vector<Row> results_;
};

class NetProbeOverlay : public tsl::Overlay {
public:
    void initServices() override { curl_global_init(CURL_GLOBAL_DEFAULT); }
    void exitServices() override {
        socketExit();
        curl_global_cleanup();
    }

    std::unique_ptr<tsl::Gui> loadInitialGui() override {
        return initially<NetProbeGui>();
    }
};

}  // namespace

int main(int argc, char** argv) {
    return tsl::loop<NetProbeOverlay>(argc, argv);
}
