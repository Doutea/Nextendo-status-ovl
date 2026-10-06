#!/bin/bash
# Builds the overlay inside a devkitPro container.
#
# Kept as a script rather than inline YAML so the build can be reproduced
# locally with the exact same steps:
#   docker run --rm -v "$PWD:/work" -w /work devkitpro/devkita64:latest bash scripts/build.sh
set -euo pipefail

# The devkitPro images only put $DEVKITPRO/tools/bin on PATH, which is not
# enough: the cross compiler, the target binutils and the portlibs live in
# sibling directories. Source the environment the image ships, falling back to
# adding the directories explicitly.
if [ -f "$DEVKITPRO/dkp-env/switchvars.sh" ]; then
    # shellcheck disable=SC1091
    source "$DEVKITPRO/dkp-env/switchvars.sh"
elif [ -f "$DEVKITPRO/switchvars.sh" ]; then
    # shellcheck disable=SC1091
    source "$DEVKITPRO/switchvars.sh"
fi

export DEVKITPRO="${DEVKITPRO:-/opt/devkitpro}"
export DEVKITARM="${DEVKITARM:-$DEVKITPRO/devkitARM}"
export DEVKITA64="${DEVKITA64:-$DEVKITPRO/devkitA64}"
export PATH="$DEVKITA64/bin:$DEVKITPRO/tools/bin:$DEVKITPRO/portlibs/switch/bin:$PATH"

echo "==> toolchain"
echo "DEVKITPRO=$DEVKITPRO"
command -v aarch64-none-elf-gcc
# Note: no `| head -n 1` here. With `set -o pipefail`, head exiting early sends
# SIGPIPE to the producer and the whole script reports 141 for no good reason.
aarch64-none-elf-gcc --version
make --version
command -v elf2nro

# devkitpro/devkita64 already ships switch-dev and switch-portlibs (which
# includes switch-curl and switch-zlib), so this is normally a no-op. It is kept
# so the script also works in a bare toolchain container.
echo "==> ensuring Switch portlibs"
if command -v dkp-pacman >/dev/null 2>&1; then
    dkp-pacman -S --needed --noconfirm switch-dev switch-portlibs
else
    echo "dkp-pacman not found; assuming the image already provides the toolchain"
fi

echo "==> building overlay"
# Capture the output and print the tail afterwards: piping make straight into
# head/tail would again trip pipefail on a build that actually succeeded.
set +e
make -j"$(nproc)" 2>&1 | tee /tmp/build.log
make_status=${PIPESTATUS[0]}
set -e
echo "--- last 60 lines of the build ---"
tail -n 60 /tmp/build.log
if [ "$make_status" -ne 0 ]; then
    echo "ERROR: make failed with status $make_status" >&2
    echo "--- first errors ---" >&2
    grep -n -m 20 -E 'error:|Error [0-9]|undefined reference|No such file' /tmp/build.log >&2 || true
    exit "$make_status"
fi

if [ ! -f nextendo-ovl.ovl ]; then
    echo "ERROR: nextendo-ovl.ovl was not produced" >&2
    exit 1
fi

echo "==> result"
ls -l nextendo-ovl.elf nextendo-ovl.ovl

# Validate the NRO header. The layout is:
#   offset 0  : 4-byte AArch64 branch instruction (jumps past the header)
#   offset 8  : "HOMEBREW" magic
#   offset 16 : "NRO0" magic
# so the NRO magic is NOT at offset 0. `dd` is used rather than `head` because
# the leading bytes contain NULs, which command substitution mangles.
magic=$(dd if=nextendo-ovl.ovl bs=1 skip=16 count=4 2>/dev/null)
if [ "$magic" != "NRO0" ]; then
    echo "ERROR: output is not a valid NRO (magic at offset 16 was '$magic')" >&2
    exit 1
fi

# The overlay must contain something, and a plausible amount of it.
size=$(stat -c %s nextendo-ovl.ovl)
if [ "$size" -lt 100000 ]; then
    echo "ERROR: the .ovl is suspiciously small ($size bytes)" >&2
    exit 1
fi

echo "==> ok: nextendo-ovl.ovl ($size bytes, NRO0 header verified)"
