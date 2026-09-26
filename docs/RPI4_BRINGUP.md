# Raspberry Pi 4B bring-up plan

Why this exists: `docs/DESIGN.md`'s Stage 5 (PMU-event-triggered sampling)
was confirmed real-hardware-only on QEMU (no PMU IRQ route in the virt
device tree, and PMU register access itself faults without a
secure-monitor boot stage). Real Cortex-A55/production hardware wasn't
available, so a Raspberry Pi 4B (Cortex-A72, BCM2711) was chosen as the
cheapest real-hardware path to validate two specific things QEMU
categorically cannot: EXIDX unwinding robustness, and whether real PMU
event counters (cache misses, branch mispredicts) actually respond to
workload behavior.

**Not A55-representative** — A72 is a different, higher-performance
core than the eventual real target. This validates the *mechanism*
(does interrupt-driven PMU sampling work at all on real silicon, does
EXIDX give more complete unwinds than the FP-chain walker), not
A55-accurate numbers.

## Why Pi 4B over the Arm Cortex-A55 FVP route

The FVP was investigated first and set aside for concrete, confirmed
reasons, not preference:

- FVP's Base Platform models use **GICv3**; all three of this project's
  overlay patches (`0001`-`0003`) target GICv2 (`gic_v2.c`). Would need a
  full GICv3-equivalent rewrite.
- FVP boots Cortex-A55 (AArch64-capable) in AArch64 at EL3 by default.
  Getting to AArch32 EL1 needs a real secure-world boot chain — normally
  Trusted Firmware-A built with `ARCH=aarch32` — which doesn't exist in
  this project at all today. LK's QEMU boot path never needed this,
  since `-cpu cortex-a15` is AArch32-only hardware with no state to
  transition from.
- The FVP download itself is blocked by Akamai's bot-protection WAF on
  both `developer.arm.com` and `support.arm.com` — confirmed via direct
  curl with full browser headers (HTTP 403, Akamai edge reference),
  not just a login wall. Needs a real, human browser session.
- Free-tier FVP PMU event fidelity (vs. the licensed "Cycle Models"
  tier bundled with Arm Development Studio) was never confirmed either
  way.

**Pi 4B's concrete advantage**: BCM2711 uses a real **GIC-400, which
implements GICv2** — confirmed via BCM2711 device tree source
(`arm,gic-400` compatible, GICv2 architecture). This project's existing
GIC hook patches have a real shot at applying with much less rework.
And Raspberry Pi's own vendor firmware solves the AArch32 boot-state
question via a plain `config.txt` setting (`arm_64bit=0`) — no TF-A
integration needed, unlike the FVP.

## Update 2026-09-26: first hardware milestone passed (steps 1-2 done)

Moved to the home PC, which has the SD card and serial adapter attached and
can write to the card normally. `experiments/pi4-baremetal/kernel7l.img`
booted on the real Pi 4B, and the serial console showed the banner and a
steadily incrementing heartbeat, with one boot and no reset loop:

```
pi4-baremetal: AArch32 boot OK, PL011 UART live
core: 0 (others parked)
heartbeat 0
heartbeat 1
...
```

Things found along the way that the plan below didn't anticipate:

- **PL011 is routed to Bluetooth on Pi 4 by default.** `enable_uart=1`
  alone puts the *mini-UART* on GPIO14/15, so the original image would have
  booted and printed nothing. The fix is applied twice: `dtoverlay=disable-bt`
  in `config.txt`, and `main.c` now muxes GPIO14/15 to ALT0 and clears their
  pulls itself (BCM2711 `GPIO_PUP_PDN_CNTRL_REG0`, not the BCM283x
  `GPPUD` sequence). An LK `rpi4` target needs the same pin setup.
