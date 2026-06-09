// SPDX-License-Identifier: GPL-2.0
/*
 * Events, GPEs, fixed events, notify/address-space handlers, sleep states,
 * register access and the global lock for the uACPI shim.
 */

#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/list.h>
#include <linux/spinlock.h>

#include "shim.h"

#include <uacpi/registers.h>
#include <uacpi/sleep.h>
#include <uacpi/notify.h>
#include <uacpi/opregion.h>
#include <uacpi/event.h>
#include <uacpi/io.h>

static uacpi_fixed_event to_uacpi_fixed_event(u32 event)
{
	switch (event) {
	case ACPI_EVENT_PMTIMER:
		return UACPI_FIXED_EVENT_TIMER_STATUS;
	case ACPI_EVENT_POWER_BUTTON:
		return UACPI_FIXED_EVENT_POWER_BUTTON;
	case ACPI_EVENT_SLEEP_BUTTON:
		return UACPI_FIXED_EVENT_SLEEP_BUTTON;
	case ACPI_EVENT_RTC:
		return UACPI_FIXED_EVENT_RTC;
	default:
		return 0;
	}
}

static acpi_event_status to_acpi_event_status(uacpi_event_info info)
{
	acpi_event_status st = 0;

	if (info & UACPI_EVENT_INFO_ENABLED)
		st |= ACPI_EVENT_FLAG_ENABLED;
	if (info & UACPI_EVENT_INFO_ENABLED_FOR_WAKE)
		st |= ACPI_EVENT_FLAG_WAKE_ENABLED;
	if (info & UACPI_EVENT_INFO_MASKED)
		st |= ACPI_EVENT_FLAG_MASKED;
	if (info & UACPI_EVENT_INFO_HAS_HANDLER)
		st |= ACPI_EVENT_FLAG_HAS_HANDLER;
	if (info & UACPI_EVENT_INFO_HW_ENABLED)
		st |= ACPI_EVENT_FLAG_ENABLE_SET;
	if (info & UACPI_EVENT_INFO_HW_STATUS)
		st |= ACPI_EVENT_FLAG_STATUS_SET;
	return st;
}

acpi_status acpi_enable_event(u32 event, u32 flags)
{
	uacpi_fixed_event fe = to_uacpi_fixed_event(event);

	if (!fe)
		return AE_BAD_PARAMETER;
	return uacpi_to_acpi_status(uacpi_enable_fixed_event(fe));
}

acpi_status acpi_disable_event(u32 event, u32 flags)
{
	uacpi_fixed_event fe = to_uacpi_fixed_event(event);

	if (!fe)
		return AE_BAD_PARAMETER;
	return uacpi_to_acpi_status(uacpi_disable_fixed_event(fe));
}

acpi_status acpi_clear_event(u32 event)
{
	uacpi_fixed_event fe = to_uacpi_fixed_event(event);

	if (!fe)
		return AE_BAD_PARAMETER;
	return uacpi_to_acpi_status(uacpi_clear_fixed_event(fe));
}

acpi_status acpi_get_event_status(u32 event, acpi_event_status *event_status)
{
	uacpi_fixed_event fe = to_uacpi_fixed_event(event);
	uacpi_event_info info;
	uacpi_status st;

	if (!fe || !event_status)
		return AE_BAD_PARAMETER;

	st = uacpi_fixed_event_info(fe, &info);
	if (st != UACPI_STATUS_OK)
		return uacpi_to_acpi_status(st);

	*event_status = to_acpi_event_status(info);
	return AE_OK;
}

struct shim_fixed_handler {
	acpi_event_handler handler;
	void *context;
};

static struct shim_fixed_handler shim_fixed[ACPI_NUM_FIXED_EVENTS];

static uacpi_interrupt_ret shim_fixed_trampoline(uacpi_handle ctx)
{
	struct shim_fixed_handler *h = ctx;

	return h->handler(h->context);
}

