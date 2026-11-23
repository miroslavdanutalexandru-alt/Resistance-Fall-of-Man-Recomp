#!/usr/bin/env bash
# Builds the port on Linux: the same Windows programs build.bat makes (they run under Proton / Wine), compiled with
# LLVM (clang-cl, lld-link) and Microsoft's C runtime + Windows SDK that setup_linux.sh downloads with xwin.
# Run ./setup_linux.sh first (it calls this script at the end); after that, ./build_linux.sh rebuilds.
#
#   ./build_linux.sh [--jobs N] [--nopgo] [--baseline]
#     --jobs N     compile jobs (default 4, or PORT_JOBS); fewer if the compiler runs out of memory
#     --nopgo      build without the PGO profile pgo/r1.profdata (needed with LLVM older than 22)
#     --baseline   also build the variant for CPUs without AVX2 (build_baseline/; automatic on such a CPU)
# Environment: PORT_LLVM_BIN=<folder of clang-cl, lld-link, llvm-rc> (default: PATH, then /usr/lib/llvm-NN/bin),
#              PORT_XWIN_DIR=<xwin splat folder> (default: tools_cache/msvc_linux).
set -euo pipefail
case "${PORT_XWIN_DIR:-/}" in /*) ;; *) PORT_XWIN_DIR="$PWD/$PORT_XWIN_DIR" ;; esac
cd "$(dirname "$0")"
PORT="$(pwd)"

JOBS="${PORT_JOBS:-4}"
PGO=auto
BASELINE=auto
while [ $# -gt 0 ]; do
    case "$1" in
        --jobs) JOBS="$2"; shift 2 ;;
        --jobs=*) JOBS="${1#*=}"; shift ;;
        --nopgo|nopgo) PGO=off; shift ;;
        --baseline) BASELINE=yes; shift ;;
        nopause) shift ;;
        -h|--help) sed -n '2,12p' "$0"; exit 0 ;;
        *) echo "Unknown option: $1 (see ./build_linux.sh --help)"; exit 2 ;;
    esac
done
[ "${PORT_PGO:-}" = "0" ] || [ "${PORT_PGO:-}" = "off" ] && PGO=off

fail() { echo; echo "BUILD FAILED: $*"; exit 1; }
mkdir -p logs
[ -f recompiled_game/ppu_recomp.h ] || fail "the game code has not been generated yet - run ./setup_linux.sh first."

# ---------------------------------------------------------------- tools
export PORT_XWIN_DIR="${PORT_XWIN_DIR:-$PORT/tools_cache/msvc_linux}"
find_llvm() {   # find_llvm <tool>: PORT_LLVM_BIN, PATH, then the newest /usr/lib/llvm-NN/bin
    local t="$1" d
    if [ -n "${PORT_LLVM_BIN:-}" ] && [ -x "$PORT_LLVM_BIN/$t" ]; then echo "$PORT_LLVM_BIN/$t"; return 0; fi
    if command -v "$t" >/dev/null 2>&1; then command -v "$t"; return 0; fi
    for d in $(ls -d /usr/lib/llvm-*/bin /usr/lib64/llvm*/bin 2>/dev/null | sort -t- -k2 -V -r); do
        if [ -x "$d/$t" ]; then echo "$d/$t"; return 0; fi
    done
    return 1
}
CLANGCL="$(find_llvm clang-cl)" || fail "clang-cl (LLVM) was not found. Install LLVM 22 or newer (clang, lld, llvm), or set PORT_LLVM_BIN."
LLVMBIN="$(dirname "$CLANGCL")"
export PORT_LLVM_BIN="${PORT_LLVM_BIN:-$LLVMBIN}"
LLDLINK="$(find_llvm lld-link)" || fail "lld-link (LLVM's lld) was not found. Install lld (same version as clang)."
LLVMRC="$(find_llvm llvm-rc)" || fail "llvm-rc (LLVM) was not found. Install llvm (same version as clang)."
find_llvm llvm-lib >/dev/null || fail "llvm-lib (LLVM) was not found. Install llvm (same version as clang)."
for t in cmake ninja python3; do command -v "$t" >/dev/null 2>&1 || fail "$t was not found - install it (see README, Linux)."; done
[ -f "$PORT_XWIN_DIR/crt/include/vcruntime.h" ] || fail "the Microsoft C runtime / Windows SDK is not in $PORT_XWIN_DIR - run ./setup_linux.sh."
CLANGMAJ="$("$CLANGCL" --version 2>/dev/null | sed -n 's/.*clang version \([0-9][0-9]*\).*/\1/p' | head -n1)"
CLANGMAJ="${CLANGMAJ:-0}"
echo "LLVM: $LLVMBIN (clang $CLANGMAJ)"
[ "$CLANGMAJ" -ge 18 ] || fail "LLVM / clang 18 or newer is needed (22 or newer recommended), found clang $CLANGMAJ."

