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
    apt-get install -y -qq gcc-arm-none-eabi
}

# Needed by scripts/dwarf_unwind.py (DWARF CFI-based stack unwinding --
# chosen over ARM's own EXIDX, see docs/RPI4_BRINGUP.md and README.md
# for why) and scripts/test_dwarf_unwind.py's regression test.
ensure_pyelftools() {
    if python3 -c "import elftools" >/dev/null 2>&1; then
        echo "pyelftools already present"
        return 0
    fi
    echo "Installing pyelftools ..."
    python3 -m pip install --quiet pyelftools
}

clone_lk() {
    if [ ! -d "$LK_DIR/.git" ]; then
        echo "Cloning littlekernel/lk into $LK_DIR ..."
        mkdir -p "$(dirname "$LK_DIR")"
        git clone -q https://github.com/littlekernel/lk.git "$LK_DIR"
    else
        echo "Pulling latest littlekernel/lk into $LK_DIR ..."
        # git reset --hard, not `checkout -- .`: the latter restores
        # tracked files from the INDEX, not HEAD, and silently does the
        # wrong thing if anything was ever `git add`ed without a
        # following commit (real bug found and fixed during this
        # project's own development -- a stray staged file left
        # apply_lk_patches() re-applying patches on top of a
        # half-applied baseline instead of pristine HEAD). Deliberately
        # NOT `git clean -fd` alongside it: that would also delete the
        # untracked app/profiler/ and project/profiler.mk overlay files
        # apply_overlay() is about to re-copy anyway, and (more
        # importantly) build-profiler/ output, forcing a needless full
        # rebuild on every setup.sh re-run for no correctness benefit.
        git -C "$LK_DIR" reset -q --hard HEAD 2>/dev/null || true
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
    [ -d "$LK_DIR/app/profiler" ] && find "$LK_DIR/app/profiler" -name "*.sh" -exec chmod +x {} \; || true

    echo "Applying rpi4 target overlay ..."
    mkdir -p "$LK_DIR/target/rpi4"
    cp "$ROOT/project/rpi4-test.mk" "$LK_DIR/project/rpi4-test.mk"
    cp "$ROOT/target/rpi4/rules.mk" "$LK_DIR/target/rpi4/rules.mk"
    rsync -a "$ROOT/app/love/" "$LK_DIR/app/love/"

    apply_lk_patches
}

# Fetched, not vendored -- same policy as LK itself (cloned fresh, not
# committed into this repo). Single standalone script, no build step,
# verified end-to-end against this project's own .folded output
# (real function names render as distinct SVG frames, exit 0).
ensure_flamegraph() {
    local dest="$ROOT/scripts/flamegraph.pl"
    if [ -e "$dest" ]; then
        echo "scripts/flamegraph.pl already present"
        return 0
    fi
    echo "Fetching flamegraph.pl (brendangregg/FlameGraph) ..."
    curl -sL -o "$dest" \
        https://raw.githubusercontent.com/brendangregg/FlameGraph/master/flamegraph.pl
}

echo "==> [1/5] Ensuring arm-none-eabi toolchain ..."
ensure_toolchain
echo "==> [2/5] Ensuring pyelftools (DWARF-CFI unwinding) ..."
ensure_pyelftools
echo "==> [3/5] Cloning/updating upstream LK (latest, no pin) ..."
clone_lk
echo "==> [4/5] Applying profiler overlay ..."
apply_overlay
echo "==> [5/5] Ensuring flamegraph.pl ..."
ensure_flamegraph

echo ""
echo "Toolchain:     $(command -v arm-none-eabi-gcc)"
echo "LK tree ready: $LK_DIR"
echo "This project targets real Raspberry Pi 4B hardware only (TARGET=rpi4)."
echo "Build:  cd $LK_DIR && make rpi4-test -j\$(nproc)"
echo "Send over serial: python3 scripts/pi4_serial_boot.py \$LK_DIR/build-rpi4-test/lk.bin --port COM5"
echo "Unwind test: python3 scripts/test_dwarf_unwind.py"
echo "Render flame graph: perl scripts/flamegraph.pl <folded-file> > flame.svg"
