# SPDX-License-Identifier: GPL-2.0
#
# uACPI library (upstream sources, compiled via thin wrappers).
# This is external code, so the kernel's stricter warnings are relaxed.
#

ccflags-y := -I $(srctree)/uACPI/include -I $(srctree)/drivers/acpi/uacpi
ccflags-y += -DUACPI_OVERRIDE_TYPES
ccflags-y += -Wno-unused-parameter -Wno-unused-but-set-variable \
	     -Wno-sign-compare -Wno-missing-prototypes \
	     -Wno-missing-declarations -Wno-override-init \
	     -Wno-shadow -Wno-format

obj-y := default_handlers.o event.o interpreter.o io.o mutex.o \
	 namespace.o notify.o opcodes.o opregion.o osi.o \
	 registers.o resources.o shareable.o sleep.o stdlib.o \
	 tables.o types.o uacpi.o utilities.o
