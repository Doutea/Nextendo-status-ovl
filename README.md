# nextendo-ovl

在 Switch 的大气层（Atmosphère）环境下，通过 Tesla 菜单唤出的 overlay 插件，用来查看 **Nextendo Network 的在线人数**（[nextendo.network](https://nextendo.network/status.html)）。

按 `L + ↓ + R` 唤出后，它只发一次请求，显示总在线人数、各游戏人数，以及 Nextendo API 的健康状态。

---

## 数据来源

```
GET https://nextendo.network/api/online-counts
```

公开接口，**不需要任何鉴权**（网站自己的前端就是直接调它）。

响应结构：

```json
{
  "counts": { "0100c2500fc20000": 13, ... },          // titleID -> 人数
  "jeux":   [ { "nom": "Splatoon 3", "joueurs": 13, "titres": ["0100c2500fc20000"] }, ... ],
  "noms":   { "0100c2500fc20000": "Splatoon 3", ... }
}
```

**总数必须用 `sum(jeux[].joueurs)`。** 不能用 `sum(counts)`：同一款游戏有多个地区 titleID 时会重复计数。实测同一时刻 `sum(jeux)=47`，而 `sum(counts)=49`（宝可梦 朱/紫 各占一个 titleID 但共享同一个数字）。这一点由 `tests/test_json.cpp` 中的断言锁定，防止以后被人「优化」错。

服务端有 5 秒缓存，网站状态页每 15 秒轮询一次。

---

## 安装

1. 确保已装好 **nx-ovlloader**（overlay 的加载器 sysmodule，非大气层自带）和 Ultrahand / Tesla 菜单。
2. 把 `nextendo-ovl.ovl` 复制到 SD 卡：
   ```
   sdmc:/switch/.overlays/nextendo-ovl.ovl
   ```
3. 在游戏里按唤出组合键（Ultrahand 默认 `ZL + ZR + ↓`，Tesla 默认 `L + ↓ + R`）。

`nextendo-ovl-sd.zip` 里已经放好了 `switch/.overlays/` 目录结构，直接解压到 SD 卡根目录即可。

> **为什么必须带 NACP（重要，别再删掉）**：Ultrahand 的 `getOverlayInfo()` 会读 NRO 主体之后的资源头，并**从 NACP 里取 overlay 的显示名和版本**。如果缺了 NACP，它返回 `ResultParseError`，列表里那个 `if (result != ResultSuccess) continue;` 就会**直接跳过整个文件——条目根本不会生成**。所以 Makefile 里不能设 `NO_NACP`，而且 `.ovl` 规则必须依赖 `.nacp`（否则 `nacptool` 不会被触发，`elf2nro` 会报 `Failed to open input nacp!`）。参考对照：能正常显示的 FPSLocker 尾部有 16,440 字节资源数据，缺 NACP 时尾部为 0。

> **如果在 Ultrahand 菜单里看不到这一项**：先确认文件大小是 **1,101,884 字节**（旧版是 1,085,444，那版缺 NACP 一定不显示）。若仍有问题，进入 `PLUS` → 设置 → **Miscellaneous** → 确认 **hide unsupported overlays** 处于**关闭**状态（该项默认关闭，打开反而会隐藏更多）。

### 操作

| 按键 | 作用 |
|---|---|
| `X` | 重新请求一次 |
| `B` | 返回 / 关闭 |

---

## 编译

`.ovl` 本质上就是一个 libnx 的 NRO（由 `elf2nro` 生成，只改了扩展名），没有大气层专属的编译步骤。

### 本地（devkitPro MSYS2 终端）

```bash
pacman -S --needed switch-dev switch-curl switch-zlib
make
```

产物：`nextendo-ovl.ovl`。打包成 SD 卡结构用 `make dist`。

### 云端（GitHub Actions）

推代码即自动编译，产物在 Actions 的 **Artifacts** 里：

- `nextendo-ovl` —— 单独的 `nextendo-ovl.ovl`
- `nextendo-ovl-sd` —— `nextendo-ovl-sd.zip`，已是可直接拷到 SD 卡根目录的 `switch/.overlays/` 结构

工作流分两个 job：

- `test`：在普通 Linux 上用 ASan/UBSan 跑 JSON 解析器单测 —— **这是真正的质量闸门**；
- `build`：在 `devkitpro/devkita64` 容器里编译出 `.ovl`，校验头部魔数（注意 `NRO0` 在**偏移 16**，偏移 0 是 AArch64 跳转指令），再打包出 SD 卡结构。

> 踩过的坑记录在案，方便以后排查：构建脚本里不要写 `gcc --version | head -n 1`（配合 `set -o pipefail` 会因 SIGPIPE 报 exit 141）；容器内必须先 `source switchvars.sh` 并把 `$DEVKITPRO/devkitA64/bin` 加进 PATH，否则找不到交叉编译器；`-fno-rtti` **不能**开（libtesla 自己用了 `dynamic_cast`）。

---

## 关键技术点（都是踩过的坑）

**HTTPS 不需要自己带 TLS 库。** devkitPro 的 `switch-curl` 是用 `--with-default-ssl-backend=libnx` 编译的，TLS 后端直接走主机自带的 `ssl` 系统服务，所以只要 `-lcurl -lz -lnx`，不用 OpenSSL、不用 mbedTLS、也不用往 SD 卡放 CA 证书包。代价是该后端只支持 TLS 1.0–1.2，所以代码里显式把版本上限锁在 1.2。`switch-curl` 的 curl 版本较老（7.69.1），目前够用。

**不能在渲染线程上做阻塞请求。** overlay 是单线程驱动输入和绘制的，直接同步请求会卡住整个界面。所以 `FetchJob` 把请求放到工作线程，通过原子变量发布结果，GUI 每帧轮询；唤出时只请求一次，隐藏时 `cancel()` 会中止传输并 `join` 线程 —— overlay 的 `main` 一返回，加载器就会解除这个 NRO 的内存映射，绝不能让线程活过那一刻。

**超时给得比较紧。** 连接 6 秒 / 总 12 秒 / 低速 6 秒，另加 `CURLOPT_NOSIGNAL`。在 overlay 里，让用户干等一个卡死的连接，比直接报错更糟。

**行只重建、不原地改。** libtesla 的 `List::addItem`/`removeItem` 是延迟到下次布局才生效的，而 `ListItem` 左侧文字没有 setter。所以数据变化时整体重建一个新的 `List` 交给 `OverlayFrame::setContent`（它会 delete 旧内容），重建前先 `removeFocus()`，避免焦点指向已删除的元素。

**内存。** overlay 堆由加载器限制（新版 nx-ovlloader 固定 4 MB），所以响应体限制在 96 KB，游戏列表最多显示 40 行。

---

## 项目结构

```
nextendo-ovl/
├── include/
│   ├── gui.hpp            # UI 接口
│   └── network.hpp        # HTTPS 取数接口
├── source/
│   ├── main.cpp           # overlay 入口（tsl::loop），服务初始化/清理
│   ├── gui.cpp            # Tesla 界面
│   ├── network.cpp        # libcurl 封装 + 工作线程
│   └── json.cpp           # 无依赖 JSON 解析（可在 PC 上单测）
├── libs/libtesla/         # vendored libtesla（MIT，header-only），固定版本
├── tests/                 # JSON 解析器单测（宿主机运行，不进交叉编译）
├── scripts/build.sh       # CI 与本地通用的构建脚本
└── Makefile
```

`source/json.cpp` 刻意不依赖任何 Switch 头文件，因此能在 PC 上直接编译测试：

```bash
make -C tests run     # 需要 g++/clang++，会启用 ASan/UBSan
```

---

## 已验证 / 未验证

诚实地说明当前状态：

**已实测通过：**

- **接口行为**：`/api/online-counts` 返回 200、公开、无需鉴权，并用真实响应验证了 `sum(jeux)` 与 `sum(counts)` 的差异（47 vs 49）；
- **JSON 解析器**：53 项断言全部通过（含 UTF-8 重音字符、`\uXXXX` 代理对、未知字段容错、截断与语法错误区分、负数/异常值钳制、越界读取防护）。本地跑过，也在 CI 的 ASan/UBSan 下通过；
- **完整交叉编译**：在 `devkitpro/devkita64` 容器里用 devkitA64 + libultrahand 编译链接成功，产物 1,085,444 字节；
- **产物格式校验**：`HOMEBREW` + `NRO0` 头部魔数正确，且末尾带 `55 4c 54 52`（`ULTR`）—— Ultrahand 据此把它认作自家 overlay。

**尚未验证：**

- **未在真机上运行过。** 网络路径（libnx 的 `ssl` 服务能否与 Cloudflare 成功握手）、Tesla 的唤出组合键与焦点行为，都需要上机确认。首次运行建议在游戏里唤出后先按 `X` 手动刷新一次，观察是否能拿到数据。
- **若 TLS 握手失败**（可能表现为 `SSL connect error`）：说明该主机/固件组合下 `ssl` 服务的证书校验未通过。可改用 libnx 原生 `ssl` API 自行控制校验选项，或换用 `switch-mbedtls` 自带信任库。

---

## 已知限制

- 只在唤出时请求一次（按你的选择），不会后台定时刷新；需要新数据按 `X`。
- 界面文字是英文（Tesla 生态惯例），游戏名直接沿用接口返回的原文（含法语重音等 UTF-8 字符）。
- 游戏列表最多显示 40 项，超出部分显示「+N more games」。
- 接口的计数含游戏服上未清理的「幽灵会话」（服务端文档提到闲置 24–47 小时的连接），因此人数可能略偏高 —— 这是接口本身的口径，网站显示的数字也有同样的问题。

## 许可

本项目代码可自由使用。`libs/libtesla` 为 [WerWolv/libtesla](https://github.com/WerWolv/libtesla)（MIT），许可证见该目录下的 `LICENSE`。Nextendo Network 与本项目无隶属关系。
