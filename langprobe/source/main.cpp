// Language diagnostic overlay.
//
// The main overlay's interface language is chosen from the console's system
// language, and that check has been falling back to English. Rather than guess
// again, this reports the exact Result code of every call involved, on screen.
//
// Put it in sdmc:/switch/.overlays/ alongside the real overlay and read the
// values off the panel.

#define TESLA_INIT_IMPL
#include <tesla.hpp>
#include <switch.h>

#include <cstdio>
#include <string>
#include <vector>

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
    std::snprintf(buf, sizeof(buf), "0x%llX", static_cast<unsigned long long>(languageCode));
    return buf;
}

std::string langName(SetLanguage language) {
    switch (language) {
        case SetLanguage_ZHCN:    return "ZHCN";
        case SetLanguage_ZHHANS:  return "ZHHANS";
        case SetLanguage_ZHTW:    return "ZHTW";
        case SetLanguage_ZHHANT:  return "ZHHANT";
        case SetLanguage_ENUS:    return "ENUS";
        case SetLanguage_ENGB:    return "ENGB";
        case SetLanguage_JA:      return "JA";
        case SetLanguage_KO:      return "KO";
        case SetLanguage_FR:      return "FR";
        case SetLanguage_DE:      return "DE";
        case SetLanguage_ES:      return "ES";
        default:                  return "other";
    }
}

std::vector<Row> probe() {
    std::vector<Row> rows;

    // 1. applet, which libnx initialises itself. This is the preferred source.
    u64 appletCode = 0;
    const Result appletRc = appletGetDesiredLanguage(&appletCode);
    rows.push_back({"appletGetDesiredLanguage", hex(appletRc)});
    if (R_SUCCEEDED(appletRc)) {
        rows.push_back({"  code", code(appletCode)});
        SetLanguage language{};
        const Result made = setMakeLanguage(appletCode, &language);
        rows.push_back({"  setMakeLanguage", hex(made)});
        rows.push_back({"  language", made == 0 ? langName(language) : "-"});
    }

    // 2. set:, opened here because nothing else opens it for an overlay.
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
            rows.push_back({"  language", made == 0 ? langName(language) : "-"});
        }
        setExit();
    }

    g_probeChinese = chineseBySet || chineseByApplet;

    // 3. set:sys. It has no language getter at all, which is why using it for the
    //    check was wrong; this just confirms the service opens.
    const Result sysRc = setsysInitialize();
    rows.push_back({"setsysInitialize", hex(sysRc)});
    if (R_SUCCEEDED(sysRc)) setsysExit();

    return rows;
}

// Required by tesla.hpp, which needs a definition to link against. The probe
// determines the language below and remembers it here.
static bool g_probeChinese = false;

extern "C" bool nextendoIsChinese() {
    return g_probeChinese;
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

int main(int argc, char** argv) {
    return tsl::loop<ProbeOverlay>(argc, argv);
}
