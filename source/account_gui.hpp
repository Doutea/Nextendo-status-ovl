#pragma once
#include <memory>

struct LoginDraft {
    std::string login, password;
    bool remember = false;
    ~LoginDraft() { wipe(password); }
};

// Overlay processes cannot reliably launch the system keyboard applet.
// This keyboard stays inside Tesla and needs only the controller.
class AccountKeyboard : public tsl::Gui {
    std::shared_ptr<LoginDraft> m_draft;
    std::string m_text;
    bool m_secret;
    size_t m_cursor = 0;
    const std::string m_chars = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789@._-+ !#$%&'()*,:;<=>?[]^`{|}~\"\\/";
public:
    AccountKeyboard(std::shared_ptr<LoginDraft> draft, bool secret)
        : m_draft(std::move(draft)), m_text(secret ? m_draft->password : m_draft->login), m_secret(secret) {}
    ~AccountKeyboard() override { wipe(m_text); }
    tsl::elm::Element* createUI() override {
        auto* frame = new tsl::elm::OverlayFrame(L("输入账号", "Enter login"),
            L("方向键选字 · A 输入 · X 删除", "D-pad select · A type · X delete"));
        if (m_secret) frame->setTitle(L("输入密码", "Enter password"));
        auto* list = new tsl::elm::List();
        list->addItem(new tsl::elm::CustomDrawer([this](tsl::gfx::Renderer* r, s32 x, s32 y, s32 w, s32) {
            const auto color = tsl::style::color::ColorText;
            std::string display = m_secret ? std::string(m_text.size(), '*') : m_text;
            if (display.size() > 26) display = "..." + display.substr(display.size() - 26);
            r->drawString(display.c_str(), false, x + 8, y + 32, 20, color, w - 16);
            for (size_t i = 0; i < m_chars.size(); ++i) {
                char key[4] = { m_chars[i], 0, 0, 0 };
                if (m_chars[i] == ' ') { key[0] = '_'; }
                r->drawString(key, false, x + 12 + (i % 10) * 30, y + 85 + (i / 10) * 34,
                    22, i == m_cursor ? tsl::style::color::ColorValueBlue : color);
            }
            r->drawString(L("+ 完成   B 取消   Y 清空", "+ Save   B Cancel   Y Clear"), false, x + 8, y + 465, 17, color, w - 16);
            r->drawString(L("空格键显示为下划线", "Space key is shown as _"), false, x + 8, y + 495, 15, color, w - 16);
        }), 525);
        frame->setContent(list);
        return frame;
    }
    bool handleInput(u64 down, u64, const HidTouchState&, HidAnalogStickState, HidAnalogStickState) override {
        if (down & HidNpadButton_Plus) {
            auto& target = m_secret ? m_draft->password : m_draft->login;
            wipe(target);
            target = m_text;
            tsl::goBack();
            return true;
        }
        if (down & HidNpadButton_B) return false;
        if (down & HidNpadButton_AnyLeft) m_cursor = (m_cursor + m_chars.size() - 1) % m_chars.size();
        if (down & HidNpadButton_AnyRight) m_cursor = (m_cursor + 1) % m_chars.size();
        if (down & HidNpadButton_AnyUp) m_cursor = m_cursor >= 10 ? m_cursor - 10 : m_cursor;
        if (down & HidNpadButton_AnyDown) m_cursor = std::min(m_cursor + 10, m_chars.size() - 1);
        if ((down & HidNpadButton_A) && m_text.size() < (m_secret ? 256u : 254u)) m_text += m_chars[m_cursor];
        if ((down & HidNpadButton_X) && !m_text.empty()) m_text.pop_back();
        if (down & HidNpadButton_Y) wipe(m_text);
        return true;
    }
};

