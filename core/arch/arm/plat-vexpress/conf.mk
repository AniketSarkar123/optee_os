PLATFORM_FLAVOR ?= qemu_virt

ifeq ($(PLATFORM_FLAVOR),qemu_virt)
include core/arch/arm/cpu/cortex-a15.mk
endif
ifeq ($(PLATFORM_FLAVOR),fvp)
include core/arch/arm/cpu/cortex-armv8-0.mk
platform-debugger-arm := 1
endif
ifeq ($(PLATFORM_FLAVOR),juno)
include core/arch/arm/cpu/cortex-armv8-0.mk
platform-debugger-arm := 1
# Workaround 808870: Unconditional VLDM instructions might cause an
# alignment fault even though the address is aligned
# Either hard float must be disabled for AArch32 or strict alignment checks
# must be disabled
ifeq ($(CFG_SCTLR_ALIGNMENT_CHECK),y)
$(call force,CFG_TA_ARM32_NO_HARD_FLOAT_SUPPORT,y)
else
$(call force,CFG_SCTLR_ALIGNMENT_CHECK,n)
endif
endif #juno
ifeq ($(PLATFORM_FLAVOR),qemu_armv8a)
include core/arch/arm/cpu/cortex-armv8-0.mk
CFG_ARM64_core ?= y
supported-ta-targets ?= ta_arm64 ta_arm32
endif


ifeq ($(platform-debugger-arm),1)
# ARM debugger needs this
platform-cflags-debug-info = -gdwarf-2
platform-aflags-debug-info = -gdwarf-2
endif

ifeq ($(platform-flavor-armv8),1)
$(call force,CFG_WITH_ARM_TRUSTED_FW,y)
endif

$(call force,CFG_PL011,y)
$(call force,CFG_SECURE_TIME_SOURCE_CNTPCT,y)

ifeq ($(CFG_CORE_TPM_EVENT_LOG),y)
# NOTE: Below values for the TPM event log are implementation
# dependent and used mostly for debugging purposes.
# Care must be taken to properly configure them if used.
CFG_TPM_LOG_BASE_ADDR ?= 0x402c951
CFG_TPM_MAX_LOG_SIZE ?= 0x200
endif

ifneq ($(CFG_ARM64_core),y)
$(call force,CFG_ARM32_core,y)
endif

CFG_WITH_STATS ?= y
CFG_ENABLE_EMBEDDED_TESTS ?= y

ifeq ($(CFG_CORE_SEL2_SPMC),y)
$(call force,CFG_CORE_RESERVED_SHM,n)
CFG_GIC ?= n
else
$(call force,CFG_GIC,y)
endif

ifeq ($(PLATFORM_FLAVOR),fvp)
CFG_MULTI_CORE_HALTING ?= y
CFG_TEE_CORE_NB_CORE ?= 8
ifeq ($(CFG_CORE_SEL2_SPMC),y)
CFG_TZDRAM_START ?= 0x06281000
CFG_TZDRAM_SIZE  ?= 0x01D80000
else
CFG_TZDRAM_START ?= 0x06000000
CFG_TZDRAM_SIZE  ?= 0x02000000
endif
CFG_SHMEM_START  ?= 0x83000000
CFG_SHMEM_SIZE   ?= 0x00200000
# DRAM1 is defined above 4G
$(call force,CFG_CORE_LARGE_PHYS_ADDR,y)
$(call force,CFG_CORE_ARM64_PA_BITS,36)
CFG_AUTO_MAX_PA_BITS ?= y
ifeq ($(CFG_SCMI_SCPFW),y)
$(call force,CFG_SCMI_SCPFW_PRODUCT,fvp)
endif
ifeq ($(CFG_CORE_SEL1_SPMC),y)
CFG_CORE_ASYNC_NOTIF_GIC_INTID ?= 9
endif
endif

ifeq ($(PLATFORM_FLAVOR),juno)
CFG_MULTI_CORE_HALTING ?= y
CFG_TEE_CORE_NB_CORE ?= 6
CFG_TZDRAM_START ?= 0xff000000
CFG_TZDRAM_SIZE  ?= 0x00ff8000
CFG_SHMEM_START  ?= 0xfee00000
CFG_SHMEM_SIZE   ?= 0x00200000
# DRAM1 is defined above 4G
$(call force,CFG_CORE_LARGE_PHYS_ADDR,y)
$(call force,CFG_CORE_ARM64_PA_BITS,36)
CFG_CRYPTO_WITH_CE ?= y
CFG_ARM_SMCCC_TRNG ?= y
CFG_WITH_SOFTWARE_PRNG ?= n
endif

