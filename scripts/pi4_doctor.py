#!/usr/bin/env python3
"""Check everything the Raspberry Pi 4B serial-boot workflow needs, in
order, and name the fix for whatever fails. Run this first on any new
machine (see "Setting up the next machine" in docs/RPI4_BRINGUP.md).

  1. Python version
  2. pyserial installed
  3. the repo's test image is intact
  4. the USB-serial adapter is detected -- on Windows 11 this also
     catches a PL2303TA that is present but driver-blocked (no COM port)
  5. the port can be opened (nothing else, e.g. PuTTY, holds it)
  6. live: power-cycle the Pi and the SD card's chainloader answers
     with "SBOOT?" (skip with --no-pi)
  7. optional (--boot-test): send experiments/pi4-baremetal/kernel7l.img
     through the chainloader and wait for its heartbeat -- the whole
     path, end to end

Usage:
    python scripts/pi4_doctor.py [--port COM8] [--boot-test] [--no-pi]
"""
from __future__ import annotations

import argparse
import os
import struct
import subprocess
import sys
import time

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TEST_IMAGE = os.path.join(REPO, "experiments", "pi4-baremetal", "kernel7l.img")
DOC = "docs/RPI4_BRINGUP.md, section \"Setting up the next machine\""

def ok(msg: str) -> None:
    print(f"  [ OK ] {msg}")


def warn(msg: str) -> None:
    print(f"  [WARN] {msg}")


def fail(msg: str, fix: str) -> None:
    print(f"  [FAIL] {msg}")
    for line in fix.strip().splitlines():
        print(f"         {line}")


def stop() -> None:
    print(f"\nStopped at the first failure above; fix it and re-run. Details: {DOC}.")
    sys.exit(1)


def check_python() -> None:
    if sys.version_info >= (3, 8):
        ok(f"Python {sys.version.split()[0]}")
    else:
        fail(f"Python {sys.version.split()[0]} is too old", "Install Python 3.8 or newer.")
        stop()


def check_pyserial() -> None:
    try:
        import serial  # noqa: F401
    except ImportError:
        fail("pyserial is not installed", "python -m pip install pyserial")
        stop()
    ok(f"pyserial {serial.__version__}")


def check_test_image() -> bytes:
    try:
        with open(TEST_IMAGE, "rb") as f:
            image = f.read()
    except OSError as e:
        fail(f"can't read {TEST_IMAGE} ({e})",
             "Run this from an up-to-date clone: git pull")
        stop()
    # Its first instruction is the MPIDR read in start.S (mrc p15,0,r0,c0,c0,5).
    # Anything else means the file was altered, e.g. by line-ending conversion.
    if len(image) < 64 or struct.unpack_from("<I", image)[0] != 0xEE100FB0:
        fail("the test image doesn't look like the committed binary",
             "git checkout -- experiments/pi4-baremetal/kernel7l.img\n"
             "(.gitattributes marks *.img as binary; an old clone may predate that)")
        stop()
    ok(f"test image intact ({len(image)} bytes)")
    return image


def windows_usb_serial_devices() -> list[tuple[str, str, str]]:
    """(status, friendly name, instance id) of present USB-serial devices,
    straight from PnP -- includes ones whose driver refused to start."""
    ps = ("Get-PnpDevice -PresentOnly | Where-Object { $_.InstanceId -match "
          "'^USB\\\\VID_(067B|1A86|10C4|0403)' } | ForEach-Object { "
          "$_.Status + '|' + $_.FriendlyName + '|' + $_.InstanceId }")
    try:
        out = subprocess.run(["powershell", "-NoProfile", "-Command", ps],
                             capture_output=True, text=True, timeout=30).stdout
    except (OSError, subprocess.TimeoutExpired):
        return []
    rows = []
    for line in out.splitlines():
        parts = line.strip().split("|")
        if len(parts) == 3:
            rows.append((parts[0], parts[1], parts[2]))
    return rows


BLOCKED_FIX = """\
Windows 11 is refusing this PL2303TA chip. Install Prolific driver 3.8.28.0
and remove every newer Prolific ser2pl package (3.8.43.0 still blocks it).
Step-by-step commands: """ + DOC + "."


def check_adapter(requested: str) -> str:
    from pi4_serial_boot import USB_SERIAL_VIDS, usb_serial_ports

    if requested != "auto":
        ok(f"using --port {requested} as given")
        return requested

    found = usb_serial_ports()
    if len(found) == 1:
        p = found[0]
        ok(f"adapter found: {p.device} ({USB_SERIAL_VIDS[p.vid]}, {p.description})")
        return p.device
    if len(found) > 1:
        listing = "\n".join(f"{p.device}: {p.description}" for p in found)
        fail("more than one USB-serial adapter is connected",
             f"Re-run with --port set to the Pi's one:\n{listing}")
        stop()

    if sys.platform == "win32":
        devices = windows_usb_serial_devices()
        for status, name, inst in devices:
            if "NOT SUPPORT" in name.upper():
                fail(f"adapter present but driver-blocked: \"{name}\"", BLOCKED_FIX)
                stop()
        if devices:
            listing = "\n".join(f"{s}: {n} ({i})" for s, n, i in devices)
            fail("adapter present but it has no COM port",
                 "Its driver isn't working. Check Device Manager; for a PL2303TA see\n"
                 f"{DOC}.\n{listing}")
            stop()
    fail("no USB-serial adapter detected",
         "Plug it in (try another USB port/cable).\n"
         "Linux: look for /dev/ttyUSB* or /dev/ttyACM* in `dmesg` after plugging in.")
    stop()
    return ""


