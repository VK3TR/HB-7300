# FT8 decoder — homebrew app design sketch (IC-7300)

**Status: design + interface scaffold. Not built, not tested.** Unlike the `cw-rx` app (which is
host-verified), this is a plan. The decode math is not ours — it's **[kgoba/ft8_lib](https://github.com/kgoba/ft8_lib)**
(MIT), the same codec [zarthur/ic-7300_firmware](https://github.com/zarthur/ic-7300_firmware) built
its FT8 prototype on. This document is the bridge between that codec and the jeskko homebrew SDK.

RX **decode only** for v1 (no TX — see §8). Same two-route reasoning as the rest of this repo: an
app on jeskko's loader is far lower-risk than patching firmware, and FT8 on the 7300 must be *built*
(the 7300 has no "ICAP" decoder framework — see `../../docs/related-work.md`).

---

## 1. Why FT8 is much harder than CW

`cw-rx` is a single-tone, self-timed decoder with no coding and no clock. FT8 is none of those:

| | CW (`cw-rx`) | FT8 |
|---|---|---|
| Signal | 1 tone, on/off | 8-FSK, 79 symbols, 6.25 Hz tone spacing |
| Timing | self-timed, free-running | **UTC-locked 15 s T/R slots**; TX is 12.64 s |
| Symbol rate | ~7–40 WPM adaptive | fixed 6.25 baud (0.16 s/symbol) |
| Sync | threshold + gaps | 3× **Costas** 7-tone arrays in the waveform |
| Coding | none (direct Morse) | **LDPC(174,91)** FEC + CRC-14, then callsign/grid packing |
| DSP | 1 Goertzel bin | windowed **FFT waterfall** + candidate search + soft-decision LDPC |
| Output | ASCII stream | structured messages (`CALL1 CALL2 GRID`, reports, 73) |

So the app is three problems stacked: **(a)** get UTC-aligned 15 s audio slots, **(b)** feed them to
ft8_lib, **(c)** fit the whole thing in **≤1 MB code + 448 KB heap** on a ~400 MHz Cortex-A9.

---

## 2. The lucky break: 12 kHz native, no resampling (confirmed in the SDK)

FT8's reference sample rate in ft8_lib is **12000 Hz**, and `hb/audio.h` confirms the app receives
**12 kHz mono int16** (`HB_AUDIO_RATE == 12000`) — the DX_REC tap is 48 kHz but *the runtime already
averages groups of 4 down to 12 kHz* ("the DSP has already low-passed the stream, so the plain
4-sample average is the only decimation filter"). So **no resampling in the app** — the stream drops
straight into ft8_lib's monitor. ft8_lib's per-symbol block is
`block_size = sample_rate × symbol_period = 12000 × 0.16 = 1920` samples. Same tap `cw-rx` uses,
"before the AF gain knob, so volume doesn't change it."

---

## 3. Architecture (mirrors `cw-rx`: portable core + host test + SDK app)

```
contrib/ft8-decoder-app/
  DESIGN.md        this file
  ft8_core.h       our thin interface over ft8_lib: slot-oriented decode + heap estimate
  ft8_core.c       glue: configure monitor, stream audio, run sync+LDPC, format messages   [TODO]
  main.c           SDK app: slot scheduler + audio capture + waterfall UI + decode list     [SCAFFOLD]
  host_test.c      feed a known FT8 WAV (or ft8_lib-encoded signal) and check the decodes    [TODO]
  third_party/ft8_lib/   vendored subset of kgoba/ft8_lib (pinned)                           [TODO]
  THIRD_PARTY_NOTICES.md MIT (ft8_lib) + BSD-3 (kiss_fft)
```

**Vendored ft8_lib subset (decode path only):** `ft8/constants.c`, `ft8/crc.c`, `ft8/ldpc.c`,
`ft8/decode.c`, `ft8/message.c` (+ `text.c`/`unpack.c` as that revision splits them), and
`fft/kiss_fftr` + `kiss_fft`. **Encode is not needed for RX** (leave `ft8/encode.c` out of the app;
the host test may use it to synthesize signals). **Pin the exact revision zarthur used
(`9fec6ca39886edbf96f4f5e71edc76da5074e871`)** as a known-good baseline before chasing newer APIs.

The only radio dependency is in `main.c` (audio/gfx/input/clock). `ft8_core.*` and ft8_lib are pure
C + float — so `host_test.c` builds and runs the *identical* decode path on a PC, exactly how we
validated `cw-rx`.

---

## 4. Slot model — the control flow

FT8 lives on a 15 s grid (slots start at UTC seconds :00/:15/:30/:45). Each slot:

```
 t=0.0s   slot begins (UTC-aligned)      -> ft8_slot_begin(): clear the waterfall
 0.0..12.8s  capture: drain hb_audio_read -> ft8_feed() in 1920-sample blocks
 ~12.8s   capture window closed
 12.8..15s  ft8_slot_decode(): Costas sync search -> candidates -> LDPC -> messages
            (the ~2.3 s of slot silence after TX is compute headroom; the next slot's
             early audio can overlap-capture while we finish)
```

ft8_lib's `monitor` is **streaming**: you feed it block-by-block and it builds the waterfall
incrementally. **We never buffer the whole 12.8 s of raw audio** — that matters for memory (§6). One
`int16_t[1920]` block buffer is enough; the waterfall is the only large structure.

### 4a. Two hard SDK constraints on the scheduler (confirmed in `hb/audio.h`)

1. **The capture ring is only 16384 samples (~1.37 s).** If the app doesn't call `hb_audio_read()`
   often enough, samples are *dropped* (counted in `hb_audio_overruns()`). FT8 cannot tolerate gaps
   in the capture window, so the main loop **must drain audio every pass** and feed `ft8_feed()`
   immediately — never accumulate.
2. **Therefore the decode cannot block.** `ft8_slot_decode()` is seconds of work on a 400 MHz A9, but
   the app is cooperative (`hb_yield()` each pass keeps the radio UI/audio alive) and the ring would
   overrun in 1.37 s if we stalled in decode. So decode must be **incremental**: `ft8_slot_decode()`
   does a *bounded slice* of sync/LDPC work per call and returns; `main.c` keeps calling it across
   successive UI passes — still draining audio between slices — until it reports done. It should
   finish within the slot's ~2.2 s quiet tail; if it spills into the next slot, we keep draining so we
   don't corrupt that slot's capture (at worst we skip decoding one slot under heavy load). *(This
   makes `ft8_slot_decode` a step function; the interface in `ft8_core.h` is written so it can return
   "more work pending" — an implementation detail to finalize against measured per-slice cost.)*

---

## 5. Time synchronization — the #1 integration risk

FT8 decode needs to know where the 15 s slot starts, to ≈±1–2 s (ft8_lib's sync search tolerates a
small time offset via its candidate DT search, but not a whole-slot error). Sources, best first:

**Confirmed in the SDK headers: there is NO wall-clock/UTC/RTC API.** `hb/firmware.h`'s "Time"
section exposes only `fw_tick_ms` (RTOS tick, +1/ms) and `hb/input.h` only `hb_millis()` (ms since
boot, **wraps after ~49 days**). Both are *relative*. The radio keeps a user-set clock internally,
but it isn't surfaced to apps. So UTC must come from one of:

1. **Manual UTC entry + free-run (v1).** The operator types the current UTC once; the app anchors it
   to `hb_millis()` and derives the 15 s grid. The 1 kHz tick drifts only milliseconds over a
   session, so one entry lasts a long operating period; offer a quick "+/- nudge" control.
2. **A firmware entry point for the radio clock (best, needs SDK help).** `hb/firmware.h` already
   exposes raw firmware function pointers (`FW_FN(...)`). If jeskko (or we) locate the RTC/clock read
   in the v1.42 image — zarthur noted an RTC exists — it can be added there and read directly,
   eliminating manual entry. **This is the cleanest fix and the first thing to raise with jeskko.**
3. **Auto-align by decode yield (v2).** Sweep the assumed slot phase over a few offsets across a
   couple of minutes; lock to the phase that maximizes successful LDPC decodes. Needs no accurate
   clock, but adds code and a cold-start delay.

**Decision for the sketch:** v1 ships with manual UTC entry (option 1) and exposes the slot phase so
`main.c` owns the scheduler; option 2 is the target once a clock entry point is found.

---

## 6. Memory & CPU budget (the other hard constraint)

Heap ceiling is **448 KB**. The waterfall dominates. ft8_lib allocates an 8-bit magnitude waterfall
of roughly:

```
mag_bytes ≈ max_blocks × time_osr × freq_osr × num_bins
  max_blocks ≈ slot / symbol_period ≈ 15 / 0.16 ≈ 93
  num_bins   ≈ decode_bandwidth / (sample_rate / nfft),  nfft = block_size × freq_osr
```

Worked cases (sample_rate 12 kHz, block 1920):

| decode BW | time_osr | freq_osr | num_bins | waterfall | verdict |
|---|---|---|---|---|---|
| 0–3000 Hz | 2 | 2 | ~960 | **~350 KB** | fits 448 KB but tight (best sensitivity) |
| 0–3000 Hz | 1 | 2 | ~960 | ~180 KB | comfortable, some sensitivity loss |
| 0–1500 Hz | 2 | 2 | ~480 | ~175 KB | comfortable, half-band only |

Plus FFT scratch (kiss_fftr for nfft=3840: tens of KB), candidate list, and LDPC working set (small).
**Conclusion: feasible, but memory-bound — decode bandwidth and oversampling must be tuned to the
heap. `ft8_core` takes `f_min/f_max/time_osr/freq_osr` as config, and `ft8_heap_estimate()` lets
`main.c` pick a safe profile at startup.** The exact fit is **the first thing to measure on target.**

**CPU:** the FFTs are cheap (~186 real-FFTs of 3840 pts per slot). The cost is sync search + LDPC
(hundreds of iterations). ft8_lib does this in well under a second on a PC; on a 400 MHz A9 expect a
few seconds — which fits the ~2.3 s tail + next-slot overlap. zarthur reports the codec running on
this class of part (less sensitive than WSJT-X: their first run scored 0/1, 4/5, 14/23 vs WSJT-X),
so **CPU is not the blocker; memory and time-sync are.**

---

## 7. UI (FT8 is a natural fit for the 480×272 canvas)

- **Waterfall strip** (top): ft8_lib already computes the magnitude waterfall we need to draw — reuse
  it, scrolling one column per slot or live per block. FT8's whole band in one view.
- **Decode list** (scrolling): `HH:MM:SS  +dB  DT  FREQ  MESSAGE` per decode, newest at bottom
  (same scroll buffer idea as `cw-rx`).
- **Status bar**: UTC + slot countdown ring, current band activity count, selected decode BW/osr
  profile, pitch/passband. Tap to cycle decode-BW profile (memory/sensitivity trade); EXIT / ✕ quits.
- Tapping a decode could pre-fill a reply (v2, needs TX).

---

## 8. TX — explicitly out of scope for v1

RX decode is the whole of v1. TX would need: ft8_lib `encode` → 79 tones → **audio injected into the
transmit path** and **PTT keyed** in hard 15 s sync. Two blockers: (1) the SDK may not expose a TX
audio-injection path at all (big unknown — RX tap ≠ TX feed); (2) automatic keying raises real
regulatory duties (identification, band/privilege limits, unattended-operation rules). Defer until RX
is solid and the SDK's TX capability is known. The vendored `encode.c` stays available for the host
test regardless.

---

## 9. Validation plan (same philosophy as `cw-rx`)

1. **Host first.** `host_test.c`: synthesize an FT8 signal with ft8_lib `encode` (+ added noise), run
   it through `ft8_core`, assert the decoded text matches — down a sweep of SNR. Also decode a few
   real off-air WAVs and compare against WSJT-X (match zarthur's methodology).
2. **Emulator.** Run in jeskko's QEMU with a WAV played into the RX slot (as `cw-rx` was exercised),
   check slot scheduling, waterfall draw, and memory headroom.
3. **On-target measurement.** Confirm the heap profile fits and a slot's decode completes within
   budget. Only then consider a radio.

---

## 10. Status of the integration questions

| # | Question | Status |
|---|---|---|
| 1 | Wall-clock/UTC for slot sync | **Answered: none in the SDK.** Only relative `hb_millis`/`fw_tick_ms`. v1 = manual UTC entry; target = add a firmware clock entry point (§5). |
| 2 | Audio rate & tap | **Answered:** app gets 12 kHz mono int16 (runtime decimates 48 kHz→12 kHz), ft8_lib-native (§2). |
| 3 | Capture without drops | **Answered + constrains design:** 1.37 s ring → drain every pass, decode incrementally (§4a). |
| 4 | Heap fit | **Open — measure.** Does 448 KB hold `osr 2/2`, 0–3000 Hz waterfall + FFT scratch, or must we drop to `osr 1`/narrower? (§6) |
| 5 | Per-slice decode cost | **Open — measure.** Tune `ft8_slot_decode`'s work-slice so it finishes in the ~2.2 s tail without starving the audio drain (§4a). |
| 6 | App size with ft8_lib linked | **Open — confirm** under the 1 MB limit (decode-only subset is small). |

Q1–Q3 are settled from the SDK headers; the remaining three are **on-target measurements**, not
design unknowns. The interface in [`ft8_core.h`](ft8_core.h) is written so `main.c` and `host_test.c`
can be built against it now (incremental decode included), with `ft8_core.c` filled in against a
pinned ft8_lib once Q4–Q6 are measured in the emulator / on hardware.