acpi_status acpi_install_fixed_event_handler(u32 event,
					     acpi_event_handler handler,
					     void *context)
{
	uacpi_fixed_event fe = to_uacpi_fixed_event(event);
	uacpi_status st;

	if (!fe || event >= ACPI_NUM_FIXED_EVENTS)
		return AE_BAD_PARAMETER;

	shim_fixed[event].handler = handler;
	shim_fixed[event].context = context;

	st = uacpi_install_fixed_event_handler(fe, shim_fixed_trampoline,
					       &shim_fixed[event]);
	return uacpi_to_acpi_status(st);
}

acpi_status acpi_remove_fixed_event_handler(u32 event,
					    acpi_event_handler handler)
{
	uacpi_fixed_event fe = to_uacpi_fixed_event(event);

	if (!fe)
		return AE_BAD_PARAMETER;

	return uacpi_to_acpi_status(uacpi_uninstall_fixed_event_handler(fe));
}

/* --- GPEs --- */

static inline uacpi_namespace_node *gpe_node(acpi_handle dev)
{
	return uacpi_node_from_handle(dev);
}

acpi_status acpi_enable_gpe(acpi_handle gpe_device, u32 gpe_number)
{
	return uacpi_to_acpi_status(uacpi_enable_gpe(gpe_node(gpe_device),
						     gpe_number));
}

acpi_status acpi_enable_gpe_cond(acpi_handle gpe_device, u32 gpe_number,
				 u8 dispatch_type)
{
	return uacpi_to_acpi_status(uacpi_enable_gpe(gpe_node(gpe_device),
						     gpe_number));
}

acpi_status acpi_disable_gpe(acpi_handle gpe_device, u32 gpe_number)
{
	return uacpi_to_acpi_status(uacpi_disable_gpe(gpe_node(gpe_device),
						      gpe_number));
}

acpi_status acpi_clear_gpe(acpi_handle gpe_device, u32 gpe_number)
{
	return uacpi_to_acpi_status(uacpi_clear_gpe(gpe_node(gpe_device),
						    gpe_number));
}

acpi_status acpi_set_gpe(acpi_handle gpe_device, u32 gpe_number, u8 action)
{
	uacpi_namespace_node *node = gpe_node(gpe_device);

	if (action == ACPI_GPE_ENABLE)
		return uacpi_to_acpi_status(uacpi_enable_gpe(node, gpe_number));
	return uacpi_to_acpi_status(uacpi_disable_gpe(node, gpe_number));
}

acpi_status acpi_finish_gpe(acpi_handle gpe_device, u32 gpe_number)
{
	return uacpi_to_acpi_status(
		uacpi_finish_handling_gpe(gpe_node(gpe_device), gpe_number));
}

acpi_status acpi_mask_gpe(acpi_handle gpe_device, u32 gpe_number, u8 is_masked)
{
	uacpi_namespace_node *node = gpe_node(gpe_device);

	if (is_masked)
		return uacpi_to_acpi_status(uacpi_mask_gpe(node, gpe_number));
	return uacpi_to_acpi_status(uacpi_unmask_gpe(node, gpe_number));
}

acpi_status acpi_mark_gpe_for_wake(acpi_handle gpe_device, u32 gpe_number)
{
	return uacpi_to_acpi_status(
		uacpi_setup_gpe_for_wake(gpe_node(gpe_device), gpe_number,
					 UACPI_NULL));
}

acpi_status acpi_setup_gpe_for_wake(acpi_handle parent_device,
				    acpi_handle gpe_device, u32 gpe_number)
{
	return uacpi_to_acpi_status(
		uacpi_setup_gpe_for_wake(gpe_node(gpe_device), gpe_number,
					 uacpi_node_from_handle(parent_device)));
}

acpi_status acpi_set_gpe_wake_mask(acpi_handle gpe_device, u32 gpe_number,
				  u8 action)
{
	uacpi_namespace_node *node = gpe_node(gpe_device);

	if (action == ACPI_GPE_ENABLE)
		return uacpi_to_acpi_status(
			uacpi_enable_gpe_for_wake(node, gpe_number));
	return uacpi_to_acpi_status(
		uacpi_disable_gpe_for_wake(node, gpe_number));
}

