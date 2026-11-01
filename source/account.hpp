#pragma once
#include "live_model.hpp"

// Included after performOnHandle(): all HTTPS work stays on the existing worker.
static const char* kSessionPath = "/switch/nextendo-status/session.txt";
static const char* kSessionTemp = "/switch/nextendo-status/session.tmp";
static const char* L(const char* zh, const char* en) { return g_useChinese ? zh : en; }
static void wipe(std::string& s) {
    volatile char* p = s.empty() ? nullptr : &s[0];
    for (size_t i = 0; i < s.size(); ++i) p[i] = 0;
    s.clear();
}
enum class AccountJob { None, Login, Refresh, Identity };
struct AccountState {
    std::string token, login, password, message;
    // Display name from /api/me, shown on the account page.
    std::string userName;
    // Set by logoutAccount so the page can say "已退出登录" until the next sign-in.
    bool signedOut = false;
    // Title ids of the game being viewed; only those are kept from the response.
    std::vector<std::string> wanted;
    std::vector<nextendo::LiveGame> games;
    AccountJob job = AccountJob::None;
    // There is no "remember me" control in the UI, so leaving this false meant the
    // token was never written and every boot asked for the account again. The
    // logout button still clears it, so a shared console can be signed out.
    bool busy = false, remember = true;
    unsigned epoch = 0, revision = 1;
    u64 fetchedTick = 0, lastAttemptTick = 0;
};
static AccountState g_account; // protected by g_mutex, including file writes
static std::atomic<bool> g_accountWake{false}, g_liveVisible{false};

