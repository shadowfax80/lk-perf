#!/bin/bash
# Install the Ubuntu ARM GNU toolchain, clone upstream LK at its current
# default-branch tip (no pin -- deliberately tracks latest), and apply the
# profiler overlay (app/profiler + a small core-LK patch).
#
# No custom toolchain needed: LK's own arch/arm/toolchain.mk auto-probes for
# `arm-none-eabi-gcc` on PATH among its candidate prefixes, so apt's
# gcc-arm-none-eabi package needs zero LK-side configuration.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
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
        git clone -q https://github.com/littlekernel/lk.git "$LK_DIR"
    else
        echo "Pulling latest littlekernel/lk into $LK_DIR ..."
        git -C "$LK_DIR" checkout -q -- . 2>/dev/null || true
        git -C "$LK_DIR" pull -q --ff-only
    fi
    echo "LK at: $(git -C "$LK_DIR" rev-parse --short HEAD) ($(git -C "$LK_DIR" log -1 --format=%s))"
}

apply_lk_patches() {
    local patch_dir="$ROOT/overlay/lk"
    [ -d "$patch_dir" ] || return 0
    for patch in "$patch_dir"/*.patch; do
        [ -e "$patch" ] || continue
        if git -C "$LK_DIR" apply --check --reverse "$patch" >/dev/null 2>&1; then
            echo "  already applied: $(basename "$patch")"
            continue
        fi
        echo "  applying: $(basename "$patch")"
        git -C "$LK_DIR" apply "$patch"
    done
}

apply_overlay() {
    echo "Applying profiler overlay ..."
    mkdir -p "$LK_DIR/app" "$LK_DIR/project"
    rsync -a "$ROOT/app/profiler/" "$LK_DIR/app/profiler/"
    cp "$ROOT/project/profiler.mk" "$LK_DIR/project/profiler.mk"
    [ -d "$LK_DIR/app/profiler" ] && find "$LK_DIR/app/profiler" -name "*.sh" -exec chmod +x {} \; || true
    apply_lk_patches
}

echo "==> [1/3] Ensuring arm-none-eabi toolchain + qemu-system-arm ..."
ensure_toolchain
echo "==> [2/3] Cloning/updating upstream LK (latest, no pin) ..."
clone_lk
echo "==> [3/3] Applying profiler overlay ..."
apply_overlay

echo ""
echo "Toolchain:     $(command -v arm-none-eabi-gcc)"
echo "LK tree ready: $LK_DIR"
echo "Build:  cd $LK_DIR && make profiler -j\$(nproc)"
echo "Boot:   qemu-system-arm -machine virt -cpu cortex-a15 -smp 1 -m 512 -nographic -kernel $LK_DIR/build-profiler/lk.elf"