# pgo/r1.profdata is LLVM profile format 13: clang 22 or newer only
PGOMODE=off
PGOPROF=""
if [ "$PGO" != off ] && [ -f pgo/r1.profdata ]; then
    if [ "$CLANGMAJ" -ge 22 ]; then
        PGOMODE=use; PGOPROF="$PORT/pgo/r1.profdata"
        echo "PGO: optimising with pgo/r1.profdata (--nopgo to build without it)"
    else
        echo "PGO: off - the profile pgo/r1.profdata needs LLVM 22 or newer (found $CLANGMAJ); the build is a little slower in game."
    fi
fi

# ---------------------------------------------------------------- the two programs (as build.bat)
TC="$PORT/tools/linux/clang-cl-cross.cmake"
# configure <build dir> <exe name> <generated code dir> [more cmake options]
configure() {
    local dir="$1" name="$2" rdir="$3"; shift 3
    echo "Configuring $dir..."
    cmake -S project -B "$dir" -G Ninja -DCMAKE_TOOLCHAIN_FILE="$TC" -DCMAKE_BUILD_TYPE=RelWithDebInfo \
        -DPORT_NAME="$name" -DPS3RECOMP_DIR="$PORT/ps3recomp" -DRECOMP_DIR="$PORT/$rdir" -DRFOM_SPU_AVX2=ON \
        -DPS3RECOMP_FFMPEG_RUNTIME=ON -DPS3RECOMP_FFMPEG_INCLUDE_DIR="$PORT/ffmpeg/include" "$@" \
        > "logs/configure_$dir.log" 2>&1 || { tail -n 30 "logs/configure_$dir.log"; fail "configure failed - see logs/configure_$dir.log"; }
}
build() {
    local dir="$1"
    echo "Building $dir ($JOBS jobs, log logs/build_$dir.log)..."
    set +e
    nice -n 10 cmake --build "$dir" -j "$JOBS" 2>&1 | tee "logs/build_$dir.log" | grep -E --line-buffered '^FAILED|error:|^\[[0-9]+/[0-9]+\] Linking'
    local rc=${PIPESTATUS[0]}
    set -e
    [ "$rc" -eq 0 ] || fail "see logs/build_$dir.log (if the compiler was killed, try fewer jobs: --jobs 2)"
}
echo "============================================================"
echo " Resistance: Fall of Man - native PC build (on Linux, for Proton)"
echo " Builds two programs: the front end (EBOOT) and the game (game.self)."
echo " The first build takes a long time. Leave it running."
echo "============================================================"
configure build ResistanceFoM recompiled -DPORT_PGO="$PGOMODE" -DPORT_PGO_PROFILE="$PGOPROF"
echo "[1/2] Building the front end (EBOOT)..."
build build
configure build_game ResistanceFoMGame recompiled_game -DPORT_PGO="$PGOMODE" -DPORT_PGO_PROFILE="$PGOPROF"
echo "[2/2] Building the game (game.self)..."
build build_game

# ---------------------------------------------------------------- small helper programs
X="$PORT_XWIN_DIR"
ARCH=x86_64; [ -d "$X/sdk/lib/um/x86_64" ] || ARCH=x64
CLFLAGS=(--target=x86_64-pc-windows-msvc -fuse-ld=lld /nologo /O2 /EHsc /W0)
for d in crt/include sdk/include/ucrt sdk/include/um sdk/include/shared sdk/include/winrt; do CLFLAGS+=(/imsvc "$X/$d"); done
LIBPATHS=("/libpath:$X/crt/lib/$ARCH" "/libpath:$X/sdk/lib/um/$ARCH" "/libpath:$X/sdk/lib/ucrt/$ARCH")
# source paths stay relative: clang-cl would read an absolute /path as an option
cl() { PATH="$LLVMBIN:$PATH" "$CLANGCL" "${CLFLAGS[@]}" "$@"; }

echo "Building the launcher..."
LRES=()
if PATH="$LLVMBIN:$PATH" "$LLVMRC" -I project -I "$X/sdk/include/um" -I "$X/sdk/include/shared" -fo build/launcher.res project/launcher.rc > logs/build_launcher_rc.log 2>&1; then
    LRES=(build/launcher.res)
fi
if cl project/launcher.cpp /Fo:build/launcher.obj /Fe:Launcher.exe /link /SUBSYSTEM:WINDOWS /MANIFEST:EMBED "${LIBPATHS[@]}" "${LRES[@]}" \
      user32.lib gdi32.lib shell32.lib gdiplus.lib dwmapi.lib > logs/build_launcher.log 2>&1; then
    echo "Launcher.exe built"
else
    echo "Launcher build failed - see logs/build_launcher.log"
fi
if cl project/rfom_switch.cpp /Fo:build/rfom_switch.obj /Fe:build/RFoMSwitch.exe /link /SUBSYSTEM:WINDOWS "${LIBPATHS[@]}" user32.lib gdi32.lib \
      > logs/build_switch.log 2>&1; then
    cp -f build/RFoMSwitch.exe build_game/
