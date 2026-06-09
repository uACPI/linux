// SPDX-License-Identifier: GPL-2.0
/*
 * Resource subsystem for the uACPI shim.
 *
 * Translates between uACPI's uacpi_resource(s) representation and ACPICA's
 * struct acpi_resource buffer format. Each converted ACPICA resource is made
 * self-contained: any pointed-to data (resource source strings, GPIO/serial
 * pin tables, vendor blobs) is copied immediately after the fixed structure
 * within the same entry and the pointer fields are fixed up to point inside it.
 * This keeps the result valid for both the streaming acpi_walk_resources()
 * callback model and the standalone buffer returned by
 * acpi_get_current_resources() & friends, and lets the uACPI source list be
 * freed right away.
 */

#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/string.h>

#include <linux/acpi.h>
#include "shim.h"

#include <uacpi/resources.h>

typedef acpi_status (*acpi_walk_resource_callback)(struct acpi_resource *res,
						   void *context);

#define RES_HDR		offsetof(struct acpi_resource, data)
#define RES_ALIGN(x)	ALIGN((x), 8)

static u32 src_bytes(const uacpi_resource_source *s)
{
	return (s->string && s->length) ? s->length : 0;
}

/* Copy a uACPI resource source string into the entry at byte offset 'off'. */
static u32 put_source(struct acpi_resource_source *dst,
		      const uacpi_resource_source *src, u8 *entry, u32 off)
{
	u32 n = src_bytes(src);

	dst->index = src->index;
	dst->string_length = n;
	if (n) {
		dst->string_ptr = (char *)(entry + off);
		memcpy(dst->string_ptr, src->string, n);
	} else {
		dst->string_ptr = NULL;
	}
	return n;
}

static void conv_addr_common(struct acpi_resource_address *dst,
			     const uacpi_resource_address_common *src)
{
	dst->resource_type = src->type;
	dst->producer_consumer = src->direction;
	dst->decode = src->decode_type;
	dst->min_address_fixed = src->fixed_min_address;
	dst->max_address_fixed = src->fixed_max_address;

	if (src->type == UACPI_RANGE_IO) {
		dst->info.io.range_type = src->attribute.io.range_type;
		dst->info.io.translation = src->attribute.io.translation;
		dst->info.io.translation_type =
			src->attribute.io.translation_type;
	} else {
		dst->info.mem.write_protect =
			src->attribute.memory.write_status;
		dst->info.mem.caching = src->attribute.memory.caching;
		dst->info.mem.range_type = src->attribute.memory.range_type;
		dst->info.mem.translation = src->attribute.memory.translation;
	}
}

/*
 * Convert one uACPI resource to ACPICA format. Returns the entry stride in
 * bytes (acpi_resource.length). When 'a' is NULL only the size is computed.
 * Returns 0 for resource types that are not represented.
 */
