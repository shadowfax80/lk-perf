LOCAL_DIR := $(GET_LOCAL_DIR)

TARGET := rpi4

# app/tests deliberately excluded: it pulls in lib/libm, whose own
# rules.mk compiles several routines (e_sqrt/e_pow/etc.) with hardware
# VFP instructions regardless of the kernel-level ARM_WITH_VFP setting
# -- calling any of them here would be a real bug, not just fidelity
# noise, since ARM_WITH_VFP is off (see platform/bcm28xx/rules.mk's
# rpi4 branch) and the kernel no longer saves/restores VFP context
# across interrupts or context switches. the target platform (Cortex-A55, this
# project's real PoC target) has no FPU/NEON hardware at all -- keep
# it that way here too rather than validating a math path that would
# never be exercisable on the real target.
MODULES += \
	app/shell \
	app/love \
	app/stringtests \
	lib/cksum \
	lib/debugcommands \

