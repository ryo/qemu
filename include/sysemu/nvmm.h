/*
 * Copyright (c) 2018-2019 Maxime Villard, All rights reserved.
 *
 * NetBSD Virtual Machine Monitor (NVMM) accelerator support.
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#ifndef QEMU_NVMM_H
#define QEMU_NVMM_H

#include <nvmm.h>

#ifdef NEED_CPU_H

#ifdef CONFIG_NVMM

int nvmm_enabled(void);

#else /* CONFIG_NVMM */

#define nvmm_enabled() (0)

#endif /* CONFIG_NVMM */

#endif /* NEED_CPU_H */

/* MI: commoaccel/nvmm/nvmm-all.c */
struct nvmm_machine *get_nvmm_mach(void);
struct nvmm_capability *get_nvmm_cap(void);
struct qemu_vcpu *nvmm_get_qemu_vcpu(CPUState *cpu);
int nvmm_vcpu_exec(CPUState *cpu);
void nvmm_cpu_synchronize_state(CPUState *cpu);
void nvmm_cpu_synchronize_post_reset(CPUState *cpu);
void nvmm_cpu_synchronize_post_init(CPUState *cpu);
void nvmm_cpu_synchronize_pre_loadvm(CPUState *cpu);
int nvmm_handle_mem(struct nvmm_machine *mach, struct nvmm_vcpu *vcpu);
int nvmm_handle_io(struct nvmm_machine *mach, struct nvmm_vcpu *vcpu);
void nvmm_ipi_signal(int sigcpu);
void nvmm_init_cpu_signals(void);

extern struct nvmm_assist_callbacks nvmm_callbacks;

/* MD: target/<arch>/nvmm/nvmm.c */
int nvmm_init_vcpu(CPUState *cpu);
void nvmm_destroy_vcpu(CPUState *cpu);
void nvmm_get_registers(CPUState *cpu);
void nvmm_set_registers(CPUState *cpu);
int nvmm_vcpu_loop(CPUState *cpu);

#endif /* QEMU_NVMM_H */