static u32 conv(const uacpi_resource *u, struct acpi_resource *a)
{
	u8 *e = (u8 *)a;
	u32 len, ao;

	switch (u->type) {
	case UACPI_RESOURCE_TYPE_IRQ: {
		u32 n = u->irq.num_irqs;
		u32 base = offsetof(struct acpi_resource_irq, interrupt);

		len = RES_ALIGN(RES_HDR + base + n);
		if (a) {
			a->type = ACPI_RESOURCE_TYPE_IRQ;
			a->data.irq.descriptor_length = 0;
			a->data.irq.triggering = u->irq.triggering;
			a->data.irq.polarity = u->irq.polarity;
			a->data.irq.shareable = u->irq.sharing;
			a->data.irq.wake_capable = u->irq.wake_capability;
			a->data.irq.interrupt_count = n;
			memcpy(a->data.irq.interrupts, u->irq.irqs, n);
		}
		break;
	}
	case UACPI_RESOURCE_TYPE_EXTENDED_IRQ: {
		u32 n = u->extended_irq.num_irqs;
		u32 base = offsetof(struct acpi_resource_extended_irq, interrupt);
		u32 ints = base + n * sizeof(u32);

		len = RES_ALIGN(RES_HDR + ints +
				src_bytes(&u->extended_irq.source));
		if (a) {
			u32 i;

			a->type = ACPI_RESOURCE_TYPE_EXTENDED_IRQ;
			a->data.extended_irq.producer_consumer =
				u->extended_irq.direction;
			a->data.extended_irq.triggering =
				u->extended_irq.triggering;
			a->data.extended_irq.polarity = u->extended_irq.polarity;
			a->data.extended_irq.shareable = u->extended_irq.sharing;
			a->data.extended_irq.wake_capable =
				u->extended_irq.wake_capability;
			a->data.extended_irq.interrupt_count = n;
			for (i = 0; i < n; i++)
				a->data.extended_irq.interrupts[i] =
					u->extended_irq.irqs[i];
			put_source(&a->data.extended_irq.resource_source,
				   &u->extended_irq.source, e, RES_HDR + ints);
		}
		break;
	}
	case UACPI_RESOURCE_TYPE_DMA: {
		u32 n = u->dma.num_channels;
		u32 base = offsetof(struct acpi_resource_dma, channel);

		len = RES_ALIGN(RES_HDR + base + n);
		if (a) {
			a->type = ACPI_RESOURCE_TYPE_DMA;
			a->data.dma.type = u->dma.channel_speed;
			a->data.dma.bus_master = u->dma.bus_master_status;
			a->data.dma.transfer = u->dma.transfer_type;
			a->data.dma.channel_count = n;
			memcpy(a->data.dma.channels, u->dma.channels, n);
		}
		break;
	}
	case UACPI_RESOURCE_TYPE_FIXED_DMA:
		len = RES_ALIGN(RES_HDR + sizeof(struct acpi_resource_fixed_dma));
		if (a) {
			a->type = ACPI_RESOURCE_TYPE_FIXED_DMA;
			a->data.fixed_dma.request_lines =
				u->fixed_dma.request_line;
			a->data.fixed_dma.channels = u->fixed_dma.channel;
			a->data.fixed_dma.width = u->fixed_dma.transfer_width;
		}
		break;
	case UACPI_RESOURCE_TYPE_IO:
		len = RES_ALIGN(RES_HDR + sizeof(struct acpi_resource_io));
		if (a) {
			a->type = ACPI_RESOURCE_TYPE_IO;
			a->data.io.io_decode = u->io.decode_type;
			a->data.io.alignment = u->io.alignment;
			a->data.io.address_length = u->io.length;
			a->data.io.minimum = u->io.minimum;
			a->data.io.maximum = u->io.maximum;
		}
		break;
	case UACPI_RESOURCE_TYPE_FIXED_IO:
		len = RES_ALIGN(RES_HDR + sizeof(struct acpi_resource_fixed_io));
		if (a) {
			a->type = ACPI_RESOURCE_TYPE_FIXED_IO;
			a->data.fixed_io.address = u->fixed_io.address;
			a->data.fixed_io.address_length = u->fixed_io.length;
		}
		break;
	case UACPI_RESOURCE_TYPE_MEMORY24:
		len = RES_ALIGN(RES_HDR + sizeof(struct acpi_resource_memory24));
		if (a) {
			a->type = ACPI_RESOURCE_TYPE_MEMORY24;
			a->data.memory24.write_protect = u->memory24.write_status;
			a->data.memory24.minimum = u->memory24.minimum;
			a->data.memory24.maximum = u->memory24.maximum;
			a->data.memory24.alignment = u->memory24.alignment;
			a->data.memory24.address_length = u->memory24.length;
		}
		break;
	case UACPI_RESOURCE_TYPE_MEMORY32:
		len = RES_ALIGN(RES_HDR + sizeof(struct acpi_resource_memory32));
		if (a) {
			a->type = ACPI_RESOURCE_TYPE_MEMORY32;
			a->data.memory32.write_protect = u->memory32.write_status;
			a->data.memory32.minimum = u->memory32.minimum;
			a->data.memory32.maximum = u->memory32.maximum;
			a->data.memory32.alignment = u->memory32.alignment;
			a->data.memory32.address_length = u->memory32.length;
		}
		break;
	case UACPI_RESOURCE_TYPE_FIXED_MEMORY32:
		len = RES_ALIGN(RES_HDR +
				sizeof(struct acpi_resource_fixed_memory32));
		if (a) {
			a->type = ACPI_RESOURCE_TYPE_FIXED_MEMORY32;
			a->data.fixed_memory32.write_protect =
				u->fixed_memory32.write_status;
			a->data.fixed_memory32.address =
				u->fixed_memory32.address;
			a->data.fixed_memory32.address_length =
				u->fixed_memory32.length;
		}
		break;
	case UACPI_RESOURCE_TYPE_ADDRESS16:
		ao = RES_HDR + sizeof(struct acpi_resource_address16);
		len = RES_ALIGN(ao + src_bytes(&u->address16.source));
		if (a) {
			a->type = ACPI_RESOURCE_TYPE_ADDRESS16;
			conv_addr_common(&a->data.address,
					 &u->address16.common);
			a->data.address16.address.granularity =
				u->address16.granularity;
			a->data.address16.address.minimum = u->address16.minimum;
			a->data.address16.address.maximum = u->address16.maximum;
			a->data.address16.address.translation_offset =
				u->address16.translation_offset;
			a->data.address16.address.address_length =
				u->address16.address_length;
			put_source(&a->data.address16.resource_source,
				   &u->address16.source, e, ao);
		}
		break;
	case UACPI_RESOURCE_TYPE_ADDRESS32:
		ao = RES_HDR + sizeof(struct acpi_resource_address32);
		len = RES_ALIGN(ao + src_bytes(&u->address32.source));
		if (a) {
			a->type = ACPI_RESOURCE_TYPE_ADDRESS32;
			conv_addr_common(&a->data.address,
					 &u->address32.common);
			a->data.address32.address.granularity =
				u->address32.granularity;
			a->data.address32.address.minimum = u->address32.minimum;
			a->data.address32.address.maximum = u->address32.maximum;
			a->data.address32.address.translation_offset =
				u->address32.translation_offset;
			a->data.address32.address.address_length =
				u->address32.address_length;
			put_source(&a->data.address32.resource_source,
				   &u->address32.source, e, ao);
		}
		break;
	case UACPI_RESOURCE_TYPE_ADDRESS64:
		ao = RES_HDR + sizeof(struct acpi_resource_address64);
		len = RES_ALIGN(ao + src_bytes(&u->address64.source));
		if (a) {
			a->type = ACPI_RESOURCE_TYPE_ADDRESS64;
			conv_addr_common(&a->data.address,
					 &u->address64.common);
			a->data.address64.address.granularity =
				u->address64.granularity;
			a->data.address64.address.minimum = u->address64.minimum;
			a->data.address64.address.maximum = u->address64.maximum;
			a->data.address64.address.translation_offset =
				u->address64.translation_offset;
			a->data.address64.address.address_length =
				u->address64.address_length;
			put_source(&a->data.address64.resource_source,
				   &u->address64.source, e, ao);
		}
		break;
	case UACPI_RESOURCE_TYPE_ADDRESS64_EXTENDED:
		len = RES_ALIGN(RES_HDR +
				sizeof(struct acpi_resource_extended_address64));
		if (a) {
			a->type = ACPI_RESOURCE_TYPE_EXTENDED_ADDRESS64;
			conv_addr_common(&a->data.address,
					 &u->address64_extended.common);
			a->data.ext_address64.revision_ID =
				u->address64_extended.revision_id;
			a->data.ext_address64.address.granularity =
				u->address64_extended.granularity;
			a->data.ext_address64.address.minimum =
				u->address64_extended.minimum;
			a->data.ext_address64.address.maximum =
				u->address64_extended.maximum;
			a->data.ext_address64.address.translation_offset =
				u->address64_extended.translation_offset;
			a->data.ext_address64.address.address_length =
				u->address64_extended.address_length;
			a->data.ext_address64.type_specific =
				u->address64_extended.attributes;
		}
		break;
	case UACPI_RESOURCE_TYPE_START_DEPENDENT:
		len = RES_ALIGN(RES_HDR +
				sizeof(struct acpi_resource_start_dependent));
		if (a) {
			a->type = ACPI_RESOURCE_TYPE_START_DEPENDENT;
			a->data.start_dpf.descriptor_length = 0;
			a->data.start_dpf.compatibility_priority =
				u->start_dependent.compatibility;
			a->data.start_dpf.performance_robustness =
				u->start_dependent.performance;
		}
		break;
	case UACPI_RESOURCE_TYPE_END_DEPENDENT:
		len = RES_ALIGN(RES_HDR);
		if (a)
			a->type = ACPI_RESOURCE_TYPE_END_DEPENDENT;
		break;
	case UACPI_RESOURCE_TYPE_VENDOR_SMALL: {
		u32 n = u->vendor.length;
		u32 base = offsetof(struct acpi_resource_vendor, byte_data);

		len = RES_ALIGN(RES_HDR + base + n);
		if (a) {
			a->type = ACPI_RESOURCE_TYPE_VENDOR;
			a->data.vendor.byte_length = n;
			memcpy(a->data.vendor.byte_data, u->vendor.data, n);
		}
		break;
	}
	case UACPI_RESOURCE_TYPE_VENDOR_LARGE: {
		u32 n = u->vendor_typed.length;
		u32 base = offsetof(struct acpi_resource_vendor_typed,
				    byte_data);

		len = RES_ALIGN(RES_HDR + base + n);
		if (a) {
			a->type = ACPI_RESOURCE_TYPE_VENDOR;
			a->data.vendor_typed.byte_length = n;
			a->data.vendor_typed.uuid_subtype =
				u->vendor_typed.sub_type;
			memcpy(a->data.vendor_typed.uuid, u->vendor_typed.uuid,
			       ACPI_UUID_LENGTH);
			memcpy(a->data.vendor_typed.byte_data,
			       u->vendor_typed.data, n);
		}
		break;
	}
	case UACPI_RESOURCE_TYPE_GENERIC_REGISTER:
		len = RES_ALIGN(RES_HDR +
				sizeof(struct acpi_resource_generic_register));
		if (a) {
			a->type = ACPI_RESOURCE_TYPE_GENERIC_REGISTER;
			a->data.generic_reg.space_id =
				u->generic_register.address_space_id;
			a->data.generic_reg.bit_width =
				u->generic_register.bit_width;
			a->data.generic_reg.bit_offset =
				u->generic_register.bit_offset;
			a->data.generic_reg.access_size =
				u->generic_register.access_size;
			a->data.generic_reg.address =
				u->generic_register.address;
		}
		break;
	case UACPI_RESOURCE_TYPE_GPIO_CONNECTION: {
		const uacpi_resource_gpio_connection *g = &u->gpio_connection;
		u32 pin_bytes = g->pin_table_length * sizeof(u16);
		u32 vnd = g->vendor_data_length;
		u32 src = src_bytes(&g->source);

		ao = RES_HDR + sizeof(struct acpi_resource_gpio);
		len = RES_ALIGN(ao + src + pin_bytes + vnd);
		if (a) {
			u32 o = ao;

			a->type = ACPI_RESOURCE_TYPE_GPIO;
			a->data.gpio.revision_id = g->revision_id;
			a->data.gpio.connection_type = g->type;
			a->data.gpio.producer_consumer = g->direction;
			a->data.gpio.pin_config = g->pull_configuration;
			a->data.gpio.drive_strength = g->drive_strength;
			a->data.gpio.debounce_timeout = g->debounce_timeout;
			if (g->type == UACPI_GPIO_CONNECTION_INTERRUPT) {
				a->data.gpio.triggering = g->intr.triggering;
				a->data.gpio.polarity = g->intr.polarity;
				a->data.gpio.shareable = g->intr.sharing;
				a->data.gpio.wake_capable =
					g->intr.wake_capability;
			} else {
				a->data.gpio.io_restriction = g->io.restriction;
				a->data.gpio.shareable = g->io.sharing;
			}
			a->data.gpio.pin_table_length = g->pin_table_length;
			a->data.gpio.vendor_length = vnd;
			o += put_source(&a->data.gpio.resource_source,
					&g->source, e, o);
			a->data.gpio.pin_table = (u16 *)(e + o);
			memcpy(a->data.gpio.pin_table, g->pin_table, pin_bytes);
			o += pin_bytes;
			a->data.gpio.vendor_data = vnd ? e + o : NULL;
			if (vnd)
				memcpy(a->data.gpio.vendor_data,
				       g->vendor_data, vnd);
		}
		break;
	}
	case UACPI_RESOURCE_TYPE_SERIAL_I2C_CONNECTION:
	case UACPI_RESOURCE_TYPE_SERIAL_SPI_CONNECTION:
	case UACPI_RESOURCE_TYPE_SERIAL_UART_CONNECTION: {
		const uacpi_resource_serial_bus_common *c =
			&u->serial_bus_common;
		u32 vnd = c->vendor_data_length;
		u32 src = src_bytes(&c->source);
		u32 fixed;

		if (u->type == UACPI_RESOURCE_TYPE_SERIAL_I2C_CONNECTION)
			fixed = sizeof(struct acpi_resource_i2c_serialbus);
		else if (u->type == UACPI_RESOURCE_TYPE_SERIAL_SPI_CONNECTION)
			fixed = sizeof(struct acpi_resource_spi_serialbus);
		else
			fixed = sizeof(struct acpi_resource_uart_serialbus);

		ao = RES_HDR + fixed;
		len = RES_ALIGN(ao + src + vnd);
		if (a) {
			struct acpi_resource_common_serialbus *cb =
				&a->data.common_serial_bus;
			u32 o = ao;

			a->type = ACPI_RESOURCE_TYPE_SERIAL_BUS;
			cb->revision_id = c->revision_id;
			cb->type = c->type;
			cb->producer_consumer = c->direction;
			cb->slave_mode = c->mode;
			cb->connection_sharing = c->sharing;
			cb->type_revision_id = c->type_revision_id;
			cb->type_data_length = c->type_data_length;
			cb->vendor_length = vnd;
			o += put_source(&cb->resource_source, &c->source, e, o);
			cb->vendor_data = vnd ? e + o : NULL;
			if (vnd)
				memcpy(cb->vendor_data, c->vendor_data, vnd);

			if (u->type ==
			    UACPI_RESOURCE_TYPE_SERIAL_I2C_CONNECTION) {
				a->data.i2c_serial_bus.access_mode =
					u->i2c_connection.addressing_mode;
				a->data.i2c_serial_bus.slave_address =
					u->i2c_connection.slave_address;
				a->data.i2c_serial_bus.connection_speed =
					u->i2c_connection.connection_speed;
			} else if (u->type ==
				   UACPI_RESOURCE_TYPE_SERIAL_SPI_CONNECTION) {
				a->data.spi_serial_bus.wire_mode =
					u->spi_connection.wire_mode;
				a->data.spi_serial_bus.device_polarity =
					u->spi_connection.device_polarity;
				a->data.spi_serial_bus.data_bit_length =
					u->spi_connection.data_bit_length;
				a->data.spi_serial_bus.clock_phase =
					u->spi_connection.phase;
				a->data.spi_serial_bus.clock_polarity =
					u->spi_connection.polarity;
				a->data.spi_serial_bus.device_selection =
					u->spi_connection.device_selection;
				a->data.spi_serial_bus.connection_speed =
					u->spi_connection.connection_speed;
			} else {
				a->data.uart_serial_bus.endian =
					u->uart_connection.endianness;
				a->data.uart_serial_bus.data_bits =
					u->uart_connection.data_bits;
				a->data.uart_serial_bus.stop_bits =
					u->uart_connection.stop_bits;
				a->data.uart_serial_bus.flow_control =
					u->uart_connection.flow_control;
				a->data.uart_serial_bus.parity =
					u->uart_connection.parity;
				a->data.uart_serial_bus.lines_enabled =
					u->uart_connection.lines_enabled;
				a->data.uart_serial_bus.rx_fifo_size =
					u->uart_connection.rx_fifo;
				a->data.uart_serial_bus.tx_fifo_size =
					u->uart_connection.tx_fifo;
				a->data.uart_serial_bus.default_baud_rate =
					u->uart_connection.baud_rate;
			}
		}
		break;
	}
	case UACPI_RESOURCE_TYPE_END_TAG:
		len = RES_ALIGN(RES_HDR + sizeof(struct acpi_resource_end_tag));
		if (a) {
			a->type = ACPI_RESOURCE_TYPE_END_TAG;
			a->data.end_tag.checksum = 0;
		}
		break;
	default:
		/* Unrepresented resource type: skip it. */
		return 0;
	}

	if (a)
		a->length = len;
	return len;
}