def check_open(port_name: str):
    import serial

    try:
        port = serial.Serial(port_name, 115200, timeout=0.2)
    except serial.SerialException as e:
        text = str(e)
        if sys.platform.startswith("linux") and "ermission" in text:
            fix = ("Add yourself to the dialout group and log in again:\n"
                   "sudo usermod -aG dialout $USER")
        elif "ccess is denied" in text or "busy" in text.lower():
            fix = "Another program holds the port -- close PuTTY or any other serial terminal."
        else:
            fix = "Check the port name, or unplug and replug the adapter."
        fail(f"can't open {port_name}: {text}", fix)
        stop()
    ok(f"{port_name} opens at 115200 8N1")
    return port


NO_OUTPUT_FIX = """\
Nothing at all arrived. Check, in order:
- the Pi is powered (red LED on) and was power-cycled after this started
- wiring, crossed: adapter RX -> GPIO14 (pin 8), adapter TX -> GPIO15
  (pin 10), GND -> pin 6; adapter's 5V/VCC wire left unconnected
- the SD card is in the Pi and holds experiments/pi4-serialboot/kernel7l.img
  with arm_64bit=0, enable_uart=1, dtoverlay=disable-bt in config.txt
- a steadily blinking green LED pattern means the firmware can't boot the card"""


def check_live(port, timeout: float) -> None:
    print(f"\n  Power-cycle the Pi now (unplug USB-C power, plug back in). "
          f"Listening for up to {timeout:.0f}s ...")
    port.reset_input_buffer()
    deadline = time.monotonic() + timeout
    buf = bytearray()
    raw_total = 0
    printable = 0
    banner = None
    payload_warned = False
    while time.monotonic() < deadline:
        data = port.read(256)
        if not data:
            continue
        raw_total += len(data)
        printable += sum(1 for b in data if 32 <= b < 127 or b in (9, 10, 13))
        buf += data
        while b"\n" in buf:
            line, _, rest = bytes(buf).partition(b"\n")
            buf = bytearray(rest)
            text = line.decode("ascii", errors="replace").strip()
            if text.startswith("lk-perf serial boot"):
                banner = text
            elif text.startswith("SBOOT?"):
                ok("chainloader is answering (SBOOT?)")
                if banner:
                    print(f"         {banner}")
                return
            elif text and not payload_warned:
                payload_warned = True
                warn(f"the Pi is printing but not the chainloader prompt: \"{text[:60]}\"")
                print("         It's probably still running an earlier payload -- power-cycle it.")
        if raw_total >= 32 and printable < 0.7 * raw_total:
            fail("receiving garbage instead of text",
                 "Baud or signal mismatch: confirm a 3.3V TTL adapter (not RS-232) and "
                 "that nothing else changed the port settings; re-check GND.")
            stop()
    if raw_total == 0:
        fail(f"no data from the Pi within {timeout:.0f}s", NO_OUTPUT_FIX)
    else:
        fail("the Pi printed text but never the SBOOT? prompt",
             "Power-cycle it once more. If it still never appears, the SD card doesn't\n"
             "hold the chainloader: copy experiments/pi4-serialboot/kernel7l.img onto it.")
    stop()


def check_boot_test(port, image: bytes) -> None:
    from pi4_serial_boot import Console, send_image, read_line

    print("\n  Sending the test image through the chainloader ...")
    send_image(port, Console(None), image, wait=10.0)
    deadline = time.monotonic() + 20.0
    while True:
        line = read_line(port, deadline)
        if line is None:
            fail("the test image was sent but no heartbeat followed",
                 "Power-cycle and retry; if it repeats, note the output above.")
            stop()
        if line.startswith("heartbeat"):
            ok(f"test image running on the Pi ({line})")
            return


def main() -> None:
    ap = argparse.ArgumentParser(description="Check the Pi 4B serial-boot prerequisites.")
    ap.add_argument("--port", default="auto",
                    help='serial port, e.g. COM8 or /dev/ttyUSB0 (default: "auto")')
    ap.add_argument("--no-pi", action="store_true",
                    help="stop after the PC-side checks (no Pi needed)")
    ap.add_argument("--boot-test", action="store_true",
                    help="also send the test image and wait for its heartbeat")
    ap.add_argument("--timeout", type=float, default=60.0,
                    help="seconds to wait for the Pi in the live check (default 60)")
    args = ap.parse_args()

    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    print("PC side:")
    check_python()
    check_pyserial()
    image = check_test_image()
    port_name = check_adapter(args.port)
    port = check_open(port_name)

    if args.no_pi:
        port.close()
        print(f"\nPC side ready. Next: python scripts/pi4_doctor.py --port {port_name} --boot-test")
        return

    print("\nPi side:")
    with port:
        check_live(port, args.timeout)
        if args.boot_test:
            check_boot_test(port, image)

    print(f"\nAll checks passed. Send an image with:\n"
          f"  python scripts/pi4_serial_boot.py <image> --port {port_name} --log <file>")
    if not args.boot_test:
        print("(add --boot-test to also run the test image end to end)")


if __name__ == "__main__":
    main()