- **The firmware normally enters only core 0, in HYP mode.** The 32-bit
  armstub keeps cores 1-3 spinning on their ARM-local mailbox 3 (so it's
  not PSCI). That answers the secondary-release question under "Open risks"
  below, and it means LK's entry path has to drop from HYP to SVC. Both
  still need confirming on hardware during the LK port.
- **PL2303TA serial adapters are blocked on Windows 11.** Prolific's
  3.9.6.0 and 3.8.43.0 drivers both refuse the chip at runtime. Driver
  3.8.28.0 (Oct 2018, Microsoft Update Catalog, Microsoft-signed) works.
  Windows Update may upgrade it again later.
- The card actually holds Raspberry Pi OS **2021-05-07** (full image, boot
  label `boot`), not a current Lite image. Its firmware boots this board,
  but a newer board revision (1.5+) would need newer firmware.
- The original files are kept on the card as `kernel7l-linux-backup.img` and
  `config-linux-backup.txt`.

**Serial chainloader working (same day).** `experiments/pi4-serialboot/` is
now the `kernel7l.img` on the card. Images are sent over the console cable
with `scripts/pi4_serial_boot.py`, so the SD card no longer moves. On first
use it loaded `pi4-baremetal` (682 bytes, CRC OK, ~14 KiB/s) and the
heartbeat came up. Its banner confirmed two things on real hardware:

- **Entry mode is HYP** (`mode=HYP`), so the LK port does need the
  HYP->SVC drop.
- **The firmware's DTB is at `0x2eff3b00`** (`r2`), near the top of low RAM
  and nowhere near the `0x8000` payload region. The loader's DTB-move path
  wasn't needed. `r1=0xc42` is the device-tree machine type.

**Next up: step 3 (LK `TARGET=rpi4` port). The detailed plan is in the
next section.**

## Start here: state at end of 2026-09-26 and the LK port plan

This section is the handoff for picking the work up on another machine.
Everything below the next heading is history.

### Resuming with Claude Code

Clone or pull the repo, start `claude` in its root, and paste the prompt
below. The session also loads `CLAUDE.md` automatically, which has the
working rules: RunPod handling, ask-first actions, commit identity and
environment gotchas.

```
Continue the Raspberry Pi 4B bring-up of lk-perf on this PC; the Pi
and its USB-serial adapter are plugged in here. Read CLAUDE.md and
the "Start here" section of docs/RPI4_BRINGUP.md first.
1. Prove the serial path: pip-install pyserial, run
   scripts/pi4_doctor.py --no-pi, then --boot-test (tell me when to
   power-cycle the Pi). Fix failures from the doc's setup table, asking
   me before driver installs or admin actions. Don't continue until
   the heartbeat shows.
2. Set up the Linux build box: I'll create a RunPod CPU pod. Give me
   this PC's SSH public key to add, wait for my "SSH over exposed TCP"
   command, then clone the repo there, run ./setup.sh and confirm the
   QEMU `make profiler` baseline builds and boots.
3. Start the LK TARGET=rpi4 port per the doc's step 3 plan, as
   overlay/lk/0004-bcm28xx-add-rpi4.patch, single-core first, aiming
   for M1 then M2. Verify the GIC IDs and the boot-args symbol against
   the sources as the doc says. Commit and push at each milestone.
```

If the new machine runs Linux, the Windows driver steps don't apply, and
it can build LK itself with `./setup.sh`, so step 2 needs no RunPod.

### What exists and works (all verified on the real Pi 4B)