struct conv_ctx {
	u8 *out;	/* NULL during the sizing pass */
	u32 off;
};

static uacpi_iteration_decision conv_cb(void *user, uacpi_resource *u)
{
	struct conv_ctx *c = user;
	struct acpi_resource *a = NULL;
	u32 len;

	if (c->out)
		a = (struct acpi_resource *)(c->out + c->off);

	len = conv(u, a);
	c->off += len;
	return UACPI_ITERATION_DECISION_CONTINUE;
}

/* Build an ACPICA-format resource buffer from a uACPI resource list. */
static acpi_status build_buffer(uacpi_resources *ures, struct acpi_buffer *out)
{
	struct conv_ctx ctx = { .out = NULL, .off = 0 };
	u32 size;

	uacpi_for_each_resource(ures, conv_cb, &ctx);
	size = ctx.off;
	if (!size)
		return AE_NOT_FOUND;

	if (out->length == ACPI_ALLOCATE_BUFFER) {
		out->pointer = kzalloc(size, GFP_KERNEL);
		if (!out->pointer)
			return AE_NO_MEMORY;
		out->length = size;
	} else {
		if (out->length < size) {
			out->length = size;
			return AE_BUFFER_OVERFLOW;
		}
		memset(out->pointer, 0, size);
		out->length = size;
	}

	ctx.out = out->pointer;
	ctx.off = 0;
	uacpi_for_each_resource(ures, conv_cb, &ctx);
	return AE_OK;
}

