# pi4-serialboot

Serial chainloader for Raspberry Pi 4B (BCM2711, AArch32). It goes on the
SD card once, as `kernel7l.img`. From then on, each new build is sent to
the Pi over the same USB-serial cable used for the console, instead of by
moving the SD card. See [`docs/RPI4_BRINGUP.md`](../../docs/RPI4_BRINGUP.md).

## How it works

- The firmware loads it at `0x8000` as usual. `start.S` copies it up to its
  link address, `0x02000000`, and continues there. That frees
  `0x8000`–`0x02000000` (about 32MB) for the payload.
- It prints `SBOOT?` once a second until the host sends a header:
  `"LKBT"`, then the size and CRC-32 as little-endian u32s. It replies
  `OK`, receives the image into `0x8000`, and checks the CRC. If the CRC
  matches it jumps; if not it reports `ER ...` and waits for another try.
- The payload is entered exactly as a direct SD-card boot would enter it:
  same mode (HYP on Pi 4), caches off, and the firmware's original
  `r0`/`r1`/`r2`. The one exception: if the firmware's device tree sits
  where the payload lands, the DTB is first moved to `0x02100000` and `r2`
  points there instead.
- The UART runs at 115200 8N1 on GPIO14/15, the same wiring as
  `pi4-baremetal`. Its clock is 48MHz rather than 3MHz, so higher baud
  rates can be added later without a clock change.
- The banner reports the CPU mode and the firmware's `r0`–`r2`. That's a
  quick way to see where the firmware put the DTB.

## Install on the SD card (once)

Use the same `config.txt` lines as `pi4-baremetal` (`arm_64bit=0`,
`enable_uart=1`, `dtoverlay=disable-bt`), and copy this directory's
`kernel7l.img` onto the boot partition in place of the existing one.

## Send an image

Close PuTTY first, since only one program can hold the COM port. Then:

```
python scripts/pi4_serial_boot.py experiments/pi4-baremetal/kernel7l.img --port COM7 --log pi4.log
```

Power the Pi on, or power-cycle it if a payload is already running. The
script waits for `SBOOT?`, sends the image, and then stays attached as a
terminal. Ctrl+C quits. Lines you type are sent to the Pi with a CR.
At 115200 baud, transfers run at about 11 KiB/s.

A payload has to be a raw binary linked to run at `0x8000`, which is what
the firmware would have required of it anyway.

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

`-fno-builtin -fno-tree-loop-distribute-patterns` stop GCC from turning
the copy loops into `memcpy`/`memset` calls, which don't exist here.
