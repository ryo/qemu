/*
 * Copyright (c) 2018-2019 Maxime Villard, All rights reserved.
 *
 * NetBSD Virtual Machine Monitor (NVMM) accelerator for QEMU.
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#include "qemu/osdep.h"
#include "cpu.h"
#include "exec/address-spaces.h"
#include "exec/ioport.h"
#include "qemu/accel.h"
#include "sysemu/nvmm.h"

#include <nvmm.h>

struct qemu_machine {
    struct nvmm_capability cap;
    struct nvmm_machine mach;
};

/* -------------------------------------------------------------------------- */

static bool nvmm_allowed;
struct qemu_machine qemu_mach;

struct qemu_vcpu *
nvmm_get_qemu_vcpu(CPUState *cpu)
{
    return (struct qemu_vcpu *)cpu->hax_vcpu;
}

struct nvmm_machine *
get_nvmm_mach(void)
{
    return &qemu_mach.mach;
}

struct nvmm_capability *
get_nvmm_cap(void)
{
    return &qemu_mach.cap;
}

/* -------------------------------------------------------------------------- */

#ifdef NVMM_VCPU_EXIT_IO
static void
nvmm_io_callback(struct nvmm_io *io)
{
    MemTxAttrs attrs = { 0 };
    int ret;

    ret = address_space_rw(&address_space_io, io->port, attrs, io->data,
        io->size, !io->in);
    if (ret != MEMTX_OK) {
        error_report("NVMM: I/O Transaction Failed "
            "[%s, port=%u, size=%zu]", (io->in ? "in" : "out"),
            io->port, io->size);
    }

    /* Needed, otherwise infinite loop. */
    current_cpu->vcpu_dirty = false;
}
#endif /* NVMM_VCPU_EXIT_IO */

static void
nvmm_mem_callback(struct nvmm_mem *mem)
{
    cpu_physical_memory_rw(mem->gpa, mem->data, mem->size, mem->write);

    /* Needed, otherwise infinite loop. */
    current_cpu->vcpu_dirty = false;
}

struct nvmm_assist_callbacks nvmm_callbacks = {
#ifdef NVMM_VCPU_EXIT_IO
    .io = nvmm_io_callback,
#endif
    .mem = nvmm_mem_callback
};

/* -------------------------------------------------------------------------- */

int
nvmm_handle_mem(struct nvmm_machine *mach, struct nvmm_vcpu *vcpu)
{
    int ret;

    ret = nvmm_assist_mem(mach, vcpu);
    if (ret == -1) {
        error_report("NVMM: Mem Assist Failed [gpa=%p]",
            (void *)vcpu->exit->u.mem.gpa);
    }

    return ret;
}

#ifdef NVMM_VCPU_EXIT_IO
int
nvmm_handle_io(struct nvmm_machine *mach, struct nvmm_vcpu *vcpu)
{
    int ret;

    ret = nvmm_assist_io(mach, vcpu);
    if (ret == -1) {
        error_report("NVMM: I/O Assist Failed [port=%d]",
            (int)vcpu->exit->u.io.port);
    }

    return ret;
}
#endif

/* -------------------------------------------------------------------------- */

static void
do_nvmm_cpu_synchronize_state(CPUState *cpu, run_on_cpu_data arg)
{
    nvmm_get_registers(cpu);
    cpu->vcpu_dirty = true;
}

static void
do_nvmm_cpu_synchronize_post_reset(CPUState *cpu, run_on_cpu_data arg)
{
    nvmm_set_registers(cpu);
    cpu->vcpu_dirty = false;
}

static void
do_nvmm_cpu_synchronize_post_init(CPUState *cpu, run_on_cpu_data arg)
{
    nvmm_set_registers(cpu);
    cpu->vcpu_dirty = false;
}

static void
do_nvmm_cpu_synchronize_pre_loadvm(CPUState *cpu, run_on_cpu_data arg)
{
    cpu->vcpu_dirty = true;
}

void nvmm_cpu_synchronize_state(CPUState *cpu)
{
    if (!cpu->vcpu_dirty) {
        run_on_cpu(cpu, do_nvmm_cpu_synchronize_state, RUN_ON_CPU_NULL);
    }
}

void nvmm_cpu_synchronize_post_reset(CPUState *cpu)
{
    run_on_cpu(cpu, do_nvmm_cpu_synchronize_post_reset, RUN_ON_CPU_NULL);
}

void nvmm_cpu_synchronize_post_init(CPUState *cpu)
{
    run_on_cpu(cpu, do_nvmm_cpu_synchronize_post_init, RUN_ON_CPU_NULL);
}

void nvmm_cpu_synchronize_pre_loadvm(CPUState *cpu)
{
    run_on_cpu(cpu, do_nvmm_cpu_synchronize_pre_loadvm, RUN_ON_CPU_NULL);
}

/* -------------------------------------------------------------------------- */

int
nvmm_vcpu_exec(CPUState *cpu)
{
    int ret, fatal;

    while (1) {
        if (cpu->exception_index >= EXCP_INTERRUPT) {
            ret = cpu->exception_index;
            cpu->exception_index = -1;
            break;
        }

        fatal = nvmm_vcpu_loop(cpu);

        if (fatal) {
            error_report("NVMM: Failed to execute a VCPU.");
            abort();
        }
    }

    return ret;
}

