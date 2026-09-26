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

**Next up: step 3 (LK `TARGET=rpi4` port).**

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

## Next steps, in order

1. **Get real SD card write access on some machine.** Either resolve
   the office PC's write block, or start a fresh Claude Code session
   on a machine that has the card physically attached and genuine
   write access (home PC, wife's PC, etc.) -- `git pull` this repo
   there to get everything above.
2. **First hardware milestone**: back up the existing `kernel7l.img` to
   `kernel7l-linux-backup.img`, add `arm_64bit=0` + `enable_uart=1` to
   `config.txt`, copy `experiments/pi4-baremetal/kernel7l.img` onto the
   boot partition, boot with PuTTY open on the correct COM port at
   115200 8N1 *before* powering on. Confirm the boot banner and
   heartbeat counter actually arrive. This is the one thing that must
   work before anything else is worth attempting.
3. **Port LK to Pi 4B.** Confirmed via direct inspection of LK's
   upstream source: there is **no existing `rpi4`/BCM2711 target** --
   only `rpi2` (BCM2836, AArch32, Cortex-A7, real structural precedent)
   and `rpi3` (BCM2837, AArch64). New work: a `bcm28xx` `TARGET=rpi4`
   block with BCM2711's peripheral base (`0xFE000000`, confirmed --
   different from earlier Pi generations). Reuse LK's existing
   `dev/interrupt/arm_gic`/`gic_v2.c` driver instead of `bcm28xx`'s
   legacy `intc.c`, since GIC-400 is genuinely GICv2-compatible.
4. **Port lk-perf's overlay patches to this new target.** `0001`
   (GIC tick hook) and `0002`/`0003` (per-CPU FP capture in
   `exceptions.S`) target the same GICv2 driver the new Pi 4 port would
   use -- expect real but bounded rework, nowhere near what the FVP's
   GICv3 mismatch would have needed.
5. **Build EXIDX unwinding**, replacing the FP-chain walker
   (`walk_fp_chain` in `scripts/pc_histogram.py`). No hardware
   dependency at all -- could genuinely be done and tested on the
   existing QEMU setup before ever touching the Pi, if useful to
   de-risk separately from the hardware bring-up above.
6. **Only once 1-5 work**: attempt real PMU event sampling -- read
   `PMCEID0`/`PMCEID1` to see what's actually implemented on this A72,
   try counting a real event (e.g. `L1D_CACHE_REFILL`) across a known
   workload, confirm it responds. This is the actual open question
   this whole detour exists to answer.

## Open risks not yet resolved

- SD card write access itself (see "Blocked" above) -- the immediate
  blocker, nothing past step 1 can happen without it.
- Whether BCM2711's exact PMU implementation (event set, counter count)
  differs meaningfully from what a real target would have -- check
  `PMCEID0`/`PMCEID1` once hardware access exists, don't assume.
- LK's SMP bring-up has never been attempted on Pi 4B specifically --
  Stage 4 only verified SMP on QEMU. `WITH_SMP := 1` already exists in
  `bcm28xx/rules.mk` at the platform level, but per-core bring-up
  (secondary core release via PSCI or direct spin-table, whichever Pi 4
  actually uses) is unverified for this specific board.
