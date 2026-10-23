#!/bin/bash
# build.sh: black-selftest.nro, the Horizon behaviour of the calls the headless game makes (selftest.cpp)
#   -s SDK folder (default ../../../XboxXBE-Recomp)   -o out folder (default ../../out/selftest)
# Links the SDK's kernel/memory.cpp (mem::FileRead, mem::FileWrite) and core/log.cpp as the game does.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
SDK=$HERE/../../../XboxXBE-Recomp; OUT=$HERE/../../out/selftest
while getopts s:o: opt; do
    case $opt in s) SDK=$OPTARG ;; o) OUT=$OPTARG ;; *) sed -n 2,4p "$0"; exit 2 ;; esac
done
: "${DEVKITPRO:?devkitPro is needed}"
SDK=$(realpath "$SDK"); mkdir -p "$OUT"; OUT=$(realpath "$OUT")
A64=$DEVKITPRO/devkitA64; NX=$DEVKITPRO/libnx; RT=$SDK/runtime
unset CC CXX AR CFLAGS CXXFLAGS CPPFLAGS LDFLAGS
"$A64/bin/aarch64-none-elf-g++" -specs="$NX/switch.specs" -march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE \
    -std=gnu++20 -O2 -g -fexceptions -D__SWITCH__ -I"$NX/include" -I"$RT" -Wall -Wno-unused-result \
    "$HERE/selftest.cpp" "$RT/kernel/memory.cpp" "$RT/core/log.cpp" -L"$NX/lib" -lnx -o "$OUT/black-selftest.elf"
"$DEVKITPRO/tools/bin/nacptool" --create "Black self-test" "BLACK-XBERecomp" "0.1" "$OUT/black-selftest.nacp" >/dev/null
"$DEVKITPRO/tools/bin/elf2nro" "$OUT/black-selftest.elf" "$OUT/black-selftest.nro" --nacp="$OUT/black-selftest.nacp" >/dev/null
echo "built $OUT/black-selftest.nro"
