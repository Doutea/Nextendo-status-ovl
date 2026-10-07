#!/usr/bin/env bash
# Build the language diagnostic overlay.
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

dkp-pacman -S --needed --noconfirm switch-dev

cd "$(dirname "$0")/../langprobe"

rm -rf build ./*.ovl ./*.elf ./*.nacp

echo "==> building language probe"
set +e
make -j"$(nproc)" 2>&1 | tee /tmp/langprobe.log
status=${PIPESTATUS[0]}
set -e
tail -n 30 /tmp/langprobe.log
if [ "$status" -ne 0 ]; then
    echo "ERROR: make failed with status $status" >&2
    grep -n -m 30 -E 'error:|Error [0-9]|undefined reference' /tmp/langprobe.log >&2 || true
    exit "$status"
fi

produced="$(ls -1 ./*.ovl 2>/dev/null | head -n 1 || true)"
if [ -z "$produced" ]; then
    echo "ERROR: no .ovl was produced" >&2
    exit 1
fi

size=$(stat -c %s "$produced")
magic=$(dd if="$produced" bs=1 skip=16 count=4 2>/dev/null)
echo "==> $produced: $size bytes, NRO magic '$magic'"
if [ "$magic" != "NRO0" ]; then
    echo "ERROR: output is not a valid NRO" >&2
    exit 1
fi

cp "$produced" ../nextendo-langprobe.ovl
echo "==> copied to $(cd .. && pwd)/nextendo-langprobe.ovl"
