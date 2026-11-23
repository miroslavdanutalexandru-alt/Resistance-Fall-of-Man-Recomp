#!/usr/bin/env bash
# Setup for Linux (and Steam Deck through a desktop distro / distrobox): the Linux counterpart of Setup.exe /
# Setup.bat, without PowerShell or Windows. It does the same five steps from your own copy of the game and
# builds the same Windows programs, which you then play through Proton. See README, "Linux / Steam Deck".
#
#   ./setup_linux.sh [--game <.iso or game folder>] [--jobs N] [--nopgo] [--regen] [--check-only]
#                    [--ffmpeg-zip <zip>] [--accept-microsoft-license]
#     --game         your disc copy of Resistance: Fall of Man (US, BCUS98107): a decrypted .iso or a folder with
#                    PS3_GAME in it (the same kinds RPCS3 uses). Asked for when needed and not given.
#     --jobs N       compile jobs (default 4); 16 GB RAM: 4 at most, 8 GB: 2
#     --nopgo        build without the PGO profile (needed with LLVM older than 22)
#     --regen        generate the game code again even if it is up to date
#     --check-only   only check the tools and the game copy, change nothing
#     --ffmpeg-zip   use this FFmpeg 7.1 win64-lgpl-shared zip instead of downloading it
#     --accept-microsoft-license   accept Microsoft's licence for the MSVC libraries / Windows SDK without asking
# Needs: python3 (3.8+, with venv), cmake, ninja, LLVM 22+ (clang, lld, llvm: clang-cl, lld-link, llvm-rc),
#        curl, tar; 7-Zip (7z/7zz) for UDF disc images. About 25 GB free space.
set -euo pipefail
cd "$(dirname "$0")"
PORT="$(pwd)"

# ---------------------------------------------------------------- this game (setup.ps1 has the same values)
GAME_NAME='Resistance: Fall of Man'
TITLE_ID='BCUS98107'
ELF_SHA1=('EBOOT.elf=a33494999e0bc60296bb7ab159d4da420f4e25e5' 'game.elf=45cca7e7ec8e3cec0b9bccc1d204f3cf7159b468')
SELF_FILES=('EBOOT.BIN' 'game.self')
REGEN=(tools/rfom_regen.py --eboot game/EBOOT.elf --game game/game.elf)
GEN_DIRS=(recompiled recompiled_game project/spu)
GEN_MARKER='recompiled_game/ppu_recomp.h'
KEYS_URL='https://raw.githubusercontent.com/RPCS3/rpcs3/master/rpcs3/Crypto/key_vault.cpp'
XWIN_URL='https://github.com/Jake-Shadle/xwin/releases/download/0.10.0/xwin-0.10.0-x86_64-unknown-linux-musl.tar.gz'
XWIN_SHA256='d870eb4b2f390878af6da1ccd3cf321d22fcb72720984853b4be732ae597fc88'
MS_LICENSE='https://go.microsoft.com/fwlink/?LinkId=2086102'

GAME=''; JOBS="${PORT_JOBS:-4}"; NOPGO=''; FORCE_REGEN=''; CHECK_ONLY=''; FFZIP=''; ACCEPT_MS=''
while [ $# -gt 0 ]; do
    case "$1" in
        --game) GAME="$2"; shift 2 ;;
        --game=*) GAME="${1#*=}"; shift ;;
        --jobs) JOBS="$2"; shift 2 ;;
        --jobs=*) JOBS="${1#*=}"; shift ;;
        --nopgo) NOPGO=1; shift ;;
        --regen) FORCE_REGEN=1; shift ;;
        --check-only) CHECK_ONLY=1; shift ;;
        --ffmpeg-zip) FFZIP="$2"; shift 2 ;;
        --ffmpeg-zip=*) FFZIP="${1#*=}"; shift ;;
        --accept-microsoft-license) ACCEPT_MS=1; shift ;;
        -h|--help) sed -n '2,20p' "$0"; exit 0 ;;
        *) echo "Unknown option: $1 (see ./setup_linux.sh --help)"; exit 2 ;;
    esac
done
[ -n "${RFOM_REGEN:-}" ] && [ "${RFOM_REGEN}" != 0 ] && FORCE_REGEN=1

