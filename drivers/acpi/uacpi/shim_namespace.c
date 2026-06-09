// SPDX-License-Identifier: GPL-2.0
/*
 * Namespace traversal and object information for the uACPI shim.
 */

#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/spinlock.h>
#include <linux/hashtable.h>

#include "shim.h"

acpi_status acpi_get_handle(acpi_handle parent, const char *pathname,
			    acpi_handle *ret_handle)
{
	uacpi_namespace_node *node = uacpi_node_from_handle(parent);
	uacpi_namespace_node *out;
	uacpi_status st;

	if (!ret_handle)
		return AE_BAD_PARAMETER;

	if (!pathname) {
		*ret_handle = uacpi_handle_from_node(node);
		return AE_OK;
	}

	st = uacpi_namespace_node_find(node, pathname, &out);
	if (st != UACPI_STATUS_OK)
		return uacpi_to_acpi_status(st);

	*ret_handle = uacpi_handle_from_node(out);
	return AE_OK;
}

acpi_status acpi_get_name(acpi_handle handle, u32 name_type,
			  struct acpi_buffer *buffer)
{
	uacpi_namespace_node *node = uacpi_node_from_handle(handle);
	size_t len;
	char *src;
	bool free_src = false;
	char namebuf[ACPI_NAMESEG_SIZE + 1];

	if (!buffer)
		return AE_BAD_PARAMETER;

	if (name_type == ACPI_SINGLE_NAME) {
		uacpi_object_name name = uacpi_namespace_node_name(node);

		memcpy(namebuf, name.text, ACPI_NAMESEG_SIZE);
		namebuf[ACPI_NAMESEG_SIZE] = '\0';
		src = namebuf;
	} else {
		src = (char *)uacpi_namespace_node_generate_absolute_path(node);
		if (!src)
			return AE_NO_MEMORY;
		free_src = true;
	}

	len = strlen(src) + 1;

	if (buffer->length == ACPI_ALLOCATE_BUFFER) {
		buffer->pointer = kmalloc(len, GFP_KERNEL);
		if (!buffer->pointer) {
			if (free_src)
				uacpi_free_absolute_path(src);
			return AE_NO_MEMORY;
		}
		buffer->length = len;
	} else if (buffer->length < len) {
		buffer->length = len;
		if (free_src)
			uacpi_free_absolute_path(src);
		return AE_BUFFER_OVERFLOW;
	}

	memcpy(buffer->pointer, src, len);
	buffer->length = len;
	if (free_src)
		uacpi_free_absolute_path(src);
	return AE_OK;
}

acpi_status acpi_get_parent(acpi_handle handle, acpi_handle *ret_handle)
{
	uacpi_namespace_node *node = uacpi_node_from_handle(handle);
	uacpi_namespace_node *parent;

	if (!ret_handle)
		return AE_BAD_PARAMETER;

	parent = uacpi_namespace_node_parent(node);
	if (!parent)
		return AE_NULL_ENTRY;

	*ret_handle = uacpi_handle_from_node(parent);
	return AE_OK;
}

acpi_status acpi_get_type(acpi_handle handle, acpi_object_type *ret_type)
{
	uacpi_namespace_node *node = uacpi_node_from_handle(handle);
	uacpi_object_type type;
	uacpi_status st;

	if (!ret_type)
		return AE_BAD_PARAMETER;

	st = uacpi_namespace_node_type(node, &type);
	if (st != UACPI_STATUS_OK)
		return uacpi_to_acpi_status(st);

	*ret_type = type;
	return AE_OK;
}

acpi_status acpi_get_next_object(acpi_object_type type, acpi_handle parent,
				 acpi_handle child, acpi_handle *ret_handle)
{
	uacpi_namespace_node *pnode = uacpi_node_from_handle(parent);
	uacpi_namespace_node *iter = child ? uacpi_node_from_handle(child) :
					     UACPI_NULL;
	uacpi_status st;

	if (type == ACPI_TYPE_ANY)
		st = uacpi_namespace_node_next(pnode, &iter);
	else
		st = uacpi_namespace_node_next_typed(pnode, &iter,
						     1u << type);

	if (st != UACPI_STATUS_OK)
		return uacpi_to_acpi_status(st);

	if (ret_handle)
		*ret_handle = uacpi_handle_from_node(iter);
	return AE_OK;
}


struct shim_walk_ctx {
	acpi_walk_callback descending;
	acpi_walk_callback ascending;
	void *context;
	void **return_value;
	acpi_status status;
};

static uacpi_iteration_decision shim_decision(acpi_status st)
{
	switch (st) {
	case AE_OK:
		return UACPI_ITERATION_DECISION_CONTINUE;
	case AE_CTRL_DEPTH:
		return UACPI_ITERATION_DECISION_NEXT_PEER;
	default:
		return UACPI_ITERATION_DECISION_BREAK;
	}
}

