# pi4-baudcal

A one-shot test payload for calibrating the highest stable UART baud
rate between the host and the Raspberry Pi 4B, without touching the SD
card. Loaded via the existing `pi4-serialboot` chainloader (which stays
at 115200 -- untouched, the safe always-works fallback).

Driven by [`scripts/pi4_baud_calibrate.py`](../../scripts/pi4_baud_calibrate.py);
see that script's docstring for the protocol and usage. Findings get
recorded in `docs/RPI4_BRINGUP.md`.

## Why this exists

At 115200 baud a serial transfer runs at ~11 KiB/s -- fine for a small
image during the edit/build/test loop, not for the larger sample dumps
`docs/RPI4_BRINGUP.md`'s roadmap plans for the profiler (M5). The
chainloader already programs the UART clock to 48MHz specifically so a
higher baud rate could be added later without touching the clock again
(see `experiments/pi4-serialboot/main.c`) -- this is that later.

## How it works

- Switches baud rate on command (`'S' <index>`), then requires a `PING`
  from the host within ~1s at the new rate before trusting it;
  otherwise it silently reverts to 115200 on its own, so a bad
  candidate rate never needs a power-cycle to recover from.
- Runs a byte-exact echo stress test on command (`'X' <size> <data>
  <crc32>`): checks the received CRC, then echoes the same bytes back
  verbatim so the host can do its own byte-for-byte comparison --
  catching corruption in either direction, not just on receive.

## Rebuild from source

```bash
CFLAGS="-mcpu=cortex-a72 -marm -ffreestanding -nostdlib -fno-builtin \
  -fno-tree-loop-distribute-patterns -fno-unwind-tables \
  -fno-asynchronous-unwind-tables -O2 -Wall -Wextra"
arm-none-eabi-gcc -c $CFLAGS start.S -o start.o
arm-none-eabi-gcc -c $CFLAGS main.c -o main.o
arm-none-eabi-ld -T linker.ld start.o main.o -o kernel7l.elf
arm-none-eabi-objcopy -O binary kernel7l.elf kernel7l.img
```
