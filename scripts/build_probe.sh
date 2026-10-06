#!/usr/bin/env bash
# Build a standalone diagnostic probe overlay.
#
# The probe (probe/main.cpp) draws text and initialises no services. Its only
# purpose is bisection on real hardware: if the probe launches but the real
# overlay crashes, the fault is in the real overlay's own code rather than in
# how it integrates with the menu/loader.
#
# It lives in its own directory with its own copy of the build rules, so the
# real overlay's Makefile stays a plain libtesla build with no special cases.
set -euo pipefail

if [ -z "${DEVKITPRO:-}" ]; then
    echo "DEVKITPRO is not set" >&2
    exit 1
fi
if [ -f "$DEVKITPRO/switchvars.sh" ]; then
    # shellcheck disable=SC1091
    source "$DEVKITPRO/switchvars.sh"
fi
export DEVKITA64="${DEVKITA64:-$DEVKITPRO/devkitA64}"
export PATH="$DEVKITA64/bin:$DEVKITPRO/tools/bin:$PATH"

echo "==> building probe"
rm -rf probe/build probe/probe-ovl.ovl probe/probe-ovl.elf probe/probe-ovl.nacp
make -C probe 2>&1 | tail -20

if [ ! -f probe/probe-ovl.ovl ]; then
    echo "ERROR: probe/probe-ovl.ovl was not produced" >&2
    exit 1
fi

size=$(stat -c %s probe/probe-ovl.ovl)
magic=$(dd if=probe/probe-ovl.ovl bs=1 skip=16 count=4 2>/dev/null)
echo "==> probe/probe-ovl.ovl: $size bytes, NRO magic '$magic'"