step() { echo; echo "== $*"; }
info() { echo "   $*"; }
fail() { echo; echo "SETUP FAILED: $*"; exit 1; }
mkdir -p logs tools_cache
exec > >(tee -a logs/setup_linux.log) 2>&1
echo "---- setup_linux.sh $(date '+%Y-%m-%d %H:%M:%S')"

# pinned downloads: read from setup.ps1 so both setups always use the same files
ps_str() { sed -n "s/^\\\$$1 *= *'\\([^']*\\)'.*/\\1/p" setup.ps1 | head -n1; }
FFMPEG_URL="$(ps_str FFmpegUrl)"; FFMPEG_SHA256="$(ps_str FFmpegSha256 | tr 'A-F' 'a-f')"
FFXFG_URL="$(ps_str FfxFgUrl)"; FFXFG_SHA256="$(ps_str FfxFgSha256 | tr 'A-F' 'a-f')"
[ -n "$FFMPEG_URL" ] && [ -n "$FFMPEG_SHA256" ] || fail "could not read the FFmpeg download from setup.ps1."

sha256_of() { sha256sum "$1" | cut -d' ' -f1; }
download() {   # download <url> <file> [sha256]
    local url="$1" out="$2" want="${3:-}"
    curl -fL --retry 3 --progress-bar -o "$out.part" "$url" || { rm -f "$out.part"; return 1; }
    if [ -n "$want" ] && [ "$(sha256_of "$out.part")" != "$want" ]; then
        rm -f "$out.part"; echo "   The download of $url is not the expected file (SHA-256 mismatch)."; return 1
    fi
    mv -f "$out.part" "$out"
}

