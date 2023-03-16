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
    uint8_t tpr;
    bool stop;

    /* Window-exiting for INTs/NMIs. */
    bool int_window_exit;
    bool nmi_window_exit;

    /* The guest is in an interrupt shadow (POP SS, etc). */
    bool int_shadow;
};

/* -------------------------------------------------------------------------- */

static void
nvmm_set_segment(struct nvmm_x64_state_seg *nseg, const SegmentCache *qseg)
{
    uint32_t attrib = qseg->flags;

    nseg->selector = qseg->selector;
    nseg->limit = qseg->limit;
    nseg->base = qseg->base;
    nseg->attrib.type = __SHIFTOUT(attrib, DESC_TYPE_MASK);
    nseg->attrib.s = __SHIFTOUT(attrib, DESC_S_MASK);
    nseg->attrib.dpl = __SHIFTOUT(attrib, DESC_DPL_MASK);
    nseg->attrib.p = __SHIFTOUT(attrib, DESC_P_MASK);
    nseg->attrib.avl = __SHIFTOUT(attrib, DESC_AVL_MASK);
    nseg->attrib.l = __SHIFTOUT(attrib, DESC_L_MASK);
    nseg->attrib.def = __SHIFTOUT(attrib, DESC_B_MASK);
    nseg->attrib.g = __SHIFTOUT(attrib, DESC_G_MASK);
}

