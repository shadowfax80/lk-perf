#!/usr/bin/env python3
"""Send an image to the Pi 4B serial chainloader
(experiments/pi4-serialboot) and then stay attached as a serial
terminal -- the edit/build/test loop without moving the SD card.

The chainloader prints "SBOOT?" once a second while it waits. This
script waits for that prompt (power-cycle the Pi if it's currently
running an earlier payload), sends "LKBT" <size:u32 LE> <crc32:u32 LE>,
waits for "OK", streams the image, and waits for "CRC OK". After that
everything the Pi prints goes to stdout (and --log, if given), and
lines typed here are sent to the Pi with a CR, which is enough for
LK's shell later on. Ctrl+C to quit.

A serial port has one owner at a time: close PuTTY on the same port
first. --port defaults to "auto": the one USB-serial adapter plugged in
(the COM number or /dev path differs per machine). If the Pi doesn't
answer, run scripts/pi4_doctor.py, which checks every prerequisite in
order.

Usage:
    python scripts/pi4_serial_boot.py experiments/pi4-baremetal/kernel7l.img \
        --log pi4.log [--port COM8]
"""
from __future__ import annotations

import argparse
import struct
import sys
import threading
import time
import zlib

try:
    import serial
    from serial.tools import list_ports
except ImportError:
    sys.exit("error: pyserial is required (python -m pip install pyserial)")

# USB vendor IDs of common USB-to-TTL serial chips: Prolific PL2303 (the
# adapter this project uses), WCH CH340, Silicon Labs CP210x, FTDI.
USB_SERIAL_VIDS = {0x067B: "Prolific", 0x1A86: "WCH", 0x10C4: "Silicon Labs", 0x0403: "FTDI"}


def usb_serial_ports() -> list:
    return [p for p in list_ports.comports() if p.vid in USB_SERIAL_VIDS]


def resolve_port(port: str) -> str:
    """Pass an explicit port through; for "auto", pick the single USB-serial
    adapter or explain why that isn't possible."""
    if port != "auto":
        return port
    found = usb_serial_ports()
    if len(found) == 1:
        return found[0].device
    if not found:
        sys.exit("error: no USB-serial adapter found. Is it plugged in? On Windows 11 a "
                 "PL2303TA can be present but driver-blocked with no COM port -- run "
                 "scripts/pi4_doctor.py")
    listing = ", ".join(f"{p.device} ({p.description})" for p in found)
    sys.exit(f"error: several USB-serial adapters ({listing}); pick one with --port")


class Console:
    """Echo-and-log sink for everything read from the Pi."""

    def __init__(self, log_path: str | None):
        self.log = open(log_path, "ab") if log_path else None

    def write(self, data: bytes) -> None:
        sys.stdout.write(data.decode("utf-8", errors="replace"))
        sys.stdout.flush()
        if self.log:
            self.log.write(data)
            self.log.flush()


def read_line(port: serial.Serial, deadline: float | None) -> str | None:
    """One CR/LF-terminated line (without the terminator), or None on timeout."""
    buf = bytearray()
    while deadline is None or time.monotonic() < deadline:
        b = port.read(1)
        if not b:
            continue
        if b == b"\n":
            return buf.decode("ascii", errors="replace").rstrip("\r")
        buf += b
    return None


def wait_for(port: serial.Serial, console: Console, want: tuple[str, ...],
             timeout: float | None, quiet: tuple[str, ...] = ()) -> str:
    """Read lines until one starts with any of `want`; echo the rest
    (except lines starting with `quiet`). Returns the matching line."""
    deadline = None if timeout is None else time.monotonic() + timeout
    while True:
        line = read_line(port, deadline)
        if line is None:
            sys.exit(f"\nerror: timed out waiting for {' / '.join(want)}")
        if line.startswith(want):
            return line
        if line and not line.startswith(quiet):
            console.write((line + "\n").encode())


def send_image(port: serial.Serial, console: Console, image: bytes,
               wait: float | None) -> None:
    print(f"waiting for the chainloader on {port.port} "
          "(power-cycle the Pi if it's running an earlier payload)...")
    wait_for(port, console, ("SBOOT?",), wait)

    crc = zlib.crc32(image) & 0xFFFFFFFF
    print(f"bootloader ready; sending {len(image)} bytes, crc32 {crc:#010x}")
    port.write(b"LKBT" + struct.pack("<II", len(image), crc))

    reply = wait_for(port, console, ("OK", "ER"), 5.0, quiet=("SBOOT?",))
    if reply.startswith("ER"):
        sys.exit(f"error: bootloader refused the image: {reply}")

    chunk = 1024
    start = time.monotonic()
    for off in range(0, len(image), chunk):
        port.write(image[off:off + chunk])
        done = min(off + chunk, len(image))
        rate = done / max(time.monotonic() - start, 1e-6)
        sys.stdout.write(f"\r  {done}/{len(image)} bytes  {100 * done // len(image):3d}%  "
                         f"{rate / 1024:.1f} KiB/s")
        sys.stdout.flush()
    port.flush()
    print()

    # The loader re-reads the whole payload from memory for a second CRC
    # before answering; with its caches off that takes seconds on a
    # large image.
    reply = wait_for(port, console, ("CRC OK", "ER"), 60.0)
    if reply.startswith("ER"):
        sys.exit(f"error: transfer failed: {reply}")
    console.write((reply + "\n").encode())


def terminal(port: serial.Serial, console: Console) -> None:
    def forward_stdin() -> None:
        for line in sys.stdin:
            port.write(line.rstrip("\r\n").encode() + b"\r")

    threading.Thread(target=forward_stdin, daemon=True).start()
    print("--- attached; Ctrl+C to quit ---")
    try:
        while True:
            data = port.read(port.in_waiting or 1)
            if data:
                console.write(data)
    except KeyboardInterrupt:
        print("\n--- detached ---")


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("image", help="raw binary to load at 0x8000 (e.g. kernel7l.img, lk.bin)")
    ap.add_argument("--port", default="auto",
                    help='serial port, e.g. COM8 or /dev/ttyUSB0 (default: "auto")')
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--log", help="append everything received to this file")
    ap.add_argument("--wait", type=float, default=None,
                    help="seconds to wait for the SBOOT? prompt (default: forever)")
    ap.add_argument("--no-term", action="store_true",
                    help="exit once the image is running instead of staying attached")
    args = ap.parse_args()

    with open(args.image, "rb") as f:
        image = f.read()
    if not image:
        sys.exit(f"error: {args.image} is empty")

    console = Console(args.log)
    port_name = resolve_port(args.port)
    try:
        port = serial.Serial(port_name, args.baud, timeout=0.1)
    except serial.SerialException as e:
        sys.exit(f"error: can't open {port_name} ({e}). Is PuTTY still holding it?")

    with port:
        port.reset_input_buffer()
        send_image(port, console, image, args.wait)
        if not args.no_term:
            terminal(port, console)


if __name__ == "__main__":
    main()
