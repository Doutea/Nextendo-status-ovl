# Nextendo Status Overlay

A Nintendo Switch overlay that shows how many players are online on
[Nextendo Network](https://nextendo.network/status.html), straight from the
console's home screen — no need to open a browser or put the console down.

<p align="center">
  <b>当前在线 47</b> · 游戏列表 · 15 秒自动刷新 · 按 A 手动刷新
</p>

## What it does

- **Total players online**, taken from the same API the website's status page uses.
- **A per-game breakdown**, busiest first, with the names shown in Chinese.
- **Refreshes itself** every 15 seconds while open, and immediately when you
  select **当前在线** and press **A**.
- **Opens instantly.** The panel is drawn before any network request runs, so it
  appears at once and fills in the numbers a moment later.
- **Never shows you a timeout.** A failed refresh keeps the numbers already on
  screen; the subtitle tells you when they were taken.

## Installation

1. Make sure [nx-ovlloader](https://github.com/ppkantorski/nx-ovlloader) (part of
   Ultrahand Overlay) is installed. Any Tesla-format overlay loader works.
2. Copy `nextendo-status.ovl` to your SD card:

   ```
   sdmc:/switch/.overlays/nextendo-status.ovl
   ```

3. Open the overlay menu with `L + Down + R-Stick` (the default Ultrahand combo)
   and pick **Nextendo Status**.

## Controls

| Button | Action |
|--------|--------|
| `A` | On **当前在线**: refresh now |
| `B` | Close the overlay |
| `D-Pad` / `Stick` | Move the selection |

## What you see

```
Nextendo 网络
更新于 03:12:45          <- when the numbers were fetched
─────────────────────────
当前在线           47     <- select and press A to refresh
─────────────────────────
游戏 (12)                 <- number of titles currently being played
  任天堂全明星大乱斗 特别版      30
  马力欧赛车 8 豪华版          19
  超级马力欧制造 2             5
  我的世界：地牢 II            4
  古惑狼赛车宝贝车             4
  ...
```

Games with nobody playing are left out, so the list only shows what is active.

## Notes

- **Chinese only.** The labels are Chinese, and game names use Nintendo's own
  Chinese titles where one exists. An unknown title falls back to the name the
  API provides, so a newly released game still appears.
- The counts come from `GET https://nextendo.network/api/online-counts`. The total
  is the sum of `jeux[].joueurs`, which is what the website displays. It is
  deliberately *not* the sum of `counts`: a title released in several regions
  repeats its player count once per regional title id, which would double-count.
- Hostnames are resolved on the console, with hard-coded addresses as a fallback
  if DNS fails.

## Building

Needs [devkitPro](https://devkitpro.org/wiki/Getting_Started) with the `switch-dev`
and `switch-portlibs` packages.

```sh
make
```

That produces `Nextendo-Status-Overlay.ovl`. Release builds are made by GitHub
Actions; pushing a tag such as `v1.0.4` publishes a release with the `.ovl`
attached.

## Credits

Based on [Chasetodie/Nextendo-Status-Overlay](https://github.com/Chasetodie/Nextendo-Status-Overlay).
Built with [libtesla](https://github.com/WerWolv/libtesla) (vendored under
`libs/libtesla`).

Changes made here:

| Version | Change |
|---------|--------|
| 1.0.1 | Polling-thread stack raised from 16 KB to 256 KB. The thread performs DNS, TLS and a libcurl transfer, and 16 KB overflowed — which is what made the overlay crash on exit. |
| 1.0.2 | The panel is drawn before any network request, so it opens immediately. A refresh control was added. |
| 1.0.3 | The refresh row and the count row were merged into one. The redundant duplicate heading was removed. |
| 1.0.4 | Chinese labels throughout. Footer hints translated. Failed refreshes retry, and keep the previous numbers instead of reporting a timeout. |
| 1.0.5 | The panel is laid out as a **当前状态** heading with **在线人数** and **游戏数量** rows under it, followed by a **游戏列表** section. Fixed the update time: it was derived from the power-on tick and so had no relation to the console clock; it is now the console's local time at the moment of the fetch. |
| 1.0.6 | Title changed to **Nextendo 在线状态**. |
| 1.0.7 | **Follows the console's system language.** Chinese labels and Chinese game names on a Chinese console; English labels and the API's own game names everywhere else. A non-Chinese console has no Chinese font loaded, so Chinese labels there would have been unreadable or missing. |
| 1.0.8 | The language check now opens `set:sys` before reading the system language. libnx closes that service during startup, so the check had been failing and falling back to English on Chinese consoles. Numeric values are drawn in `#00AAFF`, or white when the value is zero. |
| 1.0.9 | The language check now uses `appletGetDesiredLanguage()`, falling back to `set:`. It had been opening `set:sys`, but `setGetSystemLanguage()` talks to the `set:` service, so the call failed and the interface stayed English on Chinese consoles. Numeric values are `#00DDFF`, or white when zero. |
| 1.1.0 | Requests connect straight to a known address instead of resolving the hostname first, and the address that worked is reused, so the counts no longer wait on DNS. The unused `games.json` fetch was dropped, and the retry loop now abandons its remaining attempts as soon as the overlay closes. |
| 1.1.1 | Reverted to letting curl resolve the hostname. |
| 1.1.2 | Resolved the host with libnx's `getaddrinfo()` and pinned that address for curl, because curl's own resolver fails in this process. |
| 1.1.3 | Pressing A on **在线人数** shows **刷新中…** in that row immediately, and the **游戏在线** row was added. |
| 1.1.4 | **游戏在线** row removed, and the game list no longer repeats the "waiting for data" text. |
| 1.1.5 | Diagnostic build that logged fetch timings to `sdmc:/switch/nextendo-status/timing.log`. |
| 1.1.6 | **The last good result is cached to the SD card and read back during initialisation**, so the panel opens with numbers already on it. Measured on a console where one request takes 4-20s and about half time out, this removed a 60-second wait on open. The retry loop also abandons its remaining attempts as soon as the overlay closes, which cut the delay after pressing A from ~14s to under half a second. |
| 1.2.0 | Diagnostic logging removed; per-attempt timeout reduced to 12s. |

## Licence

This project's own code is MIT — see [LICENSE](LICENSE).

`libs/libtesla` is [WerWolv/libtesla](https://github.com/WerWolv/libtesla),
vendored unmodified, and is **GPL-2.0** — see
[libs/libtesla/LICENSE](libs/libtesla/LICENSE). Its terms apply to those files.