acpi_status acpi_get_gpe_status(acpi_handle gpe_device, u32 gpe_number,
				acpi_event_status *event_status)
{
	uacpi_event_info info;
	uacpi_status st;

	if (!event_status)
		return AE_BAD_PARAMETER;

	st = uacpi_gpe_info(gpe_node(gpe_device), gpe_number, &info);
	if (st != UACPI_STATUS_OK)
		return uacpi_to_acpi_status(st);

	*event_status = to_acpi_event_status(info);
	return AE_OK;
}

acpi_status acpi_update_all_gpes(void)
{
	return uacpi_to_acpi_status(uacpi_finalize_gpe_initialization());
}

acpi_status acpi_enable_all_runtime_gpes(void)
{
	return uacpi_to_acpi_status(uacpi_enable_all_runtime_gpes());
}

acpi_status acpi_enable_all_wakeup_gpes(void)
{
	return uacpi_to_acpi_status(uacpi_enable_all_wake_gpes());
}

acpi_status acpi_disable_all_gpes(void)
{
	return uacpi_to_acpi_status(uacpi_disable_all_gpes());
}

acpi_status acpi_hw_disable_all_gpes(void)
{
	return uacpi_to_acpi_status(uacpi_disable_all_gpes());
}

acpi_status acpi_hw_enable_all_wakeup_gpes(void)
{
	return uacpi_to_acpi_status(uacpi_enable_all_wake_gpes());
}

/*
 * True if a GPE has both its status and enable bits set (i.e. a pending,
 * enabled GPE). Used by the EC driver and suspend-to-idle wake detection.
 * uACPI has no aggregate GPE query, so iterate the FADT \_GPE blocks via
 * uacpi_gpe_info() (gpe_device == NULL). Invalid indices return NOT_FOUND
 * silently. GPE block devices outside the FADT blocks are not covered.
 */
u32 acpi_any_gpe_status_set(u32 gpe_skip_number)
{
	u32 gpe0 = (acpi_gbl_FADT.gpe0_block_length / 2) * 8;
	u32 gpe1 = (acpi_gbl_FADT.gpe1_block_length / 2) * 8;
	u32 max = gpe0, idx;

	if (gpe1 && acpi_gbl_FADT.gpe1_base + gpe1 > max)
		max = acpi_gbl_FADT.gpe1_base + gpe1;
	if (max > 256)
		max = 256;

	for (idx = 0; idx < max; idx++) {
		uacpi_event_info info;

		if (idx == gpe_skip_number)
			continue;
		if (uacpi_gpe_info(UACPI_NULL, idx, &info) != UACPI_STATUS_OK)
			continue;
		if ((info & UACPI_EVENT_INFO_HW_STATUS) &&
		    (info & UACPI_EVENT_INFO_HW_ENABLED))
			return 1;
	}
	return 0;
}

u32 acpi_any_fixed_event_status_set(void)
{
	uacpi_fixed_event fe;

	for (fe = UACPI_FIXED_EVENT_TIMER_STATUS; fe <= UACPI_FIXED_EVENT_RTC;
	     fe++) {
		uacpi_event_info info;

		if (uacpi_fixed_event_info(fe, &info) != UACPI_STATUS_OK)
			continue;
		if ((info & UACPI_EVENT_INFO_HW_STATUS) &&
		    (info & UACPI_EVENT_INFO_HW_ENABLED))
			return 1;
	}
	return 0;
}

u32 acpi_dispatch_gpe(acpi_handle gpe_device, u32 gpe_number)
{
	return 0;
}

acpi_status acpi_get_gpe_device(u32 gpe_index, acpi_handle *gpe_device)
{
	return AE_NOT_FOUND;
}

acpi_status acpi_install_gpe_block(acpi_handle gpe_device,
				  struct acpi_generic_address *gpe_block_address,
				  u32 register_count, u32 interrupt_number)
{
	return AE_SUPPORT;
}

