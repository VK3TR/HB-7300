# Firmware update container (`.dat`) format

Fully reverse-engineered (orchestrator `FUN_20025AE4`, MD5-verify+flash `FUN_20024DB8`) and
**empirically verified end-to-end**: all four payloads MD5-match with standard MD5, each sibling
payload LZSS-decodes to its header-declared length, and the file closes exactly. This is what the
repack/reseal toolchain targets.

## Layout
The 0x2C header holds 7 little-endian u32 that describe **four LZSS payloads** — one *stored*
length for the MAIN image, then a **(stored, decoded)** length pair for each of the three
TI-DSP / FPGA components:

```
offset   size      field
0x00     4         magic  "3wfU"   (LE32 0x55667733)
0x04     12        version string  "3.112.003.16"   (not the public release number)
0x10     u32       MAIN         stored length     hdr[0]  = 0x252BF0           }
0x14     u32       DSP_PROGRAM  stored length     hdr[1]  = 0x017E46           }
0x18     u32       DSP_PROGRAM  decoded length    hdr[2]  = 0x027F08           } 0x2C
0x1C     u32       DSP_DATA     stored length     hdr[3]  = 0x0B03AC           } header
0x20     u32       DSP_DATA     decoded length    hdr[4]  = 0x0AFF08           }
0x24     u32       FPGA         stored length     hdr[5]  = 0x0AA759           }
0x28     u32       FPGA         decoded length    hdr[6]  = 0x0D1D14           }
0x2C     hdr[0]    MAIN stored data  = MAIN-CPU flash image (maps to flash 0x18000000)
         16        MD5(MAIN stored data)
         hdr[1]    DSP_PROGRAM stored data   + 16-byte MD5   (decodes to a TI "TIPA" AIS image)
         hdr[3]    DSP_DATA    stored data   + 16-byte MD5
         hdr[5]    FPGA        stored data   + 16-byte MD5   ("31601130"-prefixed bitstream)
...      2         end marker  "7e"  (0x37 0x65)
```
- **Four payloads**, concatenated, **each followed by a 16-byte `MD5()` of its own STORED
  (on-disk, still-LZSS-compressed) bytes.** The whole file closes exactly:
  `0x2C + Σ(stored) + 4·16 + 2 == filesize` (verified: 3,954,089 bytes for v1.42).
- **Verified payload table (v1.42):**

  | payload | data @file | stored len | MD5 @file | decoded len | decoded prefix |
  |---|---|---|---|---|---|
  | MAIN | 0x00002C | 0x252BF0 | 0x252C1C | (len word inside, +0x10000) | ARM image @0x20005000 |
  | DSP_PROGRAM | 0x252C2C | 0x017E46 | 0x26AA72 | 0x027F08 | `54 49 50 41` = **"TIPA"** (TI AIS, TMS320C6745) |
  | DSP_DATA | 0x26AA82 | 0x0B03AC | 0x31AE2E | 0x0AFF08 | `40 18 00 00 …` |
  | FPGA | 0x31AE3E | 0x0AA759 | 0x3C5597 | 0x0D1D14 | `33 31 36 30 31 31 33 30` = **"31601130"** (Cyclone IV) |

- All four payloads use the **same LZSS codec** (`tools/depack_lzss.py`). The MAIN image embeds its
  decoded length as a u32 at payload `+0x10000` (file `0x1002C`); the three siblings have **no
  embedded length word** — the decoder takes the decoded length straight from the header.
- **Partial updates:** a `.dat` can carry only the payloads being updated. `tools/reseal.py` walks
  the fixed MAIN → DSP_PROGRAM → DSP_DATA → FPGA order and includes a payload only while it and its
  MD5 fit before the 2-byte trailer, so MAIN-only or MAIN+DSP files reseal correctly too.
- The header semantics and offset formulas were first identified via jeskko/ic7300-homebrew and
  independently corroborated by zarthur/ic-7300_firmware (see **Related work** in the
  [README](../README.md#related-work)); the exact values above are confirmed against our own v1.42 image.

## Integrity & flashing (from the code)
- Integrity is **standard MD5 per section** — **no signature** (see `modding-feasibility.md`).
  `FUN_20024DB8` streams each section in 64 KB chunks, runs MD5 update, and **compares each
  64 KB block against current flash — only erasing+programming blocks that differ** (differential
  update, minimises wear/time). An MD5 mismatch aborts with the error path.
- File is validated first by magic `3wfU` + model-ID fields + the `7e` end marker
  (`FUN_200253F4`/`FUN_200247E4`).

## Re-sealing a modified MAIN image
Because integrity is a keyless MD5, a modified section can be re-sealed by recomputing its MD5:

```
depack_lzss.py  → modify → (re-LZSS-compress) → place back in section 0 (keep length hdr[0]
                 if it fits) → reseal.py reseal firmware.dat out.dat   # fixes section 0's MD5
```

`tools/reseal.py` verifies **all four payloads** and re-computes **every present payload's** MD5
trailer. Proven: round-trip on an unmodified file is **byte-identical**; a MAIN-image edit and a
DSP/FPGA edit each re-seal to a valid MD5; `verify` catches a single flipped byte in any payload and
`reseal` then restores all four digests. If a modification changes a payload's **length**, update
the matching header word(s) too (subsequent payloads shift).

## Status
- ✅ Header semantics (4 payloads, stored/decoded pairs), per-payload MD5 integrity, differential
  64 KB flash — all verified, incl. exact closure `0x2C + Σstored + 4·16 + 2 == filesize`.
- ✅ Working **verify + reseal for all four payloads** (`tools/reseal.py`): MAIN, DSP_PROGRAM,
  DSP_DATA, FPGA. Each sibling confirmed to LZSS-decode to its header length (TIPA / 31601130 prefixes).
- ✅ Firmware-compatible LZSS **compressor** (`tools/pack_lzss.py`, round-trip verified) and full
  **repack chain** (`tools/repack_main.py`): extract → modify → recompress → re-seal, demonstrated
  end-to-end (produces a valid .dat whose MAIN image decodes to the modified bytes).
- ◻ DSP (TI AIS "TIPA") and FPGA bitstream *internal* formats — open (needed only to meaningfully
  edit those payloads; the container can already re-seal them).
