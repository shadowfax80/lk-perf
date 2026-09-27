#!/usr/bin/env python3
"""Calibrate the highest stable UART baud rate between this host and the
Raspberry Pi 4B, for faster image/sample-dump transfers than the default
115200 (~11 KiB/s).

Loads experiments/pi4-baudcal/kernel7l.img via the existing
pi4-serialboot chainloader (still at 115200 -- that link is untouched
and stays the safe, always-works fallback). The test payload then lets
this script try a series of candidate higher rates *without* a
power-cycle between attempts: switch, PING/PONG to confirm sync, run a
byte-exact echo stress test at a few block sizes, then explicitly
switch back to 115200 before trying the next candidate. A candidate
that doesn't sync (no PONG) means the target already reverted itself to
115200 -- this script follows it back down and continues.

Usage:
    python scripts/pi4_baud_calibrate.py [--port COM8] [--log baudcal.log]

Requires the Pi to be sitting at the pi4-serialboot "SBOOT?" prompt --
power-cycle it first (or leave it freshly powered on with that image on
the SD card).
"""
from __future__ import annotations

import argparse
import struct
import sys
import time
import zlib
from pathlib import Path

try:
    import serial
except ImportError:
    sys.exit("error: pyserial is required (python -m pip install pyserial)")

sys.path.insert(0, str(Path(__file__).parent))
from pi4_serial_boot import Console, resolve_port, send_image, wait_for, read_line

REPO_ROOT = Path(__file__).parent.parent
DEFAULT_IMAGE = REPO_ROOT / "experiments" / "pi4-baudcal" / "kernel7l.img"

# Must match experiments/pi4-baudcal/main.c's BAUD_TABLE exactly, same
# order (index 0 is the fixed 115200 fallback every revert lands on).
BAUD_TABLE = [115200, 230400, 460800, 921600, 1000000, 1500000, 2000000, 3000000]

STRESS_SIZES = [4 * 1024, 32 * 1024, 64 * 1024]
STRESS_REPEATS = 3


def switch_and_confirm(port: serial.Serial, console: Console, idx: int) -> bool:
    """Send 'S'+idx at the current baud, switch this side to the same
    rate, and require PONG within ~2s. Returns whether the rate synced.
    On failure, leaves the port at 115200 (following the target's own
    revert) and returns False."""
    rate = BAUD_TABLE[idx]
    port.write(b"S" + bytes([idx]))
    port.flush()
    port.baudrate = rate
    port.reset_input_buffer()
    port.write(b"PING")
    port.flush()

    line = read_line(port, time.monotonic() + 2.0)
    if line == "PONG":
        return True

    # Didn't sync -- the target has (or will shortly) revert itself to
    # 115200 on its own timeout. Follow it down; don't trust `line`,
    # it may be noise framed at the wrong rate.
    port.baudrate = BAUD_TABLE[0]
    port.reset_input_buffer()
    read_line(port, time.monotonic() + 1.0)  # best-effort "REVERTED", not required
    return False


def stress_once(port: serial.Serial, size: int) -> dict:
    import os
    data = os.urandom(size)
    crc = zlib.crc32(data) & 0xFFFFFFFF

    start = time.monotonic()
    port.write(b"X" + struct.pack("<I", size) + data + struct.pack("<I", crc))
    port.flush()

    deadline = time.monotonic() + max(2.0, size / max(port.baudrate / 20, 1))
    reply = read_line(port, deadline)
    rxcrc_ok = (reply == "RXCRC OK")

    echoed = bytearray()
    while len(echoed) < size and time.monotonic() < deadline:
        chunk = port.read(size - len(echoed))
        echoed += chunk
    elapsed = time.monotonic() - start

    roundtrip_ok = (bytes(echoed) == data)
    return {
        "rxcrc_ok": rxcrc_ok,
        "roundtrip_ok": roundtrip_ok,
        "got_bytes": len(echoed),
        "elapsed": elapsed,
    }


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--image", default=str(DEFAULT_IMAGE))
    ap.add_argument("--port", default="auto")
    ap.add_argument("--log", help="append everything received to this file")
    ap.add_argument("--wait", type=float, default=None,
                     help="seconds to wait for the SBOOT? prompt (default: forever)")
    args = ap.parse_args()

    with open(args.image, "rb") as f:
        image = f.read()

    console = Console(args.log)
    port_name = resolve_port(args.port)
    try:
        port = serial.Serial(port_name, 115200, timeout=0.1)
    except serial.SerialException as e:
        sys.exit(f"error: can't open {port_name} ({e}). Is PuTTY still holding it?")

    results = []
    with port:
        port.reset_input_buffer()
        send_image(port, console, image, args.wait)

        banner = wait_for(port, console, ("BAUDCAL ready",), 3.0)
        print(f"target: {banner}")

        for idx, rate in enumerate(BAUD_TABLE):
            print(f"\n--- {rate} baud ---")
            if not switch_and_confirm(port, console, idx):
                print(f"  no PONG -- {rate} did not sync, target reverted to 115200")
                results.append({"rate": rate, "synced": False, "stable": False})
                continue

            print("  synced (PONG); running echo stress test ...")
            all_ok = True
            total_bytes = 0
            total_time = 0.0
            for size in STRESS_SIZES:
                for rep in range(STRESS_REPEATS):
                    r = stress_once(port, size)
                    ok = r["rxcrc_ok"] and r["roundtrip_ok"] and r["got_bytes"] == size
                    all_ok = all_ok and ok
                    total_bytes += 2 * size  # sent + echoed back
                    total_time += r["elapsed"]
                    status = "ok" if ok else (
                        f"FAIL (rxcrc_ok={r['rxcrc_ok']} roundtrip_ok={r['roundtrip_ok']} "
                        f"got={r['got_bytes']}/{size})"
                    )
                    print(f"    {size:6d} bytes x{rep + 1}: {status}")
                    if not ok:
                        break
                if not all_ok:
                    break

            throughput_kib_s = (total_bytes / total_time / 1024) if total_time > 0 else 0.0
            print(f"  {'STABLE' if all_ok else 'UNSTABLE'} -- "
                  f"{throughput_kib_s:.1f} KiB/s measured")
            results.append({
                "rate": rate, "synced": True, "stable": all_ok,
                "throughput_kib_s": throughput_kib_s,
            })

            # Always return to the known-good baseline before the next
            # candidate, whether or not this one was stable.
            if not switch_and_confirm(port, console, 0):
                sys.exit("error: could not confirm 115200 after testing "
                         f"{rate} -- power-cycle the Pi and re-run")

        print("\n=== summary ===")
        best = None
        for r in results:
            if not r["synced"]:
                print(f"  {r['rate']:>8d} baud: did not sync")
                continue
            tag = "STABLE" if r["stable"] else "unstable"
            print(f"  {r['rate']:>8d} baud: {tag}  "
                  f"({r.get('throughput_kib_s', 0):.1f} KiB/s)")
            if r["stable"]:
                best = r["rate"]

        if best:
            print(f"\nRecommended: {best} baud "
                  f"(highest rate that passed all stress tests)")
        else:
            print("\nNo candidate above 115200 was stable; staying at 115200.")

        print("\nTarget left idle at 115200 (BAUDCAL ready) -- "
              "power-cycle it to return to normal chainloader use.")


if __name__ == "__main__":
    main()
