# QEMU ARM32 virt project for the bare-metal statistical sampling profiler.
# gcc-arm-none-eabi (apt): setup.sh installs it, LK auto-finds it on PATH.
#
# SMP: qemu-virt-arm's platform rules.mk defaults WITH_SMP := 1 already;
# boot with `qemu-system-arm ... -smp N` (N>1) to actually run multiple
# cores -- no project-level change needed to go from N=1 to SMP.
#
# -fno-omit-frame-pointer applies globally, not just to app/profiler:
# Stage 3's offline unwinder walks the FP chain through arbitrary LK
# code the sampled PC happens to be in, so every translation unit needs
# real frame pointers, not just the profiler module itself.
#
# -fno-optimize-sibling-calls: without it GCC tail-call-collapses
# profiler_workload_outer/mid (confirmed via objdump -- they compile to
# bare `b.w` branches with no real stack frame at all), which is correct
# compiler behavior but leaves nothing for ANY unwinder to recover, not
# just this one -- disabling it here is about giving the test workload a
# genuine multi-level chain to unwind, not a Stage-3-specific workaround.
GLOBAL_COMPILEFLAGS += -fno-omit-frame-pointer -fno-optimize-sibling-calls

MODULES += \
	app/shell \
	app/profiler

include project/target/qemu-virt-arm32.mk
