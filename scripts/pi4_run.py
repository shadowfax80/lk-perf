#!/usr/bin/env python3
"""Load an image onto the Pi 4B via the serial chainloader, then run a
scripted sequence of shell commands and capture everything printed.

Reuses pi4_serial_boot.py's image-transfer/baud-switch logic, then
sends each command (with a CR) and waits until the Pi goes quiet for
`--idle` seconds before moving to the next one -- no assumption about
what LK's shell prompt looks like, just "has this command's output
settled".

Usage:
    python scripts/pi4_run.py build/lk/build-rpi4-test/lk.bin --log pi4.log \
        -- "profiler start" "profiler bench 5000000" "profiler stop" "profiler dump"
"""
from __future__ import annotations

import argparse
import sys
import time
from pathlib import Path

try:
    import serial
except ImportError:
    sys.exit("error: pyserial is required (python -m pip install pyserial)")

sys.path.insert(0, str(Path(__file__).parent))
from pi4_serial_boot import Console, reboot_to_chainloader, resolve_port, send_image


def run_command(port: serial.Serial, console: Console, cmd: str,
                 idle: float, max_wait: float) -> None:
    console.write(f"\n$ {cmd}\n".encode())
    port.write(cmd.encode() + b"\r")
    port.flush()

    deadline = time.monotonic() + max_wait
    last_data = time.monotonic()
    while time.monotonic() < deadline:
        chunk = port.read(port.in_waiting or 1)
        if chunk:
            console.write(chunk)
            last_data = time.monotonic()
        elif time.monotonic() - last_data > idle:
            return


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("image", help="raw binary to load at 0x8000")
    ap.add_argument("commands", nargs="*", help="shell commands to run in order")
    ap.add_argument("--port", default="auto")
    ap.add_argument("--baud", type=int, default=115200,
                    help="initial link speed matching the chainloader (default: 115200)")
    ap.add_argument("--post-jump-baud", type=int, default=3000000,
                    help="baud after the payload jumps and runs (default: 3000000)")
    ap.add_argument("--log", help="append everything received to this file")
    ap.add_argument("--wait", type=float, default=None,
                    help="seconds to wait for the SBOOT? prompt (default: forever)")
    ap.add_argument("--idle", type=float, default=0.5,
                    help="seconds of silence that mark a command's output as done")
    ap.add_argument("--max-wait", type=float, default=15.0,
                    help="max seconds to wait for any single command")
    ap.add_argument("--reboot", action="store_true",
                    help="if LK is running (no SBOOT? prompt), send it `reboot` "
                         "first instead of waiting for a manual power-cycle")
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
        if args.reboot:
            reboot_to_chainloader(port, console, args.post_jump_baud)
        send_image(port, console, image, args.wait)

        if args.post_jump_baud != args.baud:
            port.baudrate = args.post_jump_baud
            port.reset_input_buffer()

        # Let the boot banner settle before the first command.
        run_command(port, console, "", args.idle, args.max_wait)

        for cmd in args.commands:
            run_command(port, console, cmd, args.idle, args.max_wait)


if __name__ == "__main__":
    main()
