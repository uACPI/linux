# SPDX-License-Identifier: GPL-2.0
#
# Makefile for the uACPI compatibility shim
#
# The Linux ACPI subsystem keeps using the ACPICA interface (include/acpi/*).
# The files here translate that interface into the uACPI library, which is
# compiled from drivers/acpi/uacpi/lib/.
#

ccflags-y := -I $(srctree)/uACPI/include -I $(srctree)/drivers/acpi/uacpi
ccflags-y += -DUACPI_OVERRIDE_TYPES
ccflags-$(CONFIG_ACPI_DEBUG) += -DACPI_DEBUG_OUTPUT

obj-y += shim_globals.o
obj-y += shim_init.o
obj-y += shim_tables.o
obj-y += shim_namespace.o
obj-y += shim_object.o
obj-y += shim_event.o
obj-y += shim_resource.o
obj-y += shim_misc.o
obj-y += shim_exports.o
obj-y += glue.o

obj-y += lib/