acpi_status acpi_remove_gpe_block(acpi_handle gpe_device)
{
	return AE_SUPPORT;
}

struct shim_gpe_handler {
	struct list_head list;
	uacpi_namespace_node *node;
	u32 number;
	acpi_gpe_handler handler;
	void *context;
};

static LIST_HEAD(shim_gpe_handlers);
static DEFINE_SPINLOCK(shim_gpe_lock);

static uacpi_interrupt_ret shim_gpe_trampoline(uacpi_handle ctx,
					       uacpi_namespace_node *node,
					       uacpi_u16 idx)
{
	struct shim_gpe_handler *h = ctx;

	return h->handler(uacpi_handle_from_node(node), idx, h->context);
}

static acpi_status shim_install_gpe(acpi_handle gpe_device, u32 gpe_number,
				    u32 type, acpi_gpe_handler address,
				    void *context, bool raw)
{
	uacpi_namespace_node *node = gpe_node(gpe_device);
	uacpi_gpe_triggering trig;
	struct shim_gpe_handler *h;
	uacpi_status st;

	if (!address)
		return AE_BAD_PARAMETER;

	h = kmalloc(sizeof(*h), GFP_KERNEL);
	if (!h)
		return AE_NO_MEMORY;

	h->node = node;
	h->number = gpe_number;
	h->handler = address;
	h->context = context;

	trig = (type & ACPI_GPE_XRUPT_TYPE_MASK) ?
		UACPI_GPE_TRIGGERING_LEVEL : UACPI_GPE_TRIGGERING_EDGE;

	if (raw)
		st = uacpi_install_gpe_handler_raw(node, gpe_number, trig,
						   shim_gpe_trampoline, h);
	else
		st = uacpi_install_gpe_handler(node, gpe_number, trig,
					       shim_gpe_trampoline, h);
	if (st != UACPI_STATUS_OK) {
		kfree(h);
		return uacpi_to_acpi_status(st);
	}

	spin_lock(&shim_gpe_lock);
	list_add(&h->list, &shim_gpe_handlers);
	spin_unlock(&shim_gpe_lock);
	return AE_OK;
}

acpi_status acpi_install_gpe_handler(acpi_handle gpe_device, u32 gpe_number,
				    u32 type, acpi_gpe_handler address,
				    void *context)
{
	return shim_install_gpe(gpe_device, gpe_number, type, address, context,
				false);
}

acpi_status acpi_install_gpe_raw_handler(acpi_handle gpe_device, u32 gpe_number,
					 u32 type, acpi_gpe_handler address,
					 void *context)
{
	return shim_install_gpe(gpe_device, gpe_number, type, address, context,
				true);
}

acpi_status acpi_remove_gpe_handler(acpi_handle gpe_device, u32 gpe_number,
				    acpi_gpe_handler address)
{
	uacpi_namespace_node *node = gpe_node(gpe_device);
	struct shim_gpe_handler *h, *found = NULL;
	uacpi_status st;

	spin_lock(&shim_gpe_lock);
	list_for_each_entry(h, &shim_gpe_handlers, list) {
		if (h->node == node && h->number == gpe_number &&
		    h->handler == address) {
			found = h;
			list_del(&h->list);
			break;
		}
	}
	spin_unlock(&shim_gpe_lock);

	if (!found)
		return AE_NOT_FOUND;

	st = uacpi_uninstall_gpe_handler(node, gpe_number, shim_gpe_trampoline);
	kfree(found);
	return uacpi_to_acpi_status(st);
}

/* --- Notify handlers --- */

struct shim_notify_handler {
	struct list_head list;
	uacpi_namespace_node *node;
	u32 type;
	acpi_notify_handler handler;
	void *context;
	bool uacpi_installed;
};

static LIST_HEAD(shim_notify_handlers);
static DEFINE_SPINLOCK(shim_notify_lock);