| Piece | Where | State |
|---|---|---|
| SD card | in the Pi | holds `experiments/pi4-serialboot/kernel7l.img` plus `config.txt` with `arm_64bit=0`, `enable_uart=1`, `dtoverlay=disable-bt`. The original Linux kernel and config are on the card as `-linux-backup` copies. Only needs touching again to change the bootloader or `config.txt`. |
| Serial chainloader | `experiments/pi4-serialboot/` | Boots from the card and prints `SBOOT?` once a second. Loads images to `0x8000` over the console cable with a CRC-32 check. |
| Host sender | `scripts/pi4_serial_boot.py` | `python scripts/pi4_serial_boot.py <image> --log <file>` (port auto-detected), then power-cycle the Pi. Needs pyserial. Close PuTTY first. Check a new machine with `scripts/pi4_doctor.py`. |
| Test payload | `experiments/pi4-baremetal/kernel7l.img` | Known-good image for checking the whole path: banner plus heartbeat. |
| Confirmed hardware facts | | Entered in **HYP**. `r0=0`, `r1=0xc42`, **DTB at `r2=0x2eff3b00`**. PL011 at `0xFE201000` works at 115200 on GPIO14/15 (ALT0). Generic timer counts (CNTFRQ is set by the firmware). |

### Setting up the next machine

**Run the checker first.** It goes through every prerequisite below in
order, stops at the first problem, and prints the fix:

```
python -m pip install pyserial
python scripts/pi4_doctor.py --no-pi                  # PC side only, no Pi needed
python scripts/pi4_doctor.py --boot-test              # then power-cycle the Pi when it says so
```

`--boot-test` sends the test image through the chainloader and waits for
its heartbeat. If that passes, the machine is ready: the port, driver,
wiring, SD card and chainloader all work. The port is detected
automatically; pass `--port COMx` or `--port /dev/ttyUSB0` if more than
one USB-serial adapter is plugged in. `pi4_serial_boot.py` detects the
port the same way.

| Symptom | Cause | Fix |
|---|---|---|
| `pyserial is required` | pyserial missing | `python -m pip install pyserial` |
| no adapter, or `can't open COM7` | adapter not plugged in, or it has a different COM number on this PC (it was COM7, later COM8, even on the same PC) | plug it in; let `--port` auto-detect or pass the right one |
| adapter "PL2303TA DO NOT SUPPORT WINDOWS 11 OR LATER", no COM port | Windows 11 driver block | driver 3.8.28.0 steps below (3.8.43.0 still blocks it) |
| `Access is denied` / busy | PuTTY or another terminal holds the port | close it |
| Linux `Permission denied` on `/dev/ttyUSB0` | not in `dialout` | `sudo usermod -aG dialout $USER`, log in again |
| waits forever at `waiting for the chainloader` | Pi not power-cycled, or still running an earlier image | power-cycle *after* starting the script |
| still nothing after a power-cycle | wiring or SD card | crossed RX/TX (below), common GND, card holds `pi4-serialboot/kernel7l.img` plus the three `config.txt` lines |
| text arrives but no `SBOOT?` | card holds some other kernel | put `experiments/pi4-serialboot/kernel7l.img` on it |
| garbage characters | wrong adapter type or bad ground | 3.3V TTL adapter (not RS-232), check GND |

The details behind each row:

