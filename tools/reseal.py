#!/usr/bin/env python3
"""Verify and re-seal an IC-7300 firmware .dat container (all four payloads).

Container format (reverse-engineered; see docs/container-format.md). The 0x2C header holds
7 little-endian u32 that describe FOUR LZSS payloads — one *stored* length for the MAIN image,
then a (stored, decoded) pair for each of the three TI-DSP / FPGA components:

  off    field
  0x00   "3wfU" magic (4) + version string (12)
  0x10   u32  MAIN         stored length            (hdr[0])
  0x14   u32  DSP_PROGRAM  stored length            (hdr[1])
  0x18   u32  DSP_PROGRAM  decoded length           (hdr[2])
  0x1C   u32  DSP_DATA     stored length            (hdr[3])
  0x20   u32  DSP_DATA     decoded length           (hdr[4])
  0x24   u32  FPGA         stored length            (hdr[5])
  0x28   u32  FPGA         decoded length           (hdr[6])
  0x2C   MAIN stored data        + 16-byte MD5(stored data)
         DSP_PROGRAM stored data + 16-byte MD5   (decodes to a TI "TIPA" AIS boot image)
         DSP_DATA    stored data + 16-byte MD5
         FPGA        stored data + 16-byte MD5   (decodes to a "31601130"-prefixed bitstream)
         2-byte end marker "7e" (0x37 0x65)

Each MD5 is a standard MD5 over that payload's STORED (on-disk, still-LZSS-compressed) bytes.
There is NO signature — recomputing an MD5 re-seals a modified payload (see modding-feasibility.md).
The whole file closes exactly: 0x2C + sum(stored) + 4*16 + 2 == filesize.

Empirically verified on v1.42 (3,954,089 bytes): all four MD5s match, the trailer is 0x6537,
and each sibling payload LZSS-decodes to its header-declared decoded length (DSP_PROGRAM ->
"TIPA..." AIS, FPGA -> "31601130..."). Model corroborated independently by zarthur/ic-7300_firmware.

A partial update .dat may carry only some payloads; this tool includes a payload only while it
and its MD5 fit before the 2-byte trailer, so MAIN-only or MAIN+DSP files reseal correctly too.

Usage:
  python3 reseal.py verify  firmware.dat
  python3 reseal.py reseal  firmware.dat out.dat     # recompute every present payload's MD5 in place
"""
import sys, struct, hashlib

HDR = 0x2C
# (name, header index of STORED length, header index of DECODED length or None)
PAYLOADS = [
    ("main",        0, None),
    ("dsp_program", 1, 2),
    ("dsp_data",    3, 4),
    ("fpga",        5, 6),
]

def header(d):
    if d[:4] != b"3wfU":
        sys.exit(f"not an IC-7300 container: magic={d[:4]!r} (expected b'3wfU')")
    return list(struct.unpack_from("<7I", d, 0x10))

def walk(d):
    """Yield (name, data_off, stored_len, decoded_len, md5_off) for each payload PRESENT.

    Walks the fixed MAIN/DSP_PROGRAM/DSP_DATA/FPGA order using the header's stored lengths.
    A payload counts as present only if its data and 16-byte MD5 fit before the final 2-byte
    trailer, so the same walk handles a full .dat and a partial-update .dat."""
    hdr = header(d)
    off = HDR
    for name, si, di in PAYLOADS:
        stored = hdr[si]
        decoded = hdr[di] if di is not None else None
        if off + stored + 16 > len(d) - 2:        # would collide with / overrun the trailer
            break
        yield name, off, stored, decoded, off + stored
        off += stored + 16

def verify(path):
    d = open(path, "rb").read()
    hdr = header(d)
    print(f"magic={d[:4]!r} version={d[4:16].decode('latin1')!r} size=0x{len(d):x} ({len(d)})")
    print("header u32:", [hex(x) for x in hdr])
    end = HDR
    all_ok = True
    for name, off, stored, decoded, mo in walk(d):
        ok = hashlib.md5(d[off:off+stored]).digest() == d[mo:mo+16]
        all_ok &= ok
        extra = f" decoded=0x{decoded:x}" if decoded is not None else " (MAIN @0x18000000)"
        print(f"  {name:11} 0x{off:07x}..0x{off+stored:07x} stored=0x{stored:x}  "
              f"MD5 {'OK' if ok else 'MISMATCH'}{extra}")
        end = mo + 16
    trailer = d[end:end+2]
    closes = end + 2 == len(d)
    tag = "OK" if trailer == b"\x37\x65" else "UNEXPECTED"
    print(f"  trailer @0x{end:x}: {trailer.hex()} ({tag}); file closes exactly: {closes}")
    if end + 2 != len(d):
        print(f"  note: 0x{len(d)-(end+2):x} trailing bytes beyond the modelled payloads")
    return all_ok and closes

def reseal(path, out):
    d = bytearray(open(path, "rb").read())
    for name, off, stored, decoded, mo in walk(d):
        d[mo:mo+16] = hashlib.md5(bytes(d[off:off+stored])).digest()
        print(f"  resealed {name:11}: wrote MD5 @0x{mo:07x}")
    open(out, "wb").write(d)
    print(f"wrote {out} ({len(d)} bytes)")

if __name__ == "__main__":
    if len(sys.argv) < 3:
        print(__doc__); sys.exit(1)
    cmd = sys.argv[1]
    if cmd == "verify":
        sys.exit(0 if verify(sys.argv[2]) else 1)
    elif cmd == "reseal" and len(sys.argv) == 4:
        reseal(sys.argv[2], sys.argv[3])
    else:
        print(__doc__); sys.exit(1)