class AccountGui : public tsl::Gui {
    tsl::elm::OverlayFrame* m_frame = nullptr;
    std::shared_ptr<LoginDraft> m_draft = std::make_shared<LoginDraft>();
    unsigned m_revision = 0;
    std::string m_loginShown;
    size_t m_passwordSize = 0;
    bool m_redraw = true;
public:
    tsl::elm::Element* createUI() override {
        m_frame = new tsl::elm::OverlayFrame(L("Nextendo 账号", "Nextendo account"), "");
        redraw();
        return m_frame;
    }
    void update() override {
        mutexLock(&g_mutex);
        const unsigned revision = g_account.revision;
        mutexUnlock(&g_mutex);
        if (m_redraw || revision != m_revision || m_loginShown != m_draft->login || m_passwordSize != m_draft->password.size()) redraw();
    }
private:
    void redraw() {
        mutexLock(&g_mutex);
        const bool loggedIn = !g_account.token.empty(), busy = g_account.busy, remembered = g_account.remember;
        const std::string message = g_account.message;
        m_revision = g_account.revision;
        mutexUnlock(&g_mutex);
        m_redraw = false;
        m_loginShown = m_draft->login;
        m_passwordSize = m_draft->password.size();
        this->removeFocus();
        auto* list = new tsl::elm::List();
        list->addItem(new tsl::elm::CategoryHeader(loggedIn ? L("已登录", "Signed in") : L("登录账号", "Sign in")));
        if (!message.empty()) list->addItem(new tsl::elm::ListItem(message));
        if (busy) list->addItem(new tsl::elm::ListItem(L("正在连接…", "Connecting…")));
        if (!loggedIn) {
            auto* login = new tsl::elm::ListItem(L("邮箱或用户名", "Email or username"), m_draft->login);
            login->setClickListener([this, busy](u64 keys) {
                if (busy || !(keys & HidNpadButton_A)) return false;
                tsl::changeTo<AccountKeyboard>(m_draft, false); return true;
            });
            list->addItem(login);
            auto* pass = new tsl::elm::ListItem(L("密码", "Password"), m_draft->password.empty() ? "" : "********");
            pass->setClickListener([this, busy](u64 keys) {
                if (busy || !(keys & HidNpadButton_A)) return false;
                tsl::changeTo<AccountKeyboard>(m_draft, true); return true;
            });
            list->addItem(pass);
            auto* remember = new tsl::elm::ListItem(L("记住登录", "Remember session"), m_draft->remember ? L("开", "On") : L("关", "Off"));
            remember->setClickListener([this, busy](u64 keys) {
                if (busy || !(keys & HidNpadButton_A)) return false;
                m_draft->remember = !m_draft->remember; m_redraw = true; return true;
            });
            list->addItem(remember);
            if (m_draft->remember) list->addItem(new tsl::elm::ListItem(L("登录凭证将保存到 SD 卡", "Session token will be saved to SD")));
            auto* submit = new tsl::elm::ListItem(L("登录", "Sign in"));
            submit->setClickListener([this](u64 keys) {
                if (!(keys & HidNpadButton_A)) return false;
                mutexLock(&g_mutex);
                if (!g_account.busy) {
                    if (!g_threadRunning.load()) {
                        g_account.message = L("后台网络服务未启动，请重新打开插件", "Network worker unavailable; reopen overlay");
                    } else if (m_draft->login.empty() || m_draft->password.empty()) {
                        g_account.message = L("请先输入账号和密码", "Enter login and password first");
                    } else {
                        ++g_account.epoch;
                        g_account.login = m_draft->login;
                        g_account.password = std::move(m_draft->password);
                        g_account.remember = m_draft->remember;
                        g_account.message.clear();
                        g_account.busy = true;
                        g_account.job = AccountJob::Login;
                        g_accountWake = true;
                    }
                    ++g_account.revision;
                }
                mutexUnlock(&g_mutex);
                return true;
            });
            list->addItem(submit);
        } else {
            list->addItem(new tsl::elm::ListItem(remembered ? L("登录凭证保存在 SD 卡", "Session saved on SD") : L("仅本次运行保持登录", "Session lasts until overlay exits")));
            auto* logout = new tsl::elm::ListItem(L("退出登录", "Sign out"));
            logout->setClickListener([this](u64 keys) {
                if (!(keys & HidNpadButton_A)) return false;
                logoutAccount(); wipe(m_draft->password); return true;
            });
            list->addItem(logout);
        }
        m_frame->setContent(list);
    }
};
static void addAccountEntry(tsl::elm::List* list) {
    auto* account = new tsl::elm::ListItem(L("Nextendo 账号", "Nextendo account"), L("A 登录 / 管理", "A Sign in / manage"));
    account->setClickListener([](u64 keys) {
        if (!(keys & HidNpadButton_A)) return false;
        tsl::changeTo<AccountGui>(); return true;
    });
    list->addItem(account);
}