static bool persistSession(const std::string& token) {
    FsFileSystem fs{};
    if (R_FAILED(fsOpenSdCardFileSystem(&fs))) return false;
    fsFsDeleteFile(&fs, kSessionTemp);
    if (token.empty()) {
        const Result rc = fsFsDeleteFile(&fs, kSessionPath);
        // A missing file is already logged out; do not re-create it.
        FsFile check{};
        const bool absent = R_FAILED(fsFsOpenFile(&fs, kSessionPath, FsOpenMode_Read, &check));
        if (!absent) fsFileClose(&check);
        fsFsClose(&fs);
        return R_SUCCEEDED(rc) || absent;
    }
    bool ok = false;
    if (R_SUCCEEDED(fsFsCreateFile(&fs, kSessionTemp, token.size(), 0))) {
        FsFile file{};
        if (R_SUCCEEDED(fsFsOpenFile(&fs, kSessionTemp, FsOpenMode_Write, &file))) {
            ok = R_SUCCEEDED(fsFileWrite(&file, 0, token.data(), token.size(), FsWriteOption_Flush));
            fsFileClose(&file);
        }
        if (ok) {
            fsFsDeleteFile(&fs, kSessionPath);
            ok = R_SUCCEEDED(fsFsRenameFile(&fs, kSessionTemp, kSessionPath));
        }
    }
    if (!ok) fsFsDeleteFile(&fs, kSessionTemp);
    fsFsClose(&fs);
    return ok;
}
static void loadSession() {
    FsFileSystem fs{};
    if (R_FAILED(fsOpenSdCardFileSystem(&fs))) return;
    fsFsDeleteFile(&fs, kSessionTemp);
    FsFile file{};
    if (R_SUCCEEDED(fsFsOpenFile(&fs, kSessionPath, FsOpenMode_Read, &file))) {
        s64 size = 0;
        if (R_SUCCEEDED(fsFileGetSize(&file, &size)) && size > 0 && size <= 8192) {
            std::string token(static_cast<size_t>(size), '\0');
            u64 read = 0;
            if (R_SUCCEEDED(fsFileRead(&file, 0, token.data(), token.size(), FsReadOption_None, &read)) &&
                read == token.size() && nextendo::validToken(token)) {
                g_account.token = std::move(token);
                g_account.remember = true;
                // One request at start-up so the account page can name the
                // session instead of showing an empty page.
                g_account.job = AccountJob::Identity;
                g_account.busy = true;
                g_accountWake = true;
            }
            wipe(token);
        }
        fsFileClose(&file);
    }
    fsFsClose(&fs);
}
static void requestLive(const std::vector<std::string>& only = {}) {
    mutexLock(&g_mutex);
    g_account.wanted = only;
    if (!g_account.token.empty() && !g_account.busy) {
        g_account.job = AccountJob::Refresh;
        g_account.busy = true;
        g_account.message.clear();
        ++g_account.revision;
        g_accountWake = true;
    }
    mutexUnlock(&g_mutex);
}
static void logoutAccount() {
    mutexLock(&g_mutex);
    ++g_account.epoch; // invalidates a login/live response already in flight
    wipe(g_account.token);
    g_account.userName.clear();
    g_account.signedOut = true;
    wipe(g_account.password);
    g_account.login.clear();
    g_account.games.clear();
    g_account.fetchedTick = 0;
    g_account.job = AccountJob::None;
    g_account.busy = false;
    g_account.message = persistSession("") ? L("已退出登录", "Signed out")
        : L("已退出；请手动删除 SD 卡 session.txt", "Signed out; delete session.txt on SD");
    ++g_account.revision;
    mutexUnlock(&g_mutex);
    g_accountWake = true;
}
static bool accountEpochIsCurrent(unsigned epoch) {
    mutexLock(&g_mutex);
    const bool current = epoch == g_account.epoch && g_threadRunning.load();
    mutexUnlock(&g_mutex);
    return current;
}
static int accountProgress(void* context, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
    return context && accountEpochIsCurrent(*static_cast<unsigned*>(context)) ? 0 : 1;
}
struct AccountResponse {
    bool transport = false;
    long status = 0;
    std::string body;
    // curl's own message when transport is false; empty when the request went
    // through. Without it a failure only ever read "network request failed".
    std::string error;
};
static AccountResponse accountRequest(CURL* curl, const char* path,
                                     const std::string& token, unsigned epoch,
                                     const std::string* post = nullptr,
                                     long timeoutSeconds = 20L, long connectSeconds = 10L) {
    AccountResponse result;
    if (!accountEpochIsCurrent(epoch)) return result;
    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "X-Nextendo-Client-Id: nxc_hvGUwjpszrDT");
    headers = curl_slist_append(headers, "Accept: application/json");
    if (g_relayRoute.enabled()) headers = curl_slist_append(headers, ("X-Nextendo-Relay-Key: " + g_relayRoute.key).c_str());
    if (!token.empty()) headers = curl_slist_append(headers, ("Authorization: Bearer " + token).c_str());
    if (post) headers = curl_slist_append(headers, "Content-Type: application/json");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, accountProgress);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &epoch);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, post ? post->c_str() : nullptr);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, post ? static_cast<long>(post->size()) : 0L);
    if (!post) curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
    // The shared transport owns framing, time budgets and safe IP retries.
    result.transport = performOnHandle(curl, std::string("https://nextendo.network") + path,
        "nextendo.network", NEXTENDO_FALLBACK_IPS, result.body, result.error, true,
        timeoutSeconds, connectSeconds, post != nullptr);

    // Read the status unconditionally: when the transfer is cut short it still
    // carries the real code, and "HTTP 0" was only ever a missing read.
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &result.status);

    // A complete document beats a transport error caused by the server not
    // closing the connection.
    if (!result.transport && result.status == 200 && nextendo::completeJson(result.body)) {
        result.transport = true;
        result.error.clear();
    }
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, static_cast<struct curl_slist*>(nullptr));
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, static_cast<const char*>(nullptr));
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, static_cast<void*>(nullptr));
    curl_slist_free_all(headers);
    return result;
}
static std::string accountError(const AccountResponse& r, bool login) {
    if (g_relayRoute.enabled() && r.status == 403 && r.body.find("relay_access_denied") != std::string::npos)
        return L("中转密钥不匹配，请检查 network.ini", "Relay key rejected; check network.ini");
    if (!r.transport) {
        // Show curl's reason: "Timeout was reached" and "Failure writing output"
        // are the two that looked identical before.
        std::string detail = L("网络请求失败", "Network request failed");
        if (!r.error.empty()) detail += ": " + r.error;
        return detail;
    }
    if (r.status == 401) return login ? L("账号或密码错误", "Incorrect login or password")
        : L("登录已过期，请重新登录", "Session expired; sign in again");
    if (r.status == 403) return L("服务器拒绝访问，请到官网登录确认", "Access denied; check your account on the website");
    if (r.status == 429) return L("请求过于频繁，请稍后重试", "Too many requests; retry later");
    return L("服务器返回错误", "Server error") + std::string(" (HTTP ") + std::to_string(r.status) + ")";
}
// Writes the raw live response to the SD card so a parser failure can be
// diagnosed from the actual bytes instead of from the message alone.
static void dumpLiveBody(const char* what, const std::string& body) {
    FsFileSystem fs{};
    if (R_FAILED(fsOpenSdCardFileSystem(&fs))) return;

    static const char* path = "/switch/nextendo-status/live-debug.json";
    fsFsDeleteFile(&fs, path);
    if (!body.empty() && R_SUCCEEDED(fsFsCreateFile(&fs, path, body.size(), 0))) {
        FsFile file{};
        if (R_SUCCEEDED(fsFsOpenFile(&fs, path, FsOpenMode_Write, &file))) {
            fsFileWrite(&file, 0, body.data(), body.size(), FsWriteOption_Flush);
            fsFileClose(&file);
        }
    }
    fsFsClose(&fs);
    (void)what;
}

