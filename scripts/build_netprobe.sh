#!/usr/bin/env bash
# Build the network diagnostic probe.
#
# The main overlay can only report curl's generic "Couldn't resolve host name".
# This probe performs the same operations step by step and prints the exact
# libnx result codes, so a failure can be pinned to DNS, the connection, or TLS
# without a debugger attached.
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

echo "==> building netprobe"
rm -rf netprobe/build netprobe/netprobe-ovl.ovl netprobe/netprobe-ovl.elf netprobe/netprobe-ovl.nacp
make -C netprobe 2>&1 | tail -20

if [ ! -f netprobe/netprobe-ovl.ovl ]; then
    echo "ERROR: netprobe/netprobe-ovl.ovl was not produced" >&2
    exit 1
fi

size=$(stat -c %s netprobe/netprobe-ovl.ovl)
magic=$(dd if=netprobe/netprobe-ovl.ovl bs=1 skip=16 count=4 2>/dev/null)
echo "==> netprobe/netprobe-ovl.ovl: $size bytes, NRO magic '$magic'"