# ---------------------------------------------------------------- 1. tools
step "1/5  Build tools"
DISTRO=''; [ -r /etc/os-release ] && DISTRO="$(. /etc/os-release; echo "${ID:-} ${ID_LIKE:-}")"
install_hint() {
    case " $DISTRO " in
        *" steamos "*) echo "   SteamOS: its system is read-only. Build in a distrobox container (for example Arch or Ubuntu, see README),
   or on another Linux PC, then copy the port folder over." ;;
        *" arch "*) echo "   sudo pacman -S --needed python cmake ninja clang lld llvm curl 7zip" ;;
        *" fedora "*|*" rhel "*) echo "   sudo dnf install python3 cmake ninja-build clang lld llvm curl 7zip" ;;
        *" opensuse"*|*" suse "*) echo "   sudo zypper install python3 cmake ninja clang lld llvm curl 7zip" ;;
        *" debian "*|*" ubuntu "*) echo "   sudo apt install python3 python3-venv cmake ninja-build clang lld llvm curl 7zip
   LLVM 22 or newer: if your release has an older one, add it from https://apt.llvm.org (sudo ./llvm.sh 22 all)." ;;
        *) echo "   Install with your package manager: python3, cmake, ninja, clang, lld, llvm (22 or newer), curl, 7zip." ;;
    esac
}
find_llvm() {
    local t="$1" d
    if [ -n "${PORT_LLVM_BIN:-}" ] && [ -x "$PORT_LLVM_BIN/$t" ]; then echo "$PORT_LLVM_BIN/$t"; return 0; fi
    if command -v "$t" >/dev/null 2>&1; then command -v "$t"; return 0; fi
    for d in $(ls -d /usr/lib/llvm-*/bin /usr/lib64/llvm*/bin 2>/dev/null | sort -t- -k2 -V -r); do
        if [ -x "$d/$t" ]; then echo "$d/$t"; return 0; fi
    done
    return 1
}
[ "$(uname -m)" = x86_64 ] || fail "the port is built for x86-64 PCs (this is $(uname -m))."
missing=()
for t in python3 cmake ninja curl tar sha256sum; do command -v "$t" >/dev/null 2>&1 || missing+=("$t"); done
for t in clang-cl lld-link llvm-rc llvm-lib; do find_llvm "$t" >/dev/null || missing+=("$t"); done
if [ ${#missing[@]} -gt 0 ]; then
    echo "   These tools are missing: ${missing[*]}"
    install_hint
    fail "install the missing tools, then run ./setup_linux.sh again."
fi
CLANGCL="$(find_llvm clang-cl)"
CLANGMAJ="$("$CLANGCL" --version 2>/dev/null | sed -n 's/.*clang version \([0-9][0-9]*\).*/\1/p' | head -n1)"; CLANGMAJ="${CLANGMAJ:-0}"
if [ "$CLANGMAJ" -lt 18 ]; then install_hint; fail "LLVM / clang 18 or newer is needed (22 or newer recommended), found $CLANGMAJ ($CLANGCL)."; fi
if [ "$CLANGMAJ" -lt 22 ] && [ -z "$NOPGO" ]; then
    info "LLVM $CLANGMAJ: the build skips the PGO profile (it needs LLVM 22+); the game runs a little slower. LLVM 22+ is recommended."
fi
python3 -c 'import sys; sys.exit(0 if sys.version_info >= (3, 8) else 1)' || fail "Python 3.8 or newer is needed."
info "LLVM: $(dirname "$CLANGCL") (clang $CLANGMAJ), $(cmake --version | head -n1), ninja $(ninja --version), $(python3 --version)"

# Python with pycryptodome (decrypting the game files): the system one if it has it, else a venv in tools_cache/
PY=python3
if ! python3 -c 'import Crypto' 2>/dev/null; then
    VENV="$PORT/tools_cache/linux_venv"
    if [ ! -x "$VENV/bin/python" ] || ! "$VENV/bin/python" -c 'import Crypto' 2>/dev/null; then
        if [ -n "$CHECK_ONLY" ]; then
            info "To do: the Python package pycryptodome (installed into tools_cache/linux_venv)."
        else
            info "Installing the Python package pycryptodome into tools_cache/linux_venv..."
            python3 -m venv "$VENV" || { install_hint; fail "could not create a Python venv (Debian/Ubuntu: sudo apt install python3-venv)."; }
            "$VENV/bin/python" -m pip install --quiet --disable-pip-version-check pycryptodome || fail "could not install pycryptodome."
        fi
    fi
    [ -x "$VENV/bin/python" ] && PY="$VENV/bin/python"
fi

# Microsoft's C runtime + Windows SDK (the Windows headers and libraries), downloaded from Microsoft with xwin
export PORT_XWIN_DIR="${PORT_XWIN_DIR:-$PORT/tools_cache/msvc_linux}"
xwin_ok() { [ -f "$PORT_XWIN_DIR/crt/include/vcruntime.h" ] && [ -f "$PORT_XWIN_DIR/sdk/include/um/windows.h" ] \
            && ls "$PORT_XWIN_DIR"/sdk/lib/um/*/kernel32.lib >/dev/null 2>&1; }
if ! xwin_ok; then
    if [ -n "$CHECK_ONLY" ]; then
        info "To do: the MSVC libraries and Windows SDK (about 1 GB from Microsoft, with xwin)."
    else
        info "The MSVC libraries and the Windows SDK are Microsoft's. xwin (https://github.com/Jake-Shadle/xwin) downloads"
        info "them from Microsoft's servers for you (about 1 GB), under Microsoft's licence: $MS_LICENSE"
        if [ -z "$ACCEPT_MS" ]; then
            [ -t 0 ] || fail "run with --accept-microsoft-license to accept Microsoft's licence without a prompt."
            read -r -p "   Do you accept Microsoft's licence? [y/N] " a
            case "$a" in y|Y|yes|YES) ;; *) fail "Microsoft's licence was not accepted." ;; esac
        fi
        mkdir -p tools_cache/xwin
        XW="tools_cache/xwin/xwin"
        if [ ! -x "$XW" ]; then
            download "$XWIN_URL" tools_cache/xwin/xwin.tar.gz "$XWIN_SHA256" || fail "could not download xwin from github.com."
            tar -xzf tools_cache/xwin/xwin.tar.gz -C tools_cache/xwin --strip-components=1
            rm -f tools_cache/xwin/xwin.tar.gz
        fi
        rm -rf "$PORT_XWIN_DIR"
        "$XW" --accept-license --arch x86_64 --cache-dir tools_cache/xwin_cache splat --output "$PORT_XWIN_DIR" \
            || fail "xwin could not download the MSVC libraries / Windows SDK (see above). Run ./setup_linux.sh again to retry."
        rm -rf tools_cache/xwin_cache
        xwin_ok || fail "the MSVC libraries / Windows SDK are incomplete in $PORT_XWIN_DIR."
    fi
fi
xwin_ok && info "MSVC libraries + Windows SDK: OK ($PORT_XWIN_DIR)"
command -v 7zz >/dev/null 2>&1 || command -v 7z >/dev/null 2>&1 || command -v 7za >/dev/null 2>&1 \
    || info "Note: 7-Zip (7z) is not installed. It is only needed if your .iso is a UDF image."

# ---------------------------------------------------------------- 2. game copy
step "2/5  Your copy of the game"
PROFILE=tools/installer/game_profile.psd1
GF="$PORT/gamefiles"
game_present() {
    [ ! -e "$GF.copying" ] || return 1
    local r
    for r in $(sed -n "s/^ *Required *= *@(\(.*\)).*/\1/p" "$PROFILE" | tr -d "'," ); do [ -f "$GF/$r" ] || return 1; done
}
if ! game_present; then
    while [ -z "$GAME" ]; do
        [ -t 0 ] || fail "the game copy is missing: run with --game <your .iso or game folder>."
        echo "   Where is your copy of $GAME_NAME (US disc, $TITLE_ID)? A decrypted .iso, or the folder with PS3_GAME"
        echo "   in it (for example RPCS3's games folder). You can drag it into this window."
        read -r -p "   Path: " GAME
        GAME="${GAME%\'}"; GAME="${GAME#\'}"; GAME="${GAME%\"}"; GAME="${GAME#\"}"
        if [ -n "$GAME" ]; then
            rc=0; "$PY" tools/linux/port_linux.py check-game "$PROFILE" "$GAME" || rc=$?
            if [ $rc -eq 2 ]; then
                read -r -p "   Use this copy anyway? [y/N] " a
                case "$a" in y|Y|yes|YES) ;; *) GAME='' ;; esac
            elif [ $rc -ne 0 ]; then
                GAME=''
            fi
        fi
    done
    if [ -n "$CHECK_ONLY" ]; then
        "$PY" tools/linux/port_linux.py check-game "$PROFILE" "$GAME" || [ $? -eq 2 ] || fail "this game copy can't be used (see above)."
        echo; echo "Check finished (--check-only: nothing was changed)."; exit 0
    fi
    "$PY" tools/linux/port_linux.py copy-game "$PROFILE" "$GAME" "$GF" || fail "the game files could not be copied (see above)."