void
nvmm_set_registers(CPUState *cpu)
{
    CPUX86State *env = cpu->env_ptr;
    struct nvmm_machine *mach = get_nvmm_mach();
    struct qemu_vcpu *qcpu = nvmm_get_qemu_vcpu(cpu);
    struct nvmm_vcpu *vcpu = &qcpu->vcpu;
    struct nvmm_x64_state *state = vcpu->state;
    uint64_t bitmap;
    size_t i;
    int ret;

    assert(cpu_is_stopped(cpu) || qemu_cpu_is_self(cpu));

    /* GPRs. */
    state->gprs[NVMM_X64_GPR_RAX] = env->regs[R_EAX];
    state->gprs[NVMM_X64_GPR_RCX] = env->regs[R_ECX];
    state->gprs[NVMM_X64_GPR_RDX] = env->regs[R_EDX];
    state->gprs[NVMM_X64_GPR_RBX] = env->regs[R_EBX];
    state->gprs[NVMM_X64_GPR_RSP] = env->regs[R_ESP];
    state->gprs[NVMM_X64_GPR_RBP] = env->regs[R_EBP];
    state->gprs[NVMM_X64_GPR_RSI] = env->regs[R_ESI];
    state->gprs[NVMM_X64_GPR_RDI] = env->regs[R_EDI];
#ifdef TARGET_X86_64
    state->gprs[NVMM_X64_GPR_R8]  = env->regs[R_R8];
    state->gprs[NVMM_X64_GPR_R9]  = env->regs[R_R9];
    state->gprs[NVMM_X64_GPR_R10] = env->regs[R_R10];
    state->gprs[NVMM_X64_GPR_R11] = env->regs[R_R11];
    state->gprs[NVMM_X64_GPR_R12] = env->regs[R_R12];
    state->gprs[NVMM_X64_GPR_R13] = env->regs[R_R13];
    state->gprs[NVMM_X64_GPR_R14] = env->regs[R_R14];
    state->gprs[NVMM_X64_GPR_R15] = env->regs[R_R15];
#endif

    /* RIP and RFLAGS. */
    state->gprs[NVMM_X64_GPR_RIP] = env->eip;
    state->gprs[NVMM_X64_GPR_RFLAGS] = env->eflags;

    /* Segments. */
    nvmm_set_segment(&state->segs[NVMM_X64_SEG_CS], &env->segs[R_CS]);
    nvmm_set_segment(&state->segs[NVMM_X64_SEG_DS], &env->segs[R_DS]);
    nvmm_set_segment(&state->segs[NVMM_X64_SEG_ES], &env->segs[R_ES]);
    nvmm_set_segment(&state->segs[NVMM_X64_SEG_FS], &env->segs[R_FS]);
    nvmm_set_segment(&state->segs[NVMM_X64_SEG_GS], &env->segs[R_GS]);
    nvmm_set_segment(&state->segs[NVMM_X64_SEG_SS], &env->segs[R_SS]);

    /* Special segments. */
    nvmm_set_segment(&state->segs[NVMM_X64_SEG_GDT], &env->gdt);
    nvmm_set_segment(&state->segs[NVMM_X64_SEG_LDT], &env->ldt);
    nvmm_set_segment(&state->segs[NVMM_X64_SEG_TR], &env->tr);
    nvmm_set_segment(&state->segs[NVMM_X64_SEG_IDT], &env->idt);

    /* Control registers. */
    state->crs[NVMM_X64_CR_CR0] = env->cr[0];
    state->crs[NVMM_X64_CR_CR2] = env->cr[2];
    state->crs[NVMM_X64_CR_CR3] = env->cr[3];
    state->crs[NVMM_X64_CR_CR4] = env->cr[4];
    state->crs[NVMM_X64_CR_CR8] = qcpu->tpr;
    state->crs[NVMM_X64_CR_XCR0] = env->xcr0;

    /* Debug registers. */
    state->drs[NVMM_X64_DR_DR0] = env->dr[0];
    state->drs[NVMM_X64_DR_DR1] = env->dr[1];
    state->drs[NVMM_X64_DR_DR2] = env->dr[2];
    state->drs[NVMM_X64_DR_DR3] = env->dr[3];
    state->drs[NVMM_X64_DR_DR6] = env->dr[6];
    state->drs[NVMM_X64_DR_DR7] = env->dr[7];

    /* FPU. */
    state->fpu.fx_cw = env->fpuc;
    state->fpu.fx_sw = (env->fpus & ~0x3800) | ((env->fpstt & 0x7) << 11);
    state->fpu.fx_tw = 0;
    for (i = 0; i < 8; i++) {
        state->fpu.fx_tw |= (!env->fptags[i]) << i;
    }
    state->fpu.fx_opcode = env->fpop;
    state->fpu.fx_ip.fa_64 = env->fpip;
    state->fpu.fx_dp.fa_64 = env->fpdp;
    state->fpu.fx_mxcsr = env->mxcsr;
    state->fpu.fx_mxcsr_mask = 0x0000FFFF;
    assert(sizeof(state->fpu.fx_87_ac) == sizeof(env->fpregs));
    memcpy(state->fpu.fx_87_ac, env->fpregs, sizeof(env->fpregs));
    for (i = 0; i < CPU_NB_REGS; i++) {
        memcpy(&state->fpu.fx_xmm[i].xmm_bytes[0],
            &env->xmm_regs[i].ZMM_Q(0), 8);
        memcpy(&state->fpu.fx_xmm[i].xmm_bytes[8],
            &env->xmm_regs[i].ZMM_Q(1), 8);
    }

    /* MSRs. */
    state->msrs[NVMM_X64_MSR_EFER] = env->efer;
    state->msrs[NVMM_X64_MSR_STAR] = env->star;
#ifdef TARGET_X86_64
    state->msrs[NVMM_X64_MSR_LSTAR] = env->lstar;
    state->msrs[NVMM_X64_MSR_CSTAR] = env->cstar;
    state->msrs[NVMM_X64_MSR_SFMASK] = env->fmask;
    state->msrs[NVMM_X64_MSR_KERNELGSBASE] = env->kernelgsbase;
#endif
    state->msrs[NVMM_X64_MSR_SYSENTER_CS]  = env->sysenter_cs;
    state->msrs[NVMM_X64_MSR_SYSENTER_ESP] = env->sysenter_esp;
    state->msrs[NVMM_X64_MSR_SYSENTER_EIP] = env->sysenter_eip;
    state->msrs[NVMM_X64_MSR_PAT] = env->pat;
    state->msrs[NVMM_X64_MSR_TSC] = env->tsc;

    bitmap =
        NVMM_X64_STATE_SEGS |
        NVMM_X64_STATE_GPRS |
        NVMM_X64_STATE_CRS  |
        NVMM_X64_STATE_DRS  |
        NVMM_X64_STATE_MSRS |
        NVMM_X64_STATE_FPU;

    ret = nvmm_vcpu_setstate(mach, vcpu, bitmap);
    if (ret == -1) {
        error_report("NVMM: Failed to set virtual processor context,"
            " error=%d", errno);
    }
}

