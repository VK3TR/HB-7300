# FT8 decoder — homebrew app (design sketch)

An **on-device FT8 receive decoder** for the IC-7300, as a homebrew app on the
[jeskko/ic7300-homebrew](https://github.com/jeskko/ic7300-homebrew) SDK — the natural next step after
the [`cw-rx`](../cw-decoder-app/) CW decoder.

> **Status: design + interface scaffold, NOT a working app.** The decode math is
> [kgoba/ft8_lib](https://github.com/kgoba/ft8_lib) (MIT) — not yet vendored or wired in. Nothing
> here should be flashed. This exists to make the build concrete and to capture what the SDK does and
> doesn't give us.

## What's here
| File | State |
|---|---|
| [`DESIGN.md`](DESIGN.md) | the sketch: architecture, slot model, memory/CPU budget, time-sync, open questions — **start here** |
| `ft8_core.h` | proposed slot-oriented interface over ft8_lib (compiles; host + app build against it) |
| `main.c` | SDK app **scaffold**: slot scheduler + audio drain + waterfall UI + decode list (syntax-checked vs the real SDK headers) |
| `ft8_core.c`, `host_test.c`, `third_party/ft8_lib/` | **TODO** — the actual decode glue + vendored codec |

## Why it's harder than `cw-rx`
FT8 is a UTC-slotted, LDPC-coded, 8-FSK protocol — it needs a 15 s time grid, an FFT waterfall, and
a soft-decision decoder, not a single Goertzel bin. See `DESIGN.md §1`.

## What the SDK already settles (checked against the headers)
- ✅ **12 kHz mono audio** — ft8_lib's native rate, no resampling (`hb/audio.h`; runtime decimates 48→12 kHz).
- ✅ **Audio tap works** — same `DX_REC` stream `cw-rx` uses.
- ⚠️ **1.37 s capture ring** → the app must drain every pass and **decode incrementally** (`DESIGN.md §4a`).
- ⚠️ **No UTC/RTC API** — only relative `hb_millis()`; v1 uses manual UTC entry (`DESIGN.md §5`).
- ◻ **Heap fit (448 KB), per-slice decode cost, app size** — on-target measurements (`DESIGN.md §10`).

## License / provenance
The eventual vendored codec is **ft8_lib** (MIT, Kārlis Goba) + **kiss_fft** (BSD-3). Pin the revision
zarthur used (`9fec6ca…`) as a known-good baseline. No Icom material is included. This folder is
offered, like `cw-rx`, as a candidate contribution to the jeskko SDK once built and tested.