static uacpi_iteration_decision shim_walk_descend(void *user,
						  uacpi_namespace_node *node,
						  uacpi_u32 depth)
{
	struct shim_walk_ctx *c = user;
	acpi_status st;

	if (!c->descending)
		return UACPI_ITERATION_DECISION_CONTINUE;

	st = c->descending(uacpi_handle_from_node(node), depth, c->context,
			   c->return_value);
	if (st != AE_OK && st != AE_CTRL_DEPTH)
		c->status = (st == AE_CTRL_TERMINATE) ? AE_OK : st;
	return shim_decision(st);
}

static uacpi_iteration_decision shim_walk_ascend(void *user,
						 uacpi_namespace_node *node,
						 uacpi_u32 depth)
{
	struct shim_walk_ctx *c = user;
	acpi_status st;

	if (!c->ascending)
		return UACPI_ITERATION_DECISION_CONTINUE;

	st = c->ascending(uacpi_handle_from_node(node), depth, c->context,
			  c->return_value);
	if (st != AE_OK && st != AE_CTRL_DEPTH)
		c->status = (st == AE_CTRL_TERMINATE) ? AE_OK : st;
	return shim_decision(st);
}

acpi_status acpi_walk_namespace(acpi_object_type type, acpi_handle start_object,
				u32 max_depth,
				acpi_walk_callback descending_callback,
				acpi_walk_callback ascending_callback,
				void *context, void **return_value)
{
	uacpi_namespace_node *start = uacpi_node_from_handle(start_object);
	uacpi_object_type_bits mask;
	struct shim_walk_ctx ctx = {
		.descending = descending_callback,
		.ascending = ascending_callback,
		.context = context,
		.return_value = return_value,
		.status = AE_OK,
	};
	uacpi_status st;

	if (!descending_callback && !ascending_callback)
		return AE_BAD_PARAMETER;

	mask = (type == ACPI_TYPE_ANY) ? UACPI_OBJECT_ANY_BIT : (1u << type);

	st = uacpi_namespace_for_each_child(start, shim_walk_descend,
					    shim_walk_ascend, mask,
					    max_depth ? max_depth :
							UACPI_MAX_DEPTH_ANY,
					    &ctx);
	if (st != UACPI_STATUS_OK)
		return uacpi_to_acpi_status(st);

	return ctx.status;
}

struct shim_getdev_ctx {
	acpi_walk_callback cb;
	void *context;
	void **return_value;
	acpi_status status;
};

static uacpi_iteration_decision shim_getdev_cb(void *user,
					       uacpi_namespace_node *node,
					       uacpi_u32 depth)
{
	struct shim_getdev_ctx *c = user;
	acpi_status st;

	st = c->cb(uacpi_handle_from_node(node), depth, c->context,
		   c->return_value);
	if (st != AE_OK && st != AE_CTRL_DEPTH)
		c->status = (st == AE_CTRL_TERMINATE) ? AE_OK : st;
	return shim_decision(st);
}

acpi_status acpi_get_devices(const char *HID, acpi_walk_callback user_function,
			     void *context, void **return_value)
{
	struct shim_getdev_ctx ctx = {
		.cb = user_function,
		.context = context,
		.return_value = return_value,
		.status = AE_OK,
	};
	uacpi_status st;

	if (HID)
		st = uacpi_find_devices(HID, shim_getdev_cb, &ctx);
	else
		st = uacpi_namespace_for_each_child(uacpi_namespace_root(),
						    shim_getdev_cb, UACPI_NULL,
						    UACPI_OBJECT_DEVICE_BIT,
						    UACPI_MAX_DEPTH_ANY, &ctx);
	if (st != UACPI_STATUS_OK)
		return uacpi_to_acpi_status(st);

	return ctx.status;
}

struct shim_node_data {
	struct hlist_node hnode;
	uacpi_namespace_node *node;
	acpi_object_handler handler;
	void *data;
};

static DEFINE_HASHTABLE(shim_data_ht, 8);
static DEFINE_SPINLOCK(shim_data_lock);

acpi_status acpi_attach_data(acpi_handle handle, acpi_object_handler handler,
			     void *data)
{
	uacpi_namespace_node *node = uacpi_node_from_handle(handle);
	struct shim_node_data *e;

	if (!handler)
		return AE_BAD_PARAMETER;

	e = kmalloc(sizeof(*e), GFP_KERNEL);
	if (!e)
		return AE_NO_MEMORY;

	e->node = node;
	e->handler = handler;
	e->data = data;

	spin_lock(&shim_data_lock);
	hash_add(shim_data_ht, &e->hnode, (unsigned long)node);
	spin_unlock(&shim_data_lock);
	return AE_OK;
}

acpi_status acpi_detach_data(acpi_handle handle, acpi_object_handler handler)
{
	uacpi_namespace_node *node = uacpi_node_from_handle(handle);
	struct shim_node_data *e;

	spin_lock(&shim_data_lock);
	hash_for_each_possible(shim_data_ht, e, hnode, (unsigned long)node) {
		if (e->node == node && e->handler == handler) {
			hash_del(&e->hnode);
			spin_unlock(&shim_data_lock);
			kfree(e);
			return AE_OK;
		}
	}
	spin_unlock(&shim_data_lock);
	return AE_NOT_FOUND;
}

