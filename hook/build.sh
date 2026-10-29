#!/bin/bash
# Cross-compile the AC6 5:4 hook (win64 dxgi.dll proxy) from Linux.
#
# Needs a mingw-w64 C++ cross compiler: llvm-mingw (x86_64-w64-mingw32-clang++, what the
# shipped build used) or GNU mingw-w64 (x86_64-w64-mingw32-g++). MinHook is vendored in
# third_party/minhook (BSD-2-Clause).
#
#   ./build.sh            production build -> build/dxgi-ac6-crt.dll
#   ./build.sh logger     viewport logger only (no fill, no capture) -> build/dxgi-logger.dll
#   CXX=... ./build.sh    pick a compiler explicitly
#
# Production flags: ENABLE_VERTPLUS (resource/PSO creation hooks: the bar-PSO shader-hash
# fingerprint lives here) + ENABLE_CAPTURE (Present/ExecuteCommandLists hooks: the live
# ac6ctl.txt control file and the backbuffer dump that ui/capture.sh reads). The 3D fill
# itself (asppatch=3 + autovp=1) is baked in as code defaults; diagnostics default OFF.
#
# Staging into the game is the launcher's job (launcher/armored-core-6.sh). This script
# only builds.
set -euo pipefail
cd "$(dirname "$0")"

if [ -z "${CXX:-}" ]; then
  for c in x86_64-w64-mingw32-clang++ x86_64-w64-mingw32-g++; do
    command -v "$c" >/dev/null && { CXX="$c"; break; }
  done
fi
command -v "${CXX:-}" >/dev/null || { echo "no mingw-w64 C++ compiler found (install llvm-mingw or mingw-w64-gcc)"; exit 1; }

MH=third_party/minhook
MH_SRC="$MH/src/buffer.c $MH/src/hook.c $MH/src/trampoline.c $MH/src/hde/hde32.c $MH/src/hde/hde64.c"

case "${1:-prod}" in
  prod)   DEFS="-DENABLE_VERTPLUS -DENABLE_CAPTURE"; OUT=build/dxgi-ac6-crt.dll ;;
  logger) DEFS="";                                   OUT=build/dxgi-logger.dll ;;
  *) echo "usage: $0 [prod|logger]"; exit 2 ;;
esac

mkdir -p build
echo "compiling $OUT with $CXX $DEFS ..."
# shellcheck disable=SC2086
$CXX -O2 -shared -static -static-libgcc -static-libstdc++ \
  -I"$MH/include" $DEFS \
  dxgi_vertplus.cpp $MH_SRC dxgi.def \
  -o "$OUT" \
  -ld3d12 -ldxgi -lkernel32 -luser32 -lgdi32
echo "built: $OUT"
cat <<NOTE

To run it the game folder also needs dxgi_proxy.dll: a copy of the real (DXVK) dxgi.dll from
the Proton prefix, which this proxy forwards every export to. The launcher stages both:
  launcher/armored-core-6.sh  ->  CRT
NOTE