static void
nvmm_get_segment(SegmentCache *qseg, const struct nvmm_x64_state_seg *nseg)
{
    qseg->selector = nseg->selector;
    qseg->limit = nseg->limit;
    qseg->base = nseg->base;

    qseg->flags =
        __SHIFTIN((uint32_t)nseg->attrib.type, DESC_TYPE_MASK) |
        __SHIFTIN((uint32_t)nseg->attrib.s, DESC_S_MASK) |
        __SHIFTIN((uint32_t)nseg->attrib.dpl, DESC_DPL_MASK) |
        __SHIFTIN((uint32_t)nseg->attrib.p, DESC_P_MASK) |
        __SHIFTIN((uint32_t)nseg->attrib.avl, DESC_AVL_MASK) |
        __SHIFTIN((uint32_t)nseg->attrib.l, DESC_L_MASK) |
        __SHIFTIN((uint32_t)nseg->attrib.def, DESC_B_MASK) |
        __SHIFTIN((uint32_t)nseg->attrib.g, DESC_G_MASK);
}

void
nvmm_get_registers(CPUState *cpu)
{
    CPUX86State *env = cpu->env_ptr;
    struct nvmm_machine *mach = get_nvmm_mach();
    struct qemu_vcpu *qcpu = nvmm_get_qemu_vcpu(cpu);
    struct nvmm_vcpu *vcpu = &qcpu->vcpu;
    X86CPU *x86_cpu = X86_CPU(cpu);
    struct nvmm_x64_state *state = vcpu->state;
    uint64_t bitmap, tpr;
    size_t i;
    int ret;

    assert(cpu_is_stopped(cpu) || qemu_cpu_is_self(cpu));

    bitmap =
        NVMM_X64_STATE_SEGS |
        NVMM_X64_STATE_GPRS |
        NVMM_X64_STATE_CRS  |
        NVMM_X64_STATE_DRS  |
        NVMM_X64_STATE_MSRS |
        NVMM_X64_STATE_FPU;

    ret = nvmm_vcpu_getstate(mach, vcpu, bitmap);
    if (ret == -1) {
        error_report("NVMM: Failed to get virtual processor context,"
            " error=%d", errno);
    }

    /* GPRs. */
    env->regs[R_EAX] = state->gprs[NVMM_X64_GPR_RAX];
    env->regs[R_ECX] = state->gprs[NVMM_X64_GPR_RCX];
    env->regs[R_EDX] = state->gprs[NVMM_X64_GPR_RDX];
    env->regs[R_EBX] = state->gprs[NVMM_X64_GPR_RBX];
    env->regs[R_ESP] = state->gprs[NVMM_X64_GPR_RSP];
    env->regs[R_EBP] = state->gprs[NVMM_X64_GPR_RBP];
    env->regs[R_ESI] = state->gprs[NVMM_X64_GPR_RSI];
    env->regs[R_EDI] = state->gprs[NVMM_X64_GPR_RDI];
#ifdef TARGET_X86_64
    env->regs[R_R8]  = state->gprs[NVMM_X64_GPR_R8];
    env->regs[R_R9]  = state->gprs[NVMM_X64_GPR_R9];
    env->regs[R_R10] = state->gprs[NVMM_X64_GPR_R10];
    env->regs[R_R11] = state->gprs[NVMM_X64_GPR_R11];
    env->regs[R_R12] = state->gprs[NVMM_X64_GPR_R12];
    env->regs[R_R13] = state->gprs[NVMM_X64_GPR_R13];
    env->regs[R_R14] = state->gprs[NVMM_X64_GPR_R14];
    env->regs[R_R15] = state->gprs[NVMM_X64_GPR_R15];
#endif

    /* RIP and RFLAGS. */
    env->eip = state->gprs[NVMM_X64_GPR_RIP];
    env->eflags = state->gprs[NVMM_X64_GPR_RFLAGS];

    /* Segments. */
    nvmm_get_segment(&env->segs[R_ES], &state->segs[NVMM_X64_SEG_ES]);
    nvmm_get_segment(&env->segs[R_CS], &state->segs[NVMM_X64_SEG_CS]);
    nvmm_get_segment(&env->segs[R_SS], &state->segs[NVMM_X64_SEG_SS]);
    nvmm_get_segment(&env->segs[R_DS], &state->segs[NVMM_X64_SEG_DS]);
    nvmm_get_segment(&env->segs[R_FS], &state->segs[NVMM_X64_SEG_FS]);
    nvmm_get_segment(&env->segs[R_GS], &state->segs[NVMM_X64_SEG_GS]);

    /* Special segments. */
    nvmm_get_segment(&env->gdt, &state->segs[NVMM_X64_SEG_GDT]);
    nvmm_get_segment(&env->ldt, &state->segs[NVMM_X64_SEG_LDT]);
    nvmm_get_segment(&env->tr, &state->segs[NVMM_X64_SEG_TR]);
    nvmm_get_segment(&env->idt, &state->segs[NVMM_X64_SEG_IDT]);

    /* Control registers. */
    env->cr[0] = state->crs[NVMM_X64_CR_CR0];
    env->cr[2] = state->crs[NVMM_X64_CR_CR2];
    env->cr[3] = state->crs[NVMM_X64_CR_CR3];
    env->cr[4] = state->crs[NVMM_X64_CR_CR4];
    tpr = state->crs[NVMM_X64_CR_CR8];
    if (tpr != qcpu->tpr) {
        qcpu->tpr = tpr;
        cpu_set_apic_tpr(x86_cpu->apic_state, tpr);
    }
    env->xcr0 = state->crs[NVMM_X64_CR_XCR0];

    /* Debug registers. */
    env->dr[0] = state->drs[NVMM_X64_DR_DR0];
    env->dr[1] = state->drs[NVMM_X64_DR_DR1];
    env->dr[2] = state->drs[NVMM_X64_DR_DR2];
    env->dr[3] = state->drs[NVMM_X64_DR_DR3];
    env->dr[6] = state->drs[NVMM_X64_DR_DR6];
    env->dr[7] = state->drs[NVMM_X64_DR_DR7];

    /* FPU. */
    env->fpuc = state->fpu.fx_cw;
    env->fpstt = (state->fpu.fx_sw >> 11) & 0x7;
    env->fpus = state->fpu.fx_sw & ~0x3800;
    for (i = 0; i < 8; i++) {
        env->fptags[i] = !((state->fpu.fx_tw >> i) & 1);
    }
    env->fpop = state->fpu.fx_opcode;
    env->fpip = state->fpu.fx_ip.fa_64;
    env->fpdp = state->fpu.fx_dp.fa_64;
    env->mxcsr = state->fpu.fx_mxcsr;
    assert(sizeof(state->fpu.fx_87_ac) == sizeof(env->fpregs));
    memcpy(env->fpregs, state->fpu.fx_87_ac, sizeof(env->fpregs));
    for (i = 0; i < CPU_NB_REGS; i++) {
        memcpy(&env->xmm_regs[i].ZMM_Q(0),
            &state->fpu.fx_xmm[i].xmm_bytes[0], 8);
        memcpy(&env->xmm_regs[i].ZMM_Q(1),
            &state->fpu.fx_xmm[i].xmm_bytes[8], 8);
    }

    /* MSRs. */
    env->efer = state->msrs[NVMM_X64_MSR_EFER];
    env->star = state->msrs[NVMM_X64_MSR_STAR];
#ifdef TARGET_X86_64
    env->lstar = state->msrs[NVMM_X64_MSR_LSTAR];
    env->cstar = state->msrs[NVMM_X64_MSR_CSTAR];
    env->fmask = state->msrs[NVMM_X64_MSR_SFMASK];
    env->kernelgsbase = state->msrs[NVMM_X64_MSR_KERNELGSBASE];
#endif
    env->sysenter_cs  = state->msrs[NVMM_X64_MSR_SYSENTER_CS];
    env->sysenter_esp = state->msrs[NVMM_X64_MSR_SYSENTER_ESP];
    env->sysenter_eip = state->msrs[NVMM_X64_MSR_SYSENTER_EIP];
    env->pat = state->msrs[NVMM_X64_MSR_PAT];
    env->tsc = state->msrs[NVMM_X64_MSR_TSC];

    x86_update_hflags(env);
}

