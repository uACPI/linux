// SPDX-License-Identifier: GPL-2.0
/*
 * uACPI kernel API glue for Linux.
 *
 * Implements the host interface required by the uACPI library
 * (uacpi/kernel_api.h) in terms of Linux primitives. Where the existing
 * ACPI OS layer (drivers/acpi/osl.c) already provides a battle-tested
 * implementation (physical memory mapping, RSDP discovery) it is reused.
 */

#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/mm.h>
#include <linux/io.h>
#include <linux/delay.h>
#include <linux/sched.h>
#include <linux/semaphore.h>
#include <linux/spinlock.h>
#include <linux/interrupt.h>
#include <linux/workqueue.h>
#include <linux/ktime.h>
#include <linux/sched/clock.h>
#include <linux/pci.h>
#include <linux/acpi.h>
#include <acpi/acpi_io.h>

#include <uacpi/kernel_api.h>

/* Only ever called during init, where acpi_os_get_root_pointer (__init) lives. */
uacpi_status __ref uacpi_kernel_get_rsdp(uacpi_phys_addr *out_rsdp_address)
{
	acpi_physical_address pa = acpi_os_get_root_pointer();

	if (!pa)
		return UACPI_STATUS_NOT_FOUND;

	*out_rsdp_address = pa;
	return UACPI_STATUS_OK;
}

void *uacpi_kernel_map(uacpi_phys_addr addr, uacpi_size len)
{
	void *virt = acpi_os_map_iomem(addr, len);

	/*
	 * uACPI's failure sentinel is UACPI_MAP_FAILED ((void *)-1), not NULL.
	 * Returning NULL here would pass uACPI's "== UACPI_MAP_FAILED" check and
	 * lead to a NULL dereference (e.g. in uacpi_verify_table_checksum). This
	 * matters on real firmware where early mappings of large tables can fail
	 * (the early_memremap window is only NR_FIX_BTMAPS pages).
	 */
	return virt ? virt : UACPI_MAP_FAILED;
}

void uacpi_kernel_unmap(void *addr, uacpi_size len)
{
	acpi_os_unmap_iomem((void __iomem *)addr, len);
}

void uacpi_kernel_log(uacpi_log_level level, const uacpi_char *str)
{
	const char *kern;

	switch (level) {
	case UACPI_LOG_ERROR:
		kern = KERN_ERR;
		break;
	case UACPI_LOG_WARN:
		kern = KERN_WARNING;
		break;
	case UACPI_LOG_INFO:
		kern = KERN_INFO;
		break;
	default:
		kern = KERN_DEBUG;
		break;
	}

	printk("%sACPI: %s", kern, str);
}

void *uacpi_kernel_alloc(uacpi_size size)
{
	return kmalloc(size, GFP_KERNEL);
}

void uacpi_kernel_free(void *mem)
{
	kfree(mem);
}

uacpi_u64 uacpi_kernel_get_nanoseconds_since_boot(void)
{
	/*
	 * Use sched_clock() rather than ktime_get_ns(): it is backed by the
	 * fine-grained TSC and is usable from very early boot, so consecutive
	 * reads always differ. ktime's resolution depends on the clocksource
	 * selected at the time, which is coarse during early ACPI bring-up and
	 * makes uACPI flag a "poor time source precision" warning.
	 */
	return sched_clock();
}

void uacpi_kernel_stall(uacpi_u8 usec)
{
	udelay(usec);
}

void uacpi_kernel_sleep(uacpi_u64 msec)
{
	msleep(msec);
}

uacpi_thread_id uacpi_kernel_get_thread_id(void)
{
	return (uacpi_thread_id)(uintptr_t)current;
}

uacpi_interrupt_state uacpi_kernel_disable_interrupts(void)
{
	unsigned long flags;

	local_irq_save(flags);
	return flags;
}

void uacpi_kernel_restore_interrupts(uacpi_interrupt_state state)
{
	local_irq_restore(state);
}

uacpi_handle uacpi_kernel_create_mutex(void)
{
	struct semaphore *sem = kmalloc(sizeof(*sem), GFP_KERNEL);

	if (sem)
		sema_init(sem, 1);
	return sem;
}

void uacpi_kernel_free_mutex(uacpi_handle handle)
{
	kfree(handle);
}

uacpi_status uacpi_kernel_acquire_mutex(uacpi_handle handle, uacpi_u16 timeout)
{
	struct semaphore *sem = handle;

	if (timeout == 0)
		return down_trylock(sem) ? UACPI_STATUS_TIMEOUT : UACPI_STATUS_OK;

	if (timeout == 0xFFFF) {
		down(sem);
		return UACPI_STATUS_OK;
	}

	if (down_timeout(sem, msecs_to_jiffies(timeout)))
		return UACPI_STATUS_TIMEOUT;
	return UACPI_STATUS_OK;
}

void uacpi_kernel_release_mutex(uacpi_handle handle)
{
	up(handle);
}

struct uacpi_event {
	struct semaphore sem;
};