static acpi_status get_resources(uacpi_namespace_node *node, const char *method,
				 struct acpi_buffer *out)
{
	uacpi_resources *ures;
	uacpi_status ust;
	acpi_status status;

	if (!out)
		return AE_BAD_PARAMETER;

	ust = uacpi_get_device_resources(node, method, &ures);
	if (ust != UACPI_STATUS_OK)
		return uacpi_to_acpi_status(ust);

	status = build_buffer(ures, out);
	uacpi_free_resources(ures);
	return status;
}

acpi_status acpi_get_current_resources(acpi_handle device,
				       struct acpi_buffer *ret_buffer)
{
	return get_resources(uacpi_node_from_handle(device), "_CRS", ret_buffer);
}

acpi_status acpi_get_possible_resources(acpi_handle device,
					struct acpi_buffer *ret_buffer)
{
	return get_resources(uacpi_node_from_handle(device), "_PRS", ret_buffer);
}

acpi_status acpi_get_event_resources(acpi_handle device_handle,
				     struct acpi_buffer *ret_buffer)
{
	return get_resources(uacpi_node_from_handle(device_handle), "_AEI",
			     ret_buffer);
}

/* Walk an already-built ACPICA-format resource buffer. */
acpi_status acpi_walk_resource_buffer(struct acpi_buffer *buffer,
				      acpi_walk_resource_callback user_function,
				      void *context)
{
	struct acpi_resource *res, *end;

	if (!buffer || !buffer->pointer || !user_function)
		return AE_BAD_PARAMETER;

	res = buffer->pointer;
	end = (struct acpi_resource *)((u8 *)buffer->pointer + buffer->length);

	while (res < end) {
		acpi_status status;

		if (res->type == ACPI_RESOURCE_TYPE_END_TAG)
			break;
		if (!res->length)
			return AE_AML_BAD_RESOURCE_LENGTH;

		status = user_function(res, context);
		if (status == AE_CTRL_TERMINATE)
			return AE_OK;
		if (ACPI_FAILURE(status))
			return status;

		res = ACPI_NEXT_RESOURCE(res);
	}
	return AE_OK;
}

