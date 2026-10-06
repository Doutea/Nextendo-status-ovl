#!/usr/bin/env bash
# Build the Nextendo overlay.
#
# The project is NX-FanControl's overlay with its fan-control calls replaced;
# its Makefile is used as-is apart from the target name and the libraries.
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

# curl and zlib come from switch-portlibs.
dkp-pacman -S --needed --noconfirm switch-dev switch-portlibs

cd "$(dirname "$0")/../fancontrol-base/overlay"

rm -rf build nextendo-ovl.ovl nextendo-ovl.elf nextendo-ovl.nacp

echo "==> building"
set +e
make -j"$(nproc)" 2>&1 | tee /tmp/build.log
status=${PIPESTATUS[0]}
set -e
tail -n 40 /tmp/build.log
if [ "$status" -ne 0 ]; then
    echo "ERROR: make failed with status $status" >&2
    grep -n -m 20 -E 'error:|Error [0-9]|undefined reference' /tmp/build.log >&2 || true
    exit "$status"
fi

if [ ! -f nextendo-ovl.ovl ]; then
    echo "ERROR: nextendo-ovl.ovl was not produced" >&2
    exit 1
fi

# The NRO magic sits at offset 16, not 0: offset 0 is an AArch64 branch over the
# header, offset 8 is "HOMEBREW". dd is used because the leading bytes contain
# NULs, which command substitution would mangle.
size=$(stat -c %s nextendo-ovl.ovl)
magic=$(dd if=nextendo-ovl.ovl bs=1 skip=16 count=4 2>/dev/null)
echo "==> nextendo-ovl.ovl: $size bytes, NRO magic '$magic'"
if [ "$magic" != "NRO0" ]; then
    echo "ERROR: output is not a valid NRO" >&2
    exit 1
fi

# Keep the artefact where the workflow expects it.
cp nextendo-ovl.ovl ../../nextendo-ovl.ovl
echo "==> copied to $(cd ../.. && pwd)/nextendo-ovl.ovl"
