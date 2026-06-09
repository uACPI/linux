// SPDX-License-Identifier: GPL-2.0
/*
 * Miscellaneous ACPICA interfaces for the uACPI shim: diagnostics, _OSI
 * interface management, timers, and assorted small helpers.
 */

#include <linux/kernel.h>
#include <linux/printk.h>

#include "shim.h"

#include <uacpi/osi.h>
#include <uacpi/registers.h>
#include <uacpi/sleep.h>

const char *acpi_format_exception(acpi_status exception)
{
	switch (exception) {
	case AE_OK:
		return "AE_OK";
	case AE_ERROR:
		return "AE_ERROR";
	case AE_NO_MEMORY:
		return "AE_NO_MEMORY";
	case AE_NOT_FOUND:
		return "AE_NOT_FOUND";
	case AE_NOT_EXIST:
		return "AE_NOT_EXIST";
	case AE_ALREADY_EXISTS:
		return "AE_ALREADY_EXISTS";
	case AE_TYPE:
		return "AE_TYPE";
	case AE_NULL_OBJECT:
		return "AE_NULL_OBJECT";
	case AE_NULL_ENTRY:
		return "AE_NULL_ENTRY";
	case AE_BUFFER_OVERFLOW:
		return "AE_BUFFER_OVERFLOW";
	case AE_BAD_PARAMETER:
		return "AE_BAD_PARAMETER";
	case AE_BAD_DATA:
		return "AE_BAD_DATA";
	case AE_TIME:
		return "AE_TIME";
	case AE_SUPPORT:
		return "AE_SUPPORT";
	case AE_ACCESS:
		return "AE_ACCESS";
	default:
		return "ACPI_ERROR";
	}
}

void acpi_error(const char *module_name, u32 line_number, const char *format,
		...)
{
	struct va_format vaf;
	va_list args;

	va_start(args, format);
	vaf.fmt = format;
	vaf.va = &args;
	printk(KERN_ERR "ACPI Error: %pV", &vaf);
	va_end(args);
}

void acpi_exception(const char *module_name, u32 line_number,
		    acpi_status status, const char *format, ...)
{
	struct va_format vaf;
	va_list args;

	va_start(args, format);
	vaf.fmt = format;
	vaf.va = &args;
	printk(KERN_ERR "ACPI Exception: %s, %pV",
	       acpi_format_exception(status), &vaf);
	va_end(args);
}

void acpi_warning(const char *module_name, u32 line_number, const char *format,
		  ...)
{
	struct va_format vaf;
	va_list args;

	va_start(args, format);
	vaf.fmt = format;
	vaf.va = &args;
	printk(KERN_WARNING "ACPI Warning: %pV", &vaf);
	va_end(args);
}

void acpi_info(const char *format, ...)
{
	struct va_format vaf;
	va_list args;

	va_start(args, format);
	vaf.fmt = format;
	vaf.va = &args;
	printk(KERN_INFO "ACPI: %pV", &vaf);
	va_end(args);
}

void acpi_bios_error(const char *module_name, u32 line_number,
		     const char *format, ...)
{
	struct va_format vaf;
	va_list args;

	va_start(args, format);
	vaf.fmt = format;
	vaf.va = &args;
	printk(KERN_ERR "ACPI BIOS Error (bug): %pV", &vaf);
	va_end(args);
}

void acpi_bios_exception(const char *module_name, u32 line_number,
			 acpi_status status, const char *format, ...)
{
	struct va_format vaf;
	va_list args;

	va_start(args, format);
	vaf.fmt = format;
	vaf.va = &args;
	printk(KERN_ERR "ACPI BIOS Exception (bug): %s, %pV",
	       acpi_format_exception(status), &vaf);
	va_end(args);
}

void acpi_bios_warning(const char *module_name, u32 line_number,
		       const char *format, ...)
{
	struct va_format vaf;
	va_list args;

	va_start(args, format);
	vaf.fmt = format;
	vaf.va = &args;
	printk(KERN_WARNING "ACPI BIOS Warning (bug): %pV", &vaf);
	va_end(args);
}

void acpi_debug_print(u32 requested_debug_level, u32 line_number,
		      const char *function_name, const char *module_name,
		      u32 component_id, const char *format, ...)
{
	struct va_format vaf;
	va_list args;

	if (!(requested_debug_level & acpi_dbg_level))
		return;

	va_start(args, format);
	vaf.fmt = format;
	vaf.va = &args;
	printk(KERN_DEBUG "ACPI: %pV", &vaf);
	va_end(args);
}

