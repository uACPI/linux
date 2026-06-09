/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Private helpers shared between the uACPI compatibility shim translation
 * units. This is internal to drivers/acpi/uacpi/ and is not part of the
 * ACPICA interface consumed by the rest of the kernel.
 */
#ifndef _ACPI_UACPI_SHIM_H
#define _ACPI_UACPI_SHIM_H

#include <linux/acpi.h>
#include <uacpi/uacpi.h>
#include <uacpi/tables.h>
#include <uacpi/namespace.h>
#include <uacpi/utilities.h>
#include <uacpi/types.h>
#include <uacpi/status.h>

/*
 * Translate an ACPICA acpi_handle (which may be ACPI_ROOT_OBJECT or NULL) into
 * a uACPI namespace node. NULL/ACPI_ROOT_OBJECT both map to the namespace root.
 */
static inline uacpi_namespace_node *uacpi_node_from_handle(acpi_handle handle)
{
	if (!handle || handle == ACPI_ROOT_OBJECT)
		return uacpi_namespace_root();
	return handle;
}

static inline acpi_handle uacpi_handle_from_node(uacpi_namespace_node *node)
{
	if (!node || node == uacpi_namespace_root())
		return ACPI_ROOT_OBJECT;
	return node;
}

static inline acpi_status uacpi_to_acpi_status(uacpi_status status)
{
	switch (status) {
	case UACPI_STATUS_OK:
		return AE_OK;
	case UACPI_STATUS_MAPPING_FAILED:
		return AE_NO_MEMORY;
	case UACPI_STATUS_OUT_OF_MEMORY:
		return AE_NO_MEMORY;
	case UACPI_STATUS_BAD_CHECKSUM:
		return AE_BAD_CHECKSUM;
	case UACPI_STATUS_INVALID_SIGNATURE:
		return AE_BAD_SIGNATURE;
	case UACPI_STATUS_INVALID_TABLE_LENGTH:
		return AE_BAD_HEADER;
	case UACPI_STATUS_NOT_FOUND:
		return AE_NOT_FOUND;
	case UACPI_STATUS_INVALID_ARGUMENT:
		return AE_BAD_PARAMETER;
	case UACPI_STATUS_UNIMPLEMENTED:
		return AE_SUPPORT;
	case UACPI_STATUS_ALREADY_EXISTS:
		return AE_ALREADY_EXISTS;
	case UACPI_STATUS_INTERNAL_ERROR:
		return AE_ERROR;
	case UACPI_STATUS_TYPE_MISMATCH:
		return AE_TYPE;
	case UACPI_STATUS_INIT_LEVEL_MISMATCH:
		return AE_BAD_PARAMETER;
	case UACPI_STATUS_NAMESPACE_NODE_DANGLING:
		return AE_NULL_ENTRY;
	case UACPI_STATUS_NO_HANDLER:
		return AE_NOT_EXIST;
	case UACPI_STATUS_NO_RESOURCE_END_TAG:
		return AE_AML_NO_RESOURCE_END_TAG;
	case UACPI_STATUS_COMPILED_OUT:
		return AE_SUPPORT;
	case UACPI_STATUS_HARDWARE_TIMEOUT:
	case UACPI_STATUS_TIMEOUT:
		return AE_TIME;
	case UACPI_STATUS_OVERRIDDEN:
		return AE_OK;
	case UACPI_STATUS_DENIED:
		return AE_ACCESS;
	default:
		if (status >= UACPI_STATUS_AML_UNDEFINED_REFERENCE)
			return AE_AML_INTERNAL;
		return AE_ERROR;
	}
}

/*
 * Marshal a uACPI object tree into the flat, single-allocation union
 * acpi_object representation expected by acpi_evaluate_object() callers. On
 * success *out is filled in following ACPI_ALLOCATE_BUFFER / fixed-buffer
 * semantics.
 */
acpi_status uacpi_marshal_object(uacpi_object *in, struct acpi_buffer *out,
				 uacpi_namespace_node *scope);

/* Build a uACPI argument array from an ACPICA acpi_object_list. */
uacpi_status uacpi_marshal_args(struct acpi_object_list *in,
				uacpi_object_array *out);
void uacpi_free_args(uacpi_object_array *args);

#endif /* _ACPI_UACPI_SHIM_H */
