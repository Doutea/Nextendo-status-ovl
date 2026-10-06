#!/usr/bin/env bash
# Build the diagnostic probe overlay: a minimal libultrahand overlay that draws
# text and initialises no services.
#
# Purpose: separate "libultrahand / loader integration is broken" from "this
# overlay's own code is broken". If the probe launches and the real overlay
# crashes, the fault is in the real overlay's code (its initServices, its
# network layer), not in how it integrates.
#
# The probe Makefile is generated from the project's own Makefile so the two
# cannot drift apart in flags or library list. The target name differs, so the
# real nextendo-ovl.ovl is never touched.
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

# libultrahand's TESLA_INIT_IMPL header must be compiled together with the probe
# source; use the project Makefile as the template and swap the target/sources.
mkdir -p source/diagnostic
cp source/diagnostic_main.cpp source/diagnostic/diagnostic_main.cpp

sed -e 's/^TARGET\t\t:=.*/TARGET		:=	probe-ovl/' \
    -e 's/^SOURCES\t\t:=.*/SOURCES		:=	source\/diagnostic source libs\/libultrahand\/libultra\/source libs\/libultrahand\/libtesla\/source libs\/libultrahand\/common/' \
    -e 's/^APP_TITLE\t:=.*/APP_TITLE	:=	Nextendo Probe/' \
    Makefile > Makefile.probe

echo "==> probe Makefile generated"
grep -E '^(TARGET|SOURCES|APP_TITLE)' Makefile.probe

echo "==> building probe"
rm -rf build-probe probe-ovl.ovl probe-ovl.elf probe-ovl.nacp
make -f Makefile.probe BUILD=build-probe 2>&1 | tail -30

if [ ! -f probe-ovl.ovl ]; then
    echo "ERROR: probe-ovl.ovl was not produced" >&2
    exit 1
fi

size=$(stat -c %s probe-ovl.ovl)
magic=$(dd if=probe-ovl.ovl bs=1 skip=16 count=4 2>/dev/null)
last4=$(tail -c 4 probe-ovl.ovl)
echo "==> probe-ovl.ovl: $size bytes, NRO magic '$magic', trailer '$last4'"
ls -l probe-ovl.ovl
