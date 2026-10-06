// File-write probe.
//
// The log probe proved this overlay runs but cannot create a file on the SD
// card, so the diagnostic log is unusable. This probe works out WHY, and
// reports on screen instead of in a file - the results must not depend on the
// very mechanism that is failing.
//
// It tries, in order:
//   1. stdio fopen("w") at several paths, to see whether the failure is
//      path-specific (the SD root versus a subdirectory, absolute versus
//      relative);
//   2. the raw fs API (fsOpenSdCardFileSystem + fsFsCreateFile + fsFileWrite),
//      which bypasses devoptab and stdio entirely.
//
// Every attempt records its Result code, rendered as hex so a failure can be
// looked up in Atmosphère's error tables.

#define TESLA_INIT_IMPL
#include <tesla.hpp>

#include <switch.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
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

// stdio: create a file and write a short string, then read it back.
std::string stdio_write(const char* path) {
    FILE* f = std::fopen(path, "w");
    if (f == nullptr) {
        // errno distinguishes "no such device" from "permission denied".
        char buf[48];
        std::snprintf(buf, sizeof(buf), "fopen null, errno %d", errno);
        return buf;
    }
    std::fputs("hello\n", f);
    std::fclose(f);

    FILE* r = std::fopen(path, "r");
    if (r == nullptr) return "wrote, reopen null";
    char back[16] = {0};
    const std::size_t got = std::fread(back, 1, sizeof(back) - 1, r);
    std::fclose(r);

    char buf[48];
    std::snprintf(buf, sizeof(buf), "ok, read back %zu bytes", got);
    return buf;
}

std::vector<Row> run_probe() {
    std::vector<Row> rows;

    // 1. stdio at several paths.
    rows.push_back({"fopen sdmc:/nextendo.log", stdio_write("sdmc:/nextendo.log")});
    rows.push_back({"fopen sdmc:/switch/nextendo.log", stdio_write("sdmc:/switch/nextendo.log")});
    rows.push_back({"fopen /nextendo.log", stdio_write("/nextendo.log")});
    rows.push_back({"fopen nextendo.log", stdio_write("nextendo.log")});

    // 2. Raw fs, bypassing devoptab and stdio entirely.
    FsFileSystem sdmc;
    Result rc = fsOpenSdCardFileSystem(&sdmc);
    rows.push_back({"fsOpenSdCardFileSystem", hex(rc)});

    if (R_SUCCEEDED(rc)) {
        Result create_rc = fsFsCreateFile(&sdmc, "/nextendo-raw.txt", 6, 0);
        rows.push_back({"fsFsCreateFile", hex(create_rc)});

        FsFile file;
        Result open_rc = fsFsOpenFile(&sdmc, "/nextendo-raw.txt", FsOpenMode_Write, &file);
        rows.push_back({"fsFsOpenFile(write)", hex(open_rc)});

        if (R_SUCCEEDED(open_rc)) {
            const char* payload = "hello\n";
            Result wr = fsFileWrite(&file, 0, payload, std::strlen(payload),
                                    FsWriteOption_None);
            rows.push_back({"fsFileWrite", hex(wr)});
            fsFileClose(&file);
        }

        if (R_SUCCEEDED(fsFsOpenFile(&sdmc, "/nextendo-raw.txt", FsOpenMode_Read, &file))) {
            char buf[16] = {0};
            u64 got = 0;
            Result rd = fsFileRead(&file, 0, buf, 6, FsReadOption_None, &got);
            char line[48];
            std::snprintf(line, sizeof(line), "%s, %llu bytes",
                          hex(rd), static_cast<unsigned long long>(got));
            rows.push_back({"fsFileRead", line});
            fsFileClose(&file);
        }
        fsFsClose(&sdmc);
    }

    return rows;
}

class ProbeGui : public tsl::Gui {
public:
    virtual tsl::elm::Element* createUI() override {
        auto* frame = new tsl::elm::OverlayFrame("Write Probe", "file writes");
        auto* list = new tsl::elm::List();

        list->addItem(new tsl::elm::CategoryHeader("results"));
        for (const auto& row : run_probe()) {
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
