#!/bin/bash
# Install the Ubuntu ARM GNU toolchain, clone upstream LK at a pinned commit,
# and apply the profiler overlay.
#
# No custom toolchain needed: LK's own arch/arm/toolchain.mk auto-probes for
# `arm-none-eabi-gcc` on PATH among its candidate prefixes, so apt's
# gcc-arm-none-eabi package needs zero LK-side configuration.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
LK_PIN="$(tr -d '[:space:]' < "$ROOT/LK_PIN")"
LK_DIR="${LK_DIR:-$ROOT/build/lk}"

ensure_toolchain() {
    if command -v arm-none-eabi-gcc >/dev/null 2>&1; then
        echo "arm-none-eabi-gcc already present: $(command -v arm-none-eabi-gcc)"
        return 0
    fi
    echo "Installing gcc-arm-none-eabi via apt ..."
    apt-get update -qq
    apt-get install -y -qq gcc-arm-none-eabi qemu-system-arm
}

clone_lk() {
    if [ ! -d "$LK_DIR/.git" ]; then
        echo "Cloning littlekernel/lk into $LK_DIR ..."
        mkdir -p "$(dirname "$LK_DIR")"
        git clone https://github.com/littlekernel/lk.git "$LK_DIR"
    fi

    echo "Checking out LK pin $LK_PIN ..."
    git -C "$LK_DIR" fetch origin --tags
    git -C "$LK_DIR" checkout --force "$LK_PIN"
}

apply_overlay() {
    echo "Applying profiler overlay ..."
    mkdir -p "$LK_DIR/app" "$LK_DIR/project"
    rsync -a "$ROOT/app/profiler/" "$LK_DIR/app/profiler/"
    cp "$ROOT/project/profiler.mk" "$LK_DIR/project/profiler.mk"
    [ -d "$LK_DIR/app/profiler" ] && find "$LK_DIR/app/profiler" -name "*.sh" -exec chmod +x {} \; || true
}

echo "==> [1/3] Ensuring arm-none-eabi toolchain + qemu-system-arm ..."
ensure_toolchain
echo "==> [2/3] Cloning/checking out upstream LK ..."
clone_lk
echo "==> [3/3] Applying profiler overlay ..."
apply_overlay

echo ""
echo "Toolchain:     $(command -v arm-none-eabi-gcc)"
echo "LK tree ready: $LK_DIR"
echo "Build:  cd $LK_DIR && make profiler -j\$(nproc)"
echo "Boot:   qemu-system-arm -machine virt -cpu cortex-a15 -smp 1 -m 512 -nographic -kernel $LK_DIR/build-profiler/lk.elf"
