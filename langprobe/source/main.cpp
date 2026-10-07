// Language diagnostic overlay.
//
// The main overlay picks its interface language from the console's system
// language, and that check kept falling back to English. Rather than guess
// again, this reports the exact Result code of every call involved, on screen.
//
// Copy it to sdmc:/switch/.overlays/ next to the real overlay and read the
// values off the panel.

#define TESLA_INIT_IMPL
#include <tesla.hpp>
#include <switch.h>

#include <cstdio>
#include <string>
#include <vector>

// Visible to the extern "C" definition at the bottom of this file.
bool g_chinese = false;

namespace {

struct Row {
    std::string label;
    std::string value;
};

std::string hex(Result rc) {
    if (R_SUCCEEDED(rc)) return "ok";
    char buf[24];
    std::snprintf(buf, sizeof(buf), "0x%08X", static_cast<unsigned>(rc));
    return buf;
}

std::string code(u64 languageCode) {
    char buf[24];
    std::snprintf(buf, sizeof(buf), "0x%llX",
                  static_cast<unsigned long long>(languageCode));
    return buf;
}

std::string langName(SetLanguage language) {
    switch (language) {
        case SetLanguage_ZHCN:   return "ZHCN";
        case SetLanguage_ZHHANS: return "ZHHANS";
        case SetLanguage_ZHTW:   return "ZHTW";
        case SetLanguage_ZHHANT: return "ZHHANT";
        case SetLanguage_ENUS:   return "ENUS";
        case SetLanguage_ENGB:   return "ENGB";
        case SetLanguage_JA:     return "JA";
        case SetLanguage_KO:     return "KO";
        case SetLanguage_FR:     return "FR";
        case SetLanguage_DE:     return "DE";
        case SetLanguage_ES:     return "ES";
        default:                 return "other";
    }
}

bool isChinese(SetLanguage language) {
    switch (language) {
        case SetLanguage_ZHCN:
        case SetLanguage_ZHHANS:
        case SetLanguage_ZHTW:
        case SetLanguage_ZHHANT:
            return true;
        default:
            return false;
    }
}

std::vector<Row> probe() {
    std::vector<Row> rows;
    bool chinese = false;

    // 1. applet. libnx initialises this service itself, and its own source notes
    //    this call is preferred over setGetLanguageCode.
    u64 appletCode = 0;
    const Result appletRc = appletGetDesiredLanguage(&appletCode);
    rows.push_back({"appletGetDesiredLanguage", hex(appletRc)});
    if (R_SUCCEEDED(appletRc)) {
        rows.push_back({"  code", code(appletCode)});
        SetLanguage language{};
        const Result made = setMakeLanguage(appletCode, &language);
        rows.push_back({"  setMakeLanguage", hex(made)});
        if (R_SUCCEEDED(made)) {
            rows.push_back({"  language", langName(language)});
            chinese = isChinese(language);
        }
    }

    // 2. set:. Nothing else opens it for an overlay, so it is opened here.
    const Result openRc = setInitialize();
    rows.push_back({"setInitialize", hex(openRc)});
    if (R_SUCCEEDED(openRc)) {
        u64 setCode = 0;
        const Result setRc = setGetSystemLanguage(&setCode);
        rows.push_back({"setGetSystemLanguage", hex(setRc)});
        if (R_SUCCEEDED(setRc)) {
            rows.push_back({"  code", code(setCode)});
            SetLanguage language{};
            const Result made = setMakeLanguage(setCode, &language);
            rows.push_back({"  setMakeLanguage", hex(made)});
            if (R_SUCCEEDED(made)) {
                rows.push_back({"  language", langName(language)});
                chinese = chinese || isChinese(language);
            }
        }
        setExit();
    }

    // 3. set:sys. It has no language getter at all, which is why using it for the
    //    check was wrong; this only confirms the service opens.
    const Result sysRc = setsysInitialize();
    rows.push_back({"setsysInitialize", hex(sysRc)});
    if (R_SUCCEEDED(sysRc)) setsysExit();

    g_chinese = chinese;
    rows.push_back({"VERDICT", chinese ? "Chinese" : "NOT Chinese"});
    return rows;
}

class ProbeGui : public tsl::Gui {
public:
    virtual tsl::elm::Element* createUI() override {
        auto* frame = new tsl::elm::OverlayFrame("Language Probe", "read these");
        auto* list = new tsl::elm::List();

        list->addItem(new tsl::elm::CategoryHeader("language API results"));
        for (const auto& row : probe()) {
            list->addItem(new tsl::elm::ListItem(row.label, row.value));
        }

        frame->setContent(list);
        return frame;
    }
};

class ProbeOverlay : public tsl::Overlay {
public:
    virtual void initServices() override {}
    virtual void exitServices() override {}

    virtual std::unique_ptr<tsl::Gui> loadInitialGui() override {
        return initially<ProbeGui>();
    }
};

}  // namespace

// tesla.hpp declares this and uses it for the footer labels, so every overlay
// must provide a definition.
extern "C" bool nextendoIsChinese() {
    return g_chinese;
}

int main(int argc, char** argv) {
    return tsl::loop<ProbeOverlay>(argc, argv);
}