static bool
nvmm_can_take_int(CPUState *cpu)
{
    CPUX86State *env = cpu->env_ptr;
    struct qemu_vcpu *qcpu = nvmm_get_qemu_vcpu(cpu);
    struct nvmm_vcpu *vcpu = &qcpu->vcpu;
    struct nvmm_machine *mach = get_nvmm_mach();

    if (qcpu->int_window_exit) {
        return false;
    }

    if (qcpu->int_shadow || !(env->eflags & IF_MASK)) {
        struct nvmm_x64_state *state = vcpu->state;

        /* Exit on interrupt window. */
        nvmm_vcpu_getstate(mach, vcpu, NVMM_X64_STATE_INTR);
        state->intr.int_window_exiting = 1;
        nvmm_vcpu_setstate(mach, vcpu, NVMM_X64_STATE_INTR);

        return false;
    }

    return true;
}

static bool
nvmm_can_take_nmi(CPUState *cpu)
{
    struct qemu_vcpu *qcpu = nvmm_get_qemu_vcpu(cpu);

    /*
     * Contrary to INTs, NMIs always schedule an exit when they are
     * completed. Therefore, if window-exiting is enabled, it means
     * NMIs are blocked.
     */
    if (qcpu->nmi_window_exit) {
        return false;
    }

    return true;
}

