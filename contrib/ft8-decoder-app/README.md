# FT8 decoder — homebrew app (design sketch)

An **on-device FT8 receive decoder** for the IC-7300, as a homebrew app on the
[jeskko/ic7300-homebrew](https://github.com/jeskko/ic7300-homebrew) SDK — the natural next step after
the [`cw-rx`](../cw-decoder-app/) CW decoder.

> **Status: built, host-verified, and running end-to-end in the emulator** — not yet on a real radio.
> The decode math is [kgoba/ft8_lib](https://github.com/kgoba/ft8_lib) (MIT, pinned `9fec6ca`),
> vendored under `third_party/ft8_lib/`. The host test decodes synthesized FT8 to an exact copy
> (≈ −18 dB in its noise model). `FT8.BIN` boots on jeskko's rz-a1h QEMU machine through the real
> IC-7300 firmware + homebrew loader, taps the 12 kHz RX audio, and decodes a WAV to the correct text
> on-screen (`test_emu.py`). Not hardware-tested — treat on-air use as experimental; no TX.

## What's here
| File | State |
|---|---|
| [`DESIGN.md`](DESIGN.md) | architecture, slot model, memory/CPU budget, time-sync, open questions — **start here** |
| `ft8_core.h` / `ft8_core.c` | ✅ slot interface over ft8_lib (monitor → sync → LDPC → unpack), **built & host-verified** |
| `host_test.c` + `Makefile` | ✅ encode + GFSK-synth + noise + decode self-test; `make test` runs an SNR sweep |
| `third_party/ft8_lib/` | ✅ vendored decode+encode subset of kgoba/ft8_lib (no PortAudio/WAV); `LICENSE` kept |
| `main.c` | ✅ SDK app: free-run slot scheduler + audio drain + waterfall UI + decode list, **runs in the emulator** and decodes to screen |
| `build_bin.sh` / `test_emu.py` | ✅ target build (SDK runtime + ft8_lib + shim → `FT8.BIN`) and the end-to-end emulator harness |
| `ft8_shim.c` | ✅ freestanding libc/libm shim (malloc→hb heap; string + math fns) so ft8_lib links against `-nostdlib` |

## Build & run the host test
```
make test          # builds ft8_test and runs the SNR sweep for "CQ HB9XYZ JN47"
./ft8_test "CQ HB9XYZ JN47" 0 1200   # one run: message, SNR(dB, model), audio freq(Hz)
```

## Why it's harder than `cw-rx`
FT8 is a UTC-slotted, LDPC-coded, 8-FSK protocol — it needs a 15 s time grid, an FFT waterfall, and
a soft-decision decoder, not a single Goertzel bin. See `DESIGN.md §1`.

## What the SDK already settles (checked against the headers)
- ✅ **12 kHz mono audio** — ft8_lib's native rate, no resampling (`hb/audio.h`; runtime decimates 48→12 kHz).
- ✅ **Audio tap works** — same `DX_REC` stream `cw-rx` uses.
- ⚠️ **1.37 s capture ring** → the app must drain every pass and **decode incrementally** (`DESIGN.md §4a`).
- ⚠️ **No UTC/RTC API** — only relative `hb_millis()`; v1 uses manual UTC entry (`DESIGN.md §5`).
- ✅ **Heap fit** — `ft8_heap_estimate()` = **281 KB** (default) / 199 KB (osr 1/2), under 448 KB (`DESIGN.md §6`).
- ◻ **Per-slice decode time, app size, LDPC stack** — on-target measurements (`DESIGN.md §10`).

## License / provenance
The eventual vendored codec is **ft8_lib** (MIT, Kārlis Goba) + **kiss_fft** (BSD-3). Pin the revision
zarthur used (`9fec6ca…`) as a known-good baseline. No Icom material is included. This folder is
offered, like `cw-rx`, as a candidate contribution to the jeskko SDK once built and tested.