else
    echo "RFoMSwitch build failed - see logs/build_switch.log"
fi
mkdir -p build/hdui
if cl /std:c++17 /D_CRT_SECURE_NO_WARNINGS /I ps3recomp/include /I ps3recomp/libs/video /I ps3recomp/third_party/miniz \
      /I ps3recomp/third_party/stb /I project project/hd_ui_pack.cpp ps3recomp/libs/video/rsx_texture_layout.c \
      ps3recomp/libs/video/rsx_native_hot.c ps3recomp/third_party/miniz/miniz.c /Fo:build/hdui/ /Fe:HDUIPack.exe \
      /link "${LIBPATHS[@]}" urlmon.lib bcrypt.lib > logs/build_hdui.log 2>&1; then
    echo "HDUIPack.exe built"
else
    echo "HDUIPack build failed - see logs/build_hdui.log"
fi

# shaderscan (the launcher's optional shader-cache seeding; https://github.com/miroslavdanutalexandru-alt/shaderscan,
# GPL-2.0): the release pinned in tools/shaderscan.version, checked against its SHA-256. Optional.
SSURL="$(sed -n 's/^url=//p' tools/shaderscan.version | tr -d '\r')"
SSSHA="$(sed -n 's/^sha256=//p' tools/shaderscan.version | tr -d '\r')"
if [ -n "${SHADERSCAN_EXE:-}" ] && [ -f "$SHADERSCAN_EXE" ]; then
    cp -f "$SHADERSCAN_EXE" tools/shaderscan.exe
elif [ -n "$SSURL" ] && ! { [ -f tools/shaderscan.exe ] && [ "$(sha256sum tools/shaderscan.exe | cut -d' ' -f1)" = "$SSSHA" ]; }; then
    if curl -fsSL --retry 2 -o tools/shaderscan.exe.part "$SSURL" 2>/dev/null \
       && [ "$(sha256sum tools/shaderscan.exe.part | cut -d' ' -f1)" = "$SSSHA" ]; then
        mv -f tools/shaderscan.exe.part tools/shaderscan.exe
    else
        rm -f tools/shaderscan.exe.part
        echo "shaderscan was not downloaded (offline?) - the launcher works without it."
    fi
fi

# run-time DLLs next to the programs (as build.bat): FFmpeg (LGPL), AMD FidelityFX (MIT). NVIDIA DLSS / Streamline
# and Intel XeSS are not set up on Linux: Proton does not offer DLSS to this port, FSR 3.1 is used instead.
for d in build build_game; do
    for f in avcodec-61.dll avutil-59.dll swresample-5.dll; do [ -f "tools/ffmpeg_bin/$f" ] && cp -f "tools/ffmpeg_bin/$f" "$d/"; done
    for f in amd_fidelityfx_loader_dx12.dll amd_fidelityfx_upscaler_dx12.dll; do [ -f "tools/ffx_bin/$f" ] && cp -f "tools/ffx_bin/$f" "$d/"; done
done
[ -f tools/ffx_bin/amd_fidelityfx_framegeneration_dx12.dll ] && cp -f tools/ffx_bin/amd_fidelityfx_framegeneration_dx12.dll build_game/

# ---------------------------------------------------------------- CPUs without AVX2 (as build_cpu.bat baseline)
if [ "$BASELINE" = auto ]; then
    BASELINE=no
    grep -qw avx2 /proc/cpuinfo 2>/dev/null || BASELINE=yes
fi
if [ "$BASELINE" = yes ]; then
    echo "Also building the variant for CPUs without AVX2 (build_baseline/)..."
    LTO=ON; [ "${PORT_LTO:-}" = "0" ] && LTO=OFF
    configure build_baseline ResistanceFoM recompiled -DPORT_ARCH=baseline -DPORT_LTO="$LTO" -DPORT_PGO=off
    configure build_baseline_game ResistanceFoMGame recompiled_game -DPORT_ARCH=baseline -DPORT_LTO="$LTO" -DPORT_PGO=off
    build build_baseline
    build build_baseline_game
    cp -f build_baseline_game/ResistanceFoMGame.exe build_baseline/
    [ -f build_baseline_game/ResistanceFoMGame.pdb ] && cp -f build_baseline_game/ResistanceFoMGame.pdb build_baseline/
    for d in build build_game; do
        for f in "$d"/*.dll "$d"/RFoMSwitch.exe; do [ -f "$f" ] && cp -f "$f" build_baseline/; done
    done
fi

for f in build/ResistanceFoM.exe build_game/ResistanceFoMGame.exe Launcher.exe; do
    [ -f "$f" ] || fail "$f is missing - see logs/."
done
echo
echo "BUILD SUCCEEDED: build/ResistanceFoM.exe and build_game/ResistanceFoMGame.exe"
echo "Play: in Steam, Add a Game > Add a Non-Steam Game > $PORT/Launcher.exe, then in its"
echo "Properties > Compatibility, force Proton (Proton Experimental or 9+). See README, \"Linux / Steam Deck\"."
