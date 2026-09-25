# QEMU ARM32 virt project for the bare-metal statistical sampling profiler.
# gcc-arm-none-eabi (apt): setup.sh installs it, LK auto-finds it on PATH.
#
# SMP: qemu-virt-arm's platform rules.mk defaults WITH_SMP := 1 already;
# boot with `qemu-system-arm ... -smp N` (N>1) to actually run multiple
# cores -- no project-level change needed to go from N=1 to SMP.

MODULES += \
	app/shell \
	app/profiler

include project/target/qemu-virt-arm32.mk