acpi_status acpi_walk_resources(acpi_handle device, char *name,
				acpi_walk_resource_callback user_function,
				void *context)
{
	struct acpi_buffer buffer = { ACPI_ALLOCATE_BUFFER, NULL };
	acpi_status status;

	if (!name || !user_function)
		return AE_BAD_PARAMETER;

	status = get_resources(uacpi_node_from_handle(device), name, &buffer);
	if (ACPI_FAILURE(status))
		return status;

	status = acpi_walk_resource_buffer(&buffer, user_function, context);
	kfree(buffer.pointer);
	return status;
}

/*
 * Reverse conversion (ACPICA struct acpi_resource -> uacpi_resource) for _SRS.
 * Returns the uACPI entry stride, or 0 for unrepresented types. When 'u' is
 * NULL only the size is computed.
 */
#define URES_HDR	offsetof(uacpi_resource, irq)

static u32 put_usource(uacpi_resource_source *dst,
		       const struct acpi_resource_source *src, u8 *entry,
		       u32 off)
{
	u32 n = src->string_ptr ? src->string_length : 0;

	dst->index = src->index;
	dst->index_present = src->string_ptr ? UACPI_TRUE : UACPI_FALSE;
	dst->length = n;
	if (n) {
		dst->string = (uacpi_char *)(entry + off);
		memcpy(dst->string, src->string_ptr, n);
	} else {
		dst->string = NULL;
	}
	return n;
}

