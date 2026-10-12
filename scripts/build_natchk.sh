#!/usr/bin/env bash
# Build the standalone NAT environment detection overlay.
set -euo pipefail

if [ -z "${DEVKITPRO:-}" ]; then
    echo "DEVKITPRO is not set" >&2
    exit 1
fi
if [ -f "$DEVKITPRO/switchvars.sh" ]; then
    source "$DEVKITPRO/switchvars.sh"
fi
export DEVKITA64="${DEVKITA64:-$DEVKITPRO/devkitA64}"
export PATH="$DEVKITA64/bin:$DEVKITPRO/tools/bin:$PATH"
export PORTLIBS="${PORTLIBS:-$DEVKITPRO/portlibs/switch}"

cd "$(dirname "$0")/../natchk"

rm -rf build ./*.ovl ./*.elf ./*.nacp

echo "==> building nextendo-natchk"
set +e
make -j"$(nproc)" 2>&1 | tee /tmp/natchk-build.log
status=${PIPESTATUS[0]}
set -e
tail -n 20 /tmp/natchk-build.log

if [ "$status" -ne 0 ]; then
    echo "ERROR: make failed with status $status" >&2
    grep -n -m 10 -E 'error:|Error [0-9]' /tmp/natchk-build.log >&2 || true
    exit "$status"
fi

produced="$(ls -1 ./*.ovl 2>/dev/null | head -n 1 || true)"
if [ -z "$produced" ]; then
    echo "ERROR: no .ovl produced" >&2
    exit 1
fi

size=$(stat -c %s "$produced")
magic=$(dd if="$produced" bs=1 skip=16 count=4 2>/dev/null)
echo "==> $produced: $size bytes, NRO magic '$magic'"
if [ "$magic" != "NRO0" ]; then
    echo "ERROR: not a valid NRO" >&2
    exit 1
fi

cp "$produced" ../nextendo-natchk.ovl
echo "==> copied to $(cd .. && pwd)/nextendo-natchk.ovl"