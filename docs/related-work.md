# Related work — the IC-7300 / Icom RZ/A1 landscape

Four independent projects (this one included) have reverse-engineered or modified Icom's
Renesas RZ/A1 radio firmware. They reach the **same low-level facts from different directions**,
which is the strongest validation our own findings have. This page consolidates who does what,
how they agree, and what each is best used for.

> Scope note: everything here is derived from each project's **public** repository and from our
> own analysis of a user-supplied v1.42 image. No copyrighted Icom material is reproduced. Links
> are to the upstream repos; prefer them as primary sources.

---

## The four projects at a glance

| Project | Radios | Approach | What it's best for |
|---|---|---|---|
| **[jeskko/ic7300-homebrew](https://github.com/jeskko/ic7300-homebrew)** | IC-7300 | Homebrew **app loader + SDK**, QEMU machine, deep DSP/FPGA disassembly | Running custom C apps on the radio; the most complete 7300 RE |
| **[zarthur/ic-7300_firmware](https://github.com/zarthur/ic-7300_firmware)** | IC-7300 | Independent RE + a portable **FT8** C prototype; binary repack (byte-identical) | On-device FT8; a second independent 7300 cross-check; real-hardware boot evidence |
| **[sq6emm](https://github.com/sq6emm)** (3 repos, below) | IC-705 / 9700 / 905 / 7610 | **Binary-patch** official firmware; web patcher; A/B bootloader | Shipping real custom firmware to users, legally; the RZ/A1 *family* picture |
| **HB-7300** (this repo) | IC-7300 | From-scratch RE + repack chain; full CI-V map; a CW decoder app | Container/CI-V/flash reference; independent corroboration; the `cw-rx` app |

sq6emm is actually three repos:
- **[icom-custom-firmware](https://github.com/sq6emm/icom-custom-firmware)** — the product: browser patcher + patch JSONs for IC-705/9700, transverter support, ICAP FT8/Q65/WSPR decoders, A/B bootloader, PSKReporter.
- **[ic705-ic9700-ic905-bring-your-own-firmware](https://github.com/sq6emm/ic705-ic9700-ic905-bring-your-own-firmware)** — RE tooling + format docs for those three radios (+ IC-7610, IC-7610 in docstrings).
- **[ic7610-bring-your-own-firmware](https://github.com/sq6emm/ic7610-bring-your-own-firmware)** — IC-7610 container tooling + an `UNDOCUMENTED_CIV.md` service-command map.

A fifth name appears only as a credit: both sq6emm and we point at **kgoba/ft8_lib** (MIT, Kārlis
Goba) as the FT8 codec basis; zarthur's FT8 prototype is built directly on it.

---

## The shared substrate — what everyone agrees on

All four projects independently converge on the same container and boot facts for the RZ/A1 line.
For the **IC-7300** specifically, our values, zarthur's, and jeskko's agree to the constant:

| Fact | Value (IC-7300) | Confirmed by |
|---|---|---|
| Container magic | `3wfU` (LE32 `0x55667733`) | us, zarthur |
| Header size | `0x2C` (44 bytes), 7× u32 | us, zarthur |
| End marker | `7e` (`0x6537`), fixed tag (not a checksum) | us, zarthur |
| LZSS | ring N=4096, init idx `0xFEE`, `0x00` fill, flags LSB-first, literal=bit 1, offset `lo\|((hi&0xF0)<<4)`, length `(hi&0xF)+3` (3–18), length-terminated | us, zarthur, jeskko (via sq6emm) |
| Main image load base | `0x20005000` (reset vector `0x20005050`) | us, zarthur |
| RAM loader | flash `0x18004000` → RAM `0x20004000`, 0x650 B | us, zarthur |
| Decompressor routine | `0x2000425C` | us, zarthur |
| Resources/fonts | main payload `+0x210000` (file `0x21002C`) | us, zarthur |
| MD5 routines | init `0x2003c860`, round `0x2003c890`, update `0x2003d38c` (+ finalize `0x2003d458`, trailer-check `0x200247E4`) | us, zarthur |
| Integrity | **standard MD5 per payload, no signature** | us, zarthur, sq6emm, jeskko |

Three parties reaching the decompressor address and the MD5 routine addresses independently is
about as solid as black-box RE corroboration gets.

### The container, fully modelled (folded in from zarthur)

The 0x2C header's 7 u32 are not 7 sections — they are **four LZSS payloads**: MAIN (stored length
only; its decoded length is embedded at payload `+0x10000`), then a **(stored, decoded)** pair for
each of **DSP_PROGRAM**, **DSP_DATA**, **FPGA**. Each payload is followed by a 16-byte MD5 of its
*stored* (on-disk, compressed) bytes; a 2-byte `7e` trailer closes the file exactly
(`0x2C + Σstored + 4·16 + 2 == filesize`). Verified on our v1.42 image — DSP_PROGRAM decodes to a
TI AIS **`"TIPA"`** image (for the TMS320C6745), FPGA to a **`"31601130"`** bitstream (Cyclone IV).
Full table and offsets in [`container-format.md`](container-format.md); re-sealable by
[`tools/reseal.py`](../tools/reseal.py).

---

## The RZ/A1 family picture (from sq6emm's cross-model tooling)

sq6emm supports the *newer* radios, which share the 7300's LZSS and keyless-MD5 integrity but add
a per-model obfuscation layer the 7300 lacks:

| Radio | Autokey obfuscation | Container magic | Load base | Signature? |
|---|---|---|---|---|
| **IC-7300** | **none — stream in the clear** | `3wfU` | `0x20005000` | **no** (MD5 only) |
| IC-7610 | yes — KEY `0xf4`, START `0x34` | (not parsed) | `0x20005000` | **no** (MD5 only) |
| IC-705 | yes — KEY `0x19` | `D\0wU` (`44 00 77 55`) | — | **no** (MD5 only) |
| IC-9700 | yes — KEY `0x2b` | `D\0wU` | — | **no** (MD5 only) |
| IC-905 | yes — KEY `0x7f` | — | — | **likely yes** — "multi-component" integrity, not MD5-prefix |

Takeaways:
- **No cryptographic signature across 7300 / 7610 / 705 / 9700.** Our "the 7300 has no signature,
  so a recomputed MD5 re-seals a mod" conclusion holds for four radios — and sq6emm *ships* patched
  firmware on exactly that basis.
- The **IC-905 (newest) breaks the pattern** with a multi-component integrity layout that reads like
  real crypto (RSA). The signature-free window is a *generation*, not forever.
- The **"autokey"** is a position-dependent XOR-feedback **byte scramble, not crypto** — obfuscation
  only. The 7300 predates it (≈2016 vs the 7610's ≈2017), so **7300 containers are the simplest case
  in the whole family to patch.**

sq6emm's IC-7610 `UNDOCUMENTED_CIV.md` also maps **factory/service CI-V commands** from the dispatch
table (e.g. `1A D0` factory reset, `1A E0` alignment, `1A D7` MAC write — password-gated, destructive).
Those are the kind of entries our 7300 CI-V table ([`CI-V_command_table.md`](CI-V_command_table.md))
could be extended with, since 7300 and 7610 share a dispatcher structure.

---

## Who is ahead on what

**jeskko/ic7300-homebrew** — the most complete 7300 project. Full TMS320C6745 DSP disassembly, the
SCIF5/McASP1 DSP link, the FPGA link, a QEMU RZ/A1 machine that boots real firmware to a drawn
screen, and an SDK that runs C apps from the SD card. Prefer it for the **DSP/FPGA side** and
anything **dynamic**. (Our `cw-rx` app targets this SDK.)

**zarthur/ic-7300_firmware** — a second from-scratch 7300 RE, self-described "100% vibe coded,"
aimed at **on-device FT8**. Two standout contributions:
1. The **fully-resolved 4-payload container header** (now in our tooling).
2. The landscape's most valuable **empirical** datapoint: a *modified, MD5-resealed, unsigned* image
   **booted on real hardware** (display-only, label changed; stock-return/recovery unproven) —
   turning the no-signature conclusion from analytical to **observed on metal**.
   Its FT8 is a **portable C prototype** on `kgoba/ft8_lib`, host-validated vs WSJT-X, **built from
   scratch** (no decoder framework enabled). Thinner on CI-V, flash protocol, and RTOS.

**sq6emm** — the only one **shipping custom firmware to users**, via a clean legal model (download
your own official `.dat`, patch locally in the browser, output gated to a byte-identical tested
build, A/B bootloader for rollback). Best for the **family taxonomy**, the **autokey** layer, and a
reusable **distribution pattern**.

**HB-7300 (this repo)** — ahead on **CI-V** (full dispatch table `@0x2018AA2C` → 143-case handler,
all command-0x16 subcommands), the **SPI-NOR flash write protocol**, **RTOS identification**
(FreeRTOS), the **menu string resolver**, and the **OpenVG/font graphics pipeline**. Plus a from-
scratch LZSS compressor + verified repack/reseal chain, and the `cw-rx` homebrew app. Value: an
**independent corroboration** of the shared substrate, reached without reference to the others.

---

## What this means for custom features on the IC-7300

The constellation maps cleanly onto the two routes to new features:

- **Homebrew-app route** (no firmware patch): write a portable C core + host test, wrap it in
  jeskko's SDK. Our **`cw-rx`** CW decoder is the worked example; zarthur's **FT8** codec is the
  obvious next payload in the same shape. Lowest risk — runs on an existing loader.
- **Firmware-patch route** (modify the MAIN image): proven viable — no signature (analytically by
  us/sq6emm, **empirically** by zarthur's boot), full extract→modify→recompress→reseal chain in
  [`tools/`](../tools/), and sq6emm's web-patcher/A/B-bootloader model is directly adaptable to the
  7300 (which, lacking the autokey layer, is the *easiest* family member to patch).

**FT8 specifically:** sq6emm's FT8 on the 705/9700 uses Icom's own on-radio **"ICAP" decoder
framework**. The **7300 has no such framework** (it predates it), so — as zarthur's from-scratch
prototype confirms — FT8-on-7300 means **building the codec**, exactly like the CW decoder, not
flipping on a dormant one.

---

## Credits / licenses

All projects referenced here are **MIT-licensed** (per each repo's sidebar/README at time of review).
This page summarizes their public work for comparison; it copies none of their code. The FT8 codec
lineage is **kgoba/ft8_lib** (MIT). Treat each upstream repo as the authoritative source for its own
details — summaries here were captured at a point in time and those projects move quickly.
