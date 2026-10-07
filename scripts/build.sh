#!/usr/bin/env bash
# Build the overlay and leave the result as nextendo-status.ovl in the repo root.
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

# jansson and mbedtls come from switch-portlibs.
dkp-pacman -S --needed --noconfirm switch-dev switch-portlibs

cd "$(dirname "$0")/.."
make clean >/dev/null 2>&1 || true

echo "==> building"
set +e
make -j"$(nproc)" 2>&1 | tee /tmp/build.log
status=${PIPESTATUS[0]}
set -e
tail -n 30 /tmp/build.log
if [ "$status" -ne 0 ]; then
    echo "ERROR: make failed with status $status" >&2
    grep -n -m 30 -E 'error:|Error [0-9]|undefined reference' /tmp/build.log >&2 || true
    exit "$status"
fi

# The Makefile names the output after the directory, so rename it to the name
# users are told to copy.
produced="$(ls -1 ./*.ovl 2>/dev/null | head -n 1 || true)"
if [ -z "$produced" ]; then
    echo "ERROR: no .ovl was produced" >&2
    exit 1
fi
mv -f "$produced" nextendo-status.ovl

size=$(stat -c %s nextendo-status.ovl)
magic=$(dd if=nextendo-status.ovl bs=1 skip=16 count=4 2>/dev/null)
echo "==> nextendo-status.ovl: $size bytes, NRO magic '$magic'"
if [ "$magic" != "NRO0" ]; then
    echo "ERROR: output is not a valid NRO" >&2
    exit 1
fi