static uacpi_status shim_notify_dispatch(uacpi_handle ctx,
					 uacpi_namespace_node *node,
					 uacpi_u64 value)
{
	struct shim_notify_handler *h;
	acpi_notify_handler fn;
	void *fnctx;

	/*
	 * Fan out to every registered ACPICA handler for this node. Copy the
	 * function pointer out under the lock so the callback runs unlocked.
	 */
again:
	spin_lock(&shim_notify_lock);
	list_for_each_entry(h, &shim_notify_handlers, list) {
		if (h->node != node || !h->handler)
			continue;
		fn = h->handler;
		fnctx = h->context;
		h->handler = NULL;	/* mark visited for this pass */
		spin_unlock(&shim_notify_lock);
		fn(uacpi_handle_from_node(node), value, fnctx);
		spin_lock(&shim_notify_lock);
		h->handler = fn;
		spin_unlock(&shim_notify_lock);
		goto again;
	}
	spin_unlock(&shim_notify_lock);
	return UACPI_STATUS_OK;
}

acpi_status acpi_install_notify_handler(acpi_handle device, u32 handler_type,
					acpi_notify_handler handler,
					void *context)
{
	uacpi_namespace_node *node = uacpi_node_from_handle(device);
	struct shim_notify_handler *h, *existing;
	bool need_install = true;
	uacpi_status st;

	if (!handler)
		return AE_BAD_PARAMETER;

	h = kmalloc(sizeof(*h), GFP_KERNEL);
	if (!h)
		return AE_NO_MEMORY;

	h->node = node;
	h->type = handler_type;
	h->handler = handler;
	h->context = context;
	h->uacpi_installed = false;

	spin_lock(&shim_notify_lock);
	list_for_each_entry(existing, &shim_notify_handlers, list) {
		if (existing->node == node) {
			need_install = false;
			break;
		}
	}
	h->uacpi_installed = need_install;
	list_add(&h->list, &shim_notify_handlers);
	spin_unlock(&shim_notify_lock);

	if (!need_install)
		return AE_OK;

	st = uacpi_install_notify_handler(node, shim_notify_dispatch,
					  UACPI_NULL);
	if (st != UACPI_STATUS_OK) {
		spin_lock(&shim_notify_lock);
		list_del(&h->list);
		spin_unlock(&shim_notify_lock);
		kfree(h);
		return uacpi_to_acpi_status(st);
	}
	return AE_OK;
}

acpi_status acpi_remove_notify_handler(acpi_handle device, u32 handler_type,
				       acpi_notify_handler handler)
{
	uacpi_namespace_node *node = uacpi_node_from_handle(device);
	struct shim_notify_handler *h, *found = NULL;
	bool others = false;

	spin_lock(&shim_notify_lock);
	list_for_each_entry(h, &shim_notify_handlers, list) {
		if (h->node == node && h->handler == handler &&
		    h->type == handler_type && !found) {
			found = h;
			continue;
		}
		if (h->node == node)
			others = true;
	}
	if (found)
		list_del(&found->list);
	spin_unlock(&shim_notify_lock);

	if (!found)
		return AE_NOT_FOUND;

	if (!others)
		uacpi_uninstall_notify_handler(node, shim_notify_dispatch);

	kfree(found);
	return AE_OK;
}

/* --- Address space handlers ---
 *
 * uACPI's SystemMemory/SystemIO/PCI_Config spaces are serviced by built-in
 * handlers, but custom spaces (EmbeddedController, SMBus, ...) are driven by a
 * Linux-provided ACPICA handler. Bridge ACPICA's (function, address, width,
 * value, contexts) model to uACPI's op-based uacpi_region_handler.
 */

static uacpi_status acpi_to_uacpi_status(acpi_status st)
{
	switch (st) {
	case AE_OK:
		return UACPI_STATUS_OK;
	case AE_NO_MEMORY:
		return UACPI_STATUS_OUT_OF_MEMORY;
	case AE_NOT_FOUND:
	case AE_NOT_EXIST:
		return UACPI_STATUS_NOT_FOUND;
	case AE_BAD_PARAMETER:
		return UACPI_STATUS_INVALID_ARGUMENT;
	case AE_SUPPORT:
		return UACPI_STATUS_UNIMPLEMENTED;
	default:
		return UACPI_STATUS_INTERNAL_ERROR;
	}
}

