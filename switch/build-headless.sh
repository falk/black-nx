#!/bin/bash
# build-headless.sh: black-headless.nro, the game without a renderer, to measure the game thread on a Switch
#   -s SDK folder (default ../XboxXBE-Recomp)   -g generated code of default.xbe   -o out folder (default out/switch)
# Generated code builds with clang (devkitA64's GCC stops with an internal error on it), the host code with
# devkitA64's GCC, linked by devkitA64 with libnx.
set -e
KEEP_GOING=${KEEP_GOING:-0}
REPO=$(cd "$(dirname "$0")/.." && pwd)
SDK=$REPO/../XboxXBE-Recomp; OUT=$REPO/out/switch; GEN=$REPO/generated/game
while getopts s:g:o: opt; do
    case $opt in s) SDK=$OPTARG ;; g) GEN=$OPTARG ;; o) OUT=$OPTARG ;; *) sed -n 2,5p "$0"; exit 2 ;; esac
done
[ -n "$GEN" ] || { sed -n 2,5p "$0"; exit 2; }
: "${DEVKITPRO:?devkitPro is needed}"
SDK=$(realpath "$SDK"); GEN=$(realpath "$GEN"); mkdir -p "$OUT/gen" "$OUT/host"; OUT=$(realpath "$OUT")
A64=$DEVKITPRO/devkitA64; NX=$DEVKITPRO/libnx; H=$SDK/harness; RT=$SDK/runtime; SW=$REPO/switch/headless
GCCVER=$("$A64/bin/aarch64-none-elf-gcc" -dumpversion)
TOML=${TOML:-$REPO/out/switch/tools/_deps/tomlplusplus-src/include}
[ -d "$TOML" ] || { echo "toml++ headers not found: set TOML to tomlplusplus/include"; exit 1; }
JOBS=${JOBS:-$(nproc)}
unset CC CXX AR CFLAGS CXXFLAGS CPPFLAGS LDFLAGS

COMMON="-std=gnu++20 -O2 -g ${EXTRA_CXXFLAGS:-} -fno-omit-frame-pointer -fPIE -ffunction-sections -fdata-sections -fno-strict-aliasing -ffp-contract=off -D__SWITCH__"
CLANG="clang++ --target=aarch64-none-elf -mcpu=cortex-a57 $COMMON -fno-math-errno -w -nostdinc++
    -isystem $A64/aarch64-none-elf/include/c++/$GCCVER -isystem $A64/aarch64-none-elf/include/c++/$GCCVER/aarch64-none-elf
    -isystem $A64/aarch64-none-elf/include"
GXX="$A64/bin/aarch64-none-elf-g++ -march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft $COMMON -fexceptions"
HOSTINC="-I$REPO/src -I$RT -I$GEN -I$H -I$H/compat -I$SW -I$NX/include -I$TOML -DXBE_RAW_VERTICES=0 -DXBE_RENDER_THREAD=0
    -include $H/compat/windows.h -include $SW/black_compat.h"

# Cache both the compile configuration and headers, so verification flags and
# SDK/header edits cannot silently link stale objects from an earlier build.
gen_key=$({ printf '%s\n' "$CLANG" "$RT" "$GEN"; find "$RT/cpu" "$GEN" -maxdepth 1 -name '*.h' -type f -print0 | sort -z | xargs -0 sha256sum; } | sha256sum | cut -c1-16)
GENOBJ=$OUT/gen-$gen_key
host_key=$({ printf '%s\n' "$GXX" "$HOSTINC"; find "$RT" "$REPO/src" "$SW" "$H" "$GEN" -type f \( -name '*.h' -o -name '*.inc' \) -print0 | sort -z | xargs -0 sha256sum; } | sha256sum | cut -c1-16)
HOST=$OUT/host-$host_key
mkdir -p "$GENOBJ" "$HOST"

# generated code: an object is rebuilt when its partition is newer
export CLANG RT GEN OUT GENOBJ
ls "$GEN"/x86_recomp.*.cpp "$GEN"/x86_func_mapping.cpp | xargs -P "$JOBS" -I{} sh -c '
    o="$GENOBJ/$(basename {} .cpp).o"; [ "$o" -nt {} ] && exit 0
    $CLANG -I"$RT" -I"$GEN" -c {} -o "$o.tmp" && mv "$o.tmp" "$o"'

# host code: the SDK runtime but its Windows parts and those Black replaces, Black's portable sources, the harness's
# headless stand ins, this folder
SRC="$(ls "$RT"/*/*.cpp | grep -Ev '/runtime/app/|/core/input.cpp|/gpu/(d3d12|render_thread).cpp|/audio/xaudio2.cpp|/xdk/(d3d8|dsound).cpp')
     $RT/app/game_files.cpp
     $(ls "$REPO"/src/*/*.cpp | grep -Ev '/src/(audio/xaudio2|d3d/(d3d12|render_thread)|common/(monitor|cursor|mouse_keyboard|focus_pause|menu|game_files)).cpp')
     $H/app_headless.cpp $H/null_backend.cpp $H/reference_renderer.cpp
     $(ls "$SW"/*.cpp)"
objs=(); compile_jobs=()
for f in $SRC; do
    o=$HOST/$(echo "${f#/}" | tr / _ | sed 's/\.cpp$/.o/'); objs+=("$o")
    [ -s "$o" ] && [ "$o" -nt "$f" ] && continue
    echo "compile ${f#$REPO/}"
    ( $GXX $HOSTINC -c "$f" -o "$o.tmp" && mv "$o.tmp" "$o" ) &
    compile_jobs+=("$!")
    [ "$(jobs -r | wc -l)" -ge "$JOBS" ] && wait -n
done
for j in "${compile_jobs[@]}"; do wait "$j" || exit 1; done

"$A64/bin/aarch64-none-elf-g++" -specs="$NX/switch.specs" -g -march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE \
    -Wl,--gc-sections -Wl,--wrap=__cxa_throw -Wl,--wrap=threadCreate -Wl,--wrap=threadClose -Wl,-Map,"$OUT/black-headless.map" "${objs[@]}" "$GENOBJ"/*.o -L"$NX/lib" -lnx -o "$OUT/black-headless.elf"
"$DEVKITPRO/tools/bin/nacptool" --create "Black (headless)" "BLACK-XBERecomp" "0.1" "$OUT/black-headless.nacp" >/dev/null
"$DEVKITPRO/tools/bin/elf2nro" "$OUT/black-headless.elf" "$OUT/black-headless.nro" --nacp="$OUT/black-headless.nacp" >/dev/null
echo "built $OUT/black-headless.nro"