static u32 rconv(const struct acpi_resource *a, uacpi_resource *u)
{
	u8 *e = (u8 *)u;
	u32 len;

	switch (a->type) {
	case ACPI_RESOURCE_TYPE_IRQ: {
		u32 n = a->data.irq.interrupt_count;

		len = RES_ALIGN(URES_HDR +
				offsetof(uacpi_resource_irq, irqs) + n);
		if (u) {
			u->type = UACPI_RESOURCE_TYPE_IRQ;
			u->irq.length_kind = UACPI_RESOURCE_LENGTH_KIND_FULL;
			u->irq.triggering = a->data.irq.triggering;
			u->irq.polarity = a->data.irq.polarity;
			u->irq.sharing = a->data.irq.shareable;
			u->irq.wake_capability = a->data.irq.wake_capable;
			u->irq.num_irqs = n;
			memcpy(u->irq.irqs, a->data.irq.interrupts, n);
		}
		break;
	}
	case ACPI_RESOURCE_TYPE_EXTENDED_IRQ: {
		u32 n = a->data.extended_irq.interrupt_count;
		u32 ints = offsetof(uacpi_resource_extended_irq, irqs) +
			   n * sizeof(u32);

		len = RES_ALIGN(URES_HDR + ints +
				(a->data.extended_irq.resource_source.string_ptr ?
				 a->data.extended_irq.resource_source.string_length :
				 0));
		if (u) {
			u32 i;

			u->type = UACPI_RESOURCE_TYPE_EXTENDED_IRQ;
			u->extended_irq.direction =
				a->data.extended_irq.producer_consumer;
			u->extended_irq.triggering =
				a->data.extended_irq.triggering;
			u->extended_irq.polarity =
				a->data.extended_irq.polarity;
			u->extended_irq.sharing = a->data.extended_irq.shareable;
			u->extended_irq.wake_capability =
				a->data.extended_irq.wake_capable;
			u->extended_irq.num_irqs = n;
			for (i = 0; i < n; i++)
				u->extended_irq.irqs[i] =
					a->data.extended_irq.interrupts[i];
			put_usource(&u->extended_irq.source,
				    &a->data.extended_irq.resource_source,
				    e, URES_HDR + ints);
		}
		break;
	}
	case ACPI_RESOURCE_TYPE_DMA: {
		u32 n = a->data.dma.channel_count;

		len = RES_ALIGN(URES_HDR +
				offsetof(uacpi_resource_dma, channels) + n);
		if (u) {
			u->type = UACPI_RESOURCE_TYPE_DMA;
			u->dma.transfer_type = a->data.dma.transfer;
			u->dma.bus_master_status = a->data.dma.bus_master;
			u->dma.channel_speed = a->data.dma.type;
			u->dma.num_channels = n;
			memcpy(u->dma.channels, a->data.dma.channels, n);
		}
		break;
	}
	case ACPI_RESOURCE_TYPE_IO:
		len = RES_ALIGN(URES_HDR + sizeof(uacpi_resource_io));
		if (u) {
			u->type = UACPI_RESOURCE_TYPE_IO;
			u->io.decode_type = a->data.io.io_decode;
			u->io.minimum = a->data.io.minimum;
			u->io.maximum = a->data.io.maximum;
			u->io.alignment = a->data.io.alignment;
			u->io.length = a->data.io.address_length;
		}
		break;
	case ACPI_RESOURCE_TYPE_FIXED_IO:
		len = RES_ALIGN(URES_HDR + sizeof(uacpi_resource_fixed_io));
		if (u) {
			u->type = UACPI_RESOURCE_TYPE_FIXED_IO;
			u->fixed_io.address = a->data.fixed_io.address;
			u->fixed_io.length = a->data.fixed_io.address_length;
		}
		break;
	case ACPI_RESOURCE_TYPE_MEMORY24:
		len = RES_ALIGN(URES_HDR + sizeof(uacpi_resource_memory24));
		if (u) {
			u->type = UACPI_RESOURCE_TYPE_MEMORY24;
			u->memory24.write_status =
				a->data.memory24.write_protect;
			u->memory24.minimum = a->data.memory24.minimum;
			u->memory24.maximum = a->data.memory24.maximum;
			u->memory24.alignment = a->data.memory24.alignment;
			u->memory24.length = a->data.memory24.address_length;
		}
		break;
	case ACPI_RESOURCE_TYPE_MEMORY32:
		len = RES_ALIGN(URES_HDR + sizeof(uacpi_resource_memory32));
		if (u) {
			u->type = UACPI_RESOURCE_TYPE_MEMORY32;
			u->memory32.write_status =
				a->data.memory32.write_protect;
			u->memory32.minimum = a->data.memory32.minimum;
			u->memory32.maximum = a->data.memory32.maximum;
			u->memory32.alignment = a->data.memory32.alignment;
			u->memory32.length = a->data.memory32.address_length;
		}
		break;
	case ACPI_RESOURCE_TYPE_FIXED_MEMORY32:
		len = RES_ALIGN(URES_HDR +
				sizeof(uacpi_resource_fixed_memory32));
		if (u) {
			u->type = UACPI_RESOURCE_TYPE_FIXED_MEMORY32;
			u->fixed_memory32.write_status =
				a->data.fixed_memory32.write_protect;
			u->fixed_memory32.address =
				a->data.fixed_memory32.address;
			u->fixed_memory32.length =
				a->data.fixed_memory32.address_length;
		}
		break;
	case ACPI_RESOURCE_TYPE_START_DEPENDENT:
		len = RES_ALIGN(URES_HDR +
				sizeof(uacpi_resource_start_dependent));
		if (u) {
			u->type = UACPI_RESOURCE_TYPE_START_DEPENDENT;
			u->start_dependent.length_kind =
				UACPI_RESOURCE_LENGTH_KIND_FULL;
			u->start_dependent.compatibility =
				a->data.start_dpf.compatibility_priority;
			u->start_dependent.performance =
				a->data.start_dpf.performance_robustness;
		}
		break;
	case ACPI_RESOURCE_TYPE_END_DEPENDENT:
		len = RES_ALIGN(URES_HDR);
		if (u)
			u->type = UACPI_RESOURCE_TYPE_END_DEPENDENT;
		break;
	case ACPI_RESOURCE_TYPE_VENDOR: {
		u32 n = a->data.vendor.byte_length;

		len = RES_ALIGN(URES_HDR +
				offsetof(uacpi_resource_vendor, data) + n);
		if (u) {
			u->type = UACPI_RESOURCE_TYPE_VENDOR_SMALL;
			u->vendor.length = n;
			memcpy(u->vendor.data, a->data.vendor.byte_data, n);
		}
		break;
	}
	case ACPI_RESOURCE_TYPE_END_TAG:
		len = RES_ALIGN(URES_HDR);
		if (u)
			u->type = UACPI_RESOURCE_TYPE_END_TAG;
		break;
	default:
		return 0;
	}

	if (u)
		u->length = len;
	return len;
}