/*
 * Called before the VCPU is run. We inject events generated by the I/O
 * thread, and synchronize the guest TPR.
 */
static void
nvmm_vcpu_pre_run(CPUState *cpu)
{
    CPUX86State *env = cpu->env_ptr;
    struct nvmm_machine *mach = get_nvmm_mach();
    struct qemu_vcpu *qcpu = nvmm_get_qemu_vcpu(cpu);
    struct nvmm_vcpu *vcpu = &qcpu->vcpu;
    X86CPU *x86_cpu = X86_CPU(cpu);
    struct nvmm_x64_state *state = vcpu->state;
    struct nvmm_vcpu_event *event = vcpu->event;
    bool has_event = false;
    bool sync_tpr = false;
    uint8_t tpr;
    int ret;

    qemu_mutex_lock_iothread();

    tpr = cpu_get_apic_tpr(x86_cpu->apic_state);
    if (tpr != qcpu->tpr) {
        qcpu->tpr = tpr;
        sync_tpr = true;
    }

    /*
     * Force the VCPU out of its inner loop to process any INIT requests
     * or commit pending TPR access.
     */
    if (cpu->interrupt_request & (CPU_INTERRUPT_INIT | CPU_INTERRUPT_TPR)) {
        cpu->exit_request = 1;
    }

    if (!has_event && (cpu->interrupt_request & CPU_INTERRUPT_NMI)) {
        if (nvmm_can_take_nmi(cpu)) {
            cpu->interrupt_request &= ~CPU_INTERRUPT_NMI;
            event->type = NVMM_VCPU_EVENT_INTR;
            event->vector = 2;
            has_event = true;
        }
    }

    if (!has_event && (cpu->interrupt_request & CPU_INTERRUPT_HARD)) {
        if (nvmm_can_take_int(cpu)) {
            cpu->interrupt_request &= ~CPU_INTERRUPT_HARD;
            event->type = NVMM_VCPU_EVENT_INTR;
            event->vector = cpu_get_pic_interrupt(env);
            has_event = true;
        }
    }

    /* Don't want SMIs. */
    if (cpu->interrupt_request & CPU_INTERRUPT_SMI) {
        cpu->interrupt_request &= ~CPU_INTERRUPT_SMI;
    }

    if (sync_tpr) {
        ret = nvmm_vcpu_getstate(mach, vcpu, NVMM_X64_STATE_CRS);
        if (ret == -1) {
            error_report("NVMM: Failed to get CPU state,"
                " error=%d", errno);
        }

        state->crs[NVMM_X64_CR_CR8] = qcpu->tpr;

        ret = nvmm_vcpu_setstate(mach, vcpu, NVMM_X64_STATE_CRS);
        if (ret == -1) {
            error_report("NVMM: Failed to set CPU state,"
                " error=%d", errno);
        }
    }

    if (has_event) {
        ret = nvmm_vcpu_inject(mach, vcpu);
        if (ret == -1) {
            error_report("NVMM: Failed to inject event,"
                " error=%d", errno);
        }
    }

    qemu_mutex_unlock_iothread();
}

