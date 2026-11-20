#!/usr/bin/env python3
r"""End-to-end emulator test for the FT8 receiver, modelled on sstv-rx/test_emu.py.

Builds the homebrew loader firmware + flash image (from the jeskko SDK), builds FT8.BIN (our
build_bin.sh: ft8_core + vendored ft8_lib + shim), and an SD card holding \homebrew\FT8.BIN.
Boots the rz-a1h QEMU machine, opens MENU > SET > SD Card > Homebrew Apps, launches FT8, checks
the audio hook is live, then plays an FT8 recording into the fake DSP's RX audio (DX_REC L,
looped) and waits for the app to decode it. Verifies the decoded text via the app's g_last
global read over QMP, and screenshots the result.

    SDK=/path/to/ic7300-homebrew python3 test_emu.py [--wav build/FT8.wav] [--expect "CQ HB9XYZ JN47"]

This app lives outside the SDK tree, so SDK= points at a jeskko/ic7300-homebrew checkout.
"""
from __future__ import annotations
import argparse, os, subprocess, sys, time
from pathlib import Path

HERE = Path(__file__).resolve().parent
SDK = Path(os.environ.get("SDK", "/home/user/jeskko/ic7300-homebrew"))
sys.path.insert(0, str(SDK / "sdk" / "loader"))
import test_emu as lt  # noqa: E402  loader test helpers: Emu, nm, check, qmp_*, QEMU/QMP/FPS, ROW_Y

SCRATCH = HERE / "build" / "emu"


def sh(*cmd, **kw):
    subprocess.run([str(c) for c in cmd], check=True, **kw)


def shot(shots: Path, name: str):
    """QMP screendump -> PNG using `convert` (lt.Emu.shot assumes `magick`, absent here)."""
    ppm = shots / f"{name}.ppm"
    q = lt.qmp_open(lt.QMP)
    lt.qmp_cmd(q, "screendump", filename=str(ppm))
    q.close()
    subprocess.run(["convert", str(ppm), str(ppm.with_suffix(".png"))], check=True)
    ppm.unlink()


def build(wav: Path) -> tuple[Path, Path]:
    SCRATCH.mkdir(parents=True, exist_ok=True)
    # loader firmware + flash (from the SDK; firmware/7300_142.dat must be staged there)
    sh(sys.executable, SDK / "sdk/loader/build.py")
    flash = SCRATCH / "flash.bin"
    sh(sys.executable, SDK / "qemu-machine/tools/build_flash.py",
       SDK / "scratch/hb_loader_142.dat", flash, stdout=subprocess.DEVNULL)
    # our app -> FT8.BIN (keeps build/app.elf for symbol reads)
    app_bin = HERE / "FT8.BIN"
    sh("bash", HERE / "build_bin.sh", app_bin, env=dict(os.environ, SDK=str(SDK)))
    # SD card with \homebrew\FT8.BIN
    card = SCRATCH / "sdcard.img"
    sh(sys.executable, SDK / "qemu-machine/tools/build_sdcard.py", "-o", card, "--size-mb", "128",
       stdout=subprocess.DEVNULL)
    sh("mmd", "-i", f"{card}@@1M", "::homebrew")
    sh("mcopy", "-i", f"{card}@@1M", app_bin, "::homebrew/")
    if not wav.exists():
        sys.exit(f"missing {wav}; generate it with gen_ft8 (see README)")
    return flash, card