ifeq ($(PLATFORM_FLAVOR),qemu_virt)
CFG_CORE_HEAP_SIZE ?= 98304
CFG_MULTI_CORE_HALTING ?= y
CFG_TEE_CORE_NB_CORE ?= 4
# [0e00.0000 0e0f.ffff] is reserved to early boot
CFG_TZDRAM_START ?= 0x0e100000
CFG_TZDRAM_SIZE  ?= 0x00f00000
CFG_SHMEM_START  ?= 0x7fe00000
CFG_SHMEM_SIZE   ?= 0x00200000
# When Secure Data Path is enable, last MByte of TZDRAM is SDP test memory.
CFG_TEE_SDP_MEM_SIZE ?= 0x00400000
# Set VA space to 2MB for Kasan offset to match LPAE and 32bit MMU configs
CFG_TEE_RAM_VA_SIZE ?= 0x00200000
ifeq ($(CFG_CORE_SANITIZE_KADDRESS),y)
# CFG_ASAN_SHADOW_OFFSET is calculated as:
# (&__asan_shadow_start - (TEE_RAM_VA_START / 8)
# This is unfortunately currently not possible to do in make so we have to
# calculate it offline, there's some asserts in
# core/arch/arm/kernel/generic_boot.c to check that we got it right
CFG_ASAN_SHADOW_OFFSET ?= 0xc6a71c0
endif
$(call force,CFG_BOOT_SECONDARY_REQUEST,y)
$(call force,CFG_PSCI_ARM32,y)
$(call force,CFG_DT,y)
CFG_DTB_MAX_SIZE ?= 0x100000
CFG_CORE_ASYNC_NOTIF ?= y
CFG_CORE_ASYNC_NOTIF_GIC_INTID ?= 219
endif

ifeq ($(PLATFORM_FLAVOR),qemu_armv8a)
CFG_CORE_TZSRAM_EMUL_SIZE ?= 655360
CFG_CORE_HEAP_SIZE ?= 196608
CFG_MULTI_CORE_HALTING ?= y
CFG_TEE_CORE_NB_CORE ?= 4
CFG_AUTO_MAX_PA_BITS ?= y
ifneq ($(CFG_CORE_SEL2_SPMC),y)
# [0e00.0000 0e0f.ffff] is reserved to early boot
CFG_TZDRAM_START ?= 0x0e100000
CFG_TZDRAM_SIZE  ?= 0x00f00000
# SHM chosen arbitrary, in a way that it does not interfere
# with initial location of linux kernel, dtb and initrd.
CFG_SHMEM_START ?= 0x42000000
CFG_SHMEM_SIZE  ?= 0x00200000
# When Secure Data Path is enable, last MByte of TZDRAM is SDP test memory.
CFG_TEE_SDP_MEM_SIZE ?= 0x00400000
ifeq ($(CFG_CORE_SANITIZE_KADDRESS),y)
# See comment above
CFG_ASAN_SHADOW_OFFSET ?= 0xc6a71c0
endif
endif
$(call force,CFG_DT,y)
CFG_DTB_MAX_SIZE ?= 0x100000
ifeq ($(CFG_SCMI_SCPFW),y)
$(call force,CFG_SCMI_SCPFW_PRODUCT,fvp)
endif

CFG_CORE_ASYNC_NOTIF ?= y
ifeq ($(CFG_CORE_SEL1_SPMC),y)
CFG_CORE_ASYNC_NOTIF_GIC_INTID ?= 8
else ifneq ($(CFG_CORE_SEL2_SPMC),y)
CFG_CORE_ASYNC_NOTIF_GIC_INTID ?= 219
endif
endif #PLATFORM_FLAVOR==qemu_armv8a

ifneq (,$(filter $(PLATFORM_FLAVOR),qemu_virt qemu_armv8a))
CFG_DT_DRIVER_EMBEDDED_TEST ?= y
ifeq ($(CFG_DT_DRIVER_EMBEDDED_TEST),y)
$(call force,CFG_EMBED_DTB_SOURCE_FILE,embedded_dtb_test.dts,Mandated for DT tests)
endif
endif

CFG_PKCS11_TA ?= y
CFG_PKCS11_TA_RSA_X_509 ?= y