acpi_status acpi_set_current_resources(acpi_handle device,
				       struct acpi_buffer *in_buffer)
{
	uacpi_resources ures;
	struct acpi_resource *res, *end;
	uacpi_status ust;
	u32 size = 0;
	u8 *out;

	if (!in_buffer || !in_buffer->pointer)
		return AE_BAD_PARAMETER;

	res = in_buffer->pointer;
	end = (struct acpi_resource *)((u8 *)in_buffer->pointer +
				       in_buffer->length);

	/* Size pass (stop after the END_TAG). */
	for (; res < end; res = ACPI_NEXT_RESOURCE(res)) {
		u32 l = rconv(res, NULL);

		if (!l)
			return AE_SUPPORT;
		size += l;
		if (res->type == ACPI_RESOURCE_TYPE_END_TAG)
			break;
	}
	if (!size)
		return AE_BAD_PARAMETER;

	out = kzalloc(size, GFP_KERNEL);
	if (!out)
		return AE_NO_MEMORY;

	ures.entries = (uacpi_resource *)out;
	ures.length = size;

	res = in_buffer->pointer;
	for (; res < end; res = ACPI_NEXT_RESOURCE(res)) {
		out += rconv(res, (uacpi_resource *)out);
		if (res->type == ACPI_RESOURCE_TYPE_END_TAG)
			break;
	}

	ust = uacpi_set_resources(uacpi_node_from_handle(device), &ures);
	kfree(ures.entries);
	return uacpi_to_acpi_status(ust);
}

acpi_status acpi_get_irq_routing_table(acpi_handle device,
				       struct acpi_buffer *ret_buffer)
{
	uacpi_pci_routing_table *uprt;
	struct acpi_pci_routing_table *entry;
	uacpi_status ust;
	size_t size = 0;
	u32 i;
	u8 *p;

	if (!ret_buffer)
		return AE_BAD_PARAMETER;

	ust = uacpi_get_pci_routing_table(uacpi_node_from_handle(device),
					  &uprt);
	if (ust != UACPI_STATUS_OK)
		return uacpi_to_acpi_status(ust);

	/* Size pass: each entry + its source path string, plus a null terminator. */
	for (i = 0; i < uprt->num_entries; i++) {
		const char *path = NULL;
		size_t slen = 0;

		if (uprt->entries[i].source) {
			path = uacpi_namespace_node_generate_absolute_path(
				uprt->entries[i].source);
			if (path)
				slen = strlen(path);
			uacpi_free_absolute_path(path);
		}
		size += ALIGN(offsetof(struct acpi_pci_routing_table, source) +
			      slen + 1, 4);
	}
	size += sizeof(struct acpi_pci_routing_table); /* zero-length terminator */

	if (ret_buffer->length == ACPI_ALLOCATE_BUFFER) {
		ret_buffer->pointer = kzalloc(size, GFP_KERNEL);
		if (!ret_buffer->pointer) {
			uacpi_free_pci_routing_table(uprt);
			return AE_NO_MEMORY;
		}
		ret_buffer->length = size;
	} else if (ret_buffer->length < size) {
		ret_buffer->length = size;
		uacpi_free_pci_routing_table(uprt);
		return AE_BUFFER_OVERFLOW;
	}

