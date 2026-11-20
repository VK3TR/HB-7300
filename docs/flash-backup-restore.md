# IC-7300 flash backup & restore procedure

A concrete procedure for **dumping the IC-7300's SPI-NOR flash and restoring it** — the backup that
makes a hard brick recoverable (see `recovery.md` for *why* you need it). This is the one step that
separates "recoverable" from "paperweight": a hardware restore can only put back an image you
captured **before** anything went wrong.

> ⚠️ **Read this first.** This opens the radio and attaches to the flash chip or the debug port.
> It **voids your warranty**, risks damaging the board, and is **reverse-engineered, not
> field-proven on a 7300** — no one has published a 7300 unbrick, so you are establishing the recipe,
> not following a vetted one. Everything here is at your own risk. Work on a bench, not your only rig.
> The chip identity and sizes below are inferred from the firmware and jeskko's schematic trace;
> **confirm them against your actual hardware** (a JEDEC-ID read settles the chip in seconds).

---

## 0. What you're touching

- **Chip:** a **Winbond W25-series SPI-NOR**, 3.3 V, on the RZ/A1 **SPIBSC0** controller, memory-mapped
  (XIP) at `0x18000000` (`flash-driver.md`). The active-slot marker lives at flash `0x7f0000`, which
  is exactly the **top 64 KB block of an 8 Mbyte (W25Q64, 64 Mbit) device** — so it is **almost
  certainly a W25Q64 (8 MB)**. **Confirm with a JEDEC-ID read** before trusting the size.
- **Flash map** (what a full dump contains — see `recovery.md`):

  | Flash offset | Contents |
  |---|---|
  | `0x000000`–`0x010000` | bootloader (XIP boot stub + loader) — single copy |
  | `0x010000`–`0x3fffff` | **Slot A** main image |
  | `0x400000`–`0x7effff` | **Slot B** main image |
  | `0x7f0000`–`0x7fffff` | active-slot marker (top 64 KB block) |

- **Debug/flash connector `J491`** (Icom part `6510025142`): a **JST FLT-series FFC/FFC, 10-position,
  0.5 mm pitch**, 3.3 V. Mates with a **JST `10FLT-SM2-TB`** + a 0.5 mm FFC breakout. Confirmed pinout:

  | Pin | Signal | | Pin | Signal |
  |---|---|---|---|---|
  | 1 | **3.3 V** | | 6 | MTMS (`TMS`) |
  | 2 | RES1 (`RES`, reset) | | 7 | MTDI (`TDI`) |
  | 3 | MTDO (`TDO`) | | 8 | MTRST (`TRST`) |
  | 4 | NC | | 9 | NC |
  | 5 | MTCK (`TCK`) | | 10 | **GND** |

  JTAG is left fully enabled by the firmware (`recovery.md`), so a standard ARM probe works here.

---

## Which route?

Three ways to reach the flash, cleanest-signal last:

- **Route A — in-circuit SOIC-8 clip + `flashrom`.** No debug probe; clip onto the chip, hold the
  CPU in reset so it doesn't fight the bus. Easiest kit, but in-circuit reads can be marginal (other
  parts on the 3.3 V rail load the programmer). **Best first attempt for most people.**
- **Route B — JTAG via `J491` + a J-Link.** Uses the debug port instead of clipping the chip. No
  soldering to the flash, but needs a RAM flash-loader that drives SPIBSC (more setup).
- **Route C — desolder the chip (out-of-circuit).** The cleanest, most reliable dump/restore, and the
  most invasive. Reserve for when A/B are unreliable or the radio is already dead.

Whatever route: **capture the backup first, verify it, keep two copies off-device.** Then (and only
then) consider writing.

---

## Route A — in-circuit, SOIC-8 clip + flashrom (recommended first)

**Kit:** a 3.3 V SPI programmer (CH341A — *check it's the 3.3 V-modded kind, not 5 V*; or a Bus
Pirate / FT2232 / Raspberry Pi SPI), an **SOIC-8 (SOP8) test clip**, jumper wires,
[`flashrom`](https://www.flashrom.org/).

1. **Power down and open the radio.** Disconnect DC and antenna. Locate the W25 SPI-NOR (8-pin SOIC)
   near the RZ/A1 SoC. Note pin 1 (dot/bevel).
2. **Hold the CPU in reset** so it stops driving the flash: pull **`J491` pin 2 (`RES`) to GND** (via
   `J491` pin 10). This is what lets an in-circuit clip read cleanly. (If you skip this, reads are
   often garbage because the booting CPU is hammering the bus.)
3. **Clip onto the flash**, programmer **not yet** powering it. Match SOIC pin 1 to the clip's pin 1.
4. **Identify the chip (JEDEC ID)** — this also proves your wiring:
   ```bash
   flashrom -p ch341a_spi        # lists the detected chip, e.g. "W25Q64.V"
   ```
   If it reports multiple candidates, pass the exact one with `-c` below. If it finds nothing, fix
   wiring/clip seating before going further. **Do not write anything yet.**
5. **Back up — read the whole chip twice and compare:**
   ```bash
   flashrom -p ch341a_spi -c W25Q64.V -r ic7300_stock_A.bin -VV
   flashrom -p ch341a_spi -c W25Q64.V -r ic7300_stock_B.bin -VV
   sha256sum ic7300_stock_A.bin ic7300_stock_B.bin   # the two hashes MUST match
   ```
   If the two reads differ, the read is unreliable (bus contention / clip) — **do not trust it**;
   reseat, re-confirm reset is held, retry. A trustworthy backup is two identical reads.
6. **Record the size** (should be 8 MB = 8388608 bytes for W25Q64) and keep **two copies off the
   device** (different media). Optionally sanity-check the dump decodes:
   ```bash
   # the bootloader starts with the ARM vector table; byte 0 region is real code, not 0xFF
   xxd -l 16 ic7300_stock_A.bin
   ```
   You can also point this repo's tools at it (the dump is the raw flash, i.e. the container body as
   laid out above; `reseal.py`/`depack_lzss.py` operate on `.dat` containers, not the raw flash
   image, so they won't read it directly — the dump's value is as a byte-exact restore source).