struct shim_space_handler {
	struct list_head list;
	uacpi_namespace_node *node;
	acpi_adr_space_type space;
	acpi_adr_space_handler handler;
	acpi_adr_space_setup setup;
	void *context;
};

static LIST_HEAD(shim_space_handlers);
static DEFINE_SPINLOCK(shim_space_lock);

static uacpi_status shim_region_trampoline(uacpi_region_op op,
					   uacpi_handle op_data)
{
	switch (op) {
	case UACPI_REGION_OP_ATTACH: {
		uacpi_region_attach_data *d = op_data;
		struct shim_space_handler *h = d->handler_context;
		void *rc = h->context;
		acpi_status st = AE_OK;

		if (h->setup)
			st = h->setup(uacpi_handle_from_node(d->region_node),
				      ACPI_REGION_ACTIVATE, h->context, &rc);
		d->out_region_context = rc;
		return acpi_to_uacpi_status(st);
	}
	case UACPI_REGION_OP_DETACH: {
		uacpi_region_detach_data *d = op_data;
		struct shim_space_handler *h = d->handler_context;
		void *rc = d->region_context;
		acpi_status st = AE_OK;

		if (h->setup)
			st = h->setup(uacpi_handle_from_node(d->region_node),
				      ACPI_REGION_DEACTIVATE, h->context, &rc);
		return acpi_to_uacpi_status(st);
	}
	case UACPI_REGION_OP_READ:
	case UACPI_REGION_OP_WRITE: {
		uacpi_region_rw_data *d = op_data;
		struct shim_space_handler *h = d->handler_context;
		acpi_status st;

		st = h->handler(op == UACPI_REGION_OP_READ ? ACPI_READ :
							     ACPI_WRITE,
				d->address, d->byte_width * 8, &d->value,
				h->context, d->region_context);
		return acpi_to_uacpi_status(st);
	}
	default:
		return UACPI_STATUS_UNIMPLEMENTED;
	}
}

static acpi_status shim_install_space(acpi_handle device,
				      acpi_adr_space_type space_id,
				      acpi_adr_space_handler handler,
				      acpi_adr_space_setup setup, void *context)
{
	uacpi_namespace_node *node = uacpi_node_from_handle(device);
	struct shim_space_handler *h;
	uacpi_status st;

	if (!handler)
		return AE_BAD_PARAMETER;

	h = kmalloc(sizeof(*h), GFP_KERNEL);
	if (!h)
		return AE_NO_MEMORY;

	h->node = node;
	h->space = space_id;
	h->handler = handler;
	h->setup = setup;
	h->context = context;

	st = uacpi_install_address_space_handler(node, space_id,
						 shim_region_trampoline, h);
	if (st != UACPI_STATUS_OK) {
		kfree(h);
		return uacpi_to_acpi_status(st);
	}

	spin_lock(&shim_space_lock);
	list_add(&h->list, &shim_space_handlers);
	spin_unlock(&shim_space_lock);
	return AE_OK;
}

acpi_status acpi_install_address_space_handler(acpi_handle device,
					       acpi_adr_space_type space_id,
					       acpi_adr_space_handler handler,
					       acpi_adr_space_setup setup,
					       void *context)
{
	return shim_install_space(device, space_id, handler, setup, context);
}

acpi_status acpi_install_address_space_handler_no_reg(acpi_handle device,
				acpi_adr_space_type space_id,
				acpi_adr_space_handler handler,
				acpi_adr_space_setup setup, void *context)
{
	return shim_install_space(device, space_id, handler, setup, context);
}