	p = ret_buffer->pointer;
	for (i = 0; i < uprt->num_entries; i++) {
		const char *path = NULL;
		size_t slen = 0;

		entry = (struct acpi_pci_routing_table *)p;
		if (uprt->entries[i].source) {
			path = uacpi_namespace_node_generate_absolute_path(
				uprt->entries[i].source);
			if (path)
				slen = strlen(path);
		}
		entry->length = ALIGN(offsetof(struct acpi_pci_routing_table,
					       source) + slen + 1, 4);
		entry->pin = uprt->entries[i].pin;
		entry->address = uprt->entries[i].address;
		entry->source_index = uprt->entries[i].index;
		if (slen)
			memcpy(entry->source, path, slen);
		entry->source[slen] = '\0';
		uacpi_free_absolute_path(path);
		p += entry->length;
	}
	((struct acpi_pci_routing_table *)p)->length = 0;

	uacpi_free_pci_routing_table(uprt);
	return AE_OK;
}

acpi_status acpi_resource_to_address64(struct acpi_resource *resource,
				       struct acpi_resource_address64 *out)
{
	if (!resource || !out)
		return AE_BAD_PARAMETER;

	switch (resource->type) {
	case ACPI_RESOURCE_TYPE_ADDRESS16: {
		struct acpi_resource_address16 *a = &resource->data.address16;

		memcpy(out, a, offsetof(struct acpi_resource_address,
					info) + sizeof(out->info));
		out->address.granularity = a->address.granularity;
		out->address.minimum = a->address.minimum;
		out->address.maximum = a->address.maximum;
		out->address.translation_offset = a->address.translation_offset;
		out->address.address_length = a->address.address_length;
		break;
	}
	case ACPI_RESOURCE_TYPE_ADDRESS32: {
		struct acpi_resource_address32 *a = &resource->data.address32;

		memcpy(out, a, offsetof(struct acpi_resource_address,
					info) + sizeof(out->info));
		out->address.granularity = a->address.granularity;
		out->address.minimum = a->address.minimum;
		out->address.maximum = a->address.maximum;
		out->address.translation_offset = a->address.translation_offset;
		out->address.address_length = a->address.address_length;
		break;
	}
	case ACPI_RESOURCE_TYPE_ADDRESS64:
		memcpy(out, &resource->data.address64, sizeof(*out));
		break;
	default:
		return AE_BAD_PARAMETER;
	}
	return AE_OK;
}

acpi_status acpi_buffer_to_resource(u8 *aml_buffer, u16 aml_buffer_length,
				    struct acpi_resource **resource_ptr)
{
	uacpi_data_view view;
	uacpi_resource *ures;
	struct acpi_resource *out;
	uacpi_status ust;
	u32 len;

	if (!aml_buffer || !resource_ptr)
		return AE_BAD_PARAMETER;

	view.bytes = aml_buffer;
	view.length = aml_buffer_length;

	ust = uacpi_get_resource_from_buffer(view, &ures);
	if (ust != UACPI_STATUS_OK)
		return uacpi_to_acpi_status(ust);

	len = conv(ures, NULL);
	if (!len) {
		uacpi_free_resource(ures);
		return AE_SUPPORT;
	}

	out = kzalloc(len, GFP_KERNEL);
	if (!out) {
		uacpi_free_resource(ures);
		return AE_NO_MEMORY;
	}

	conv(ures, out);
	uacpi_free_resource(ures);
	*resource_ptr = out;
	return AE_OK;
}

acpi_status acpi_get_vendor_resource(acpi_handle device, char *name,
				     struct acpi_vendor_uuid *uuid,
				     struct acpi_buffer *ret_buffer)
{
	uacpi_resources *ures;
	uacpi_resource *u;
	uacpi_status ust;
	acpi_status status = AE_NOT_FOUND;
	u8 *cur, *end;

	if (!name || !uuid || !ret_buffer)
		return AE_BAD_PARAMETER;

	ust = uacpi_get_device_resources(uacpi_node_from_handle(device), name,
					 &ures);
	if (ust != UACPI_STATUS_OK)
		return uacpi_to_acpi_status(ust);

	cur = (u8 *)ures->entries;
	end = cur + ures->length;
	while (cur < end) {
		u = (uacpi_resource *)cur;
		if (u->type == UACPI_RESOURCE_TYPE_END_TAG)
			break;
		if (u->type == UACPI_RESOURCE_TYPE_VENDOR_LARGE &&
		    u->vendor_typed.sub_type == uuid->subtype &&
		    !memcmp(u->vendor_typed.uuid, uuid->data, ACPI_UUID_LENGTH)) {
			u32 len = conv(u, NULL);

			if (ret_buffer->length == ACPI_ALLOCATE_BUFFER) {
				ret_buffer->pointer = kzalloc(len, GFP_KERNEL);
				if (!ret_buffer->pointer) {
					status = AE_NO_MEMORY;
					break;
				}
				ret_buffer->length = len;
			} else if (ret_buffer->length < len) {
				ret_buffer->length = len;
				status = AE_BUFFER_OVERFLOW;
				break;
			}
			conv(u, ret_buffer->pointer);
			status = AE_OK;
			break;
		}
		cur += u->length;
	}

	uacpi_free_resources(ures);
	return status;
}
