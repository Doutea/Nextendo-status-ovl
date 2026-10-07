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

## Licence

This project's own code is MIT — see [LICENSE](LICENSE).

`libs/libtesla` is [WerWolv/libtesla](https://github.com/WerWolv/libtesla),
vendored unmodified, and is **GPL-2.0** — see
[libs/libtesla/LICENSE](libs/libtesla/LICENSE). Its terms apply to those files.