static void
nvmm_update_mapping(hwaddr start_pa, ram_addr_t size, uintptr_t hva,
    bool add, bool rom, const char *name)
{
    struct nvmm_machine *mach = get_nvmm_mach();
    int ret, prot;

    if (add) {
        prot = PROT_READ | PROT_EXEC;
        if (!rom) {
            prot |= PROT_WRITE;
        }
        ret = nvmm_gpa_map(mach, hva, start_pa, size, prot);
    } else {
        ret = nvmm_gpa_unmap(mach, hva, start_pa, size);
    }

    if (ret == -1) {
        error_report("NVMM: Failed to %s GPA range '%s' PA:%p, "
            "Size:%p bytes, HostVA:%p, error=%d",
            (add ? "map" : "unmap"), name, (void *)(uintptr_t)start_pa,
            (void *)size, (void *)hva, errno);
    }
}

static void
nvmm_process_section(MemoryRegionSection *section, int add)
{
    MemoryRegion *mr = section->mr;
    hwaddr start_pa = section->offset_within_address_space;
    ram_addr_t size = int128_get64(section->size);
    unsigned int delta;
    uintptr_t hva;

    if (!memory_region_is_ram(mr)) {
        return;
    }

    /* Adjust start_pa and size so that they are page-aligned. */
    delta = qemu_real_host_page_size() - (start_pa & ~qemu_real_host_page_mask());
    delta &= ~qemu_real_host_page_mask();
    if (delta > size) {
        return;
    }
    start_pa += delta;
    size -= delta;
    size &= qemu_real_host_page_mask();
    if (!size || (start_pa & ~qemu_real_host_page_mask())) {
        return;
    }

    hva = (uintptr_t)memory_region_get_ram_ptr(mr) +
        section->offset_within_region + delta;

    nvmm_update_mapping(start_pa, size, hva, add,
        memory_region_is_rom(mr), mr->name);
}

static void
nvmm_region_add(MemoryListener *listener, MemoryRegionSection *section)
{
    memory_region_ref(section->mr);
    nvmm_process_section(section, 1);
}

static void
nvmm_region_del(MemoryListener *listener, MemoryRegionSection *section)
{
    nvmm_process_section(section, 0);
    memory_region_unref(section->mr);
}

static void
nvmm_transaction_begin(MemoryListener *listener)
{
    /* nothing */
}

static void
nvmm_transaction_commit(MemoryListener *listener)
{
    /* nothing */
}

static void
nvmm_log_sync(MemoryListener *listener, MemoryRegionSection *section)
{
    MemoryRegion *mr = section->mr;

    if (!memory_region_is_ram(mr)) {
        return;
    }

    memory_region_set_dirty(mr, 0, int128_get64(section->size));
}

static MemoryListener nvmm_memory_listener = {
    .name = "nvmm",
    .begin = nvmm_transaction_begin,
    .commit = nvmm_transaction_commit,
    .region_add = nvmm_region_add,
    .region_del = nvmm_region_del,
    .log_sync = nvmm_log_sync,
    .priority = 10,
};

static void
nvmm_ram_block_added(RAMBlockNotifier *n, void *host, size_t size,
                     size_t max_size)
{
    struct nvmm_machine *mach = get_nvmm_mach();
    uintptr_t hva = (uintptr_t)host;
    int ret;

    ret = nvmm_hva_map(mach, hva, max_size);

    if (ret == -1) {
        error_report("NVMM: Failed to map HVA, HostVA:%p "
            "Size:%p bytes, error=%d",
            (void *)hva, (void *)size, errno);
    }
}

static struct RAMBlockNotifier nvmm_ram_notifier = {
    .ram_block_added = nvmm_ram_block_added
};

/* -------------------------------------------------------------------------- */

static int
nvmm_accel_init(MachineState *ms)
{
    int ret, err;

    ret = nvmm_init();
    if (ret == -1) {
        err = errno;
        error_report("NVMM: Initialization failed, error=%d", errno);
        return -err;
    }

    ret = nvmm_capability(&qemu_mach.cap);
    if (ret == -1) {
        err = errno;
        error_report("NVMM: Unable to fetch capability, error=%d", errno);
        return -err;
    }
    if (qemu_mach.cap.version < NVMM_KERN_VERSION) {
        error_report("NVMM: Unsupported version %u", qemu_mach.cap.version);
        return -EPROGMISMATCH;
    }
    if (qemu_mach.cap.state_size != sizeof(struct nvmm_vcpu_state)) {
        error_report("NVMM: Wrong state size %u", qemu_mach.cap.state_size);
        return -EPROGMISMATCH;
    }

    ret = nvmm_machine_create(&qemu_mach.mach);
    if (ret == -1) {
        err = errno;
        error_report("NVMM: Machine creation failed, error=%d", errno);
        return -err;
    }

    memory_listener_register(&nvmm_memory_listener, &address_space_memory);
    ram_block_notifier_add(&nvmm_ram_notifier);

    printf("NetBSD Virtual Machine Monitor accelerator is operational\n");
    return 0;
}

int
nvmm_enabled(void)
{
    return nvmm_allowed;
}

static void
nvmm_accel_class_init(ObjectClass *oc, void *data)
{
    AccelClass *ac = ACCEL_CLASS(oc);
    ac->name = "NVMM";
    ac->init_machine = nvmm_accel_init;
    ac->allowed = &nvmm_allowed;
}

static const TypeInfo nvmm_accel_type = {
    .name = ACCEL_CLASS_NAME("nvmm"),
    .parent = TYPE_ACCEL,
    .class_init = nvmm_accel_class_init,
};

static void
nvmm_type_init(void)
{
    type_register_static(&nvmm_accel_type);
}

type_init(nvmm_type_init);
