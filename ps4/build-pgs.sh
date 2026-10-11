#!/bin/bash
# PCSX2 PS4 port (AI-assisted): builds paraLLEl-GS (Arntzen-Software/parallel-gs, LGPL-3.0+, the PS2 GS in Vulkan
# compute) and the parts of Granite it uses as PS4 static libraries, with CEMUPS4's dependency toolchain. The eboot
# links them when Makefile.ps4 is run with PGS=1 (build-ps4.sh does that when this script has run). Run it in WSL.
# Needs proper testing on the console.
#
#   ps4/build-pgs.sh            configure + build into ~/pcsx2ps4-build/pgs
#
# PGS_SRC is the paraLLEl-GS checkout with its Granite submodule (git clone --recursive).
set -euo pipefail

PGS_SRC=${PGS_SRC:-$(cd "$(dirname "$0")/../.." && pwd)/parallel-gs}   # default: next to this repository
DEPS=${PS4DEPS:-$HOME/pcsx2ps4-build/deps}
SRC=$DEPS/parallel-gs
BUILD=${PGS_BUILD:-$HOME/pcsx2ps4-build/pgs}
TOOLCHAIN=${TOOLCHAIN:-$HOME/cemups4/toolchain/ps4-deps.cmake}

mkdir -p "$DEPS" "$BUILD"
# A copy on the Linux side: the build reads thousands of headers, slow over /mnt.
rsync -a --delete --exclude .git "$PGS_SRC"/ "$SRC"/

cmake -S "$SRC" -B "$BUILD" -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN" \
  -DCMAKE_BUILD_TYPE=Release \
  -DPARALLEL_GS_STANDALONE=ON \
  -DGRANITE_RENDERDOC_CAPTURE=OFF \
  -DGRANITE_TOOLS=OFF \
  -DGRANITE_INSTALL_TARGETS=OFF \
  -DGRANITE_VULKAN_DXGI_INTEROP=OFF \
  -DCMAKE_C_FLAGS_RELEASE="-O2 -DNDEBUG" \
  -DCMAKE_CXX_FLAGS_RELEASE="-O2 -DNDEBUG"

cmake --build "$BUILD" --target parallel-gs -j"$(nproc)"

# The static libraries the eboot needs, in link order.
find "$BUILD" -name 'libparallel-gs.a' -o -name 'libgranite-vulkan.a' -o -name 'libgranite-math.a' \
  -o -name 'libgranite-util.a' -o -name 'libgranite-volk.a' -o -name 'libgranite-application-global-interface.a' \
  | sort > "$BUILD/libs.txt"
cat "$BUILD/libs.txt"