class LiveGui : public tsl::Gui {
    std::string m_name;
    std::vector<std::string> m_ids;
    tsl::elm::OverlayFrame* m_frame = nullptr;
    unsigned m_revision = 0;
    int m_room = -1;
    bool m_redraw = true;
    u64 m_tick = 0;
public:
    LiveGui(std::string name, std::vector<std::string> ids) : m_name(std::move(name)), m_ids(std::move(ids)) {
        g_liveVisible = true;
        requestLive();
    }
    ~LiveGui() override { g_liveVisible = false; }
    tsl::elm::Element* createUI() override {
        m_frame = new tsl::elm::OverlayFrame(m_name, L("A 选择房间 · X 刷新", "A Choose lobby · X Refresh"));
        redraw(); return m_frame;
    }
    void update() override {
        mutexLock(&g_mutex);
        const unsigned revision = g_account.revision;
        mutexUnlock(&g_mutex);
        if (m_redraw || revision != m_revision) redraw();
    }
    bool handleInput(u64 keys, u64, const HidTouchState&, HidAnalogStickState, HidAnalogStickState) override {
        if (keys & HidNpadButton_X) { requestLive(); return true; }
        if ((keys & HidNpadButton_B) && m_room >= 0) { m_room = -1; m_redraw = true; return true; }
        return false;
    }
private:
    static void addPlayers(tsl::elm::List* list, const std::vector<nextendo::Player>& players) {
        if (players.empty()) list->addItem(new tsl::elm::ListItem(L("服务器未返回玩家名单", "No player list returned")));
        for (const auto& player : players) {
            std::string tag = player.you ? L("自己", "You") : player.friendPlayer ? L("好友", "Friend") : "";
            list->addItem(new tsl::elm::ListItem(player.name.empty() ? L("未命名玩家", "Unnamed player") : player.name, tag));
            if (!player.activity.empty()) list->addItem(new tsl::elm::ListItem(player.activity));
        }
    }
    void redraw() {
        nextendo::LiveGame game;
        bool found = false;
        mutexLock(&g_mutex);
        const bool loggedIn = !g_account.token.empty(), busy = g_account.busy;
        const std::string error = g_account.message;
        if (m_tick != g_account.fetchedTick) m_room = -1; // room indexes belong to one snapshot only
        m_tick = g_account.fetchedTick;
        m_revision = g_account.revision;
        for (const auto& g : g_account.games) if (nextendo::matches(g, m_ids)) { game = g; found = true; break; }
        mutexUnlock(&g_mutex);
        this->removeFocus();
        m_redraw = false;
        auto* list = new tsl::elm::List();
        if (!loggedIn) {
            list->addItem(new tsl::elm::CategoryHeader(L("登录后查看房间和玩家", "Sign in to view lobbies and players")));
            if (!error.empty()) list->addItem(new tsl::elm::ListItem(error));
            addAccountEntry(list);
        } else {
            if (busy) list->addItem(new tsl::elm::ListItem(L("刷新房间中…", "Refreshing lobbies…")));
            if (!error.empty()) list->addItem(new tsl::elm::ListItem(error));
            if (m_tick) {
                const u64 age = armTicksToNs(armGetSystemTick() - m_tick) / 1000000000ULL;
                list->addItem(new tsl::elm::ListItem(L("数据距本次刷新", "Snapshot age at refresh"), std::to_string(age) + L(" 秒", " s")));
            }
            if (!found && !busy) list->addItem(new tsl::elm::ListItem(L("该游戏当前没有公开的房间或玩家详情", "No published lobby or player details for this game")));
            if (found && m_room >= 0 && static_cast<size_t>(m_room) < game.lobbies.size()) {
                const auto& room = game.lobbies[m_room];
                list->addItem(new tsl::elm::CategoryHeader(L("房间 ", "Lobby ") + std::to_string(m_room + 1)));
                if (!room.mode.empty()) list->addItem(new tsl::elm::ListItem(room.mode));
                list->addItem(new tsl::elm::ListItem(room.state, std::to_string(room.count) + (room.capacity ? "/" + std::to_string(room.capacity) : "")));
                addPlayers(list, room.players);
            } else if (found) {
                if (!game.roomsAvailable) list->addItem(new tsl::elm::ListItem(L("玩家在线；房间详情暂不可用", "Players online; room details unavailable")));
                list->addItem(new tsl::elm::CategoryHeader(L("房间列表", "Lobbies")));
                if (game.lobbies.empty()) list->addItem(new tsl::elm::ListItem(L("当前没有公开房间", "No published lobbies")));
                for (size_t i = 0; i < game.lobbies.size(); ++i) {
                    const auto& room = game.lobbies[i];
                    auto* row = new tsl::elm::ListItem(L("房间 ", "Lobby ") + std::to_string(i + 1) + " " + room.mode,
                        std::to_string(room.count) + (room.capacity ? "/" + std::to_string(room.capacity) : ""));
                    row->setClickListener([this, i](u64 keys) {
                        if (!(keys & HidNpadButton_A)) return false;
                        m_room = static_cast<int>(i); m_redraw = true; return true;
                    });
                    list->addItem(row);
                }
                list->addItem(new tsl::elm::CategoryHeader(L("全部在线玩家", "All online players")));
                addPlayers(list, game.players);
            }
        }
        m_frame->setContent(list);
    }
};