1. `git clone https://github.com/shadowfax80/lk-perf.git` (or `git pull`).
2. **The serial side runs on whichever PC the Pi's USB-serial cable is
   plugged into.** That PC needs Python with `pyserial`. On Windows 11 the
   PL2303TA adapter also needs Prolific driver **3.8.28.0**, because newer
   ones refuse the chip (see the 2026-09-26 update above). You can tell
   it's blocked when Device Manager names the adapter "PL2303TA DO NOT
   SUPPORT WINDOWS 11 OR LATER" and no COM port appears. The fix, from an
   elevated PowerShell:
   - Download the Microsoft-signed package (Update Catalog, "Prolific -
     Ports - 3.8.28.0"):
     `https://catalog.s.download.windowsupdate.com/d/msdownload/update/driver/drvs/2019/04/959b8377-7fe8-4ac6-8893-6a3e95b0e8fe_c88b1f07c9075bbfa998a67df0511e2e6d98af10.cab`
     (its SHA1 is the hex after the `_`).
   - Unpack it: `expand driver.cab -F:* .`
   - Install it: `pnputil /add-driver ser2pl.inf`.
   - `pnputil /enum-drivers`, then delete every *newer* Prolific
     `ser2pl` package with `pnputil /delete-driver oemNN.inf /uninstall`.
     Windows always prefers the newest version, so leaving one behind
     means it wins again.
   - `pnputil /remove-device <USB\VID_067B&PID_2303\...>` then
     `pnputil /scan-devices`. The adapter comes back as "Prolific
     USB-to-Serial Comm Port (COMn)".

   Windows Update may later re-upgrade the driver; the same steps undo
   that. Wiring: adapter
   RX to GPIO14 (pin 8), adapter TX to GPIO15 (pin 10), GND to pin 6.
   115200 8N1.
3. **The build runs on Linux: a RunPod CPU pod, WSL or any Linux box.** The
   repo's `setup.sh` expects apt: it installs `gcc-arm-none-eabi` and
   `qemu-system-arm`, clones upstream LK into `build/lk`, and applies
   `overlay/lk/*.patch`. On RunPod, register the machine's SSH public key
   under Settings, start the cheapest **CPU** pod on an Ubuntu template, and
   use the "SSH over exposed TCP" command so `scp` works. Stop the pod when
   you're done; the repo holds all the state.
4. Sanity check before any port work: on the build box, `./setup.sh`, then
   `cd build/lk && make profiler -j$(nproc)`, and boot it under QEMU. This
   confirms upstream LK tip plus the overlay still build, so later failures
   are really about the port.

### The round trip once the port builds

```
# build box
cd build/lk && make rpi4-test -j$(nproc)            # -> build-rpi4-test/lk.bin
# serial PC
scp -P <port> root@<pod-ip>:lk-perf/build/lk/build-rpi4-test/lk.bin .
python scripts/pi4_serial_boot.py lk.bin --log lk-rpi4.log      # port auto-detected
# then power-cycle the Pi
```

`lk.bin` is the raw image LK builds next to `lk.elf`, linked to run at
`KERNEL_LOAD_OFFSET`, which must stay `0x8000` for the chainloader. Keep
`lk.elf` on the build box for `addr2line` and `objdump` when something
crashes. At the measured ~14 KiB/s, a few-hundred-KB image takes 20-30s.

### Step 3 plan: LK `TARGET=rpi4` (AArch32)

Upstream LK checked 2026-09-26: `platform/bcm28xx` supports `rpi2` (BCM2836,
AArch32) and `rpi3` (BCM2837, arm64) only. What's already there and
directly usable:

- **HYP to SVC is already handled.** `arch/arm/arm/start.S` checks for mode
  `0x1a` and calls `arm32_hyp_to_svc` when `ARM_WITH_HYP=1`, which
  `ARM_CPU := cortex-a15` sets. Use `cortex-a15`, the same as the QEMU
  project, so the profiler patches see the same arch code. A72 runs
  ARMv7-A code fine in AArch32.
- **Secondary-core release already matches Pi 4.** The rpi2 path writes the
  entry address to `ARM_LOCAL_BASE + 0x8c + 0x10*i`, which is ARM-local
  mailbox 3, exactly what the Pi 4 armstub spins on. Only `ARM_LOCAL_BASE`
  moves, to `0xFF800000` physical on BCM2711.
- **The PL011 driver (`uart.c`) is reusable.** `uart_init_early()` only sets
  `CR`; it doesn't program baud, pins or clock. When LK is chain-loaded,
  the chainloader has already left PL011 at 115200 on GPIO14/15, so this
  works as-is. For a direct SD-card boot later, add the GPIO ALT0 mux and
  the 48MHz mailbox clock plus `IBRD=26`/`FBRD=3` from
  `experiments/pi4-serialboot/main.c`.
- `dev/timer/arm_generic`: pass freq `0` so it reads CNTFRQ (54MHz), as
  the rpi3 path does. rpi2 hardcodes 1MHz, which would be wrong here.

What has to change:

1. **Peripheral base and map.** BCM2711 low-peripheral mode puts the main
   peripherals at `0xFE000000`, ARM-local at `0xFF800000` and the GIC-400 at
   `0xFF840000`. Map `0xFC000000`-`0xFFFFFFFF` (64MB) as device memory in
   `mmu_initial_mappings`, e.g. at virt `0xE0000000`. Add a `BCM2711`
   branch in `bcm28xx.h`/`platform.c` instead of editing the `BCM2836`
   one.
2. **Interrupt controller: GIC-400, not `intc.c`.** On Pi 4 the firmware
   enables the GIC (`enable_gic` defaults on), so the legacy BCM2836
   controller that `intc.c` drives isn't where interrupts arrive. For
   `TARGET=rpi4`, drop `intc.c` and add `dev/interrupt/arm_gic` with
   `GIC_VERSION=2`, `GICBASE(0)` = virt of `0xFF840000`,
   `GICD_OFFSET=0x1000`, `GICC_OFFSET=0x2000`. Model the defines on
   `platform/qemu-virt-arm`. Interrupt IDs:
   - non-secure physical timer (CNTPNSIRQ) = PPI 14 = **ID 30**
   - PL011 UART0 = SPI 121 = **ID 153**

   Both come from the BCM2711 device tree, so check them against
   `bcm2711.dtsi` when writing the code. Using `arm_gic`/`gic_v2.c` is also
   what the profiler's patch `0001` hooks, so the overlay ports with no
   GIC rework.
3. **Memory.** Start with `MEMBASE 0`, `MEMSIZE 0x10000000` (256MB),
   `KERNEL_BASE 0x80000000`, `KERNEL_LOAD_OFFSET 0x8000`, the same as rpi2.
   The DTB at `0x2eff3b00` is outside that, so nothing to reserve yet.
   Later: read the real size from the DTB passed in `r2`. LK's arm start
   code saves r0-r3 as boot args; confirm the symbol name (`lk_boot_args`)
   in `start.S`. Don't use the rpi3 code's "DTB at `KERNEL_BASE`"
   assumption, which isn't true on Pi 4.
4. **New files.** `target/rpi4/rules.mk` (`PLATFORM := bcm28xx`), a
   `TARGET=rpi4` block in `platform/bcm28xx/rules.mk`, and
   `project/rpi4-test.mk` (copy of `rpi2-test.mk` with `TARGET := rpi4`).
   Keep all of it as a new patch, **`overlay/lk/0004-bcm28xx-add-rpi4.patch`**,
   so `setup.sh` applies it like 0001-0003 and upstream stays un-forked.
5. **SMP off for the first boot.** Build with `WITH_SMP=0` or
   `SMP_MAX_CPUS=1`, get a single core to the shell, and only then turn on
   the mailbox release.

Milestones, each checked through the chainloader log:

- **M1 banner:** LK's `welcome to lk` banner and init log arrive after
  `CRC OK, jumping to 0x00008000`. If the log stops right after that
  line, LK died before the console came up (MMU or early init). The
  cheapest probe: in `start.S`, before the MMU is enabled, store a
  character to the PL011 data register at `0xFE201000`; the chainloader has
  the UART ready. Move the probe forward to bisect.
- **M2 interrupts:** a shell prompt, typing works (UART RX interrupt via
  GIC ID 153; lines typed into `pi4_serial_boot.py` are forwarded), and
  timer-driven sleep/`threads` behave (GIC ID 30).
- **M3 SMP:** all 4 cores up via mailbox 3, confirmed in LK's boot log or
  shell.
- **M4 memory:** real memory size from the DTB.
- **M5 profiler on hardware:** `project/profiler-rpi4.mk` (the rpi4 target
  plus `app/profiler`, same `-fno-omit-frame-pointer` flags), with patches
  0001-0003 checked on this build. **Note:** `scripts/pc_histogram.py`
  pulls the sample ring buffers out through QEMU's QMP `pmemsave`, which
  real hardware doesn't have. On the Pi the buffers have to come out over
  the serial console, e.g. a shell command that hex-dumps them, parsed by
  a host script. Plan that as part of M5.

After M5 the original plan continues: step 5 (EXIDX unwinding) and step 6
(real PMU events via `PMCEID0`/`PMCEID1`). They're listed under "Next
steps, in order" below.

### Things that would make iteration faster (optional)

- A `reboot` shell command in LK: write the BCM2711 PM watchdog, `PM_RSTC`
  and `PM_WDOG` at peripheral base `+0x100000`. Pi then comes back to
  `SBOOT?` without a physical power-cycle, and the host script could
  trigger it itself.
- A faster baud rate in the chainloader and script. The UART clock is
  already 48MHz, so 921600 is `IBRD=3`/`FBRD=16`. This needs a
  bootloader update on the SD card.
- JTAG (`enable_jtag_gpio=1`, GPIO22-27) with an FT2232H adapter and
  OpenOCD, for when M1 dies before any output.

## Status before the 2026-09-26 update

**Physical setup, on the *office* PC (write-blocked, see below):**
- Raspberry Pi 4B, official USB-C PSU, microSD card
- SD card flashed with Raspberry Pi OS Lite (32-bit) via Raspberry Pi
  Imager — this is what supplies correctly-matched firmware files
  (`start4.elf`, `fixup4.dat`, `bcm2711-rpi-4-b.dtb`) rather than
  hand-sourcing them
- USB-to-TTL serial adapter (identifies as "Prolific USB-to-Serial",
  COM5 on the office PC, currently `Status: OK` — flagged as a
  chipset to watch since counterfeit PL2303 chips are common and
  sometimes get blocked by later Windows updates, but working today)
- PuTTY installed (via `winget install PuTTY.PuTTY`), confirmed
  launchable pre-configured for serial/COM5/115200

**Blocked**: writing to the SD card from the office PC failed --
`Copy-Item` reported "The media is write protected." Checked the two
standard native Windows write-block mechanisms
(`HKLM:\SYSTEM\CurrentControlSet\Control\StorageDevicePolicies\WriteProtect`
and the `RemovableStorageDevices` GPO key) -- neither is present, so
it's not the common native Windows policy. Likely either the card
adapter's physical write-lock switch, or a third-party endpoint
security/DLP agent intercepting removable-storage writes at a lower
level (common on managed office machines, and not something to probe
or try to work around on a work machine). **Not yet resolved.**

**Verified this SD card is the correct one** (not a mismatched Android
phone card that was briefly plugged in instead) -- confirmed by drive
label `boot`, FAT32, 256MB, containing exactly the expected Pi
firmware file set.

**Session-portability dead end, confirmed via Claude Code's own guide
agent**: a Claude Code session is machine-specific. "Remote Control"
gives *remote access to the original machine's session* -- execution
still happens there, it does not let a different physical machine
(home PC, wife's PC) get tool access through this same session. The
only real fix is starting a **fresh Claude Code session on whichever
machine actually has write access to the SD card**, which then picks up
full context from this repo + memory without needing this specific
conversation to carry over.

**Validation firmware already written, compiled, and disassembly-verified
-- not yet flashed or boot-tested on real hardware.** Source and
prebuilt binary are both in this repo now (see below) specifically so
they're available via `git pull` on whichever machine ends up with
working SD card access.

## The validation firmware (`experiments/pi4-baremetal/`)

Deliberately the smallest possible thing that proves the whole chain
works -- toolchain -> SD card -> firmware -> AArch32 boot -> UART --
before attempting any LK port:

- `start.S`: reads MPIDR, parks cores 1-3 in a `wfe` loop (Pi 4
  firmware releases all 4 Cortex-A72 cores simultaneously in AArch32
  when `arm_64bit=0`, unlike QEMU which only ever starts core 0 unless
  `-smp` is passed -- this parking is not optional), core 0 sets up a
  stack and branches to `main`.
- `main.c`: brings up **PL011** (not the mini-UART -- its clock is set
  deterministically via one mailbox call to exactly 3MHz, rather than
  depending on an inferred `core_freq` side effect that different
  sources documented inconsistently), computes `IBRD=1`/`FBRD=40` for
  115200 baud from that known 3MHz reference, then prints a boot
  banner and an incrementing heartbeat forever.
- `linker.ld`: flat layout, entry at `0x8000` (the standard physical
  load address Pi firmware expects for a 32-bit kernel image).
- Built with the same `arm-none-eabi-gcc` toolchain already used
  throughout this whole project -- nothing new to install.
- `kernel7l.img` (638 bytes) is the prebuilt output, committed
  directly -- copy it straight onto the SD card's boot partition, no
  rebuild required, though the source is there to rebuild if needed.

Disassembly-verified (not just "it compiled"): MPIDR read -> mask ->
park non-zero cores -> stack setup -> branch to `main`, exactly as
designed.

## Status (2026-09-26)

Steps 1-4 below are done: `TARGET=rpi4` boots on real Pi 4B hardware
over the serial chainloader, all the way to an interactive shell
(`entering main console loop`, `help` command list echoed back over
UART RX). Three real bugs were found and fixed getting there --
`ARM_CPU=cortex-a15` doesn't set `ARM_WITH_HYP` (only `cortex-a7`
does, so the Hyp->SVC drop was silently compiling out), a GICD
mapping-size panic (`GICD_MIN_SIZE` vs the real 4KB register block),
and `arm_gic_init_map()` being called too early -- before the VM
subsystem it depends on (`vmm_alloc_physical`) is initialized, which
was silently corrupting kernel BSS. See `overlay/lk/0004-bcm28xx-add-rpi4.patch`
for the fixes and commit `7bcd92a` for the full writeup.

## Next steps, in order

1. **M3 -- SMP.** Turn `WITH_SMP` back on for `rpi4` and release cores
   1-3 via the ARM-local mailbox-3 write (`platform_early_init`'s
   non-BCM2837 branch already has this code path; it just needs
   re-enabling and verifying on real hardware -- Stage 4 of the
   profiler work only ever verified SMP on QEMU, never this board).
2. **M4 -- real memory size.** Parse the actual DTB the firmware hands
   off instead of the hardcoded 256MB `MEMSIZE`.
3. **M5 -- profiler on hardware.** Port `project/profiler-rpi4.mk` +
   the existing profiler patches (`0001`-`0003`) onto this target.
   `scripts/pc_histogram.py`'s current sample extraction is QMP-based
   (QEMU-only) and has no real-hardware equivalent -- it needs to
   become a serial dump instead.
4. **EXIDX unwinding**, replacing the FP-chain walker
   (`walk_fp_chain` in `scripts/pc_histogram.py`). Developed and
   tested directly on the Pi 4B home-lab hardware, same as everything
   else here -- QEMU is not used for this project going forward.
5. **Real PMU event validation.** Read `PMCEID0`/`PMCEID1` on hardware
   to see what's actually implemented on this SoC's cores, then try
   counting a real event (e.g. `L1D_CACHE_REFILL`) across a known
   workload and confirm it responds. This is the actual open question
   the whole hardware bring-up exists to answer -- unverifiable on
   QEMU.

## Open risks not yet resolved

- Whether BCM2711's exact PMU implementation (event set, counter count)
  differs meaningfully from what's assumed -- check `PMCEID0`/`PMCEID1`
  once step 5 is reached, don't assume.
- LK's SMP bring-up has never been attempted on Pi 4B specifically --
  `WITH_SMP` is currently off for `rpi4` (deliberate first-cut choice
  to get one core working before adding secondary-core complexity).