def read_cstr(qmp_path: str, addr: int, n: int = 32) -> str:
    f = SCRATCH / "g_last.bin"
    q = lt.qmp_open(qmp_path)
    lt.qmp_cmd(q, "pmemsave", val=addr, size=n, filename=str(f))
    q.close()
    b = f.read_bytes()
    return b.split(b"\x00", 1)[0].decode("latin1")


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--wav", type=Path, default=HERE / "build/FT8.wav")
    ap.add_argument("--expect", default="CQ HB9XYZ JN47")
    ap.add_argument("--shots", type=Path, default=SCRATCH / "shots")
    ap.add_argument("--boot-wait", type=float, default=30.0)
    ap.add_argument("--timeout", type=float, default=1200.0, help="wall s to wait for a decode")
    ap.add_argument("--icount", default="shift=1")
    ap.add_argument("--log", type=Path)
    args = ap.parse_args()
    args.shots.mkdir(parents=True, exist_ok=True)
    wav = args.wav.resolve()

    flash, card = build(wav)
    loader = lt.nm(SDK / "sdk/loader/build/loader.elf")
    app = lt.nm(HERE / "build/app.elf")
    api = loader["g_api"]

    for p in (lt.QMP, lt.FPS):
        Path(p).unlink(missing_ok=True)
    env = dict(os.environ, RZA1H_AF_TONE="none")
    proc = subprocess.Popen([
        str(lt.QEMU), "-M", "rz-a1h", "-display", "none", "-monitor", "none", "-serial", "none",
        "-kernel", str(flash),
        "-global", f"rza1h-riic.image={SDK / 'qemu-machine' / 'riic2_eeprom.img'}",
        "-qmp", f"unix:{lt.QMP},server,nowait",
        "-chardev", f"socket,id=fpctl,path={lt.FPS},server=on,wait=off",
        "-drive", f"if=sd,format=raw,file={card}",
    ] + ([] if args.icount == "off" else ["-icount", args.icount]),
        env=env, stdin=subprocess.DEVNULL,
        stderr=open(args.log, "w") if args.log else subprocess.DEVNULL)
    try:
        time.sleep(args.boot_wait)
        emu = lt.Emu(args.shots)
        word = lambda sym: emu.word(app[sym])

        # MENU > SET > (pg2) > SD Card > (pg2>pg3) > Homebrew Apps  (same path as sstv-rx)
        emu.press("MENU")
        emu.touch(430, 180)                    # SET
        emu.touch(448, 170)                    # page 2/2
        emu.touch(150, 117)                    # SD Card
        emu.touch(448, 170, 1.0)               # page 2/3
        emu.touch(448, 170)                    # page 3/3
        emu.touch(150, 55, 3.0)                # Homebrew Apps
        shot(args.shots, "0-picker")
        labels = lt.picker_labels(emu)
        print(f"     picker labels: {labels}")
        lt.check(labels == [b"FT8"], "picker lists FT8")
        print("     launching FT8")
        emu.touch(150, lt.ROW_Y[0], 1.0)
        t0 = time.time()
        while emu.word(api + 8) == 0 and time.time() - t0 < 90:
            time.sleep(1)
        print(f"     app resident {time.time() - t0:.0f} s after the tap")
        time.sleep(2)
        shot(args.shots, "1-launched")
        lt.check(emu.word(api + 8) != 0, "app resident (idle hook set)")
        lt.check(emu.word(api + 16) == app["audio_isr"], "audio hook installed (ABI v4)")

        s0 = word("g_samples"); time.sleep(3); s1 = word("g_samples")
        lt.check(s1 >= s0, f"audio hook alive (g_samples {s0}->{s1})")

        # play the FT8 recording into RX audio, looped so successive free-run slots get a clean window
        s = lt.qmp_open(lt.QMP)
        r = lt.qmp_cmd(s, "qom-set", path="/machine/ssif", property="af-file",
                       value=f"{wav},loop")
        s.close()
        lt.check("error" not in r, f"playing {wav.name} into DX_REC L (looped)")

        print("     waiting for a decode (soft-float FFT+LDPC under TCG is slow)...")
        t0, last = time.time(), None
        decoded = ""
        while time.time() - t0 < args.timeout:
            time.sleep(10)
            dec, slot, samp = word("g_decodes"), word("g_slot"), word("g_samples")
            if (dec, slot) != last:
                last = (dec, slot)
                print(f"     {time.time()-t0:5.0f} s  decodes={dec}  slot={slot}  samples={samp}")
            if dec > 0:
                decoded = read_cstr(lt.QMP, app["g_last"])
                break
        shot(args.shots, "2-decoded")
        lt.check(word("g_decodes") > 0, f"decoded at least one message (g_decodes={word('g_decodes')})")
        print(f"     g_last = {decoded!r}")
        lt.check(decoded.strip() == args.expect, f"decoded text == {args.expect!r} (got {decoded!r})")
        print(f"all checks passed; screenshots in {args.shots}")
    finally:
        proc.terminate(); proc.wait()


if __name__ == "__main__":
    main()