elif [ -n "$CHECK_ONLY" ]; then
    info "Game files: $GF (already copied)"
    echo; echo "Check finished (--check-only: nothing was changed)."; exit 0
fi
mkdir -p "$GF/cache/$TITLE_ID" "$GF/game/$TITLE_ID/USRDIR"
info "Game files: $GF"

# ---------------------------------------------------------------- 3. decrypt
step "3/5  Decrypting the game program files"
mkdir -p game
need=''
for e in "${ELF_SHA1[@]}"; do
    n="${e%%=*}"; h="${e#*=}"
    { [ -f "game/$n" ] && [ "$("$PY" tools/linux/port_linux.py hash sha1 "game/$n")" = "$h" ]; } || need=1
done
if [ -n "$need" ]; then
    if [ ! -f tools/key_vault.cpp ]; then
        info "Fetching the PS3 application keys from RPCS3's public source code..."
        download "$KEYS_URL" tools/key_vault.cpp || fail "could not download key_vault.cpp from github.com (no internet?)."
    fi
    args=()
    for s in "${SELF_FILES[@]}"; do args+=("$GF/PS3_GAME/USRDIR/$s"); done
    "$PY" tools/unself.py --keys tools/key_vault.cpp --out game "${args[@]}" || fail "decrypting ${SELF_FILES[*]} failed."
    for e in "${ELF_SHA1[@]}"; do
        n="${e%%=*}"; h="${e#*=}"
        [ "$("$PY" tools/linux/port_linux.py hash sha1 "game/$n")" = "$h" ] \
            || fail "game/$n is not the expected version. The port supports the unpatched disc ($TITLE_ID v1.00); a game update or another release won't match."
    done
fi
info "${ELF_SHA1[*]%%=*}: OK"

