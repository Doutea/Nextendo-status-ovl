#!/usr/bin/env bash
# Build the reference overlay (Chasetodie/Nextendo-Status-Overlay) with the
# fixes applied on top.
#
# Its Makefile is used unchanged, so the build flags and library set are exactly
# the author's: -mtp=soft, jansson for JSON, mbedtls for TLS.
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

echo "==> checking the libraries the Makefile links against"
missing=0
for header in jansson.h mbedtls/ssl.h curl/curl.h; do
    if [ -f "$PORTLIBS/include/$header" ]; then
        echo "  ok      $PORTLIBS/include/$header"
    else
        echo "  MISSING $PORTLIBS/include/$header" >&2
        missing=1
    fi
done
if [ "$missing" -ne 0 ]; then
    echo "ERROR: a required portlib header is absent" >&2
    exit 1
fi

cd "$(dirname "$0")/../ref-overlay"

rm -rf build ref-overlay.ovl ref-overlay.elf ref-overlay.nacp

echo "==> building"
set +e
make -j"$(nproc)" 2>&1 | tee /tmp/ref.log
status=${PIPESTATUS[0]}
set -e
tail -n 40 /tmp/ref.log
if [ "$status" -ne 0 ]; then
    echo "ERROR: make failed with status $status" >&2
    grep -n -m 30 -E 'error:|Error [0-9]|undefined reference|No such file' /tmp/ref.log >&2 || true
    exit "$status"
fi

# The Makefile derives TARGET from the directory name.
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

cp "$produced" ../nextendo-ref.ovl
echo "==> copied to $(cd .. && pwd)/nextendo-ref.ovl"
