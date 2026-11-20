# Third-party notices

This app vendors third-party source under `third_party/`. Our own files (`ft8_core.*`, `main.c`,
`host_test.c`, `DESIGN.md`, `Makefile`) are part of the HB-7300 project; the vendored code keeps its
own licenses, reproduced via the files below.

## ft8_lib — MIT License
- Upstream: https://github.com/kgoba/ft8_lib
- Author: Kārlis Goba (YL3JG) and contributors
- Pinned commit: `9fec6ca39886edbf96f4f5e71edc76da5074e871`
- Vendored subset (decode + encode path only; no PortAudio/WAV I/O):
  `third_party/ft8_lib/ft8/` (constants, crc, decode, encode, ldpc, message, text, debug.h),
  `third_party/ft8_lib/common/monitor.{c,h}` + `common.h`,
  `third_party/ft8_lib/fft/` (kiss_fft, kiss_fftr, _kiss_fft_guts.h).
- Full license text: `third_party/ft8_lib/LICENSE`.
- The GFSK synthesis in our `host_test.c` (`gfsk_pulse` / `synth_gfsk`) is adapted from ft8_lib's
  `demo/gen_ft8.c` and is likewise covered by ft8_lib's MIT license.

## Kiss FFT — BSD-3-Clause License
- Bundled within ft8_lib under `third_party/ft8_lib/fft/`.
- Author: Mark Borgerding.
- The BSD-3-Clause notice is carried in the headers of those files.

No Icom firmware, fonts, or other Icom material is included here or anywhere in this repository.