acpi_status acpi_get_data_full(acpi_handle handle, acpi_object_handler handler,
			       void **data, void (*callback)(void *))
{
	uacpi_namespace_node *node = uacpi_node_from_handle(handle);
	struct shim_node_data *e;

	if (!data)
		return AE_BAD_PARAMETER;

	spin_lock(&shim_data_lock);
	hash_for_each_possible(shim_data_ht, e, hnode, (unsigned long)node) {
		if (e->node == node && e->handler == handler) {
			*data = e->data;
			spin_unlock(&shim_data_lock);
			if (callback)
				callback(e->data);
			return AE_OK;
		}
	}
	spin_unlock(&shim_data_lock);
	return AE_NOT_FOUND;
}

acpi_status acpi_get_data(acpi_handle handle, acpi_object_handler handler,
			  void **data)
{
	return acpi_get_data_full(handle, handler, data, NULL);
}

static void shim_copy_id(struct acpi_pnp_device_id *dst, uacpi_id_string *src,
			 char **strpool)
{
	dst->length = src->size;
	dst->string = *strpool;
	memcpy(dst->string, src->value, src->size);
	*strpool += src->size;
}

acpi_status acpi_get_object_info(acpi_handle handle,
				 struct acpi_device_info **return_buffer)
{
	uacpi_namespace_node *node = uacpi_node_from_handle(handle);
	uacpi_namespace_node_info *uinfo;
	struct acpi_device_info *info;
	uacpi_object_name name;
	size_t size, str_size = 0, cid_size = 0, cid_str = 0;
	char *strpool;
	uacpi_status st;
	u32 i;

	if (!return_buffer)
		return AE_BAD_PARAMETER;

	st = uacpi_get_namespace_node_info(node, &uinfo);
	if (st != UACPI_STATUS_OK)
		return uacpi_to_acpi_status(st);

	if (uinfo->flags & UACPI_NS_NODE_INFO_HAS_HID)
		str_size += uinfo->hid.size;
	if (uinfo->flags & UACPI_NS_NODE_INFO_HAS_UID)
		str_size += uinfo->uid.size;
	if (uinfo->flags & UACPI_NS_NODE_INFO_HAS_CLS)
		str_size += uinfo->cls.size;
	if (uinfo->flags & UACPI_NS_NODE_INFO_HAS_CID) {
		cid_size = uinfo->cid.num_ids *
			   sizeof(struct acpi_pnp_device_id);
		for (i = 0; i < uinfo->cid.num_ids; i++)
			cid_str += uinfo->cid.ids[i].size;
		str_size += cid_str;
	}

	size = sizeof(*info) + cid_size + str_size;
	info = kzalloc(size, GFP_KERNEL);
	if (!info) {
		uacpi_free_namespace_node_info(uinfo);
		return AE_NO_MEMORY;
	}

	info->info_size = size;
	name = uacpi_namespace_node_name(node);
	info->name = name.id;
	info->type = uinfo->type;
	info->param_count = uinfo->num_params;

	/* String pool lives after the (possibly empty) trailing CID array. */
	strpool = (char *)info->compatible_id_list.ids + cid_size;

	if (uinfo->flags & UACPI_NS_NODE_INFO_HAS_ADR) {
		info->address = uinfo->adr;
		info->valid |= ACPI_VALID_ADR;
	}
	if (uinfo->flags & UACPI_NS_NODE_INFO_HAS_HID) {
		shim_copy_id(&info->hardware_id, &uinfo->hid, &strpool);
		info->valid |= ACPI_VALID_HID;
	}
	if (uinfo->flags & UACPI_NS_NODE_INFO_HAS_UID) {
		shim_copy_id(&info->unique_id, &uinfo->uid, &strpool);
		info->valid |= ACPI_VALID_UID;
	}
	if (uinfo->flags & UACPI_NS_NODE_INFO_HAS_CLS) {
		shim_copy_id(&info->class_code, &uinfo->cls, &strpool);
		info->valid |= ACPI_VALID_CLS;
	}
	if (uinfo->flags & UACPI_NS_NODE_INFO_HAS_CID) {
		info->compatible_id_list.count = uinfo->cid.num_ids;
		info->compatible_id_list.list_size = cid_size + cid_str;
		for (i = 0; i < uinfo->cid.num_ids; i++)
			shim_copy_id(&info->compatible_id_list.ids[i],
				     &uinfo->cid.ids[i], &strpool);
		info->valid |= ACPI_VALID_CID;
	}
	if (uinfo->flags & UACPI_NS_NODE_INFO_HAS_SXD) {
		memcpy(info->highest_dstates, uinfo->sxd,
		       sizeof(info->highest_dstates));
		info->valid |= ACPI_VALID_SXDS;
	}
	if (uinfo->flags & UACPI_NS_NODE_INFO_HAS_SXW) {
		memcpy(info->lowest_dstates, uinfo->sxw,
		       sizeof(info->lowest_dstates));
		info->valid |= ACPI_VALID_SXWS;
	}

	uacpi_free_namespace_node_info(uinfo);
	*return_buffer = info;
	return AE_OK;
}