acpi_status acpi_remove_address_space_handler(acpi_handle device,
					      acpi_adr_space_type space_id,
					      acpi_adr_space_handler handler)
{
	uacpi_namespace_node *node = uacpi_node_from_handle(device);
	struct shim_space_handler *h, *found = NULL;
	uacpi_status st;

	spin_lock(&shim_space_lock);
	list_for_each_entry(h, &shim_space_handlers, list) {
		if (h->node == node && h->space == space_id &&
		    h->handler == handler) {
			found = h;
			list_del(&h->list);
			break;
		}
	}
	spin_unlock(&shim_space_lock);

	if (!found)
		return AE_NOT_FOUND;

	st = uacpi_uninstall_address_space_handler(node, space_id);
	kfree(found);
	return uacpi_to_acpi_status(st);
}

acpi_status acpi_execute_reg_methods(acpi_handle device, u32 max_depth,
				     acpi_adr_space_type space_id)
{
	return uacpi_to_acpi_status(
		uacpi_reg_all_opregions(uacpi_node_from_handle(device),
					space_id));
}

acpi_status acpi_install_sci_handler(acpi_sci_handler address, void *context)
{
	return AE_OK;
}

acpi_status acpi_remove_sci_handler(acpi_sci_handler address)
{
	return AE_OK;
}

acpi_status acpi_install_global_event_handler(acpi_gbl_event_handler handler,
					      void *context)
{
	return AE_OK;
}

acpi_status acpi_install_exception_handler(acpi_exception_handler handler)
{
	return AE_OK;
}

acpi_status acpi_install_initialization_handler(acpi_init_handler handler,
						u32 function)
{
	return AE_OK;
}

acpi_status acpi_acquire_global_lock(u16 timeout, u32 *handle)
{
	uacpi_u32 seq;
	uacpi_status st = uacpi_acquire_global_lock(timeout, &seq);

	if (st != UACPI_STATUS_OK)
		return uacpi_to_acpi_status(st);
	if (handle)
		*handle = seq;
	return AE_OK;
}

acpi_status acpi_release_global_lock(u32 handle)
{
	return uacpi_to_acpi_status(uacpi_release_global_lock(handle));
}

acpi_status acpi_enter_sleep_state_prep(u8 sleep_state)
{
	return uacpi_to_acpi_status(uacpi_prepare_for_sleep_state(sleep_state));
}

acpi_status acpi_enter_sleep_state(u8 sleep_state)
{
	return uacpi_to_acpi_status(uacpi_enter_sleep_state(sleep_state));
}

acpi_status acpi_enter_sleep_state_s4bios(void)
{
	return AE_SUPPORT;
}

acpi_status acpi_leave_sleep_state_prep(u8 sleep_state)
{
	return uacpi_to_acpi_status(
		uacpi_prepare_for_wake_from_sleep_state(sleep_state));
}

acpi_status acpi_leave_sleep_state(u8 sleep_state)
{
	return uacpi_to_acpi_status(uacpi_wake_from_sleep_state(sleep_state));
}

acpi_status acpi_set_firmware_waking_vector(acpi_physical_address physical_address,
					    acpi_physical_address physical_address64)
{
	return uacpi_to_acpi_status(
		uacpi_set_waking_vector(physical_address, physical_address64));
}

acpi_status acpi_get_sleep_type_data(u8 sleep_state, u8 *slp_typ_a,
				     u8 *slp_typ_b)
{
	char path[5] = "_S0_";
	uacpi_object *ret = UACPI_NULL;
	uacpi_object_array pkg;
	u64 a = 0, b = 0;
	uacpi_status st;

	if (sleep_state > 5 || !slp_typ_a || !slp_typ_b)
		return AE_BAD_PARAMETER;

	path[2] = '0' + sleep_state;

	st = uacpi_eval_simple(uacpi_namespace_root(), path, &ret);
	if (st != UACPI_STATUS_OK || !ret)
		return AE_NOT_FOUND;

	if (uacpi_object_get_package(ret, &pkg) != UACPI_STATUS_OK ||
	    pkg.count < 1) {
		uacpi_object_unref(ret);
		return AE_AML_OPERAND_TYPE;
	}

	uacpi_object_get_integer(pkg.objects[0], &a);
	if (pkg.count > 1)
		uacpi_object_get_integer(pkg.objects[1], &b);

	*slp_typ_a = a;
	*slp_typ_b = b;
	uacpi_object_unref(ret);
	return AE_OK;
}