void acpi_debug_print_raw(u32 requested_debug_level, u32 line_number,
			  const char *function_name, const char *module_name,
			  u32 component_id, const char *format, ...)
{
	struct va_format vaf;
	va_list args;

	if (!(requested_debug_level & acpi_dbg_level))
		return;

	va_start(args, format);
	vaf.fmt = format;
	vaf.va = &args;
	printk(KERN_DEBUG "%pV", &vaf);
	va_end(args);
}

void acpi_trace_point(acpi_trace_event_type type, u8 begin, u8 *aml,
		      char *pathname)
{
}

acpi_status acpi_debug_trace(const char *name, u32 debug_level, u32 debug_layer,
			     u32 flags)
{
	acpi_gbl_trace_method_name = name;
	acpi_gbl_trace_dbg_level = debug_level;
	acpi_gbl_trace_dbg_layer = debug_layer;
	acpi_gbl_trace_flags = flags;
	return AE_OK;
}

/*
 * uACPI pre-registers the ACPI-spec OS feature strings ("Module Device" etc.)
 * as "host features" that are present but disabled by default, and toggled via
 * uacpi_{enable,disable}_host_interface() rather than the vendor-string
 * install/uninstall API. Map those names so that acpi_install_interface()
 * actually makes _OSI() report them as supported (matching ACPICA).
 */
static uacpi_host_interface shim_host_interface(const char *name)
{
	if (!strcmp(name, "Module Device"))
		return UACPI_HOST_INTERFACE_MODULE_DEVICE;
	if (!strcmp(name, "Processor Device"))
		return UACPI_HOST_INTERFACE_PROCESSOR_DEVICE;
	if (!strcmp(name, "3.0 Thermal Model"))
		return UACPI_HOST_INTERFACE_3_0_THERMAL_MODEL;
	if (!strcmp(name, "3.0 _SCP Extensions"))
		return UACPI_HOST_INTERFACE_3_0_SCP_EXTENSIONS;
	if (!strcmp(name, "Processor Aggregator Device"))
		return UACPI_HOST_INTERFACE_PROCESSOR_AGGREGATOR_DEVICE;
	return 0;
}

acpi_status acpi_install_interface(acpi_string interface_name)
{
	uacpi_host_interface hi = shim_host_interface(interface_name);

	if (hi)
		return uacpi_to_acpi_status(uacpi_enable_host_interface(hi));

	return uacpi_to_acpi_status(
		uacpi_install_interface(interface_name,
					UACPI_INTERFACE_KIND_VENDOR));
}

acpi_status acpi_remove_interface(acpi_string interface_name)
{
	uacpi_host_interface hi = shim_host_interface(interface_name);

	if (hi)
		return uacpi_to_acpi_status(uacpi_disable_host_interface(hi));

	return uacpi_to_acpi_status(uacpi_uninstall_interface(interface_name));
}

acpi_status acpi_update_interfaces(u8 action)
{
	return AE_OK;
}

acpi_status acpi_install_interface_handler(acpi_interface_handler handler)
{
	return AE_OK;
}

acpi_status acpi_purge_cached_objects(void)
{
	return AE_OK;
}

acpi_status acpi_get_system_info(struct acpi_buffer *ret_buffer)
{
	return AE_SUPPORT;
}

acpi_status acpi_get_statistics(struct acpi_statistics *stats)
{
	return AE_SUPPORT;
}

u32 acpi_check_address_range(acpi_adr_space_type space_id,
			     acpi_physical_address address, acpi_size length,
			     u8 warn)
{
	return 0;
}

/* Extract a bitfield of 'bits' width at bit 'off' from a 32-bit dword. */
static u32 pld_bits(u32 dw, u32 off, u32 bits)
{
	return (dw >> off) & ((1u << bits) - 1);
}

acpi_status acpi_decode_pld_buffer(u8 *in_buffer, acpi_size length,
				   struct acpi_pld_info **return_buffer)
{
	struct acpi_pld_info *pld;
	u32 dw[5] = {};
	u32 n, i;

	if (!in_buffer || !return_buffer ||
	    length < ACPI_PLD_REV1_BUFFER_SIZE)
		return AE_BAD_PARAMETER;

	pld = kzalloc(sizeof(*pld), GFP_KERNEL);
	if (!pld)
		return AE_NO_MEMORY;

