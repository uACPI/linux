// SPDX-License-Identifier: GPL-2.0
/*
 * Object marshalling and method evaluation for the uACPI shim.
 *
 * Converts between ACPICA's union acpi_object representation and uACPI's
 * uacpi_object, and implements acpi_evaluate_object() and friends.
 */

#include <linux/slab.h>
#include <linux/string.h>

#include "shim.h"


static uacpi_object *to_uacpi_object(union acpi_object *in)
{
	uacpi_data_view view;

	switch (in->type) {
	case ACPI_TYPE_INTEGER:
		return uacpi_object_create_integer(in->integer.value);
	case ACPI_TYPE_STRING:
		view.text = in->string.pointer;
		view.length = in->string.length + 1;
		return uacpi_object_create_string(view);
	case ACPI_TYPE_BUFFER:
		view.bytes = in->buffer.pointer;
		view.length = in->buffer.length;
		return uacpi_object_create_buffer(view);
	case ACPI_TYPE_PACKAGE: {
		uacpi_object_array arr;
		uacpi_object *pkg;
		u32 i, j;

		arr.count = in->package.count;
		arr.objects = UACPI_NULL;
		if (arr.count) {
			arr.objects = kcalloc(arr.count, sizeof(uacpi_object *),
					      GFP_KERNEL);
			if (!arr.objects)
				return UACPI_NULL;
			for (i = 0; i < arr.count; i++) {
				arr.objects[i] = to_uacpi_object(
					&in->package.elements[i]);
				if (!arr.objects[i]) {
					for (j = 0; j < i; j++)
						uacpi_object_unref(arr.objects[j]);
					kfree(arr.objects);
					return UACPI_NULL;
				}
			}
		}

		/* create_package takes its own reference to each element. */
		pkg = uacpi_object_create_package(arr);
		for (i = 0; i < arr.count; i++)
			uacpi_object_unref(arr.objects[i]);
		kfree(arr.objects);
		return pkg;
	}
	default:
		return UACPI_NULL;
	}
}

uacpi_status uacpi_marshal_args(struct acpi_object_list *in,
				uacpi_object_array *out)
{
	u32 i;

	out->objects = UACPI_NULL;
	out->count = 0;

	if (!in || !in->count)
		return UACPI_STATUS_OK;

	out->objects = kcalloc(in->count, sizeof(uacpi_object *), GFP_KERNEL);
	if (!out->objects)
		return UACPI_STATUS_OUT_OF_MEMORY;

	for (i = 0; i < in->count; i++) {
		out->objects[i] = to_uacpi_object(&in->pointer[i]);
		if (!out->objects[i]) {
			out->count = i;
			uacpi_free_args(out);
			return UACPI_STATUS_TYPE_MISMATCH;
		}
	}
	out->count = in->count;
	return UACPI_STATUS_OK;
}

void uacpi_free_args(uacpi_object_array *args)
{
	uacpi_size i;

	if (!args->objects)
		return;

	for (i = 0; i < args->count; i++)
		uacpi_object_unref(args->objects[i]);
	kfree(args->objects);
	args->objects = UACPI_NULL;
	args->count = 0;
}

/* --- Result marshalling: uacpi_object -> flat union acpi_object --- */

static size_t obj_extra_size(uacpi_object *obj, uacpi_namespace_node *scope)
{
	uacpi_object_array pkg;
	uacpi_data_view view;
	size_t total = 0;
	uacpi_size i;

	/*
	 * Name references inside packages (e.g. _PR0, _AL0, _PSL, _DSD) come
	 * back as AML-namepath string objects; they marshal to an inline
	 * LOCAL_REFERENCE, which needs no trailing storage.
	 */
	if (uacpi_object_is_aml_namepath(obj))
		return 0;

	switch (uacpi_object_get_type(obj)) {
	case UACPI_OBJECT_STRING:
		uacpi_object_get_string(obj, &view);
		return ALIGN(view.length ? view.length : 1, sizeof(void *));
	case UACPI_OBJECT_BUFFER:
		uacpi_object_get_buffer(obj, &view);
		return ALIGN(view.length, sizeof(void *));
	case UACPI_OBJECT_PACKAGE:
		if (uacpi_object_get_package(obj, &pkg) != UACPI_STATUS_OK)
			return 0;
		total = ALIGN(pkg.count * sizeof(union acpi_object),
			      sizeof(void *));
		for (i = 0; i < pkg.count; i++)
			total += obj_extra_size(pkg.objects[i], scope);
		return total;
	default:
		return 0;
	}
}

/*
 * Resolve an AML-namepath object to a LOCAL_REFERENCE pointing at the named
 * node, matching what ACPICA returns for package name references. Returns true
 * if 'src' was a namepath (and was written as a reference into 'dst').
 */
static bool write_namepath_ref(union acpi_object *dst, uacpi_object *src,
			       uacpi_namespace_node *scope)
{
	uacpi_namespace_node *target = UACPI_NULL;
	uacpi_object_type type = ACPI_TYPE_ANY;

	if (!uacpi_object_is_aml_namepath(src))
		return false;

	dst->type = ACPI_TYPE_LOCAL_REFERENCE;
	if (uacpi_object_resolve_as_aml_namepath(src, scope, &target) ==
	    UACPI_STATUS_OK) {
		uacpi_namespace_node_type(target, &type);
		dst->reference.handle = uacpi_handle_from_node(target);
	} else {
		dst->reference.handle = NULL;
	}
	dst->reference.actual_type = type;
	return true;
}

