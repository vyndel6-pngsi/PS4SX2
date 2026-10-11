#!/usr/bin/env bash
# PCSX2 PS4 port (AI-assisted): compile (Makefile.ps4), link (link/CMakeLists.txt) and package PCSX2 for the PS4.
#   ps4/build-ps4.sh [imports|package|upload]
#     imports: after the link, list the imported names no PS4 system library provides (PS5-only calls)
#     package: also make the pkg (PCSX00002) in ~/pcsx2ps4-build/out
#     upload:  package, then put the pkg in the console's /data/pkg over FTP (PS4_HOST, PS4_FTP_PORT)
# Uses CEMUPS4's toolchain and runtime (~/cemups4) and its sysroot and Mesa build (~/cemups4-build), read only.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
PCSX2="$(cd "$HERE/.." && pwd)"
PORT="${PS4_PORT_DIR:-$HOME/cemups4}"
PSB="${PCSX2PS4_BUILD:-$HOME/pcsx2ps4-build}"
export PS4_SYSROOT="${PS4_SYSROOT:-$HOME/cemups4-build/sysroot}"
OPENORBIS="${OPENORBIS:-/opt/pacbrew/ps4/openorbis}"
mode="${1:-}"

echo "[build-ps4] compiling"
# paraLLEl-GS as a second renderer when ps4/build-pgs.sh has built its libraries.
pgs=0
[ -s "$PSB/pgs/libs.txt" ] && pgs=1 && echo "[build-ps4] with paraLLEl-GS"
make -C "$HERE" -f Makefile.ps4 -j"${JOBS:-20}" PGS=$pgs all objects.txt > "$PSB/compile.log" 2>&1 \
    || { grep -E "error" "$PSB/compile.log" | head -40; exit 1; }
tail -1 "$PSB/compile.log"

echo "[build-ps4] linking"
source "$PORT/scripts/ps4-env.sh"
link="$PSB/build/link"
if [ ! -f "$link/Makefile" ]; then
    ps4_cmake -S "$HERE/link" -B "$link" -G "Unix Makefiles" \
        -DCMAKE_TOOLCHAIN_FILE="$PORT/toolchain/ps4-cemu.cmake" -DPS4_PORT_DIR="$PORT" \
        -DPCSX2_OBJECTS_FILE="$HERE/objects.txt" -DPS4_PAID=0x3800000000000035 > "$PSB/link-configure.log" 2>&1 \
        || { tail -30 "$PSB/link-configure.log"; exit 1; }
fi
elf="$link/pcsx2"
eboot="$link/pcsx2_eboot/eboot.bin"
rm -f "$eboot" # a failed link must not leave the last eboot to be packaged
make -C "$link" -j"${JOBS:-20}" > "$PSB/link.log" 2>&1 || true
grep -E "error|Failed|missing" "$PSB/link.log" | grep -v "no version information" | head -20 || true

