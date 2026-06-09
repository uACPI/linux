// SPDX-License-Identifier: GPL-2.0
/*
 * Subsystem initialization for the uACPI shim.
 *
 * Maps the ACPICA init state machine used by the Linux ACPI core
 * (acpi_table_init -> acpi_early_init -> acpi_bus_init) onto the uACPI
 * initialization flow, and populates acpi_gbl_FADT.
 */

#include <linux/kernel.h>
#include <linux/string.h>

#include "shim.h"

#include <acpi/acpi_io.h>

#include <uacpi/sleep.h>

/*
 * Temporary storage handed to uACPI for early (pre-heap) table bookkeeping.
 * ~56 bytes are needed per table; sized for well over ACPI_MAX_TABLES entries.
 */
static u8 uacpi_early_table_buf[16384] __initdata __aligned(8);

static void uacpi_shim_populate_fadt(void)
{
	struct acpi_fadt *ufadt;
	uacpi_bool reduced = UACPI_FALSE;

	if (uacpi_table_fadt(&ufadt) == UACPI_STATUS_OK)
		memcpy(&acpi_gbl_FADT, ufadt, sizeof(acpi_gbl_FADT));

	uacpi_is_platform_reduced_hardware(&reduced);
	acpi_gbl_reduced_hardware = reduced;
}

acpi_status __init acpi_initialize_tables(struct acpi_table_desc *initial_storage,
					  u32 initial_table_count, u8 allow_resize)
{
	uacpi_status st;

	st = uacpi_setup_early_table_access(uacpi_early_table_buf,
					    sizeof(uacpi_early_table_buf));
	if (st != UACPI_STATUS_OK)
		return uacpi_to_acpi_status(st);

	uacpi_shim_populate_fadt();
	return AE_OK;
}

acpi_status acpi_reallocate_root_table(void)
{
	return AE_OK;
}

acpi_status acpi_initialize_subsystem(void)
{
	uacpi_status st;

	st = uacpi_initialize(UACPI_FLAG_NO_ACPI_MODE);
	if (st != UACPI_STATUS_OK)
		return uacpi_to_acpi_status(st);

	uacpi_shim_populate_fadt();
	return AE_OK;
}

acpi_status acpi_enable_subsystem(u32 flags)
{
	if (!(flags & ACPI_NO_ACPI_ENABLE))
		return uacpi_to_acpi_status(uacpi_enter_acpi_mode());
	return AE_OK;
}

acpi_status acpi_load_tables(void)
{
	return uacpi_to_acpi_status(uacpi_namespace_load());
}

acpi_status acpi_initialize_objects(u32 flags)
{
	return uacpi_to_acpi_status(uacpi_namespace_initialize());
}

acpi_status acpi_enable(void)
{
	return uacpi_to_acpi_status(uacpi_enter_acpi_mode());
}

acpi_status acpi_disable(void)
{
	return uacpi_to_acpi_status(uacpi_leave_acpi_mode());
}

acpi_status acpi_terminate(void)
{
	uacpi_state_reset();
	return AE_OK;
}

acpi_status acpi_subsystem_status(void)
{
	return uacpi_get_current_init_level() >=
		UACPI_INIT_LEVEL_SUBSYSTEM_INITIALIZED ? AE_OK : AE_ERROR;
}

acpi_status acpi_find_root_pointer(acpi_physical_address *out_address)
{
	acpi_physical_address pa = acpi_os_get_root_pointer();

	if (!pa)
		return AE_NOT_FOUND;
	*out_address = pa;
	return AE_OK;
}