uacpi_handle uacpi_kernel_create_event(void)
{
	struct uacpi_event *evt = kmalloc(sizeof(*evt), GFP_KERNEL);

	if (evt)
		sema_init(&evt->sem, 0);
	return evt;
}

void uacpi_kernel_free_event(uacpi_handle handle)
{
	kfree(handle);
}

uacpi_bool uacpi_kernel_wait_for_event(uacpi_handle handle, uacpi_u16 timeout)
{
	struct uacpi_event *evt = handle;

	if (timeout == 0xFFFF) {
		down(&evt->sem);
		return UACPI_TRUE;
	}

	return down_timeout(&evt->sem, msecs_to_jiffies(timeout)) ?
		UACPI_FALSE : UACPI_TRUE;
}

void uacpi_kernel_signal_event(uacpi_handle handle)
{
	struct uacpi_event *evt = handle;

	up(&evt->sem);
}

void uacpi_kernel_reset_event(uacpi_handle handle)
{
	struct uacpi_event *evt = handle;

	while (down_trylock(&evt->sem) == 0)
		;
}

uacpi_handle uacpi_kernel_create_spinlock(void)
{
	spinlock_t *lock = kmalloc(sizeof(*lock), GFP_KERNEL);

	if (lock)
		spin_lock_init(lock);
	return lock;
}

void uacpi_kernel_free_spinlock(uacpi_handle handle)
{
	kfree(handle);
}

uacpi_cpu_flags uacpi_kernel_lock_spinlock(uacpi_handle handle)
{
	unsigned long flags;

	spin_lock_irqsave((spinlock_t *)handle, flags);
	return flags;
}

void uacpi_kernel_unlock_spinlock(uacpi_handle handle, uacpi_cpu_flags flags)
{
	spin_unlock_irqrestore((spinlock_t *)handle, flags);
}


struct uacpi_io_range {
	uacpi_io_addr base;
	uacpi_size len;
};

uacpi_status uacpi_kernel_io_map(uacpi_io_addr base, uacpi_size len,
				 uacpi_handle *out_handle)
{
	struct uacpi_io_range *r = kmalloc(sizeof(*r), GFP_KERNEL);

	if (!r)
		return UACPI_STATUS_OUT_OF_MEMORY;

	r->base = base;
	r->len = len;
	*out_handle = r;
	return UACPI_STATUS_OK;
}

void uacpi_kernel_io_unmap(uacpi_handle handle)
{
	kfree(handle);
}

