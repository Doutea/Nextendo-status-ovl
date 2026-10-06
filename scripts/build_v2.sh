#!/usr/bin/env bash
# Build the v2 (minimal) overlay.
#
# Its Makefile is probe-ovl's, the only overlay here that has never crashed on
# the target console; only the target name, title and the curl libraries differ.
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

dkp-pacman -S --needed --noconfirm switch-dev switch-portlibs

cd "$(dirname "$0")/../v2"

rm -rf build nextendo-ovl.ovl nextendo-ovl.elf nextendo-ovl.nacp

echo "==> building v2"
set +e
make -j"$(nproc)" 2>&1 | tee /tmp/v2.log
status=${PIPESTATUS[0]}
set -e
tail -n 40 /tmp/v2.log
if [ "$status" -ne 0 ]; then
    echo "ERROR: make failed with status $status" >&2
    grep -n -m 20 -E 'error:|Error [0-9]|undefined reference' /tmp/v2.log >&2 || true
    exit "$status"
fi

if [ ! -f nextendo-ovl.ovl ]; then
    echo "ERROR: nextendo-ovl.ovl was not produced" >&2
    exit 1
fi

size=$(stat -c %s nextendo-ovl.ovl)
magic=$(dd if=nextendo-ovl.ovl bs=1 skip=16 count=4 2>/dev/null)
echo "==> nextendo-ovl.ovl: $size bytes, NRO magic '$magic'"
if [ "$magic" != "NRO0" ]; then
    echo "ERROR: output is not a valid NRO" >&2
    exit 1
fi

cp nextendo-ovl.ovl ../nextendo-v2.ovl
echo "==> copied to $(cd .. && pwd)/nextendo-v2.ovl"