if [ "$mode" = imports ] || [ ! -f "$eboot" ]; then
    [ -f "$elf" ] || { echo "[build-ps4] no ELF: the link failed (log: $PSB/link.log)" >&2; exit 1; }
    # Every name the system libraries export, then the ELF's undefined names that none of them has.
    index="$PSB/ps4-exports.txt"
    if [ ! -s "$index" ]; then
        for so in "$OPENORBIS"/lib/*.so; do
            llvm-nm-21 -D --defined-only "$so" 2>/dev/null | awk -v lib="$(basename "$so" .so)" '{print $NF" "lib}'
        done | sort -u -k1,1 > "$index"
    fi
    llvm-nm-21 -u "$elf" | awk '{print $NF}' | sort -u > "$PSB/undefined.txt"
    join -a1 "$PSB/undefined.txt" "$index" | awk 'NF==1' | grep -v "^annotate_" > "$PSB/unresolved.txt" || true
    echo "[build-ps4] $(wc -l < "$PSB/undefined.txt") imports, $(wc -l < "$PSB/unresolved.txt") with no PS4 library (list: $PSB/unresolved.txt)"
    head -80 "$PSB/unresolved.txt"
    [ -f "$eboot" ] || exit 1
fi
echo "[build-ps4] eboot: $eboot ($(du -h "$eboot" | cut -f1))"

if [ "$mode" = package ] || [ "$mode" = upload ]; then
    stage="$PSB/build/stage"
    rm -rf "$stage"
    mkdir -p "$stage/sce_sys"
    cp "$eboot" "$stage/"
    cp "$PCSX2/ps4/sce_sys/icon0.png" "$stage/sce_sys/" # PS4SX2 icon, 512x512 RGB, as the PS4 wants
    cp -r "$PCSX2/bin/resources" "$stage/resources"    # /app0/resources (main-boot.cpp, ORBIS_PS4)
    # Your own BIOS dumps, built into the package (/app0/bios; main-boot.cpp copies them to /data/PCSX2/bios). They come
    # from outside the repository and must never be committed or shared: a package built with them is for your console.
    bios="${PCSX2_PS4_BIOS_DIR:-$PSB/bios}"
    if [ -d "$bios" ] && [ -n "$(ls -A "$bios")" ]; then
        mkdir -p "$stage/bios"
        cp "$bios"/* "$stage/bios/"
        echo "[build-ps4] BIOS files in the package: $(ls "$stage/bios" | wc -l) from $bios (personal build: don't share it)"
    fi
    # Plain homebrew launch values (CEMUPS4's), plus PS4 Pro mode and the seventh CPU core. No Sony modules: no Piglet.
    out="$PSB/out"
    mkdir -p "$out"
    rm -f "$out"/IV0000-PCSX*.pkg # only the newest package is kept
    pkg="$(SFO_STYLE=plain NO_RIGHT_SPRX=1 SFO_ATTRIBUTE="${SFO_ATTRIBUTE:-0x00810000}" \
        DOLPHIN_PS4_MODULES_DIR=/nonexistent "$HERE/make-pkg.sh" "$stage" PCSX00002 "PS4SX2" \
        "${PCSX2_PS4_VERSION:-00.01}" PCSX2PS4 "$out" | tail -1)"
    echo "[build-ps4] package: $pkg ($(du -h "$pkg" | cut -f1))"
    # Optionally a copy elsewhere (PCSX2_PS4_PKG_DIR, e.g. a Windows folder): the current package, and the one it replaces
    # moved into backups/ with its date, keeping the last 5. Personal builds carry your BIOS: keep them to yourself.
    keep="${PCSX2_PS4_PKG_DIR:-}"
    if [ -n "$keep" ] && mkdir -p "$keep/backups" 2>/dev/null; then
        for prev in "$keep"/IV0000-PCSX*.pkg; do
            [ -f "$prev" ] || continue
            mv "$prev" "$keep/backups/$(basename "$prev" .pkg)_$(date -r "$prev" +%Y-%m-%d_%H%M).pkg"
        done
        ls -1t "$keep"/backups/IV0000-PCSX*.pkg 2>/dev/null | tail -n +6 | while read -r old; do rm -f "$old"; done
        cp "$pkg" "$keep/" && echo "[build-ps4] copied to $keep (older ones in backups/, last 5 kept)"
    fi
    if [ "$mode" = upload ]; then
        [ -n "${PS4_HOST:-}" ] || { echo "[build-ps4] set PS4_HOST to the console's IP address (FTP on PS4_FTP_PORT, default 2121)" >&2; exit 1; }
        ftp="ftp://${PS4_HOST}:${PS4_FTP_PORT:-2121}"
        # This port's older packages (other PCSX titles and versions) leave the console's /data/pkg first.
        for old in $(curl -sS --connect-timeout 10 --list-only "$ftp/data/pkg/" | tr -d '\r' | grep '^IV0000-PCSX.*\.pkg$' || true); do
            if [ "$old" != "$(basename "$pkg")" ]; then
                curl -sS --connect-timeout 10 -Q "DELE /data/pkg/$old" "$ftp/data/pkg/" -o /dev/null && echo "[build-ps4] removed old $old"
            fi
        done
        curl -sS --connect-timeout 10 -T "$pkg" "$ftp/data/pkg/"
        echo "[build-ps4] uploaded $(basename "$pkg") to ${PS4_HOST}"
    fi
fi