static void write_obj(union acpi_object *dst, uacpi_object *src, u8 **cursor,
		      uacpi_namespace_node *scope)
{
	uacpi_object_array pkg;
	uacpi_data_view view;
	u64 ival;
	uacpi_size i;

	if (write_namepath_ref(dst, src, scope))
		return;

	switch (uacpi_object_get_type(src)) {
	case UACPI_OBJECT_INTEGER:
		dst->type = ACPI_TYPE_INTEGER;
		uacpi_object_get_integer(src, &ival);
		dst->integer.value = ival;
		break;
	case UACPI_OBJECT_STRING:
		dst->type = ACPI_TYPE_STRING;
		uacpi_object_get_string(src, &view);
		/* uACPI string length includes the trailing NUL. */
		dst->string.length = view.length ? view.length - 1 : 0;
		dst->string.pointer = (char *)*cursor;
		if (dst->string.length)
			memcpy(dst->string.pointer, view.text,
			       dst->string.length);
		dst->string.pointer[dst->string.length] = '\0';
		*cursor += ALIGN(view.length ? view.length : 1, sizeof(void *));
		break;
	case UACPI_OBJECT_BUFFER:
		dst->type = ACPI_TYPE_BUFFER;
		uacpi_object_get_buffer(src, &view);
		dst->buffer.length = view.length;
		dst->buffer.pointer = *cursor;
		memcpy(dst->buffer.pointer, view.bytes, view.length);
		*cursor += ALIGN(view.length, sizeof(void *));
		break;
	case UACPI_OBJECT_PACKAGE:
		dst->type = ACPI_TYPE_PACKAGE;
		uacpi_object_get_package(src, &pkg);
		dst->package.count = pkg.count;
		dst->package.elements = (union acpi_object *)*cursor;
		*cursor += ALIGN(pkg.count * sizeof(union acpi_object),
				 sizeof(void *));
		for (i = 0; i < pkg.count; i++)
			write_obj(&dst->package.elements[i], pkg.objects[i],
				  cursor, scope);
		break;
	case UACPI_OBJECT_PROCESSOR: {
		uacpi_processor_info info;

		dst->type = ACPI_TYPE_PROCESSOR;
		if (uacpi_object_get_processor_info(src, &info) ==
		    UACPI_STATUS_OK) {
			dst->processor.proc_id = info.id;
			dst->processor.pblk_address = info.block_address;
			dst->processor.pblk_length = info.block_length;
		}
		break;
	}
	case UACPI_OBJECT_POWER_RESOURCE: {
		uacpi_power_resource_info info;

		dst->type = ACPI_TYPE_POWER;
		if (uacpi_object_get_power_resource_info(src, &info) ==
		    UACPI_STATUS_OK) {
			dst->power_resource.system_level = info.system_level;
			dst->power_resource.resource_order = info.resource_order;
		}
		break;
	}
	case UACPI_OBJECT_REFERENCE:
		dst->type = ACPI_TYPE_LOCAL_REFERENCE;
		dst->reference.actual_type = ACPI_TYPE_ANY;
		dst->reference.handle = NULL;
		break;
	default:
		dst->type = ACPI_TYPE_ANY;
		break;
	}
}

acpi_status uacpi_marshal_object(uacpi_object *in, struct acpi_buffer *out,
				 uacpi_namespace_node *scope)
{
	size_t needed;
	u8 *cursor;

	if (out->length == ACPI_NO_BUFFER)
		return AE_OK;

	needed = sizeof(union acpi_object) + obj_extra_size(in, scope);

	if (out->length == ACPI_ALLOCATE_BUFFER) {
		out->pointer = kzalloc(needed, GFP_KERNEL);
		if (!out->pointer)
			return AE_NO_MEMORY;
		out->length = needed;
	} else {
		if (out->length < needed) {
			out->length = needed;
			return AE_BUFFER_OVERFLOW;
		}
		memset(out->pointer, 0, needed);
		out->length = needed;
	}

	cursor = (u8 *)out->pointer + sizeof(union acpi_object);
	write_obj((union acpi_object *)out->pointer, in, &cursor, scope);
	return AE_OK;
}

acpi_status acpi_evaluate_object(acpi_handle handle, acpi_string pathname,
				 struct acpi_object_list *external_params,
				 struct acpi_buffer *return_buffer)
{
	uacpi_namespace_node *node = uacpi_node_from_handle(handle);
	uacpi_object_array args;
	uacpi_object *ret = UACPI_NULL;
	uacpi_status ust;
	acpi_status status;

	ust = uacpi_marshal_args(external_params, &args);
	if (ust != UACPI_STATUS_OK)
		return uacpi_to_acpi_status(ust);

	ust = uacpi_eval(node, pathname, &args,
			 return_buffer ? &ret : UACPI_NULL);
	uacpi_free_args(&args);

	if (ust != UACPI_STATUS_OK)
		return uacpi_to_acpi_status(ust);

	if (return_buffer && ret) {
		status = uacpi_marshal_object(ret, return_buffer, node);
		uacpi_object_unref(ret);
		return status;
	}

	return AE_OK;
}

acpi_status acpi_evaluate_object_typed(acpi_handle handle,
				       acpi_string pathname,
				       struct acpi_object_list *params,
				       struct acpi_buffer *return_buffer,
				       acpi_object_type return_type)
{
	acpi_status status;
	bool must_free = false;

	if (!return_buffer)
		return AE_BAD_PARAMETER;

	if (return_buffer->length == ACPI_ALLOCATE_BUFFER)
		must_free = true;

	status = acpi_evaluate_object(handle, pathname, params, return_buffer);
	if (ACPI_FAILURE(status))
		return status;

	if (((union acpi_object *)return_buffer->pointer)->type != return_type) {
		if (must_free) {
			kfree(return_buffer->pointer);
			return_buffer->pointer = NULL;
			return_buffer->length = 0;
		}
		return AE_TYPE;
	}

	return AE_OK;
}
