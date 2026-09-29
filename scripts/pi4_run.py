#!/usr/bin/env python3
"""Load an image onto the Pi 4B via the serial chainloader, then run a
scripted sequence of shell commands and capture everything printed.

Reuses pi4_serial_boot.py's image-transfer/baud-switch logic, then
sends each command (with a CR) and waits for LK's own shell prompt
("] ", fputs'd by lib/console/console.c right before it reads the next
line) to come back before sending the next one.

Review finding #7, fixed: this used to wait for `--idle` seconds of
silence instead, capped at `--max-wait` overall -- a real bug, not
just imprecision. A full `profiler dump` can run for tens of seconds
with no gap anywhere near that long (measured: ~6MB at the calibrated
~213 KiB/s is ~29s), so the old defaults (0.5s idle, 15s max) silently
cut it off mid-stream and then typed the *next* command straight into
LK's still-busy 16-byte UART receive buffer, corrupting or dropping
bytes with no error from either side. Waiting for the real prompt has
no such ceiling for a well-behaved command; `--max-wait` is now a much
larger hard safety timeout for a genuine hang, not the normal
completion signal.

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
from pi4_serial_boot import Console, reboot_to_chainloader, resolve_port, send_image, switch_baud

PROMPT = b"] "


def run_command(port: serial.Serial, console: Console, cmd: str, max_wait: float) -> bool:
    """Send one command, wait for LK's own "] " prompt to reappear.
    Returns False (and warns) if it doesn't within max_wait -- a real
    hang or a crash, not just a slow command."""
    console.write(f"\n$ {cmd}\n".encode())
    port.write(cmd.encode() + b"\r")
    port.flush()

    deadline = time.monotonic() + max_wait
    tail = b""
    while time.monotonic() < deadline:
        chunk = port.read(port.in_waiting or 1)
        if not chunk:
            continue
        console.write(chunk)
        tail = (tail + chunk)[-len(PROMPT):]
        if tail == PROMPT:
            return True

    print(f"warning: no prompt within {max_wait}s after {cmd!r} -- "
          f"the Pi may be hung or still mid-command; stopping here "
          f"rather than typing the next command into a busy shell",
          file=sys.stderr)
    return False


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
    ap.add_argument("--max-wait", type=float, default=90.0,
                    help="hard timeout per command if LK's prompt never comes back "
                         "(default: 90s, generous enough for a full profiler dump)")
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
            switch_baud(port, args.post_jump_baud)

        # Let the boot banner settle before the first command.
        if not run_command(port, console, "", args.max_wait):
            sys.exit(1)

        for cmd in args.commands:
            if not run_command(port, console, cmd, args.max_wait):
                sys.exit(1)


if __name__ == "__main__":
    main()
