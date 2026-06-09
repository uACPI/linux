// SPDX-License-Identifier: GPL-2.0
/*
 * Definitions of the ACPICA global variables consumed by the rest of the
 * kernel. ACPICA normally defines these in acglobal.c; since that file is not
 * built in the uACPI configuration, we materialize the exact same set (with
 * their canonical initial values) by enabling the DEFINE_ACPI_GLOBALS path of
 * the unchanged ACPICA headers.
 *
 * acpi_gbl_FADT is populated from uACPI at init time (see shim_init.c).
 */

#define DEFINE_ACPI_GLOBALS

#include <linux/acpi.h>

/*
 * A couple of ACPICA globals live in the internal acglobal.h rather than the
 * public headers, so DEFINE_ACPI_GLOBALS does not cover them. osl.c writes
 * these GPE block shadow addresses directly, so define them here.
 */
unsigned long acpi_gbl_xgpe0_block_logical_address;
unsigned long acpi_gbl_xgpe1_block_logical_address;
