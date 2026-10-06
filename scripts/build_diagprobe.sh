#!/usr/bin/env bash
# Build the log probe alongside the overlay.
#
# The probe exists because every earlier attempt to capture a diagnostic log from
# the overlay produced no file. It proves the write works from the same call
# sites, before the log is relied on.
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

cd "$(dirname "$0")/../diagprobe"

rm -rf build nextendo-diagprobe.ovl nextendo-diagprobe.elf nextendo-diagprobe.nacp

echo "==> building log probe"
set +e
make -j"$(nproc)" 2>&1 | tee /tmp/probe.log
status=${PIPESTATUS[0]}
set -e
tail -n 30 /tmp/probe.log
if [ "$status" -ne 0 ]; then
    echo "ERROR: make failed with status $status" >&2
    grep -n -m 20 -E 'error:|Error [0-9]|undefined reference' /tmp/probe.log >&2 || true
    exit "$status"
fi

if [ ! -f nextendo-diagprobe.ovl ]; then
    echo "ERROR: nextendo-diagprobe.ovl was not produced" >&2
    exit 1
fi

size=$(stat -c %s nextendo-diagprobe.ovl)
magic=$(dd if=nextendo-diagprobe.ovl bs=1 skip=16 count=4 2>/dev/null)
echo "==> nextendo-diagprobe.ovl: $size bytes, NRO magic '$magic'"
if [ "$magic" != "NRO0" ]; then
    echo "ERROR: output is not a valid NRO" >&2
    exit 1
fi

cp nextendo-diagprobe.ovl ../nextendo-diagprobe.ovl
echo "==> copied to $(cd .. && pwd)/nextendo-diagprobe.ovl"
