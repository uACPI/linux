# Linux ACPI on uACPI — compatibility shim

This directory replaces ACPICA (`drivers/acpi/acpica/`) with the
[uACPI](https://github.com/UltraOS/uACPI) library, without touching the rest of
the kernel. The Linux ACPI subsystem keeps calling the unchanged ACPICA
interface declared in `include/acpi/*` (`acpi_get_handle`, `acpi_evaluate_object`,
`acpi_walk_namespace`, `acpi_gbl_FADT`, …). The files here are a *shim* that:

1. implement that ACPICA C interface on top of uACPI's public API, and
2. implement uACPI's host interface (`uacpi/kernel_api.h`) on top of Linux.

No new ACPICA headers are introduced — only `include/acpi/*` (kept) plus the
uACPI headers under `uACPI/include/` are used.

Targets uACPI 5.0 (uses `uacpi_table_get_by_index()` for
`/sys/firmware/acpi/tables`).

## Selecting the implementation

`drivers/acpi/Kconfig` defines a choice:

* `CONFIG_ACPI_ACPICA` (default) — the in-tree ACPICA reference implementation.
* `CONFIG_ACPI_UACPI` — this shim.

`drivers/acpi/Makefile` builds `acpica/` or `uacpi/` accordingly:

```make
obj-$(CONFIG_ACPI_ACPICA)	+= acpica/
obj-$(CONFIG_ACPI_UACPI)	+= uacpi/
```

To build the uACPI variant:

```sh
./scripts/config -d ACPI_ACPICA -e ACPI_UACPI
make olddefconfig
make -j$(nproc)
```

> **Always validate with a clean build.** Stale `drivers/acpi/acpica/*.o` left
> on disk from a previous ACPICA build can satisfy the link and mask a broken
> shim — a dirty tree may "boot" on leftover ACPICA code. Confirm
> `ACPI: starting uACPI, version X.Y.Z` appears in `dmesg` to be sure the shim is
> actually live.

## Layout

| File | Responsibility |
|------|----------------|
| `glue.c` | uACPI host interface (`uacpi_kernel_*`): map/unmap, alloc, log, mutex/event/spinlock, IO/PCI, interrupts, deferred work, timers. |
| `shim_globals.c` | Defines the ACPICA globals (`acpi_gbl_*`, `acpi_dbg_level`, …) via the headers' `DEFINE_ACPI_GLOBALS` path, plus a couple of `acglobal.h`-only globals. |
| `shim_init.c` | Subsystem init state machine + `acpi_gbl_FADT` population. |
| `shim_tables.c` | Table access (`acpi_get_table`, `acpi_put_table`, install/load, handlers). |
| `shim_namespace.c` | Handles, names, walk, devices, attached data, `acpi_get_object_info`. |
| `shim_object.c` | Status translation, object marshalling, `acpi_evaluate_object`. |
| `shim_event.c` | GPEs, fixed events, notify/address-space handlers, sleep, registers, global lock. |
| `shim_resource.c` | Resource interfaces (`_CRS`/`_PRS`/`_SRS`/`_PRT`, walk, vendor) — full uACPI↔ACPICA `struct acpi_resource` translation. |
| `shim_misc.c` | Diagnostics, `_OSI` interfaces, timers, misc helpers. |
| `shim_exports.c` | `EXPORT_SYMBOL` for the public `acpi_*` interface (so modules can use it), mirroring ACPICA's `ACPI_EXPORT_SYMBOL` set. |
| `lib/*.c` | Thin wrappers that `#include` the uACPI sources from `uACPI/source/`. |
| `shim.h`, `uacpi_types.h` | Internal-only headers (not part of the ACPICA interface). |

## How the init flow maps

The Linux core drives ACPICA through a fixed sequence; the shim maps it onto
uACPI's flow (`shim_init.c`):

| ACPICA call (Linux core) | uACPI action |
|--------------------------|--------------|
| `acpi_initialize_tables()` (early, in `setup_arch`) | `uacpi_setup_early_table_access()` + populate `acpi_gbl_FADT` |
| `acpi_reallocate_root_table()` | no-op (uACPI owns table storage) |
| `acpi_initialize_subsystem()` | `uacpi_initialize(UACPI_FLAG_NO_ACPI_MODE)` |
| `acpi_enable_subsystem(~ACPI_NO_ACPI_ENABLE)` | `uacpi_enter_acpi_mode()` |
| `acpi_load_tables()` | `uacpi_namespace_load()` |
| `acpi_initialize_objects()` | `uacpi_namespace_initialize()` |
| `acpi_update_all_gpes()` | `uacpi_finalize_gpe_initialization()` |

## Type bridging notes

* `acpi_handle` is `void *`; uACPI namespace handles are `uacpi_namespace_node *`
  — they alias directly. `ACPI_ROOT_OBJECT`/`NULL` map to
  `uacpi_namespace_root()` (`uacpi_node_from_handle()` in `shim.h`).
* `acpi_status` (u32 `AE_*`) ↔ `uacpi_status` (enum) via
  `uacpi_to_acpi_status()` (`shim_object.c`).
* ACPI object types are spec-defined and **numerically identical** between
  ACPICA (`ACPI_TYPE_*`) and uACPI (`UACPI_OBJECT_*`), so no per-type table is
  needed.
* `struct acpi_table_fadt` (ACPICA) and `struct acpi_fadt` (uACPI) share the
  spec layout and are both 276 bytes — `acpi_gbl_FADT` is filled by a direct
  `memcpy` of uACPI's sanitized FADT.
* `acpi_evaluate_object()` output uses ACPICA's "flatten into one allocation"
  convention: `shim_object.c` does a two-pass size/serialize so the whole
  `union acpi_object` tree lives in a single `kmalloc` block freed by one
  `kfree`/`ACPI_FREE`.

## Hacks / non-obvious decisions (read before hacking)

These are the things that will bite you:

1. **`DEFINE_ACPI_GLOBALS`.** `shim_globals.c` defines `#define
   DEFINE_ACPI_GLOBALS` before `#include <linux/acpi.h>` so the *unchanged*
   ACPICA headers emit the global *definitions* (with canonical initializers) in
   exactly one TU — the same trick `acglobal.c` uses. A few globals live only in
   the internal `acglobal.h` (e.g. `acpi_gbl_xgpe{0,1}_block_logical_address`,
   written by `osl.c`); those are defined by hand in `shim_globals.c`.

2. **`-DUACPI_OVERRIDE_TYPES` + `uacpi_types.h`.** The kernel builds with
   `-nostdinc`, and mixing uACPI's default `<stdint.h>`/`<stdbool.h>` types with
   the kernel's own `int64_t`/`bool` causes conflicts. `uacpi_types.h` maps
   uACPI's base types onto kernel types; selected via the Makefile.

3. **`-DACPI_DEBUG_OUTPUT` for the shim TUs.** Must match the rest of the kernel
   (set under `CONFIG_ACPI_DEBUG`). Otherwise the ACPICA headers turn
   `acpi_debug_print` etc. into `static inline` stubs in the shim TUs while the
   rest of the kernel expects real out-of-line symbols → redefinition vs.
   undefined-reference mismatches.

4. **`<uacpi/event.h>` / `<uacpi/io.h>` pull in `<uacpi/acpi.h>`**, whose ACPI
   table structs (`acpi_madt_*`, `acpi_srat_*`, `acpi_resource_*`, `acpi_gas`,
   …) collide with the identically named ACPICA ones from `<linux/acpi.h>`. Any
   shim TU that includes `<linux/acpi.h>` must **not** include those uACPI
   headers. `shim_event.c` forward-declares the handful of uACPI event/GPE/GAS
   prototypes it needs instead. `registers.h`, `sleep.h`, `notify.h` are safe.

5. **`acpi_get_table()` runs before the slab allocator exists**
   (`early_acpi_boot_init` → `acpi_blacklisted`). Its ptr→index bookkeeping must
   be allocation-free — `shim_tables.c` uses a fixed static array, not
   `kmalloc`. Anything you add on the early-table path must not allocate.

6. **`acpi_get_table_by_index()` must return `AE_BAD_PARAMETER`** when out of
   range. Callers (`acpi_tables_sysfs_init`) loop incrementing the index and
   only break on `AE_BAD_PARAMETER`, treating `AE_NOT_FOUND` as
   "skip and continue" — returning the wrong code is an infinite loop.

7. **`_OSI` host features vs vendor strings.** uACPI pre-registers the ACPI
   OS-feature strings ("Module Device", "Processor Device", "3.0 _SCP
   Extensions", "3.0 Thermal Model", "Processor Aggregator Device") as *host
   features* that exist but are **disabled** by default and are toggled via
   `uacpi_{enable,disable}_host_interface(enum)` — not the vendor-string
   `uacpi_install_interface()` API (which would return `ALREADY_EXISTS` and
   leave `_OSI()` reporting them unsupported). `acpi_install_interface()` in
   `shim_misc.c` maps those names to the enum; everything else is treated as a
   vendor string. Without this, `_OSI("Module Device")` etc. wrongly return
   false and the core never prints `Added _OSI(...)`.

8. **uACPI library sources are compiled via `lib/*.c` wrappers** that `#include`
   `../../../../uACPI/source/<file>.c`. This keeps the upstream tree pristine
   (no edits) and avoids out-of-tree object paths kbuild dislikes. Upstream
   `uACPI/` is otherwise unmodified.

## Status / what's implemented

Working (verified booting in QEMU through full ACPI bring-up):

* Table subsystem, FADT, init/enable/sleep-prep flow, by-index table access
  (`/sys/firmware/acpi/tables/*` populated).
* Namespace: handles, names, parent/type, walk, `acpi_get_devices`,
  attached-data, `acpi_get_object_info` (`_HID/_UID/_CID/_CLS/_ADR/_SxD/_SxW`).
* Method evaluation with full `union acpi_object` marshalling.
* Notify handlers (per-node dispatcher), GPE + fixed-event handlers and
  enable/disable/clear/wake, sleep states (incl. S5 poweroff verified),
  bit/GAS register access, global lock.
* `_OSI` interface management, diagnostics, PM timer.
* **Resource subsystem** (`shim_resource.c`): full bidirectional translation
  between `uacpi_resource` and ACPICA's `struct acpi_resource` —
  `acpi_walk_resources`/`acpi_walk_resource_buffer`,
  `acpi_get_current_resources` (`_CRS`), `acpi_get_possible_resources`
  (`_PRS`), `acpi_set_current_resources` (`_SRS`),
  `acpi_get_irq_routing_table` (`_PRT`), `acpi_resource_to_address64`,
  `acpi_buffer_to_resource`, `acpi_get_vendor_resource`. PnP enumeration and
  PCI interrupt-link configuration work. Each converted entry is
  self-contained (pointed-to strings/pin-tables/vendor blobs copied inline).
  IRQ/extended-IRQ/DMA/IO/memory/address/vendor/generic-register/GPIO/serial
  bus types are converted; the `_SRS` reverse path covers the legacy ISA and
  interrupt-link types (GPIO/serial/pin/clock reverse conversion is not needed
  for `_SRS` and is not implemented).

* **Address-space handlers** (`shim_event.c`): custom spaces
  (EmbeddedController, SMBus, …) are bridged from ACPICA's
  `(function,address,width,value,contexts)` model to uACPI's op-based
  `uacpi_region_handler` (ATTACH→setup/ACTIVATE, DETACH→setup/DEACTIVATE,
  READ/WRITE→handler). `acpi_execute_reg_methods` maps to
  `uacpi_reg_all_opregions`. SystemMemory/SystemIO/PCI_Config remain uACPI
  built-ins.
* **Method arguments** marshal Integer/String/Buffer **and Package**
  (recursively) — required for `_DSM`, whose 4th argument is a package. Missing
  this made every `_DSM` evaluation fail with `AE_TYPE`.
* `acpi_decode_pld_buffer` decodes `_PLD` (device physical location).

**Requires a small uACPI fix** (`uacpi-power-resource-fix.patch`, applied to the
`uACPI/` tree): `uacpi_object_assign()` omits `UACPI_OBJECT_POWER_RESOURCE`, so
`uacpi_eval()` of any PowerResource returns `UACPI_STATUS_UNIMPLEMENTED`.
Without it, power resources are never enumerated ("New power resource" missing,
`_PR0`-based power management broken). Upstream-worthy.

**Known limitations (uACPI public API has no equivalent):**

* Package name references (`_PR0`, `_AL0`, `_PSL`, `_DSD`, …) come back from
  uACPI as AML-namepath string objects; the shim resolves them to
  `ACPI_TYPE_LOCAL_REFERENCE` with the target node handle (see
  `write_namepath_ref()` in `shim_object.c`). Genuine reference-typed results
  (e.g. a bare `RefOf` return) still get `reference.handle == NULL` — uACPI
  exposes no "node behind a live reference object" accessor — but these are rare
  and degrade gracefully. Integer/String/Buffer/Package/Processor/Power marshal
  fully.
* `acpi_dispatch_gpe()` (no in-tree callers) and `acpi_get_gpe_device()`
  (only `/sys/firmware/acpi/interrupts` GPE block naming) return 0 /
  `AE_NOT_FOUND` — uACPI has no dispatch-by-index or GPE-block-by-index API.
  `acpi_any_gpe_status_set()` / `acpi_any_fixed_event_status_set()` (used for
  suspend-to-idle wake-source detection and EC GPE polling) ARE implemented by
  iterating the FADT \_GPE blocks and fixed events via `uacpi_gpe_info()` /
  `uacpi_fixed_event_info()`; GPEs in non-FADT block devices aren't covered.
  S3 deep-suspend/resume is verified working.
* ACPI 6.2+ CSI-2 / PinFunction / PinConfig / PinGroup* / ClockInput resource
  descriptors are not converted by `shim_resource.c` (skipped). These are
  ARM/embedded-only and never appear on x86; everything else (IRQ, ext-IRQ,
  DMA, IO, memory, address, GPIO, I2C/SPI/UART, vendor, generic register) is.

**Stubbed — remaining niche gaps (not on any common path):**

* `acpi_unload_table`/`acpi_unload_parent_table` (dynamic SSDT unload),
  AML-mutex acquire-by-path (`acpi_acquire_mutex`), `acpi_install_method`,
  and `acpi_get_system_info`/`acpi_get_statistics` return `AE_SUPPORT`
  (no uACPI public equivalent / debug-only).

## Testing

```sh
qemu-system-x86_64 -kernel arch/x86/boot/bzImage \
  -append "console=ttyS0 earlyprintk=serial,ttyS0,115200 nokaslr panic=1" \
  -nographic -no-reboot -m 512
```

Expect `ACPI: starting uACPI`, table dump, `Interpreter enabled`,
`PCI Root Bridge`, and (with no disk) the normal rootfs panic. For debugging an
early hang, add `-s` and attach `gdb vmlinux -ex 'target remote :1234'`.
`earlyprintk=serial` is important: after the console handover, a hang with no
further printk leaves later messages unflushed, so the last serial line is not
necessarily the hang site.
