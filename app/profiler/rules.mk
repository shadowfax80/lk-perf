LOCAL_DIR := $(GET_LOCAL_DIR)

MODULE := $(LOCAL_DIR)

MODULE_SRCS += \
	$(LOCAL_DIR)/profiler.c

MODULE_DEPS += \
	kernel \
	lib/console

include make/module.mk
