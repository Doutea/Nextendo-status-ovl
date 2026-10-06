# Version log

Every build handed to testing gets its own version number, so a report can be
matched to an exact build. The number lives in the project's Makefile as
`APP_VERSION` and is written into the overlay's `.nacp`.

## v2 - minimal build (`v2/`)

Written from scratch against `probe-ovl`, which is the only overlay in this
repository that has never crashed on the target console.

| Version | Change | Data | Exit |
|---------|--------|------|------|
| 2.0.0 | Departs from every earlier revision. `probe-ovl`'s exact shape and flags are kept: `initServices()` adds only curl's global state, `exitServices()` is empty, one static list built in `createUI()`, no worker thread, no `atexit` hook, no list rebuilding, and the same `ARCH`/`LDFLAGS` (no `-mtp=soft`, no `-Wl,--gc-sections`). **No system service is initialised**: not `smInitialize()`, not `socketInitialize()`, not `nifmInitialize()`. | to verify | to verify |

Rationale for dropping the system services: every revision that called any of
them crashed the loader on close, while `probe-ovl` (which calls none) has not.
The log captured from the last revision showed the overlay's own code running to
completion - `exitServices()` printed both of its lines - so the fault is
downstream of the overlay and the first step is to stop perturbing the host
process. A service can be reintroduced one at a time afterwards, with the cause
known each time.

## Earlier attempts (`fancontrol-base/`, `source/`)

Kept for reference. Their common findings, all measured on the console:

* `fopen()` fails for every path with `errno 88` (`ENOSYS`) inside the overlay
  process - the `sdmc:` stdio device is not registered. Diagnostics must use the
  raw fs API (`fsOpenSdCardFileSystem` + `fsFsOpenFile` + `fsFileWrite`), which
  works.
* Name resolution needs an `sm:` session held for the overlay's whole lifetime;
  opening one only around the request fails with `Couldn't resolve host name`.
* `socketInitialize()` reports `LibnxError_AlreadyInitialized`, i.e. the socket
  stack was already up before the overlay ran.
* The loader's `fsFileRead` of the next NRO failing (`Atmosphère fatal
  2347-0004`, `Module_HomebrewLoader` 4) is what the exit crash looks like.