/*
 * Called after the VCPU ran. We synchronize the host view of the TPR and
 * RFLAGS.
 */
static void
nvmm_vcpu_post_run(CPUState *cpu, struct nvmm_vcpu_exit *exit)
{
    struct qemu_vcpu *qcpu = nvmm_get_qemu_vcpu(cpu);
    CPUX86State *env = cpu->env_ptr;
    X86CPU *x86_cpu = X86_CPU(cpu);
    uint64_t tpr;

    env->eflags = exit->exitstate.rflags;
    qcpu->int_shadow = exit->exitstate.int_shadow;
    qcpu->int_window_exit = exit->exitstate.int_window_exiting;
    qcpu->nmi_window_exit = exit->exitstate.nmi_window_exiting;

    tpr = exit->exitstate.cr8;
    if (qcpu->tpr != tpr) {
        qcpu->tpr = tpr;
        qemu_mutex_lock_iothread();
        cpu_set_apic_tpr(x86_cpu->apic_state, qcpu->tpr);
        qemu_mutex_unlock_iothread();
    }
}

/* -------------------------------------------------------------------------- */

static int
nvmm_handle_rdmsr(struct nvmm_machine *mach, CPUState *cpu,
    struct nvmm_vcpu_exit *exit)
{
    struct qemu_vcpu *qcpu = nvmm_get_qemu_vcpu(cpu);
    struct nvmm_vcpu *vcpu = &qcpu->vcpu;
    X86CPU *x86_cpu = X86_CPU(cpu);
    struct nvmm_x64_state *state = vcpu->state;
    uint64_t val;
    int ret;

    switch (exit->u.rdmsr.msr) {
    case MSR_IA32_APICBASE:
        val = cpu_get_apic_base(x86_cpu->apic_state);
        break;
    case MSR_MTRRcap:
    case MSR_MTRRdefType:
    case MSR_MCG_CAP:
    case MSR_MCG_STATUS:
        val = 0;
        break;
    default: /* More MSRs to add? */
        val = 0;
        error_report("NVMM: Unexpected RDMSR 0x%x, ignored",
            exit->u.rdmsr.msr);
        break;
    }

    ret = nvmm_vcpu_getstate(mach, vcpu, NVMM_X64_STATE_GPRS);
    if (ret == -1) {
        return -1;
    }

    state->gprs[NVMM_X64_GPR_RAX] = (val & 0xFFFFFFFF);
    state->gprs[NVMM_X64_GPR_RDX] = (val >> 32);
    state->gprs[NVMM_X64_GPR_RIP] = exit->u.rdmsr.npc;

