#!/bin/bash
# Builds the overlay inside a devkitPro container.
#
# Kept as a script rather than inline YAML so the build can be reproduced
# locally with the exact same steps:
#   docker run --rm -v "$PWD:/work" -w /work devkitpro/devkita64:latest bash scripts/build.sh
set -euo pipefail

echo "==> toolchain"
echo "DEVKITPRO=${DEVKITPRO:-<unset>}"
aarch64-none-elf-gcc --version | head -n 1
make --version | head -n 1

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
