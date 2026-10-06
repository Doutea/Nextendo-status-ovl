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
aarch64-none-elf-gcc --version | head -n 1
make --version | head -n 1
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
make -j"$(nproc)"

if [ ! -f nextendo-ovl.ovl ]; then
    echo "ERROR: nextendo-ovl.ovl was not produced" >&2
    exit 1
fi

# An .ovl is a libnx NRO with a different extension, so the magic must be NRO0.
magic=$(head -c 4 nextendo-ovl.ovl)
if [ "$magic" != "NRO0" ]; then
    echo "ERROR: output is not a valid NRO (magic was '$magic')" >&2
    exit 1
fi

echo "==> ok"
ls -l nextendo-ovl.ovl
