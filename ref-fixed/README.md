# Nextendo Status Overlay - fixed build

This is [Chasetodie/Nextendo-Status-Overlay](https://github.com/Chasetodie/Nextendo-Status-Overlay)
with **one** change, made because the upstream build crashed.

## The fix

`source/main.cpp`:

```diff
-alignas(0x1000) static u8 g_threadStack[0x4000];
+alignas(0x1000) static u8 g_threadStack[0x40000];
```

The polling thread's stack was 16 KB. That thread performs DNS resolution, a TLS
handshake and a libcurl transfer; libcurl's own guidance is a minimum of 32 KB
for the resolver alone, before TLS, and mbedTLS keeps further state on the stack.
16 KB overflows, and on this target a stack overflow corrupts adjacent memory.

Everything else is the author's: the Makefile, the library set
(`-lcurl -ljansson -lmbedtls -lmbedx509 -lmbedcrypto -lz -lnx`), the flags
(`-mtp=soft`, no `-Wl,--gc-sections`), the polling design, the UI and the
`games.json` asset layout.

Confirmed on hardware: the panel loads the player counts and closing it with `B`
no longer crashes.

## Version

`APP_VERSION` in the Makefile, also written into the `.nacp`:

| Version | Change | Result |
|---------|--------|--------|
| 1.0.0 | upstream as published | crashed on exit |
| 1.0.1 | polling-thread stack 16 KB -> 256 KB | loads counts, exits cleanly |

## What this rules out, from earlier work in this repository

The many failed attempts in `fancontrol-base/` and `source/` blamed service
teardown. That was wrong: this upstream overlay calls `socketExit()`,
`nifmExit()`, `timeExit()`, `curl_global_cleanup()` and
`fsdevUnmountDevice("sdmc")` on the way out and exits cleanly. Closing services
is not what broke it. The fault was the undersized thread stack all along.

Two other findings from that work do still hold, both measured on the console:

* `fopen()` fails with `errno 88` (`ENOSYS`) in an overlay process when the
  `sdmc:` devoptab device has not been mounted, so file access needs either
  `fsdevMountSdmc()` first or the raw fs API. This overlay mounts first, which is
  why its `debugLog` works.
* Name resolution needs an `sm:` session at the moment the lazily-initialised
  resolver first runs.
