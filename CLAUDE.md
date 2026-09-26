# lk-perf: working notes for Claude Code sessions

Bare-metal statistical sampling profiler for LK (Little Kernel) on
AArch32. Stages 1-4 run on QEMU (see README.md, docs/DESIGN.md). The
current work is the **Raspberry Pi 4B real-hardware track**.
**docs/RPI4_BRINGUP.md is the source of truth for its state and plan:
read its "Start here" section before doing anything on the Pi.**

## Current task

Step 3 of docs/RPI4_BRINGUP.md: port LK to `TARGET=rpi4` (AArch32),
milestones M1-M5 in that doc. Before porting on a new machine, prove the
serial path works:

    python scripts/pi4_doctor.py --no-pi
    python scripts/pi4_doctor.py --boot-test   # user power-cycles the Pi when told

Don't start porting work until `--boot-test` shows the heartbeat.

## How the Pi is driven

- The SD card holds the serial chainloader (`experiments/pi4-serialboot`).
  It doesn't change during normal work; images go over the USB-serial
  cable with `scripts/pi4_serial_boot.py <image> --log <file>`. The port
  is auto-detected.
- There's no reset line, so **the user power-cycles the Pi by hand**.
  Ask them to, then read the log. Don't claim the Pi rebooted until the
  log shows the chainloader banner.
- One program per serial port: close PuTTY before running the scripts,
  and stop the scripts before the user opens PuTTY or moves the Pi.
- Nothing writes the SD card at runtime, so unplugging the Pi is always
  safe.

## Rules the user has set

- **RunPod (the Linux build box):** controlled programmatically. The API
  key lives in the `RUNPOD_API_KEY` Windows user environment variable on
  this machine (not in any repo file, not in Claude's memory files) --
  read it from there for pod create/stop/balance checks via the RunPod
  GraphQL API (`https://api.runpod.io/graphql`). Never write the key's
  value into a file, commit, or memory entry. Any Linux box or WSL works
  the same way (`./setup.sh` expects apt).
- **Ask first** before driver installs, anything needing admin/UAC,
  writing to the SD card, or anything that costs money.
- **Commits:** author `Somraj Mani <somraj.mani@gmail.com>`. Set it
  per-repo (`git config user.name/user.email`) if the machine's global
  identity differs; one machine's global identity belongs to someone
  else, so don't change globals. Commit and push at each working
  milestone and record hardware findings in docs/RPI4_BRINGUP.md.
- LK changes go in `overlay/lk/NNNN-*.patch` (applied by `setup.sh`);
  upstream LK is cloned, never forked or vendored.

## Environment gotchas

- **`git push` from Claude Code's shell can fail with "Authentication
  failed"** without ever showing a sign-in prompt. Claude's shell sets
  `GCM_INTERACTIVE=never` and `GIT_TERMINAL_PROMPT=0`. Push from a
  separate window with those variables removed so Git Credential Manager
  can open its browser sign-in once. On Windows:
  `Start-Process powershell -Wait -ArgumentList '-NoProfile','-Command',"Remove-Item Env:GCM_INTERACTIVE,Env:GIT_TERMINAL_PROMPT -EA 0; Set-Location '<repo>'; git push origin main"`
- **Windows 11 + PL2303TA serial adapter:** needs Prolific driver
  3.8.28.0 (steps in docs/RPI4_BRINGUP.md). `pi4_doctor.py` detects the
  block.
- **Rebuilding the small Pi images on Windows** (`experiments/pi4-*`):
  `winget install Arm.GnuArmEmbeddedToolchain`. It isn't added to PATH;
  the binaries are in
  `C:\Program Files (x86)\Arm GNU Toolchain arm-none-eabi\<ver>\bin`.
  Build commands are in each experiment's README. LK itself needs Linux.
- `core.autocrlf` warnings on Windows are harmless; `.gitattributes`
  keeps `*.img` binary.
