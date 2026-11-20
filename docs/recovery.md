# IC-7300 flash layout, brick windows, and recovery

**What this is:** a honest map of how the IC-7300 updates its firmware, where the real
brick risk is, and how a bricked radio can (and cannot) be recovered. It exists because the
apps and modding tools in this repo involve flashing, and anyone doing that should understand
the failure modes *first*.

**Provenance / confidence.** The SD-card *update procedure* is **officially documented** (Icom's
manual, §15). Everything about the **slot layout, boot path, and recovery internals is
reverse-engineered** — from this repo's analysis and, in more depth, from
[jeskko/ic7300-homebrew](https://github.com/jeskko/ic7300-homebrew)'s live boot trace
(`notes/firmware-update.md`, `notes/base-loader.md`, `notes/hardware-debug-access.md`). A public
search found **no official Icom recovery procedure** for a non-booting radio, and **no published
account of anyone un-bricking a 7300** by hardware. So the hardware-recovery path below is
*standard practice for this class of flash*, not a field-proven 7300 recipe. Treat it accordingly.

---

## Flash map (SPI-NOR, XIP at `0x18000000`)

The CPU boots **execute-in-place directly from the SPI-NOR flash** (reset vector at
`0x18000000`). The updater (`firmware_update_main` @ `0x20025ae4`) uses a dual A/B scheme:

| Flash offset | Address | Region | A/B? |
|---|---|---|---|
| `0x000000`–`0x010000` | `0x18000000` | **Bootloader** (`base.dat`, the XIP boot stub + loader) | **single copy** |
| `0x010000`–… | `0x18010000` | **Slot A** — main image (body + fonts + DSP/FPGA chunks) | A |
| `0x400000`–… | `0x18400000` | **Slot B** — main image | B |
| `0x7f0000` | `0x187f0000` | 16-byte **active-slot marker** | — |

**How an update works:** it rewrites the bootloader region first, writes the new main image into
**whichever slot is *not* currently active**, then flips the marker so the *next* boot uses the new
slot. Integrity is a plain **MD5** (no signature — see `container-format.md`).

---

## Where the brick risk actually is

Two findings from the updater's disassembly matter:

1. **The bootloader region is a single copy and is rewritten first, every update.** Lose power
   while `0x0`–`0x10000` is mid-write and you've damaged the *only* bootloader; since the CPU
   XIPs from there, there is nothing to fall back to → **hard brick.** This is the one unprotected
   window and matches sq6emm's "power loss during the bootloader rewrite can require a flash
   programmer" warning.
2. **Flash is written *before* the MD5 is checked, and in the common case the marker flips before
   the check too.** So the updater does **not** roll back a bad write, and an image that is broken
   but still MD5-valid will be committed and booted. MD5 guards against transit corruption, not a
   bad build.

**What *is* protected:** because the new image goes to the *inactive* slot and the marker flips
only after the body write completes, a failure *during the body write* leaves the marker pointing
at your **old, good slot** — the next boot runs the old firmware. So the bulk of an update is
genuinely A/B-safe; the exposure is the bootloader write and the post-flip "bad-but-valid image".

---

## Recovery options, best case to worst

1. **Radio still boots to its menus → re-update from SD (documented).** Copy a stock Icom `.dat`
   into the card's `IC-7300` folder, **MENU → SET → SD Card → Firmware Update**, pick it, hold YES.
   This is the normal, Icom-documented path. It only works if the radio boots far enough to reach
   the menu. (A transient "DSP/FPGA firmware is wrong version" after an update has been reported to
   sometimes clear after a few full power cycles — try that before assuming the worst.)
2. **Radio won't boot → there is NO software/SD recovery.** No boot-ROM SD-recovery, no held-key
   recovery, and no serial-boot fallback was found in the boot trace. A dead bootloader is dead as
   far as software is concerned.
3. **Hardware recovery — the real backstop (needs tools + a prior backup).**
   - **JTAG/SWD:** the RZ/A1's debug port is **left fully enabled** — firmware never disables it,
     the pins power up in JTAG mode, invasive debug (`BSCANP`) is hardwired on. There is a **10-pin
     JST FLT connector (J491)** on the board (0.5 mm pitch, 3.3 V, standard ARM JTAG). A J-Link
     (or SWD-capable probe) should work once wired, and can reprogram the SPI flash (e.g. **J-Flash
     SPI**).
   - **Direct SPI-NOR:** the chip is a standard **W25-series SPI-NOR** (driver mapped in
     `flash-driver.md`). An **SOIC-8 clip + a CH341A or similar programmer + `flashrom`** can read
     and restore it out-of-circuit or in-circuit. **Match 3.3 V.**

---

## The one thing that makes recovery possible: back up *first*

Hardware recovery restores a **known-good image you captured beforehand**. Without that dump, a
hard brick is much harder to undo and may be unrecoverable. Before flashing anything:

1. **Dump the whole flash** via J491 (JTAG → J-Flash SPI) or an SOIC-8 clip on the W25 chip
   (`flashrom -p <programmer> -r ic7300_stock_full.bin`), **twice**, and **verify** the reads match.
   Keep a copy off-device.
2. Also keep a **stock Icom `.dat`** on an SD card for the software path.
3. Use a **stable supply** and never interrupt power during any write.
4. Have the **probe/clip wired and tested** *before* you flash — not after something goes wrong.

---

## Bottom line

- Officially documented recovery: **only the SD *update*** (which needs a booting radio).
- Automatic protection: **real for the body write** (A/B slots), **absent for the bootloader write**.
- Non-booting radio: **hardware only** — JTAG on J491 or an SPI programmer on the W25 chip — and
  **only if you took a verified backup first.**
- This is reverse-engineered and **not field-proven on a 7300**; you would be establishing the
  recipe, not following one.

### Sources
- Icom update procedure: IC-7300 Full Manual, §15 ([manualslib](https://www.manualslib.com/manual/1106166/Icom-Ic-7300.html?page=142)); [Icom firmware downloads](https://www.icomamerica.com/support/firmware_driver/2389/).
- Reported post-update symptoms / no official unbrick: [RadioReference IC-7300 thread](https://forums.radioreference.com/threads/problem-with-icom-ic-7300.463584/).
- General W25 SPI-NOR recovery practice: [flashrom](https://firmware.buspirate.com/software/flashrom), [SEGGER J-Flash SPI](https://segger.com/jflash-spi.html), [SPI programmer best practices](https://osresearch.net/SPI-Programmer-Best-Practices/).
- Slot/boot/JTAG internals: this repo's `flash-driver.md` / `container-format.md`, and jeskko/ic7300-homebrew's `notes/firmware-update.md` & `notes/hardware-debug-access.md`.