# ---------------------------------------------------------------- 4. game code
step "4/5  Generating the game code from your copy"
STAMP="${GEN_DIRS[0]}/.linux_gen_stamp"
GENHASH="$("$PY" tools/linux/port_linux.py gen-hash "$PORT")"
why=''
if [ ! -f "$GEN_MARKER" ]; then why=new
elif [ -n "$FORCE_REGEN" ]; then why=forced
elif [ "$(cat "$STAMP" 2>/dev/null)" != "$GENHASH" ]; then why=changed
fi
if [ -n "$why" ]; then
    info "Generating the game code (10-25 minutes)..."
    SNAP=''
    if [ "$why" != new ]; then
        SNAP=tools_cache/gen_snapshot.json
        "$PY" tools/linux/port_linux.py gen-snapshot "$PORT" "$SNAP" "${GEN_DIRS[@]}"
    fi
    rm -f "$STAMP"
    PYTHONUTF8=1 PYTHONUNBUFFERED=1 "$PY" "${REGEN[@]}" || fail "generating the game code failed (see the messages above)."
    if [ -n "$SNAP" ]; then "$PY" tools/linux/port_linux.py gen-restore "$PORT" "$SNAP"; rm -f "$SNAP"; fi
    [ -f "$GEN_MARKER" ] || fail "the code generator did not write $GEN_MARKER."
    echo "$GENHASH" > "$STAMP"
else
    info "Already generated (--regen to force)."
fi

# ---------------------------------------------------------------- 5. build
step "5/5  Building"
FFD=tools/ffmpeg_bin
ff_ok() { [ -f "$FFD/avcodec-61.dll" ] && [ -f "$FFD/avutil-59.dll" ] && [ -f "$FFD/swresample-5.dll" ] \
          && head -n1 "$FFD/LICENSE.txt" 2>/dev/null | grep -q 'GNU LESSER GENERAL PUBLIC LICENSE'; }
if [ -n "$FFZIP" ] || ! ff_ok; then
    zip="tools_cache/${TITLE_ID}_ffmpeg_lgpl.zip"
    if [ -n "$FFZIP" ]; then
        [ -f "$FFZIP" ] || fail "--ffmpeg-zip: there is no file at $FFZIP"
        zip="$FFZIP"; from="$FFZIP"
    else
        info "Fetching FFmpeg 7.1, LGPL build (~60 MB, for the game's videos)..."
        download "$FFMPEG_URL" "$zip" "$FFMPEG_SHA256" || fail "could not download FFmpeg ($FFMPEG_URL).
If that link is gone, download an FFmpeg 7.1 'win64-lgpl-shared-7.1' zip from https://github.com/BtbN/FFmpeg-Builds/releases and run:
./setup_linux.sh --ffmpeg-zip <path of the zip>"
        from="$FFMPEG_URL"
    fi
    "$PY" tools/linux/port_linux.py ffmpeg "$zip" "$FFD" || fail "this FFmpeg zip can't be used (see above)."
    printf '%s\n' 'FFmpeg shared libraries, LGPL v3 (see LICENSE.txt), unmodified, from:' "$from" \
        'FFmpeg source code: https://github.com/FFmpeg/FFmpeg (release/7.1). Build scripts: https://github.com/BtbN/FFmpeg-Builds' > "$FFD/SOURCE.txt"
    [ -n "$FFZIP" ] || rm -f "$zip"
fi
info "FFmpeg (LGPL build): OK"
FG=tools/ffx_bin/amd_fidelityfx_framegeneration_dx12.dll
if [ ! -f "$FG" ] && [ -n "$FFXFG_URL" ]; then
    info "Fetching AMD FidelityFX frame generation (optional, FSR 3 Frame Generation, MIT)..."
    mkdir -p tools/ffx_bin
    download "$FFXFG_URL" "$FG" "$FFXFG_SHA256" || info "Not downloaded - the FSR 3 Frame Generation choice stays off."
fi

bargs=(--jobs "$JOBS")
[ -n "$NOPGO" ] && bargs+=(--nopgo)
./build_linux.sh "${bargs[@]}" || fail "the build failed - see logs/build_build.log and logs/build_build_game.log.
If the compiler was killed (out of memory), run ./build_linux.sh --jobs 2 (it continues where it stopped)."

echo
echo "SETUP COMPLETE."
echo "Play it through Proton: in Steam, Games > Add a Non-Steam Game to My Library > Browse > $PORT/Launcher.exe,"
echo "then the game's Properties > Compatibility > Force the use of a specific Steam Play compatibility tool > Proton"
echo "(Proton Experimental or 9.0+). The tools in tools_cache/ are only needed to build; delete that folder to save space."