uacpi_status uacpi_kernel_io_read8(uacpi_handle h, uacpi_size offset,
				   uacpi_u8 *out)
{
	struct uacpi_io_range *r = h;

	*out = inb(r->base + offset);
	return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_io_read16(uacpi_handle h, uacpi_size offset,
				    uacpi_u16 *out)
{
	struct uacpi_io_range *r = h;

	*out = inw(r->base + offset);
	return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_io_read32(uacpi_handle h, uacpi_size offset,
				    uacpi_u32 *out)
{
	struct uacpi_io_range *r = h;

	*out = inl(r->base + offset);
	return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_io_write8(uacpi_handle h, uacpi_size offset,
				    uacpi_u8 val)
{
	struct uacpi_io_range *r = h;

	outb(val, r->base + offset);
	return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_io_write16(uacpi_handle h, uacpi_size offset,
				     uacpi_u16 val)
{
	struct uacpi_io_range *r = h;

	outw(val, r->base + offset);
	return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_io_write32(uacpi_handle h, uacpi_size offset,
				     uacpi_u32 val)
{
	struct uacpi_io_range *r = h;

	outl(val, r->base + offset);
	return UACPI_STATUS_OK;
}

struct uacpi_pci_dev {
	uacpi_pci_address addr;
};

uacpi_status uacpi_kernel_pci_device_open(uacpi_pci_address address,
					  uacpi_handle *out_handle)
{
	struct uacpi_pci_dev *dev = kmalloc(sizeof(*dev), GFP_KERNEL);

	if (!dev)
		return UACPI_STATUS_OUT_OF_MEMORY;

	dev->addr = address;
	*out_handle = dev;
	return UACPI_STATUS_OK;
}

void uacpi_kernel_pci_device_close(uacpi_handle handle)
{
	kfree(handle);
}

static uacpi_status uacpi_pci_read(uacpi_handle handle, uacpi_size offset,
				   int size, u32 *value)
{
	struct uacpi_pci_dev *dev = handle;

	if (raw_pci_read(dev->addr.segment, dev->addr.bus,
			 PCI_DEVFN(dev->addr.device, dev->addr.function),
			 offset, size, value))
		return UACPI_STATUS_INTERNAL_ERROR;
	return UACPI_STATUS_OK;
}

static uacpi_status uacpi_pci_write(uacpi_handle handle, uacpi_size offset,
				    int size, u32 value)
{
	struct uacpi_pci_dev *dev = handle;

	if (raw_pci_write(dev->addr.segment, dev->addr.bus,
			  PCI_DEVFN(dev->addr.device, dev->addr.function),
			  offset, size, value))
		return UACPI_STATUS_INTERNAL_ERROR;
	return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_pci_read8(uacpi_handle h, uacpi_size off, uacpi_u8 *v)
{
	u32 tmp;
	uacpi_status st = uacpi_pci_read(h, off, 1, &tmp);

	*v = tmp;
	return st;
}

uacpi_status uacpi_kernel_pci_read16(uacpi_handle h, uacpi_size off, uacpi_u16 *v)
{
	u32 tmp;
	uacpi_status st = uacpi_pci_read(h, off, 2, &tmp);

	*v = tmp;
	return st;
}

uacpi_status uacpi_kernel_pci_read32(uacpi_handle h, uacpi_size off, uacpi_u32 *v)
{
	return uacpi_pci_read(h, off, 4, v);
}

uacpi_status uacpi_kernel_pci_write8(uacpi_handle h, uacpi_size off, uacpi_u8 v)
{
	return uacpi_pci_write(h, off, 1, v);
}

uacpi_status uacpi_kernel_pci_write16(uacpi_handle h, uacpi_size off, uacpi_u16 v)
{
	return uacpi_pci_write(h, off, 2, v);
}

uacpi_status uacpi_kernel_pci_write32(uacpi_handle h, uacpi_size off, uacpi_u32 v)
{
	return uacpi_pci_write(h, off, 4, v);
}

struct uacpi_irq_ctx {
	uacpi_interrupt_handler handler;
	uacpi_handle ctx;
	u32 irq;
};

static irqreturn_t uacpi_irq_trampoline(int irq, void *dev)
{
	struct uacpi_irq_ctx *c = dev;

	return c->handler(c->ctx) == UACPI_INTERRUPT_HANDLED ?
		IRQ_HANDLED : IRQ_NONE;
}

uacpi_status uacpi_kernel_install_interrupt_handler(u32 irq,
		uacpi_interrupt_handler handler, uacpi_handle ctx,
		uacpi_handle *out_irq_handle)
{
	struct uacpi_irq_ctx *c = kmalloc(sizeof(*c), GFP_KERNEL);

	if (!c)
		return UACPI_STATUS_OUT_OF_MEMORY;

	c->handler = handler;
	c->ctx = ctx;
	c->irq = irq;

	if (request_irq(irq, uacpi_irq_trampoline, IRQF_SHARED, "uacpi", c)) {
		kfree(c);
		return UACPI_STATUS_INTERNAL_ERROR;
	}

	*out_irq_handle = c;
	return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_uninstall_interrupt_handler(
		uacpi_interrupt_handler handler, uacpi_handle irq_handle)
{
	struct uacpi_irq_ctx *c = irq_handle;

	free_irq(c->irq, c);
	kfree(c);
	return UACPI_STATUS_OK;
}


static struct workqueue_struct *uacpi_wq;
static struct workqueue_struct *uacpi_gpe_wq;

struct uacpi_work {
	struct work_struct work;
	uacpi_work_handler handler;
	uacpi_handle ctx;
};

static void uacpi_work_fn(struct work_struct *work)
{
	struct uacpi_work *w = container_of(work, struct uacpi_work, work);

	w->handler(w->ctx);
	kfree(w);
}

uacpi_status uacpi_kernel_schedule_work(uacpi_work_type type,
		uacpi_work_handler handler, uacpi_handle ctx)
{
	struct uacpi_work *w;

	if (!uacpi_wq) {
		uacpi_wq = alloc_ordered_workqueue("kuacpi", 0);
		uacpi_gpe_wq = alloc_ordered_workqueue("kuacpi_gpe", 0);
		if (!uacpi_wq || !uacpi_gpe_wq)
			return UACPI_STATUS_INTERNAL_ERROR;
	}

	w = kmalloc(sizeof(*w), GFP_ATOMIC);
	if (!w)
		return UACPI_STATUS_OUT_OF_MEMORY;

	INIT_WORK(&w->work, uacpi_work_fn);
	w->handler = handler;
	w->ctx = ctx;

	if (type == UACPI_WORK_GPE_EXECUTION)
		queue_work_on(0, uacpi_gpe_wq, &w->work);
	else
		queue_work(uacpi_wq, &w->work);

	return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_wait_for_work_completion(void)
{
	if (uacpi_gpe_wq)
		flush_workqueue(uacpi_gpe_wq);
	if (uacpi_wq)
		flush_workqueue(uacpi_wq);
	return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_handle_firmware_request(uacpi_firmware_request *req)
{
	switch (req->type) {
	case UACPI_FIRMWARE_REQUEST_TYPE_FATAL:
		pr_err("ACPI: firmware fatal request: type %u code %u arg %llu\n",
		       req->fatal.type, req->fatal.code,
		       (unsigned long long)req->fatal.arg);
		break;
	case UACPI_FIRMWARE_REQUEST_TYPE_BREAKPOINT:
		break;
	}
	return UACPI_STATUS_OK;
}
