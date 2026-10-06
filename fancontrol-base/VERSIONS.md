# Version log

Every build handed over for testing gets its own version number, recorded here
so a screenshot or a report can be matched to an exact build.

The version lives in `fancontrol-base/overlay/Makefile` as `APP_VERSION`, and is
written into the overlay's `.nacp`, so it can also be read off the file itself.

| Version | Change | Data on open | Exit |
|---------|--------|--------------|------|
| 1.0.0 | First build on the NX-FanControl base: template's `main.cpp`, Makefile and lifecycle, with the fan-control calls replaced by the Nextendo fetch | no (`Couldn't resolve host name`) | ok |
| 1.0.1 | Added `socketInitialize()`; `exitServices()` emptied — the template's `fsdevUnmountAll()` was an unmatched call (this overlay never mounts) and libnx already unmounts on its own exit path | yes | to verify |

Historical note for 1.0.1: name resolution only works when an `sm:` session is
held for the overlay's whole lifetime (opened in `initServices`), so that is what
the build does; it is deliberately never closed, because closing it on the way
out is what made nx-ovlloader fail with an Atmosphère fatal 2347-0004.
