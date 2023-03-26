/*
 * NetBSD Virtual Machine Monitor (NVMM) support -- ARM specifics
 *
 * Copyright (c) 2023 Ryo Shimizu
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 *
 */

#ifndef QEMU_NVMM_ARM_H
#define QEMU_NVMM_ARM_H

#include "cpu.h"

void nvmm_arm_set_cpu_features_from_host(ARMCPU *cpu);

#endif /* QEMU_NVMM_ARM_H */
