#!/usr/bin/env bash
# Build FT8.BIN for the IC-7300 homebrew loader (jeskko SDK, ABI v4).
#
# This mirrors sdk/tools/build_app.py's flags, but the FT8 app has third-party sources
# (vendored ft8_lib) that need an extra include dir and a freestanding libm, so we invoke
# the compiler directly. Point SDK= at a jeskko/ic7300-homebrew checkout.
#
#   SDK=/path/to/ic7300-homebrew ./build_bin.sh
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SDK="${SDK:-/home/user/jeskko/ic7300-homebrew}"
RT="$SDK/sdk/runtime"
INC="$SDK/sdk/include"
LIB="$HERE/third_party/ft8_lib"
OUT="${1:-$HERE/FT8.BIN}"
CC=arm-none-eabi-gcc
OBJCOPY=arm-none-eabi-objcopy

CFLAGS=(-mcpu=cortex-a9 -marm -mfloat-abi=soft -Os -std=c11
        -ffreestanding -fno-builtin -nostdlib -fno-pic
        -ffunction-sections -fdata-sections
        -D_POSIX_C_SOURCE=200809L -DNDEBUG "-DLOG_PRINTF(...)="
        -I"$INC" -I"$LIB" -I"$HERE")

RT_SRC=(crt0.S runtime.c ui_dialog.c gfx.c font.c heap.c input.c math.c libc.c audio.c)
APP_SRC=("$HERE/main.c" "$HERE/ft8_core.c" "$HERE/ft8_shim.c")
FT8_SRC=(ft8/constants.c ft8/crc.c ft8/decode.c ft8/ldpc.c ft8/message.c ft8/text.c
         common/monitor.c fft/kiss_fft.c fft/kiss_fftr.c)   # NO encode.c (RX only)

SRCS=()
for s in "${RT_SRC[@]}";  do SRCS+=("$RT/$s"); done
for s in "${APP_SRC[@]}"; do SRCS+=("$s"); done
for s in "${FT8_SRC[@]}"; do SRCS+=("$LIB/$s"); done

WORK="$HERE/build"; mkdir -p "$WORK"      # keep app.elf/app.map for the emulator test (nm symbols)
echo "Linking ${#SRCS[@]} sources -> $OUT"
"$CC" "${CFLAGS[@]}" -T "$RT/app.ld" -Wl,--gc-sections -Wl,--no-warn-rwx-segments \
      -Wl,-Map,"$WORK/app.map" "${SRCS[@]}" -lgcc -o "$WORK/app.elf"
"$OBJCOPY" -O binary "$WORK/app.elf" "$OUT"

# header / region / heap report, same checks as build_app.py
python3 - "$OUT" <<'PY'
import struct, sys
img = open(sys.argv[1], "rb").read()
APP_REGION, SIZE, MAGIC = 0x20610000, 0x100000, 0x31304248
magic, abi, entry, image_end = struct.unpack_from("<4I", img)
assert magic == MAGIC, f"bad magic {magic:#x}"
assert APP_REGION <= entry < APP_REGION + len(img), f"entry {entry:#x} outside file"
foot = image_end - APP_REGION
assert image_end <= APP_REGION + SIZE, f"image end {image_end:#x} overflows region"
print(f"{sys.argv[1]}: {len(img)} bytes, entry {entry:#x}, ABI {abi}, "
      f"RAM footprint {foot:#x} of {SIZE:#x} ({100*foot/SIZE:.1f}%)")
PY
echo "map: $WORK/app.map"
