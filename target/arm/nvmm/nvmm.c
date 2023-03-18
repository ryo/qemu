/*
 * QEMU NetBSD Virtual Machine Monitor (NVMM) accelerator support for AARCH64
 *
 * Copyright (c) 2023 Ryo Shimizu <ryo@nerv.org>
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
#include "sysemu/cpus.h"
#include "sysemu/runstate.h"
#include "qemu/main-loop.h"
#include "qemu/error-report.h"
#include "qapi/error.h"
#include "qemu/queue.h"
#include "migration/blocker.h"
#include "strings.h"

struct qemu_vcpu {
    struct nvmm_vcpu vcpu;
    bool stop;
};

void
nvmm_set_registers(CPUState *cpu)
{
    CPUArchState *env = cpu->env_ptr;
    struct nvmm_machine *mach = get_nvmm_mach();
    struct qemu_vcpu *qcpu = nvmm_get_qemu_vcpu(cpu);
    struct nvmm_vcpu *vcpu = &qcpu->vcpu;
    struct nvmm_aarch64_state *state = vcpu->state;
    int i, ret;

    assert(cpu_is_stopped(cpu) || qemu_cpu_is_self(cpu));

    /* GPRs. */
    for (i = 0; i < 32; i++) {
        state->gprs[NVMM_AARCH64_GPR_X0 + i ] = env->xregs[i];
    }

    /* SPRs. */
    state->sprs[NVMM_AARCH64_SPR_PC] = env->pc;
    //XXXXXXXXXXXXXXXXX more sysregs

    ret = nvmm_vcpu_setstate(mach, vcpu, NVMM_AARCH64_STATE_ALL);
    if (ret == -1) {
        error_report("NVMM: Failed to set virtual processor context,"
            " error=%d", errno);
    }
}

void
nvmm_get_registers(CPUState *cpu)
{
    CPUArchState *env = cpu->env_ptr;
    struct nvmm_machine *mach = get_nvmm_mach();
    struct qemu_vcpu *qcpu = nvmm_get_qemu_vcpu(cpu);
    struct nvmm_vcpu *vcpu = &qcpu->vcpu;
    struct nvmm_aarch64_state *state = vcpu->state;
    int i, ret;

    assert(cpu_is_stopped(cpu) || qemu_cpu_is_self(cpu));

    ret = nvmm_vcpu_getstate(mach, vcpu, NVMM_AARCH64_STATE_ALL);
    if (ret == -1) {
        error_report("NVMM: Failed to get virtual processor context,"
            " error=%d", errno);
    }

    /* GPRs. */
    for (i = 0; i < 32; i++) {
        env->xregs[i] = state->gprs[NVMM_AARCH64_GPR_X0 + i ];
    }

    /* SPRs. */
    env->pc = state->sprs[NVMM_AARCH64_SPR_PC];
    //XXXXXXXXXXXXXXXXX more sysregs

}

static void
nvmm_vcpu_pre_run(CPUState *cpu)
{
	//XXXXXXXXXX
}

static void
nvmm_vcpu_post_run(CPUState *cpu, struct nvmm_vcpu_exit *exit)
{
	//XXXXXXXXXX
}

static int
nvmm_handle_halted(struct nvmm_machine *mach, CPUState *cpu,
    struct nvmm_vcpu_exit *exit)
{
	fprintf(stderr, "%s:%d: XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX\n", __func__, __LINE__);

	return 0;
}


int
nvmm_vcpu_loop(CPUState *cpu)
{
//    CPUArchState *env = cpu->env_ptr;
    struct nvmm_machine *mach = get_nvmm_mach();
    struct qemu_vcpu *qcpu = nvmm_get_qemu_vcpu(cpu);
    struct nvmm_vcpu *vcpu = &qcpu->vcpu;
    struct nvmm_vcpu_exit *exit = vcpu->exit;
    int ret;


    //XXXXXXXXXX: nvmm cannot send multiple events at the same time?
    if (cpu->interrupt_request & CPU_INTERRUPT_FIQ) {
        vcpu->event->type = NVMM_VCPU_EVENT_FIQ;
        nvmm_vcpu_inject(mach, vcpu);
    }
    if (cpu->interrupt_request & CPU_INTERRUPT_HARD) {
        vcpu->event->type = NVMM_VCPU_EVENT_IRQ;
        nvmm_vcpu_inject(mach, vcpu);
    }

    if (cpu->halted) {
        return EXCP_HLT;
    }

    qemu_mutex_unlock_iothread();
    cpu_exec_start(cpu);

    /*
     * Inner VCPU loop.
     */
    do {
        if (cpu->vcpu_dirty) {
            nvmm_set_registers(cpu);
            cpu->vcpu_dirty = false;
        }

        if (qcpu->stop) {
            cpu->exception_index = EXCP_INTERRUPT;
            qcpu->stop = false;
            ret = 1;
            break;
        }

        nvmm_vcpu_pre_run(cpu);

        /* Read exit_request before the kernel reads the immediate exit flag */
        smp_rmb();
        ret = nvmm_vcpu_run(mach, vcpu);
        if (ret == -1) {
            error_report("NVMM: Failed to exec a virtual processor,"
                " error=%d", errno);
            break;
        }

        nvmm_vcpu_post_run(cpu, exit);

        switch (exit->reason) {
        case NVMM_VCPU_EXIT_NONE:
            break;
        case NVMM_VCPU_EXIT_STOPPED:
            /*
             * The kernel cleared the immediate exit flag; cpu->exit_request
             * must be cleared after
             */
            smp_wmb();
            qcpu->stop = true;
            break;
        case NVMM_VCPU_EXIT_MEMORY:
            ret = nvmm_handle_mem(mach, vcpu);
            break;
        case NVMM_VCPU_EXIT_HALTED:
            ret = nvmm_handle_halted(mach, cpu, exit);
            break;
        case NVMM_VCPU_EXIT_SHUTDOWN:
            qemu_system_reset_request(SHUTDOWN_CAUSE_GUEST_RESET);
            cpu->exception_index = EXCP_INTERRUPT;
            ret = 1;
            break;
//        case NVMM_VCPU_EXIT_MSR:
//            ret = nvmm_handle_rdmsr(mach, cpu, exit);
//            break;
//        case NVMM_VCPU_EXIT_MSR:
//            ret = nvmm_handle_wrmsr(mach, cpu, exit);
//            break;
        default:
            error_report("NVMM: Unexpected VM exit code"
                " 0x%lx [hw=0x%lx, esr=0x%lx]",
                exit->reason, exit->u.inv.hwcode, exit->esr);
            nvmm_get_registers(cpu);
            qemu_mutex_lock_iothread();
            qemu_system_guest_panicked(cpu_get_crash_info(cpu));
            qemu_mutex_unlock_iothread();
            ret = -1;
            break;
        }
    } while (ret == 0);

    cpu_exec_end(cpu);
    qemu_mutex_lock_iothread();

    qatomic_set(&cpu->exit_request, false);

    return ret < 0;
}