    ret = nvmm_vcpu_setstate(mach, vcpu, NVMM_X64_STATE_GPRS);
    if (ret == -1) {
        return -1;
    }

    return 0;
}

static int
nvmm_handle_wrmsr(struct nvmm_machine *mach, CPUState *cpu,
    struct nvmm_vcpu_exit *exit)
{
    struct qemu_vcpu *qcpu = nvmm_get_qemu_vcpu(cpu);
    struct nvmm_vcpu *vcpu = &qcpu->vcpu;
    X86CPU *x86_cpu = X86_CPU(cpu);
    struct nvmm_x64_state *state = vcpu->state;
    uint64_t val;
    int ret;

    val = exit->u.wrmsr.val;

    switch (exit->u.wrmsr.msr) {
    case MSR_IA32_APICBASE:
        cpu_set_apic_base(x86_cpu->apic_state, val);
        break;
    case MSR_MTRRdefType:
    case MSR_MCG_STATUS:
        break;
    default: /* More MSRs to add? */
        error_report("NVMM: Unexpected WRMSR 0x%x [val=0x%lx], ignored",
            exit->u.wrmsr.msr, val);
        break;
    }

    ret = nvmm_vcpu_getstate(mach, vcpu, NVMM_X64_STATE_GPRS);
    if (ret == -1) {
        return -1;
    }

    state->gprs[NVMM_X64_GPR_RIP] = exit->u.wrmsr.npc;

    ret = nvmm_vcpu_setstate(mach, vcpu, NVMM_X64_STATE_GPRS);
    if (ret == -1) {
        return -1;
    }

    return 0;
}

static int
nvmm_handle_halted(struct nvmm_machine *mach, CPUState *cpu,
    struct nvmm_vcpu_exit *exit)
{
    CPUX86State *env = cpu->env_ptr;
    int ret = 0;

    qemu_mutex_lock_iothread();

    if (!((cpu->interrupt_request & CPU_INTERRUPT_HARD) &&
          (env->eflags & IF_MASK)) &&
        !(cpu->interrupt_request & CPU_INTERRUPT_NMI)) {
        cpu->exception_index = EXCP_HLT;
        cpu->halted = true;
        ret = 1;
    }

    qemu_mutex_unlock_iothread();

    return ret;
}

static int
nvmm_inject_ud(struct nvmm_machine *mach, struct nvmm_vcpu *vcpu)
{
    struct nvmm_vcpu_event *event = vcpu->event;

    event->type = NVMM_VCPU_EVENT_EXCP;
    event->vector = 6;
    event->u.excp.error = 0;

    return nvmm_vcpu_inject(mach, vcpu);
}

