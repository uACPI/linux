// SPDX-License-Identifier: GPL-2.0
/*
 * ACPI table access for the uACPI shim.
 */

#include <linux/kernel.h>
#include <linux/string.h>
#include <linux/spinlock.h>

#include "shim.h"

/*
 * uACPI identifies a mapped table by its internal index, while the ACPICA
 * interface hands callers a bare header pointer and later expects
 * acpi_put_table() to release it. Track the pointer -> index association so the
 * matching uacpi_table_unref() can be issued.
 *
 * acpi_get_table() is invoked extremely early (e.g. early_acpi_boot_init ->
 * acpi_blacklisted), long before the slab allocator is online, so this lookup
 * must not allocate. A small fixed-size table is more than enough: the number
 * of distinct ACPI tables on real systems is in the low tens.
 */
struct shim_table_map {
	void *ptr;
	uacpi_size index;
};

#define SHIM_TABLE_MAP_MAX 256
static struct shim_table_map shim_table_map[SHIM_TABLE_MAP_MAX];
static DEFINE_SPINLOCK(shim_table_lock);

static void shim_table_remember(void *ptr, uacpi_size index)
{
	int i, free = -1;

	spin_lock(&shim_table_lock);
	for (i = 0; i < SHIM_TABLE_MAP_MAX; i++) {
		if (shim_table_map[i].ptr == ptr) {
			shim_table_map[i].index = index;
			spin_unlock(&shim_table_lock);
			return;
		}
		if (free < 0 && !shim_table_map[i].ptr)
			free = i;
	}
	if (free >= 0) {
		shim_table_map[free].ptr = ptr;
		shim_table_map[free].index = index;
	}
	spin_unlock(&shim_table_lock);
}

static bool shim_table_lookup(void *ptr, uacpi_size *index)
{
	int i;

	spin_lock(&shim_table_lock);
	for (i = 0; i < SHIM_TABLE_MAP_MAX; i++) {
		if (shim_table_map[i].ptr == ptr) {
			*index = shim_table_map[i].index;
			spin_unlock(&shim_table_lock);
			return true;
		}
	}
	spin_unlock(&shim_table_lock);
	return false;
}

acpi_status acpi_get_table(acpi_string signature, u32 instance,
			   struct acpi_table_header **out_table)
{
	uacpi_table tbl;
	uacpi_status st;
	u32 want, n;

	if (!signature || !out_table)
		return AE_BAD_PARAMETER;

	*out_table = NULL;

	st = uacpi_table_find_by_signature(signature, &tbl);
	if (st != UACPI_STATUS_OK)
		return uacpi_to_acpi_status(st);

	want = instance ? instance : 1;
	for (n = 1; n < want; n++) {
		st = uacpi_table_find_next_with_same_signature(&tbl);
		if (st != UACPI_STATUS_OK)
			return uacpi_to_acpi_status(st);
	}

	shim_table_remember(tbl.ptr, tbl.index);
	*out_table = tbl.ptr;
	return AE_OK;
}

void acpi_put_table(struct acpi_table_header *table)
{
	uacpi_table tbl;
	uacpi_size index;

	if (!table)
		return;

	if (!shim_table_lookup(table, &index))
		return;

	tbl.ptr = table;
	tbl.index = index;
	uacpi_table_unref(&tbl);
}

acpi_status acpi_get_table_header(acpi_string signature, u32 instance,
				  struct acpi_table_header *out_header)
{
	uacpi_size i, count, want, found = 0;

	if (!signature || !out_header)
		return AE_BAD_PARAMETER;

	/*
	 * Unlike acpi_get_table(), this only needs the header. Map just the
	 * header bytes rather than going through uacpi_table_find_by_signature()
	 * (which maps and checksums the entire table). That matters during early
	 * boot: a large firmware table (e.g. a big DSDT/SSDT) exceeds the
	 * early_memremap window, so full-mapping it would fail there.
	 */
	want = instance ? instance : 1;
	count = uacpi_table_count();

	for (i = 0; i < count; i++) {
		uacpi_table_info info;

		if (uacpi_table_info_get_by_index(i, &info) != UACPI_STATUS_OK)
			continue;
		if (memcmp(info.signature, signature, ACPI_NAMESEG_SIZE))
			continue;
		if (++found < want)
			continue;

		if (info.origin == UACPI_TABLE_ORIGIN_HOST_VIRTUAL) {
			memcpy(out_header, info.virt_addr, sizeof(*out_header));
		} else {
			void *p = uacpi_kernel_map(info.phys_addr,
						   sizeof(*out_header));

			if (p == UACPI_MAP_FAILED)
				return AE_NO_MEMORY;
			memcpy(out_header, p, sizeof(*out_header));
			uacpi_kernel_unmap(p, sizeof(*out_header));
		}
		return AE_OK;
	}

	return AE_NOT_FOUND;
}

acpi_status acpi_get_table_by_index(u32 table_index,
				    struct acpi_table_header **out_table)
{
	uacpi_table tbl;
	uacpi_status st;

	if (!out_table)
		return AE_BAD_PARAMETER;

	*out_table = NULL;

	st = uacpi_table_get_by_index(table_index, &tbl);
	if (st != UACPI_STATUS_OK)
		return uacpi_to_acpi_status(st);

	shim_table_remember(tbl.ptr, tbl.index);
	*out_table = tbl.ptr;
	return AE_OK;
}

acpi_status acpi_install_table(struct acpi_table_header *table)
{
	uacpi_table tbl;

	return uacpi_to_acpi_status(uacpi_table_install(table, &tbl));
}

acpi_status acpi_install_physical_table(acpi_physical_address address)
{
	uacpi_table tbl;

	return uacpi_to_acpi_status(uacpi_table_install_physical(address, &tbl));
}

acpi_status acpi_load_table(struct acpi_table_header *table, u32 *table_idx)
{
	uacpi_table tbl;
	uacpi_status st;

	st = uacpi_table_install(table, &tbl);
	if (st != UACPI_STATUS_OK)
		return uacpi_to_acpi_status(st);

	st = uacpi_table_load(tbl.index);
	if (st != UACPI_STATUS_OK)
		return uacpi_to_acpi_status(st);

	if (table_idx)
		*table_idx = tbl.index;
	return AE_OK;
}

acpi_status acpi_unload_table(u32 table_index)
{
	return AE_SUPPORT;
}

acpi_status acpi_unload_parent_table(acpi_handle object)
{
	return AE_SUPPORT;
}

/* Table load/unload event notifications (used for sysfs and dynamic SSDTs). */
static acpi_table_handler shim_table_handler;
static void *shim_table_handler_ctx;

acpi_status acpi_install_table_handler(acpi_table_handler handler, void *context)
{
	if (!handler)
		return AE_BAD_PARAMETER;
	if (shim_table_handler)
		return AE_ALREADY_EXISTS;

	shim_table_handler = handler;
	shim_table_handler_ctx = context;
	return AE_OK;
}

acpi_status acpi_remove_table_handler(acpi_table_handler handler)
{
	if (handler != shim_table_handler)
		return AE_BAD_PARAMETER;

	shim_table_handler = NULL;
	shim_table_handler_ctx = NULL;
	return AE_OK;
}