/* -------------------------------------------------------------------------- */

static Error *nvmm_migration_blocker;

/*
 * The nvmm_vcpu_stop() mechanism breaks races between entering the VMM
 * and another thread signaling the vCPU thread to exit.
 */

void
nvmm_ipi_signal(int sigcpu)
{
    if (current_cpu) {
        struct qemu_vcpu *qcpu = nvmm_get_qemu_vcpu(current_cpu);
        struct nvmm_vcpu *vcpu = &qcpu->vcpu;
        nvmm_vcpu_stop(vcpu);
    }
}

void
nvmm_init_cpu_signals(void)
{
    struct sigaction sigact;
    sigset_t set;

    /* Install the IPI handler. */
    memset(&sigact, 0, sizeof(sigact));
    sigact.sa_handler = nvmm_ipi_signal;
    sigaction(SIG_IPI, &sigact, NULL);

    /* Allow IPIs on the current thread. */
    sigprocmask(SIG_BLOCK, NULL, &set);
    sigdelset(&set, SIG_IPI);
    pthread_sigmask(SIG_SETMASK, &set, NULL);
}

int
nvmm_init_vcpu(CPUState *cpu)
{
    struct nvmm_machine *mach = get_nvmm_mach();
//    struct nvmm_capability *cap = get_nvmm_cap();
//    struct nvmm_vcpu_conf_cpuid cpuid;
    Error *local_error = NULL;
    struct qemu_vcpu *qcpu;
    int ret, err;

    nvmm_init_cpu_signals();

    if (nvmm_migration_blocker == NULL) {
        error_setg(&nvmm_migration_blocker,
            "NVMM: Migration not supported");

        if (migrate_add_blocker(nvmm_migration_blocker, &local_error) < 0) {
            error_report_err(local_error);
            error_free(nvmm_migration_blocker);
            return -EINVAL;
        }
    }

    qcpu = g_malloc0(sizeof(*qcpu));
    if (qcpu == NULL) {
        error_report("NVMM: Failed to allocate VCPU context.");
        return -ENOMEM;
    }

    ret = nvmm_vcpu_create(mach, cpu->cpu_index, &qcpu->vcpu);
    if (ret == -1) {
        err = errno;
        error_report("NVMM: Failed to create a virtual processor,"
            " error=%d", err);
        g_free(qcpu);
        return -err;
    }

#if 0
XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX
// setup MPIDR_EL1
    memset(&cpuid, 0, sizeof(cpuid));
    cpuid.mask = 1;
    cpuid.leaf = 0x00000001;
    cpuid.u.mask.set.edx = CPUID_MCE | CPUID_MCA | CPUID_MTRR;
    ret = nvmm_vcpu_configure(mach, &qcpu->vcpu, NVMM_VCPU_CONF_CPUID,
        &cpuid);
    if (ret == -1) {
        err = errno;
        error_report("NVMM: Failed to configure a virtual processor,"
            " error=%d", err);
        g_free(qcpu);
        return -err;
    }
#endif

    ret = nvmm_vcpu_configure(mach, &qcpu->vcpu, NVMM_VCPU_CONF_CALLBACKS,
        &nvmm_callbacks);
    if (ret == -1) {
        err = errno;
        error_report("NVMM: Failed to configure a virtual processor,"
            " error=%d", err);
        g_free(qcpu);
        return -err;
    }

    cpu->vcpu_dirty = true;
    cpu->hax_vcpu = (struct hax_vcpu_state *)qcpu;

    return 0;
}

void
nvmm_destroy_vcpu(CPUState *cpu)
{
    struct nvmm_machine *mach = get_nvmm_mach();
    struct qemu_vcpu *qcpu = nvmm_get_qemu_vcpu(cpu);

    nvmm_vcpu_destroy(mach, &qcpu->vcpu);
    g_free(cpu->hax_vcpu);
}