**Restore (only if you need to recover):**
```bash
flashrom -p ch341a_spi -c W25Q64.V -w ic7300_stock_A.bin -VV   # erases + writes + verifies
```
`flashrom` erases, writes, and read-back-verifies in one go; it only reprograms blocks that differ.
Keep reset held during the write, same as the read. On success it prints `VERIFIED`. Power-cycle the
radio and confirm it boots to the stock version screen.

---

## Route B — JTAG via J491 (J-Link)

Use this if you'd rather not clip the flash. Wire a **J-Link** to `J491`: pin 1 → VTref (3.3 V),
5 `TCK`, 6 `TMS`, 7 `TDI`, 3 `TDO`, 8 `TRST`, 2 `RES`/nRESET, 10 `GND`. Build a 0.5 mm FFC breakout
from a `10FLT-SM2-TB`.

The flash is **external SPI-NOR**, not CPU-internal, so J-Link can't read it as plain memory. Two
sub-options:

- **SEGGER J-Flash + an external-flash loader for SPIBSC0.** If a RAM loader for the RZ/A1 SPIBSC is
  available (SEGGER device support or a community loader), J-Flash can read/write the flash through
  the CPU. Point it at the RZ/A1 (R7S721000) device, external flash at `0x18000000`.
- **A small RAM stub using the SPIBSC register sequence we reversed.** Halt the CPU via JTAG, load a
  tiny routine into on-chip RAM (`0x2000_0000`+) that drives SPIBSC0 (`0x3FEFA000`) directly, and
  read/write pages with it. The exact register recipe is in **`flash-driver.md`**: switch SPIBSC out
  of XIP read into command mode, then `RDSR`/read for backup, or `WREN → WRSR 0 → 64 KB block erase
  (0xD8) → 256 B page program (0x02) → poll WIP` for restore. **The code and its data must be
  RAM-resident** (you're erasing the flash the CPU would otherwise execute from) and **interrupts
  off** — getting this wrong hangs or bricks the radio. This is the advanced path; prefer Route A
  unless you're comfortable here.

Dump/verify discipline is the same: **read twice, hashes must match, two copies off-device.**

---

## Route C — out-of-circuit (desolder)

Hot-air the 8-pin SOIC off, read/write it in a **SOIC-8 socket/adapter** on the programmer (same
`flashrom` commands as Route A, minus the reset trick and bus-contention worries), then resolder.
Cleanest signal and the most certain restore, but it needs rework skills and risks lifting pads. This
is the fallback when in-circuit is unreliable, and often the realistic path for an already-dead radio.

---

## Partial restore (advanced, optional)

A full-chip restore from a verified dump is the safe default. If you understand the layout you *can*
do less:
- **Un-flip a bad update:** if a new (MD5-valid but bad) image was committed, the *old* image is
  still physically in the other slot; rewriting the 64 KB marker block at `0x7f0000` to point back at
  it reverts the boot choice without touching the slots. You need to know the marker's exact encoding
  for your releases (see jeskko's `notes/firmware-update.md`); get it wrong and you brick differently.
- **Restore just the bootloader** (`0x0`–`0x10000`) from your dump if only that region was damaged.

These are precise, reversible-in-principle edits, but they assume you can already read/write the chip
reliably — so prove the full backup/restore loop first.

---

## Checklist before you ever write flash

- [ ] Radio open, W25 chip located, pin 1 identified.
- [ ] JEDEC-ID read succeeds and matches an 8 MB W25Q64 (or whatever it actually is).
- [ ] **Two full reads, identical SHA-256.** Two copies stored off-device.
- [ ] A **stock Icom `.dat`** also kept on an SD card (software path, if the radio still boots).
- [ ] Stable 3.3 V, nothing that can interrupt power mid-write.
- [ ] Reset-hold on `J491` pin 2 confirmed working (in-circuit routes).
- [ ] You've verified a **restore** writes back and `flashrom` prints `VERIFIED` — ideally rehearsed
      before you ever *need* it.

## Honest limits

- Chip part/size and the slot layout are **inferred** (firmware + schematic trace), not from Icom
  service docs. The JEDEC-ID read is the authoritative check.
- In-circuit clip reads can be unreliable on a populated board; identical double-reads are your only
  proof the dump is good.
- This procedure has **not been executed on a real 7300** by this project — it is assembled from the
  reversed flash driver, jeskko's confirmed `J491` pinout, and standard W25 SPI-NOR practice.

### Sources
- Flash driver / SPIBSC register recipe: this repo's [`flash-driver.md`](flash-driver.md); slot map: [`recovery.md`](recovery.md).
- `J491` connector + pinout, JTAG-enabled state: jeskko/ic7300-homebrew `notes/hardware-debug-access.md`.
- Tooling: [flashrom](https://www.flashrom.org/), [SEGGER J-Flash SPI](https://segger.com/jflash-spi.html), [SPI programmer best practices](https://osresearch.net/SPI-Programmer-Best-Practices/).