acpi_status acpi_read(u64 *value, struct acpi_generic_address *reg)
{
	return uacpi_to_acpi_status(
		uacpi_gas_read((const struct acpi_gas *)reg, value));
}

acpi_status acpi_write(u64 value, struct acpi_generic_address *reg)
{
	return uacpi_to_acpi_status(
		uacpi_gas_write((const struct acpi_gas *)reg, value));
}

static const int shim_bitreg_to_field[ACPI_BITREG_MAX + 1] = {
	[ACPI_BITREG_TIMER_STATUS]	= UACPI_REGISTER_FIELD_TMR_STS,
	[ACPI_BITREG_BUS_MASTER_STATUS]	= UACPI_REGISTER_FIELD_BM_STS,
	[ACPI_BITREG_GLOBAL_LOCK_STATUS] = UACPI_REGISTER_FIELD_GBL_STS,
	[ACPI_BITREG_POWER_BUTTON_STATUS] = UACPI_REGISTER_FIELD_PWRBTN_STS,
	[ACPI_BITREG_SLEEP_BUTTON_STATUS] = UACPI_REGISTER_FIELD_SLPBTN_STS,
	[ACPI_BITREG_RT_CLOCK_STATUS]	= UACPI_REGISTER_FIELD_RTC_STS,
	[ACPI_BITREG_WAKE_STATUS]	= UACPI_REGISTER_FIELD_WAK_STS,
	[ACPI_BITREG_PCIEXP_WAKE_STATUS] = UACPI_REGISTER_FIELD_PCIEX_WAKE_STS,
	[ACPI_BITREG_TIMER_ENABLE]	= UACPI_REGISTER_FIELD_TMR_EN,
	[ACPI_BITREG_GLOBAL_LOCK_ENABLE] = UACPI_REGISTER_FIELD_GBL_EN,
	[ACPI_BITREG_POWER_BUTTON_ENABLE] = UACPI_REGISTER_FIELD_PWRBTN_EN,
	[ACPI_BITREG_SLEEP_BUTTON_ENABLE] = UACPI_REGISTER_FIELD_SLPBTN_EN,
	[ACPI_BITREG_RT_CLOCK_ENABLE]	= UACPI_REGISTER_FIELD_RTC_EN,
	[ACPI_BITREG_PCIEXP_WAKE_DISABLE] = UACPI_REGISTER_FIELD_PCIEXP_WAKE_DIS,
	[ACPI_BITREG_SCI_ENABLE]	= UACPI_REGISTER_FIELD_SCI_EN,
	[ACPI_BITREG_BUS_MASTER_RLD]	= UACPI_REGISTER_FIELD_BM_RLD,
	[ACPI_BITREG_GLOBAL_LOCK_RELEASE] = UACPI_REGISTER_FIELD_GBL_RLS,
	[ACPI_BITREG_SLEEP_TYPE]	= UACPI_REGISTER_FIELD_SLP_TYP,
	[ACPI_BITREG_SLEEP_ENABLE]	= UACPI_REGISTER_FIELD_SLP_EN,
	[ACPI_BITREG_ARB_DISABLE]	= UACPI_REGISTER_FIELD_ARB_DIS,
};

acpi_status acpi_read_bit_register(u32 register_id, u32 *return_value)
{
	uacpi_u64 val;
	uacpi_status st;

	if (register_id > ACPI_BITREG_MAX || !return_value)
		return AE_BAD_PARAMETER;

	st = uacpi_read_register_field(shim_bitreg_to_field[register_id], &val);
	if (st != UACPI_STATUS_OK)
		return uacpi_to_acpi_status(st);

	*return_value = val;
	return AE_OK;
}

acpi_status acpi_write_bit_register(u32 register_id, u32 value)
{
	if (register_id > ACPI_BITREG_MAX)
		return AE_BAD_PARAMETER;

	return uacpi_to_acpi_status(
		uacpi_write_register_field(shim_bitreg_to_field[register_id],
					   value));
}