	n = length >= ACPI_PLD_REV2_BUFFER_SIZE ? 5 : 4;
	for (i = 0; i < n; i++)
		dw[i] = in_buffer[i * 4] | (in_buffer[i * 4 + 1] << 8) |
			(in_buffer[i * 4 + 2] << 16) |
			((u32)in_buffer[i * 4 + 3] << 24);

	pld->revision = pld_bits(dw[0], 0, 7);
	pld->ignore_color = pld_bits(dw[0], 7, 1);
	pld->red = pld_bits(dw[0], 8, 8);
	pld->green = pld_bits(dw[0], 16, 8);
	pld->blue = pld_bits(dw[0], 24, 8);

	pld->width = pld_bits(dw[1], 0, 16);
	pld->height = pld_bits(dw[1], 16, 16);

	pld->user_visible = pld_bits(dw[2], 0, 1);
	pld->dock = pld_bits(dw[2], 1, 1);
	pld->lid = pld_bits(dw[2], 2, 1);
	pld->panel = pld_bits(dw[2], 3, 3);
	pld->vertical_position = pld_bits(dw[2], 6, 2);
	pld->horizontal_position = pld_bits(dw[2], 8, 2);
	pld->shape = pld_bits(dw[2], 10, 4);
	pld->group_orientation = pld_bits(dw[2], 14, 1);
	pld->group_token = pld_bits(dw[2], 15, 8);
	pld->group_position = pld_bits(dw[2], 23, 8);
	pld->bay = pld_bits(dw[2], 31, 1);

	pld->ejectable = pld_bits(dw[3], 0, 1);
	pld->ospm_eject_required = pld_bits(dw[3], 1, 1);
	pld->cabinet_number = pld_bits(dw[3], 2, 8);
	pld->card_cage_number = pld_bits(dw[3], 10, 8);
	pld->reference = pld_bits(dw[3], 18, 1);
	pld->rotation = pld_bits(dw[3], 19, 4);
	pld->order = pld_bits(dw[3], 23, 5);

	if (n == 5) {
		pld->vertical_offset = pld_bits(dw[4], 0, 16);
		pld->horizontal_offset = pld_bits(dw[4], 16, 16);
	}

	*return_buffer = pld;
	return AE_OK;
}

acpi_status acpi_install_method(u8 *buffer)
{
	return AE_SUPPORT;
}

acpi_status acpi_acquire_mutex(acpi_handle handle, acpi_string pathname,
			       u16 timeout)
{
	return AE_SUPPORT;
}

acpi_status acpi_release_mutex(acpi_handle handle, acpi_string pathname)
{
	return AE_SUPPORT;
}

acpi_status acpi_reset(void)
{
	return uacpi_to_acpi_status(uacpi_reboot());
}

acpi_status acpi_get_timer(u32 *ticks)
{
	uacpi_u64 val;
	uacpi_status st;

	if (!ticks)
		return AE_BAD_PARAMETER;

	st = uacpi_read_register(UACPI_REGISTER_PM_TMR, &val);
	if (st != UACPI_STATUS_OK)
		return uacpi_to_acpi_status(st);

	*ticks = val;
	return AE_OK;
}

acpi_status acpi_get_timer_resolution(u32 *resolution)
{
	if (!resolution)
		return AE_BAD_PARAMETER;

	*resolution = (acpi_gbl_FADT.flags & ACPI_FADT_32BIT_TIMER) ? 32 : 24;
	return AE_OK;
}

acpi_status acpi_get_timer_duration(u32 start_ticks, u32 end_ticks,
				    u32 *time_elapsed)
{
	u32 delta, resolution = 24;

	if (!time_elapsed)
		return AE_BAD_PARAMETER;

	if (acpi_gbl_FADT.flags & ACPI_FADT_32BIT_TIMER)
		resolution = 32;

	if (start_ticks < end_ticks)
		delta = end_ticks - start_ticks;
	else if (start_ticks > end_ticks)
		delta = (((resolution == 32) ? 0xFFFFFFFF : 0x00FFFFFF) -
			 start_ticks) + end_ticks + 1;
	else
		delta = 0;

	*time_elapsed = (u32)div_u64((u64)delta * 286331153ULL, 1000000000ULL);
	return AE_OK;
}

acpi_status acpi_initialize_debugger(void)
{
	return AE_OK;
}

void acpi_terminate_debugger(void)
{
}

void acpi_set_debugger_thread_id(acpi_thread_id thread_id)
{
}
