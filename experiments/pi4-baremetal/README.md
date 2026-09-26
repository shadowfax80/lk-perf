# pi4-baremetal

Minimal AArch32 validation image for Raspberry Pi 4B (BCM2711) --
proves the whole chain (toolchain -> SD card -> firmware -> AArch32
boot -> UART) before any LK port is attempted. See
[`docs/RPI4_BRINGUP.md`](../../docs/RPI4_BRINGUP.md) for the full plan
and rationale.

Not related to LK or the overlay pattern used elsewhere in this repo --
this is a completely standalone, from-scratch bring-up test.

## What it does

Parks CPU cores 1-3 (Pi 4 firmware releases all 4 Cortex-A72 cores
simultaneously in AArch32), brings up PL011 with its clock pinned to
exactly 3MHz via one mailbox call, then prints a boot banner and an
incrementing heartbeat over serial forever.

## Flash it (prebuilt binary, no rebuild needed)

1. **Back up the existing kernel first**: rename `kernel7l.img` on the
   SD card's boot partition to `kernel7l-linux-backup.img`.
2. Add to `config.txt` on the same partition:
   ```
   arm_64bit=0
   enable_uart=1
   ```
3. Copy this directory's `kernel7l.img` onto the boot partition,
   overwriting the original.
4. Wire a 3.3V USB-to-TTL adapter: adapter RX -> Pi GPIO14, adapter TX
   -> Pi GPIO15, adapter GND -> Pi GND (crossed, not same-name-to-same-name).
5. Open a serial terminal (PuTTY, etc.) on the adapter's COM port at
   115200 8N1 -- **before** powering the Pi, so you don't miss early
   boot output.
6. Power on. Expect:
   ```
   pi4-baremetal: AArch32 boot OK, PL011 UART live
   core: 0 (others parked)
   heartbeat 0
   heartbeat 1
   ...
   ```

## Rebuild from source

Same toolchain as the rest of this repo -- no new install:

```bash
arm-none-eabi-gcc -c -mcpu=cortex-a72 -marm -ffreestanding -O2 -Wall -Wextra start.S -o start.o
arm-none-eabi-gcc -c -mcpu=cortex-a72 -marm -ffreestanding -nostdlib -O2 -Wall -Wextra main.c -o main.o
arm-none-eabi-ld -T linker.ld start.o main.o -o kernel7l.elf
arm-none-eabi-objcopy -O binary kernel7l.elf kernel7l.img
```