// Reads account.name (falling back to account.username) out of /api/me.
static std::string parseIdentity(const std::string& body) {
    json_error_t err{};
    json_t* root = json_loadb(body.data(), body.size(), 0, &err);
    if (!json_is_object(root)) {
        if (root) json_decref(root);
        return std::string();
    }

    std::string name;
    json_t* account = json_object_get(root, "account");
    if (json_is_object(account)) {
        for (const char* key : {"name", "username"}) {
            json_t* value = json_object_get(account, key);
            if (json_is_string(value) && json_string_length(value) > 0) {
                name.assign(json_string_value(value), json_string_length(value));
                break;
            }
        }
    }

    json_decref(root);
    return name;
}

// Called only by pollThreadFunc. Returns true when a queued action was processed.
static bool serviceAccount() {
    g_accountWake = false;
    mutexLock(&g_mutex);
    const AccountJob job = g_account.job;
    if (job == AccountJob::None) { mutexUnlock(&g_mutex); return false; }
    const unsigned epoch = g_account.epoch;
    g_account.lastAttemptTick = armGetSystemTick();
    std::string token = g_account.token, login = g_account.login;
    std::string password = std::move(g_account.password);
    // Copied under the lock: the UI thread writes this when a page opens.
    const std::vector<std::string> wanted = g_account.wanted;
    g_account.job = AccountJob::None;
    mutexUnlock(&g_mutex);

    CURL* curl = curl_easy_init();
    bool ok = false, expired = false;
    std::string message;
    std::string identity;
    std::vector<nextendo::LiveGame> games;
    if (!curl) message = L("无法创建网络请求", "Could not create request");
    else if (job == AccountJob::Login) {
        json_t* obj = json_pack("{s:s,s:s}", "login", login.c_str(), "password", password.c_str());
        char* dumped = obj ? json_dumps(obj, JSON_COMPACT) : nullptr;
        std::string post = dumped ? dumped : "";
        if (dumped) { volatile char* p = dumped; for (size_t i = 0; i < post.size(); ++i) p[i] = 0; free(dumped); }
        if (obj) json_decref(obj);
        wipe(password);
        AccountResponse r;
        if (!post.empty()) r = accountRequest(curl, "/api/login", "", epoch, &post);
        wipe(post);
        if (r.transport && r.status >= 200 && r.status < 300) {
            json_error_t err{};
            json_t* root = json_loadb(r.body.data(), r.body.size(), JSON_REJECT_DUPLICATES, &err);
            json_t* value = json_object_get(root, "token");
            if (json_is_string(value)) token.assign(json_string_value(value), json_string_length(value));
            ok = nextendo::validToken(token);
            if (root) json_decref(root);
            if (!ok) message = L("登录响应无效，请重试", "Invalid sign-in response; retry");
            // One extra request while the session is fresh, so the account page
            // can name it without a second round trip later.
            if (ok) {
                AccountResponse me = accountRequest(curl, "/api/me", token, epoch, nullptr, 12L, 8L);
                if (me.transport && me.status == 200) identity = parseIdentity(me.body);
            }
        } else message = accountError(r, true);
        wipe(r.body);
    } else if (job == AccountJob::Identity) {
        AccountResponse r = accountRequest(curl, "/api/me", token, epoch, nullptr, 12L, 8L);
        expired = r.transport && r.status == 401;
        if (r.transport && r.status == 200) {
            identity = parseIdentity(r.body);
            ok = !identity.empty();
            if (!ok) message = L("无法读取账号信息", "Could not read the account");
        } else message = accountError(r, false);
    } else {
        const u64 liveStart = armGetSystemTick();
        const auto livePath = nextendo::relayLivePath(g_relayRoute, "/api/status/live", wanted);
        AccountResponse r = accountRequest(curl, livePath.c_str(), token, epoch);
        expired = r.transport && r.status == 401;
        if (r.transport && r.status == 200) {
            ok = nextendo::parseLive(r.body, false, games, &wanted);
            // The raw document is the biggest single allocation here; drop it before
            // anything else is built on top of it.
            std::string().swap(r.body);
            if (!ok) {
                // The parser failed, so keep the bytes for inspection.
                dumpLiveBody("parse", r.body);
                message = L("房间数据格式无效", "Invalid lobby data") +
                          std::string(" (HTTP 200, ") + std::to_string(r.body.size()) + L(" 字节)", " bytes)");
            } else if (games.empty()) {
                // Parsed fine but named no games: that is a protocol mismatch too,
                // not an empty roster, so it is worth keeping the body as well.
                dumpLiveBody("empty", r.body);
                message = L("房间数据为空", "Lobby data empty") +
                          std::string(" (HTTP 200, ") + std::to_string(r.body.size()) + L(" 字节)", " bytes)");
            }
        } else message = accountError(r, false) +
                         std::string(" (HTTP ") + std::to_string(r.status) + ")";
        // These games expose their detailed data on separate official feeds.
        //
        // Both are extras, and both cost another round trip. On a slow link the
        // first request alone can take most of the deadline, so piling three more
        // on top is what made the page look frozen; skip them unless it was quick.
        const bool mainWasQuick = armTicksToNs(armGetSystemTick() - liveStart) < 15'000'000'000ULL;
        if (!expired && mainWasQuick) for (const auto& feed : {std::pair<const char*, const char*>{"/game-stats/kirby-live", "0100a8e016236000"},
                                      {"/game-stats/uno-live", "0100b6e012ebe000"}}) {
            if (!wanted.empty() && std::find(wanted.begin(), wanted.end(), feed.second) == wanted.end()) continue;
            AccountResponse extra = accountRequest(curl, feed.first, token, epoch, nullptr, 8L, 5L);
            std::vector<nextendo::LiveGame> replacement;
            if (extra.transport && extra.status == 200 && nextendo::parseLive(extra.body, true, replacement)) {
                games.erase(std::remove_if(games.begin(), games.end(), [&](const auto& g) {
                    return nextendo::matches(g, {feed.second}); }), games.end());
                games.insert(games.end(), replacement.begin(), replacement.end());
                ok = true;
            }
        }
        if (!expired && mainWasQuick) {
            // The website routes these titles to the service catalog, even when
            // that source is unavailable; do not substitute a fictional roster.
            const std::vector<std::string> catalogTitles = {"0100d870045b6000", "01008d300c50c000", "0100c62011050000",
                "010012f017576000", "0100b3c014bda000", "0100ea6014bb8000", "0100fcf002a58000", "0100ab401f136000",
                "01003fb00c5a8000", "0100227010460000"};
            bool needCatalog = wanted.empty();
            for (const auto& id : wanted)
                if (std::find(catalogTitles.begin(), catalogTitles.end(), id) != catalogTitles.end()) needCatalog = true;
            games.erase(std::remove_if(games.begin(), games.end(), [&](const auto& g) {
                return nextendo::matches(g, catalogTitles); }), games.end());
            AccountResponse extra;
            if (needCatalog) {
                const auto catalogPath = nextendo::relayLivePath(g_relayRoute, "/service-catalog/v1/status/live", wanted);
                extra = accountRequest(curl, catalogPath.c_str(), token, epoch, nullptr, 12L, 6L);
            }
            std::vector<nextendo::LiveGame> catalog;
            if (extra.transport && extra.status == 200 && nextendo::parseLive(extra.body, false, catalog)) {
                const size_t before = games.size();
                for (auto& game : catalog) if (game.titles.size() == 1 && nextendo::matches(game, catalogTitles)) games.push_back(std::move(game));
                if (games.size() > before) ok = true;
            }
        }
    }
    if (curl) curl_easy_cleanup(curl);
    wipe(password);
    mutexLock(&g_mutex);
    const bool current = (epoch == g_account.epoch && g_threadRunning.load());
    if (current) {
        g_account.lastAttemptTick = armGetSystemTick();
        if (!identity.empty()) g_account.userName = identity;
        if (job == AccountJob::Login && ok) {
            g_account.signedOut = false;
            g_account.token = token;
            g_account.games.clear();
            g_account.fetchedTick = 0;
            const bool saved = persistSession(g_account.remember ? token : "");
            message = saved ? L("登录成功", "Signed in") : L("登录成功，但无法保存凭证", "Signed in; could not save session");
        } else if (job == AccountJob::Refresh && ok) {
            g_account.games = std::move(games);
            g_account.fetchedTick = armGetSystemTick();
        } else if (expired) {
            wipe(g_account.token);
            g_account.games.clear();
            g_account.fetchedTick = 0;
            persistSession("");
        }
        g_account.message = message;
    }
    // Cleared outside the epoch test on purpose. A logout while a request was in
    // flight bumped the epoch, and leaving the flag set pinned the page on
    // "refreshing" for the rest of the session.
    g_account.busy = false;
    ++g_account.revision;
    mutexUnlock(&g_mutex);
    wipe(token);
    return true;
}