int
nvmm_vcpu_loop(CPUState *cpu)
{
    CPUX86State *env = cpu->env_ptr;
    struct nvmm_machine *mach = get_nvmm_mach();
    struct qemu_vcpu *qcpu = nvmm_get_qemu_vcpu(cpu);
    struct nvmm_vcpu *vcpu = &qcpu->vcpu;
    X86CPU *x86_cpu = X86_CPU(cpu);
    struct nvmm_vcpu_exit *exit = vcpu->exit;
    int ret;

    /*
     * Some asynchronous events must be handled outside of the inner
     * VCPU loop. They are handled here.
     */
    if (cpu->interrupt_request & CPU_INTERRUPT_INIT) {
        nvmm_cpu_synchronize_state(cpu);
        do_cpu_init(x86_cpu);
        /* set int/nmi windows back to the reset state */
    }
    if (cpu->interrupt_request & CPU_INTERRUPT_POLL) {
        cpu->interrupt_request &= ~CPU_INTERRUPT_POLL;
        apic_poll_irq(x86_cpu->apic_state);
    }
    if (((cpu->interrupt_request & CPU_INTERRUPT_HARD) &&
         (env->eflags & IF_MASK)) ||
        (cpu->interrupt_request & CPU_INTERRUPT_NMI)) {
        cpu->halted = false;
    }
    if (cpu->interrupt_request & CPU_INTERRUPT_SIPI) {
        nvmm_cpu_synchronize_state(cpu);
        do_cpu_sipi(x86_cpu);
    }
    if (cpu->interrupt_request & CPU_INTERRUPT_TPR) {
        cpu->interrupt_request &= ~CPU_INTERRUPT_TPR;
        nvmm_cpu_synchronize_state(cpu);
        apic_handle_tpr_access_report(x86_cpu->apic_state, env->eip,
            env->tpr_access_type);
    }

    if (cpu->halted) {
        cpu->exception_index = EXCP_HLT;
        qatomic_set(&cpu->exit_request, false);
        return 0;
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

        if (qatomic_read(&cpu->exit_request)) {
#if NVMM_USER_VERSION >= 2
            nvmm_vcpu_stop(vcpu);
#else
            qemu_cpu_kick_self();
#endif
        }

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
#if NVMM_USER_VERSION >= 2
        case NVMM_VCPU_EXIT_STOPPED:
            /*
             * The kernel cleared the immediate exit flag; cpu->exit_request
             * must be cleared after
             */
            smp_wmb();
            qcpu->stop = true;
            break;
#endif
        case NVMM_VCPU_EXIT_MEMORY:
            ret = nvmm_handle_mem(mach, vcpu);
            break;
        case NVMM_VCPU_EXIT_IO:
            ret = nvmm_handle_io(mach, vcpu);
            break;
        case NVMM_VCPU_EXIT_INT_READY:
        case NVMM_VCPU_EXIT_NMI_READY:
        case NVMM_VCPU_EXIT_TPR_CHANGED:
            break;
        case NVMM_VCPU_EXIT_HALTED:
            ret = nvmm_handle_halted(mach, cpu, exit);
            break;
        case NVMM_VCPU_EXIT_SHUTDOWN:
            qemu_system_reset_request(SHUTDOWN_CAUSE_GUEST_RESET);
            cpu->exception_index = EXCP_INTERRUPT;
            ret = 1;
            break;
        case NVMM_VCPU_EXIT_RDMSR:
            ret = nvmm_handle_rdmsr(mach, cpu, exit);
            break;
        case NVMM_VCPU_EXIT_WRMSR:
            ret = nvmm_handle_wrmsr(mach, cpu, exit);
            break;
        case NVMM_VCPU_EXIT_MONITOR:
        case NVMM_VCPU_EXIT_MWAIT:
            ret = nvmm_inject_ud(mach, vcpu);
            break;
        default:
            error_report("NVMM: Unexpected VM exit code 0x%lx [hw=0x%lx]",
                exit->reason, exit->u.inv.hwcode);
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
#if NVMM_USER_VERSION >= 2
        struct nvmm_vcpu *vcpu = &qcpu->vcpu;
        nvmm_vcpu_stop(vcpu);
#else
        qcpu->stop = true;
#endif
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
    struct nvmm_capability *cap = get_nvmm_cap();
    struct nvmm_vcpu_conf_cpuid cpuid;
    struct nvmm_vcpu_conf_tpr tpr;
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

    ret = nvmm_vcpu_configure(mach, &qcpu->vcpu, NVMM_VCPU_CONF_CALLBACKS,
        &nvmm_callbacks);
    if (ret == -1) {
        err = errno;
        error_report("NVMM: Failed to configure a virtual processor,"
            " error=%d", err);
        g_free(qcpu);
        return -err;
    }

    if (cap->arch.vcpu_conf_support & NVMM_CAP_ARCH_VCPU_CONF_TPR) {
        memset(&tpr, 0, sizeof(tpr));
        tpr.exit_changed = 1;
        ret = nvmm_vcpu_configure(mach, &qcpu->vcpu, NVMM_VCPU_CONF_TPR, &tpr);
        if (ret == -1) {
            err = errno;
            error_report("NVMM: Failed to configure a virtual processor,"
                " error=%d", err);
            g_free(qcpu);
            return -err;
        }
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
