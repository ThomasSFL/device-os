include ../../shared/system_module_version.mk
USER_PART_MODULE_PATH ?= $(PROJECT_ROOT)/modules/xenon/user-part
include $(call rwildcard,$(USER_PART_MODULE_PATH)/,include.mk)


