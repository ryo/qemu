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
#include "nvmm_arm.h"
#include "cpregs.h"
#include "strings.h"

#include <sys/sysctl.h>
#include <aarch64/armreg.h>

struct qemu_vcpu {
    struct nvmm_vcpu vcpu;
    bool stop;
};

struct nvmm_sreg_match {
	int group;
#define SREG_GROUP_SPR	0
#define SREG_GROUP_TID	1
	int reg;
	uint32_t key;
	uint32_t cp_idx;
};

#define ENCODE_SYSREG(crn, crm, op0, op1, op2)	\
	ENCODE_AA64_CP_REG(CP_REG_ARM64_SYSREG_CP, crn, crm, op0, op1, op2)


static struct nvmm_sreg_match nvmm_sreg_match[] = {
//  { SREG_GROUP_SPR, NVMM_AARCH64_SPR_ACTLR_EL1,           ENCODE_SYSREG(  1,  0, 3, 0, 1) },
//  { SREG_GROUP_SPR, NVMM_AARCH64_SPR_AFSR0_EL1,           ENCODE_SYSREG(  5,  1, 3, 0, 0) },
//  { SREG_GROUP_SPR, NVMM_AARCH64_SPR_AFSR1_EL1,           ENCODE_SYSREG(  5,  1, 3, 0, 1) },
//  { SREG_GROUP_SPR, NVMM_AARCH64_SPR_AIDR_EL1,            ENCODE_SYSREG(  0,  0, 3, 1, 7) },
    { SREG_GROUP_SPR, NVMM_AARCH64_SPR_AMAIR_EL1,           ENCODE_SYSREG( 10,  3, 3, 0, 0) },
//  { SREG_GROUP_SPR, NVMM_AARCH64_SPR_APDAKEYHI_EL1,       ENCODE_SYSREG(  2,  2, 3, 0, 1) },
//  { SREG_GROUP_SPR, NVMM_AARCH64_SPR_APDAKEYLO_EL1,       ENCODE_SYSREG(  2,  2, 3, 0, 0) },
//  { SREG_GROUP_SPR, NVMM_AARCH64_SPR_APDBKEYHI_EL1,       ENCODE_SYSREG(  2,  2, 3, 0, 3) },
//  { SREG_GROUP_SPR, NVMM_AARCH64_SPR_APDBKEYLO_EL1,       ENCODE_SYSREG(  2,  2, 3, 0, 2) },
//  { SREG_GROUP_SPR, NVMM_AARCH64_SPR_APGAKEYHI_EL1,       ENCODE_SYSREG(  2,  3, 3, 0, 1) },
//  { SREG_GROUP_SPR, NVMM_AARCH64_SPR_APGAKEYLO_EL1,       ENCODE_SYSREG(  2,  3, 3, 0, 0) },
//  { SREG_GROUP_SPR, NVMM_AARCH64_SPR_APIAKEYHI_EL1,       ENCODE_SYSREG(  2,  1, 3, 0, 1) },
//  { SREG_GROUP_SPR, NVMM_AARCH64_SPR_APIAKEYLO_EL1,       ENCODE_SYSREG(  2,  1, 3, 0, 0) },
//  { SREG_GROUP_SPR, NVMM_AARCH64_SPR_APIBKEYHI_EL1,       ENCODE_SYSREG(  2,  1, 3, 0, 3) },
//  { SREG_GROUP_SPR, NVMM_AARCH64_SPR_APIBKEYLO_EL1,       ENCODE_SYSREG(  2,  1, 3, 0, 2) },
//  { SREG_GROUP_SPR, NVMM_AARCH64_SPR_CCSIDR2_EL1,         ENCODE_SYSREG(  0,  0, 3, 1, 2) },
//  { SREG_GROUP_SPR, NVMM_AARCH64_SPR_CCSIDR_EL1,          ENCODE_SYSREG(  0,  0, 3, 1, 0) },
//  { SREG_GROUP_SPR, NVMM_AARCH64_SPR_CLIDR_EL1,           ENCODE_SYSREG(  0,  0, 3, 1, 1) },
//  { SREG_GROUP_SPR, NVMM_AARCH64_SPR_CNTFRQ_EL0,          ENCODE_SYSREG( 14,  0, 3, 3, 0) },
    { SREG_GROUP_SPR, NVMM_AARCH64_SPR_CNTKCTL_EL1,         ENCODE_SYSREG( 14,  1, 3, 0, 0) },
//  { SREG_GROUP_SPR, NVMM_AARCH64_SPR_CNTPCT_EL0,          ENCODE_SYSREG( 14,  0, 3, 3, 1) },
//  { SREG_GROUP_SPR, NVMM_AARCH64_SPR_CNTPS_CTL_EL1,       ENCODE_SYSREG( 14,  2, 3, 7, 1) },
//  { SREG_GROUP_SPR, NVMM_AARCH64_SPR_CNTPS_CVAL_EL1,      ENCODE_SYSREG( 14,  2, 3, 7, 2) },
//  { SREG_GROUP_SPR, NVMM_AARCH64_SPR_CNTPS_TVAL_EL1,      ENCODE_SYSREG( 14,  2, 3, 7, 0) },
//  { SREG_GROUP_SPR, NVMM_AARCH64_SPR_CNTP_CTL_EL0,        ENCODE_SYSREG( 14,  2, 3, 3, 1) },
//  { SREG_GROUP_SPR, NVMM_AARCH64_SPR_CNTP_CVAL_EL0,       ENCODE_SYSREG( 14,  2, 3, 3, 2) },
//  { SREG_GROUP_SPR, NVMM_AARCH64_SPR_CNTP_TVAL_EL0,       ENCODE_SYSREG( 14,  2, 3, 3, 0) },
//  { SREG_GROUP_SPR, NVMM_AARCH64_SPR_CNTVCT_EL0,          ENCODE_SYSREG( 14,  0, 3, 3, 2) },
//  { SREG_GROUP_SPR, NVMM_AARCH64_SPR_CNTV_CTL_EL0,        ENCODE_SYSREG( 14,  3, 3, 3, 1) },
//  { SREG_GROUP_SPR, NVMM_AARCH64_SPR_CNTV_CVAL_EL0,       ENCODE_SYSREG( 14,  3, 3, 3, 2) },
//  { SREG_GROUP_SPR, NVMM_AARCH64_SPR_CNTV_TVAL_EL0,       ENCODE_SYSREG( 14,  3, 3, 3, 0) },
    { SREG_GROUP_SPR, NVMM_AARCH64_SPR_CONTEXTIDR_EL1,      ENCODE_SYSREG( 13,  0, 3, 0, 1) },
    { SREG_GROUP_SPR, NVMM_AARCH64_SPR_CPACR_EL1,           ENCODE_SYSREG(  1,  0, 3, 0, 2) },
    { SREG_GROUP_SPR, NVMM_AARCH64_SPR_CSSELR_EL1,          ENCODE_SYSREG(  0,  0, 3, 2, 0) },
//  { SREG_GROUP_SPR, NVMM_AARCH64_SPR_CTR_EL0,             ENCODE_SYSREG(  0,  0, 3, 3, 1) },
//  { SREG_GROUP_SPR, NVMM_AARCH64_SPR_CURRENTEL,           ENCODE_SYSREG(  4,  2, 3, 0, 2) },
//  { SREG_GROUP_SPR, NVMM_AARCH64_SPR_DAIF,                ENCODE_SYSREG(  4,  2, 3, 3, 1) },
//  { SREG_GROUP_SPR, NVMM_AARCH64_SPR_DBGAUTHSTATUS_EL1,   ENCODE_SYSREG(  7, 14, 2, 0, 6) },
//  { SREG_GROUP_SPR, NVMM_AARCH64_SPR_DBGBCR0_EL1,         ENCODE_SYSREG(  0,  0, 2, 0, 5) },
//  { SREG_GROUP_SPR, NVMM_AARCH64_SPR_DBGBCR10_EL1,        ENCODE_SYSREG(  0, 10, 2, 0, 5) },
//  { SREG_GROUP_SPR, NVMM_AARCH64_SPR_DBGBCR11_EL1,        ENCODE_SYSREG(  0, 11, 2, 0, 5) },
//  { SREG_GROUP_SPR, NVMM_AARCH64_SPR_DBGBCR12_EL1,        ENCODE_SYSREG(  0, 12, 2, 0, 5) },
//  { SREG_GROUP_SPR, NVMM_AARCH64_SPR_DBGBCR13_EL1,        ENCODE_SYSREG(  0, 13, 2, 0, 5) },
//  { SREG_GROUP_SPR, NVMM_AARCH64_SPR_DBGBCR14_EL1,        ENCODE_SYSREG(  0, 14, 2, 0, 5) },
//  { SREG_GROUP_SPR, NVMM_AARCH64_SPR_DBGBCR15_EL1,        ENCODE_SYSREG(  0, 15, 2, 0, 5) },
//  { SREG_GROUP_SPR, NVMM_AARCH64_SPR_DBGBCR1_EL1,         ENCODE_SYSREG(  0,  1, 2, 0, 5) },
//  { SREG_GROUP_SPR, NVMM_AARCH64_SPR_DBGBCR2_EL1,         ENCODE_SYSREG(  0,  2, 2, 0, 5) },
//  { SREG_GROUP_SPR, NVMM_AARCH64_SPR_DBGBCR3_EL1,         ENCODE_SYSREG(  0,  3, 2, 0, 5) },
//  { SREG_GROUP_SPR, NVMM_AARCH64_SPR_DBGBCR4_EL1,         ENCODE_SYSREG(  0,  4, 2, 0, 5) },
//  { SREG_GROUP_SPR, NVMM_AARCH64_SPR_DBGBCR5_EL1,         ENCODE_SYSREG(  0,  5, 2, 0, 5) },
//  { SREG_GROUP_SPR, NVMM_AARCH64_SPR_DBGBCR6_EL1,         ENCODE_SYSREG(  0,  6, 2, 0, 5) },
//  { SREG_GROUP_SPR, NVMM_AARCH64_SPR_DBGBCR7_EL1,         ENCODE_SYSREG(  0,  7, 2, 0, 5) },
//  { SREG_GROUP_SPR, NVMM_AARCH64_SPR_DBGBCR8_EL1,         ENCODE_SYSREG(  0,  8, 2, 0, 5) },
//  { SREG_GROUP_SPR, NVMM_AARCH64_SPR_DBGBCR9_EL1,         ENCODE_SYSREG(  0,  9, 2, 0, 5) },
//  { SREG_GROUP_SPR, NVMM_AARCH64_SPR_DBGBVR0_EL1,         ENCODE_SYSREG(  0,  0, 2, 0, 4) },
//  { SREG_GROUP_SPR, NVMM_AARCH64_SPR_DBGBVR10_EL1,        ENCODE_SYSREG(  0, 10, 2, 0, 4) },
//  { SREG_GROUP_SPR, NVMM_AARCH64_SPR_DBGBVR11_EL1,        ENCODE_SYSREG(  0, 11, 2, 0, 4) },
//  { SREG_GROUP_SPR, NVMM_AARCH64_SPR_DBGBVR12_EL1,        ENCODE_SYSREG(  0, 12, 2, 0, 4) },
//  { SREG_GROUP_SPR, NVMM_AARCH64_SPR_DBGBVR13_EL1,        ENCODE_SYSREG(  0, 13, 2, 0, 4) },
//  { SREG_GROUP_SPR, NVMM_AARCH64_SPR_DBGBVR14_EL1,        ENCODE_SYSREG(  0, 14, 2, 0, 4) },
//  { SREG_GROUP_SPR, NVMM_AARCH64_SPR_DBGBVR15_EL1,        ENCODE_SYSREG(  0, 15, 2, 0, 4) },
//  { SREG_GROUP_SPR, NVMM_AARCH64_SPR_DBGBVR1_EL1,         ENCODE_SYSREG(  0,  1, 2, 0, 4) },
//  { SREG_GROUP_SPR, NVMM_AARCH64_SPR_DBGBVR2_EL1,         ENCODE_SYSREG(  0,  2, 2, 0, 4) },
//  { SREG_GROUP_SPR, NVMM_AARCH64_SPR_DBGBVR3_EL1,         ENCODE_SYSREG(  0,  3, 2, 0, 4) },
//  { SREG_GROUP_SPR, NVMM_AARCH64_SPR_DBGBVR4_EL1,         ENCODE_SYSREG(  0,  4, 2, 0, 4) },
//  { SREG_GROUP_SPR, NVMM_AARCH64_SPR_DBGBVR5_EL1,         ENCODE_SYSREG(  0,  5, 2, 0, 4) },
//  { SREG_GROUP_SPR, NVMM_AARCH64_SPR_DBGBVR6_EL1,         ENCODE_SYSREG(  0,  6, 2, 0, 4) },
//  { SREG_GROUP_SPR, NVMM_AARCH64_SPR_DBGBVR7_EL1,         ENCODE_SYSREG(  0,  7, 2, 0, 4) },
//  { SREG_GROUP_SPR, NVMM_AARCH64_SPR_DBGBVR8_EL1,         ENCODE_SYSREG(  0,  8, 2, 0, 4) },
//  { SREG_GROUP_SPR, NVMM_AARCH64_SPR_DBGBVR9_EL1,         ENCODE_SYSREG(  0,  9, 2, 0, 4) },
//  { SREG_GROUP_SPR, NVMM_AARCH64_SPR_DBGCLAIMCLR_EL1,     ENCODE_SYSREG(  7,  9, 2, 0, 6) },
//  { SREG_GROUP_SPR, NVMM_AARCH64_SPR_DBGCLAIMSET_EL1,     ENCODE_SYSREG(  7,  8, 2, 0, 6) },
//  { SREG_GROUP_SPR, NVMM_AARCH64_SPR_DBGDTRRX_EL0,        ENCODE_SYSREG(  0,  5, 2, 3, 0) },
//  { SREG_GROUP_SPR, NVMM_AARCH64_SPR_DBGDTR_EL0,          ENCODE_SYSREG(  0,  4, 2, 3, 0) },
//  { SREG_GROUP_SPR, NVMM_AARCH64_SPR_DBGPRCR_EL1,         ENCODE_SYSREG(  1,  4, 2, 0, 4) },
//  { SREG_GROUP_SPR, NVMM_AARCH64_SPR_DBGWCR0_EL1,         ENCODE_SYSREG(  0,  0, 2, 0, 7) },
//  { SREG_GROUP_SPR, NVMM_AARCH64_SPR_DBGWCR10_EL1,        ENCODE_SYSREG(  0, 10, 2, 0, 7) },
//  { SREG_GROUP_SPR, NVMM_AARCH64_SPR_DBGWCR11_EL1,        ENCODE_SYSREG(  0, 11, 2, 0, 7) },
//  { SREG_GROUP_SPR, NVMM_AARCH64_SPR_DBGWCR12_EL1,        ENCODE_SYSREG(  0, 12, 2, 0, 7) },
//  { SREG_GROUP_SPR, NVMM_AARCH64_SPR_DBGWCR13_EL1,        ENCODE_SYSREG(  0, 13, 2, 0, 7) },
//  { SREG_GROUP_SPR, NVMM_AARCH64_SPR_DBGWCR14_EL1,        ENCODE_SYSREG(  0, 14, 2, 0, 7) },
//  { SREG_GROUP_SPR, NVMM_AARCH64_SPR_DBGWCR15_EL1,        ENCODE_SYSREG(  0, 15, 2, 0, 7) },
//  { SREG_GROUP_SPR, NVMM_AARCH64_SPR_DBGWCR1_EL1,         ENCODE_SYSREG(  0,  1, 2, 0, 7) },
//  { SREG_GROUP_SPR, NVMM_AARCH64_SPR_DBGWCR2_EL1,         ENCODE_SYSREG(  0,  2, 2, 0, 7) },
//  { SREG_GROUP_SPR, NVMM_AARCH64_SPR_DBGWCR3_EL1,         ENCODE_SYSREG(  0,  3, 2, 0, 7) },
//  { SREG_GROUP_SPR, NVMM_AARCH64_SPR_DBGWCR4_EL1,         ENCODE_SYSREG(  0,  4, 2, 0, 7) },
//  { SREG_GROUP_SPR, NVMM_AARCH64_SPR_DBGWCR5_EL1,         ENCODE_SYSREG(  0,  5, 2, 0, 7) },
//  { SREG_GROUP_SPR, NVMM_AARCH64_SPR_DBGWCR6_EL1,         ENCODE_SYSREG(  0,  6, 2, 0, 7) },
//  { SREG_GROUP_SPR, NVMM_AARCH64_SPR_DBGWCR7_EL1,         ENCODE_SYSREG(  0,  7, 2, 0, 7) },
//  { SREG_GROUP_SPR, NVMM_AARCH64_SPR_DBGWCR8_EL1,         ENCODE_SYSREG(  0,  8, 2, 0, 7) },
//  { SREG_GROUP_SPR, NVMM_AARCH64_SPR_DBGWCR9_EL1,         ENCODE_SYSREG(  0,  9, 2, 0, 7) },
//  { SREG_GROUP_SPR, NVMM_AARCH64_SPR_DBGWVR0_EL1,         ENCODE_SYSREG(  0,  0, 2, 0, 6) },
//  { SREG_GROUP_SPR, NVMM_AARCH64_SPR_DBGWVR10_EL1,        ENCODE_SYSREG(  0, 10, 2, 0, 6) },
//  { SREG_GROUP_SPR, NVMM_AARCH64_SPR_DBGWVR11_EL1,        ENCODE_SYSREG(  0, 11, 2, 0, 6) },
//  { SREG_GROUP_SPR, NVMM_AARCH64_SPR_DBGWVR12_EL1,        ENCODE_SYSREG(  0, 12, 2, 0, 6) },
//  { SREG_GROUP_SPR, NVMM_AARCH64_SPR_DBGWVR13_EL1,        ENCODE_SYSREG(  0, 13, 2, 0, 6) },
//  { SREG_GROUP_SPR, NVMM_AARCH64_SPR_DBGWVR14_EL1,        ENCODE_SYSREG(  0, 14, 2, 0, 6) },
//  { SREG_GROUP_SPR, NVMM_AARCH64_SPR_DBGWVR15_EL1,        ENCODE_SYSREG(  0, 15, 2, 0, 6) },
//  { SREG_GROUP_SPR, NVMM_AARCH64_SPR_DBGWVR1_EL1,         ENCODE_SYSREG(  0,  1, 2, 0, 6) },
//  { SREG_GROUP_SPR, NVMM_AARCH64_SPR_DBGWVR2_EL1,         ENCODE_SYSREG(  0,  2, 2, 0, 6) },
//  { SREG_GROUP_SPR, NVMM_AARCH64_SPR_DBGWVR3_EL1,         ENCODE_SYSREG(  0,  3, 2, 0, 6) },
//  { SREG_GROUP_SPR, NVMM_AARCH64_SPR_DBGWVR4_EL1,         ENCODE_SYSREG(  0,  4, 2, 0, 6) },
//  { SREG_GROUP_SPR, NVMM_AARCH64_SPR_DBGWVR5_EL1,         ENCODE_SYSREG(  0,  5, 2, 0, 6) },
//  { SREG_GROUP_SPR, NVMM_AARCH64_SPR_DBGWVR6_EL1,         ENCODE_SYSREG(  0,  6, 2, 0, 6) },
//  { SREG_GROUP_SPR, NVMM_AARCH64_SPR_DBGWVR7_EL1,         ENCODE_SYSREG(  0,  7, 2, 0, 6) },
//  { SREG_GROUP_SPR, NVMM_AARCH64_SPR_DBGWVR8_EL1,         ENCODE_SYSREG(  0,  8, 2, 0, 6) },
//  { SREG_GROUP_SPR, NVMM_AARCH64_SPR_DBGWVR9_EL1,         ENCODE_SYSREG(  0,  9, 2, 0, 6) },
//  { SREG_GROUP_SPR, NVMM_AARCH64_SPR_DCZID_EL0,           ENCODE_SYSREG(  0,  0, 3, 3, 7) },
//  { SREG_GROUP_SPR, NVMM_AARCH64_SPR_DIT,                 ENCODE_SYSREG(  4,  2, 3, 3, 5) },
//  { SREG_GROUP_SPR, NVMM_AARCH64_SPR_DLR_EL0,             ENCODE_SYSREG(  4,  5, 3, 3, 1) },
//  { SREG_GROUP_SPR, NVMM_AARCH64_SPR_DSPSR_EL0,           ENCODE_SYSREG(  4,  5, 3, 3, 0) },
    { SREG_GROUP_SPR, NVMM_AARCH64_SPR_ELR_EL1,             ENCODE_SYSREG(  4,  0, 3, 0, 1) },
    { SREG_GROUP_SPR, NVMM_AARCH64_SPR_ESR_EL1,             ENCODE_SYSREG(  5,  2, 3, 0, 0) },
    { SREG_GROUP_SPR, NVMM_AARCH64_SPR_FAR_EL1,             ENCODE_SYSREG(  6,  0, 3, 0, 0) },
//NO_RAW  { SREG_GROUP_SPR, NVMM_AARCH64_SPR_FPCR,                ENCODE_SYSREG(  4,  4, 3, 3, 0) },
//NO_RAW  { SREG_GROUP_SPR, NVMM_AARCH64_SPR_FPSR,                ENCODE_SYSREG(  4,  4, 3, 3, 1) },
//  { SREG_GROUP_SPR, NVMM_AARCH64_SPR_GCR_EL1,             ENCODE_SYSREG(  1,  0, 3, 0, 6) },
//  { SREG_GROUP_SPR, NVMM_AARCH64_SPR_GMID_EL1,            ENCODE_SYSREG(  0,  0, 3, 1, 4) },
    { SREG_GROUP_TID, NVMM_AARCH64_TID_ID_AA64AFR0_EL1,     ENCODE_SYSREG(  0,  5, 3, 0, 4) },
    { SREG_GROUP_TID, NVMM_AARCH64_TID_ID_AA64AFR1_EL1,     ENCODE_SYSREG(  0,  5, 3, 0, 5) },
    { SREG_GROUP_TID, NVMM_AARCH64_TID_ID_AA64DFR0_EL1,     ENCODE_SYSREG(  0,  5, 3, 0, 0) },
    { SREG_GROUP_TID, NVMM_AARCH64_TID_ID_AA64DFR1_EL1,     ENCODE_SYSREG(  0,  5, 3, 0, 1) },
    { SREG_GROUP_TID, NVMM_AARCH64_TID_ID_AA64ISAR0_EL1,    ENCODE_SYSREG(  0,  6, 3, 0, 0) },
    { SREG_GROUP_TID, NVMM_AARCH64_TID_ID_AA64ISAR1_EL1,    ENCODE_SYSREG(  0,  6, 3, 0, 1) },
    { SREG_GROUP_TID, NVMM_AARCH64_TID_ID_AA64MMFR0_EL1,    ENCODE_SYSREG(  0,  7, 3, 0, 0) },
    { SREG_GROUP_TID, NVMM_AARCH64_TID_ID_AA64MMFR1_EL1,    ENCODE_SYSREG(  0,  7, 3, 0, 1) },
    { SREG_GROUP_TID, NVMM_AARCH64_TID_ID_AA64MMFR2_EL1,    ENCODE_SYSREG(  0,  7, 3, 0, 2) },
//NO_RAW  { SREG_GROUP_TID, NVMM_AARCH64_TID_ID_AA64PFR0_EL1,     ENCODE_SYSREG(  0,  4, 3, 0, 0) },
//NO_RAW  { SREG_GROUP_TID, NVMM_AARCH64_TID_ID_AA64PFR1_EL1,     ENCODE_SYSREG(  0,  4, 3, 0, 1) },
//  { SREG_GROUP_TID, NVMM_AARCH64_TID_ID_AFR0_EL1,         ENCODE_SYSREG(  0,  1, 3, 0, 3) },
//  { SREG_GROUP_TID, NVMM_AARCH64_TID_ID_DFR0_EL1,         ENCODE_SYSREG(  0,  1, 3, 0, 2) },
//  { SREG_GROUP_TID, NVMM_AARCH64_TID_ID_DFR1_EL1,         ENCODE_SYSREG(  0,  3, 3, 0, 5) },
//  { SREG_GROUP_TID, NVMM_AARCH64_TID_ID_ISAR0_EL1,        ENCODE_SYSREG(  0,  2, 3, 0, 0) },
//  { SREG_GROUP_TID, NVMM_AARCH64_TID_ID_ISAR1_EL1,        ENCODE_SYSREG(  0,  2, 3, 0, 1) },
//  { SREG_GROUP_TID, NVMM_AARCH64_TID_ID_ISAR2_EL1,        ENCODE_SYSREG(  0,  2, 3, 0, 2) },
//  { SREG_GROUP_TID, NVMM_AARCH64_TID_ID_ISAR3_EL1,        ENCODE_SYSREG(  0,  2, 3, 0, 3) },
//  { SREG_GROUP_TID, NVMM_AARCH64_TID_ID_ISAR4_EL1,        ENCODE_SYSREG(  0,  2, 3, 0, 4) },
//  { SREG_GROUP_TID, NVMM_AARCH64_TID_ID_ISAR5_EL1,        ENCODE_SYSREG(  0,  2, 3, 0, 5) },
//  { SREG_GROUP_TID, NVMM_AARCH64_TID_ID_ISAR6_EL1,        ENCODE_SYSREG(  0,  2, 3, 0, 7) },
//  { SREG_GROUP_TID, NVMM_AARCH64_TID_ID_MMFR0_EL1,        ENCODE_SYSREG(  0,  1, 3, 0, 4) },
//  { SREG_GROUP_TID, NVMM_AARCH64_TID_ID_MMFR1_EL1,        ENCODE_SYSREG(  0,  1, 3, 0, 5) },
//  { SREG_GROUP_TID, NVMM_AARCH64_TID_ID_MMFR2_EL1,        ENCODE_SYSREG(  0,  1, 3, 0, 6) },
//  { SREG_GROUP_TID, NVMM_AARCH64_TID_ID_MMFR3_EL1,        ENCODE_SYSREG(  0,  1, 3, 0, 7) },
//  { SREG_GROUP_TID, NVMM_AARCH64_TID_ID_MMFR4_EL1,        ENCODE_SYSREG(  0,  2, 3, 0, 6) },
//  { SREG_GROUP_TID, NVMM_AARCH64_TID_ID_MMFR5_EL1,        ENCODE_SYSREG(  0,  3, 3, 0, 6) },
//  { SREG_GROUP_TID, NVMM_AARCH64_TID_ID_PFR0_EL1,         ENCODE_SYSREG(  0,  1, 3, 0, 0) },
//  { SREG_GROUP_TID, NVMM_AARCH64_TID_ID_PFR1_EL1,         ENCODE_SYSREG(  0,  1, 3, 0, 1) },
//  { SREG_GROUP_TID, NVMM_AARCH64_TID_ID_PFR2_EL1,         ENCODE_SYSREG(  0,  3, 3, 0, 4) },
//  { SREG_GROUP_SPR, NVMM_AARCH64_SPR_ISR_EL1,             ENCODE_SYSREG( 12,  1, 3, 0, 0) },
//  { SREG_GROUP_SPR, NVMM_AARCH64_SPR_LORC_EL1,            ENCODE_SYSREG( 10,  4, 3, 0, 3) },
//  { SREG_GROUP_SPR, NVMM_AARCH64_SPR_LOREA_EL1,           ENCODE_SYSREG( 10,  4, 3, 0, 1) },
//  { SREG_GROUP_SPR, NVMM_AARCH64_SPR_LORID_EL1,           ENCODE_SYSREG( 10,  4, 3, 0, 7) },
//  { SREG_GROUP_SPR, NVMM_AARCH64_SPR_LORN_EL1,            ENCODE_SYSREG( 10,  4, 3, 0, 2) },
//  { SREG_GROUP_SPR, NVMM_AARCH64_SPR_LORSA_EL1,           ENCODE_SYSREG( 10,  4, 3, 0, 0) },
    { SREG_GROUP_SPR, NVMM_AARCH64_SPR_MAIR_EL1,            ENCODE_SYSREG( 10,  2, 3, 0, 0) },
//  { SREG_GROUP_SPR, NVMM_AARCH64_SPR_MDCCINT_EL1,         ENCODE_SYSREG(  0,  2, 2, 0, 0) },
//  { SREG_GROUP_SPR, NVMM_AARCH64_SPR_MDCCSR_EL0,          ENCODE_SYSREG(  0,  1, 2, 3, 0) },
//  { SREG_GROUP_SPR, NVMM_AARCH64_SPR_MDRAR_EL1,           ENCODE_SYSREG(  1,  0, 2, 0, 0) },
    { SREG_GROUP_SPR, NVMM_AARCH64_SPR_MDSCR_EL1,           ENCODE_SYSREG(  0,  2, 2, 0, 2) },
//NO_RAW  { SREG_GROUP_SPR, NVMM_AARCH64_SPR_MIDR_EL1,            ENCODE_SYSREG(  0,  0, 3, 0, 0) },
//NO_RAW  { SREG_GROUP_SPR, NVMM_AARCH64_SPR_MPIDR_EL1,           ENCODE_SYSREG(  0,  0, 3, 0, 5) },
    { SREG_GROUP_TID, NVMM_AARCH64_TID_MVFR0_EL1,           ENCODE_SYSREG(  0,  3, 3, 0, 0) },
    { SREG_GROUP_TID, NVMM_AARCH64_TID_MVFR1_EL1,           ENCODE_SYSREG(  0,  3, 3, 0, 1) },
    { SREG_GROUP_TID, NVMM_AARCH64_TID_MVFR2_EL1,           ENCODE_SYSREG(  0,  3, 3, 0, 2) },
//  { SREG_GROUP_SPR, NVMM_AARCH64_SPR_NZCV,                ENCODE_SYSREG(  4,  2, 3, 3, 0) },
//  { SREG_GROUP_SPR, NVMM_AARCH64_SPR_OSDLR_EL1,           ENCODE_SYSREG(  1,  3, 2, 0, 4) },
//  { SREG_GROUP_SPR, NVMM_AARCH64_SPR_OSDTRRX_EL1,         ENCODE_SYSREG(  0,  0, 2, 0, 2) },
//  { SREG_GROUP_SPR, NVMM_AARCH64_SPR_OSDTRTX_EL1,         ENCODE_SYSREG(  0,  3, 2, 0, 2) },
//  { SREG_GROUP_SPR, NVMM_AARCH64_SPR_OSECCR_EL1,          ENCODE_SYSREG(  0,  6, 2, 0, 2) },
//  { SREG_GROUP_SPR, NVMM_AARCH64_SPR_OSLAR_EL1,           ENCODE_SYSREG(  1,  0, 2, 0, 4) },
//  { SREG_GROUP_SPR, NVMM_AARCH64_SPR_OSLSR_EL1,           ENCODE_SYSREG(  1,  1, 2, 0, 4) },
//  { SREG_GROUP_SPR, NVMM_AARCH64_SPR_PAN,                 ENCODE_SYSREG(  4,  2, 3, 0, 3) },
    { SREG_GROUP_SPR, NVMM_AARCH64_SPR_PAR_EL1,             ENCODE_SYSREG(  7,  4, 3, 0, 0) },
//  { SREG_GROUP_SPR, NVMM_AARCH64_SPR_PMCCFILTR_EL0,       ENCODE_SYSREG( 14, 15, 3, 3, 7) },
//  { SREG_GROUP_SPR, NVMM_AARCH64_SPR_PMCCNTR_EL0,         ENCODE_SYSREG(  9, 13, 3, 3, 0) },
//  { SREG_GROUP_SPR, NVMM_AARCH64_SPR_PMCEID0_EL0,         ENCODE_SYSREG(  9, 12, 3, 3, 6) },
//  { SREG_GROUP_SPR, NVMM_AARCH64_SPR_PMCEID1_EL0,         ENCODE_SYSREG(  9, 12, 3, 3, 7) },
//  { SREG_GROUP_SPR, NVMM_AARCH64_SPR_PMCNTENCLR_EL0,      ENCODE_SYSREG(  9, 12, 3, 3, 2) },
//  { SREG_GROUP_SPR, NVMM_AARCH64_SPR_PMCNTENSET_EL0,      ENCODE_SYSREG(  9, 12, 3, 3, 1) },
//  { SREG_GROUP_SPR, NVMM_AARCH64_SPR_PMCR_EL0,            ENCODE_SYSREG(  9, 12, 3, 3, 0) },
//  { SREG_GROUP_SPR, NVMM_AARCH64_SPR_PMEVCNTR0_EL0,       ENCODE_SYSREG( 14,  8, 3, 3, 0) },
//  { SREG_GROUP_SPR, NVMM_AARCH64_SPR_PMEVCNTR10_EL0,      ENCODE_SYSREG( 14,  9, 3, 3, 2) },
//  { SREG_GROUP_SPR, NVMM_AARCH64_SPR_PMEVCNTR11_EL0,      ENCODE_SYSREG( 14,  9, 3, 3, 3) },
//  { SREG_GROUP_SPR, NVMM_AARCH64_SPR_PMEVCNTR12_EL0,      ENCODE_SYSREG( 14,  9, 3, 3, 4) },
//  { SREG_GROUP_SPR, NVMM_AARCH64_SPR_PMEVCNTR13_EL0,      ENCODE_SYSREG( 14,  9, 3, 3, 5) },
//  { SREG_GROUP_SPR, NVMM_AARCH64_SPR_PMEVCNTR14_EL0,      ENCODE_SYSREG( 14,  9, 3, 3, 6) },
//  { SREG_GROUP_SPR, NVMM_AARCH64_SPR_PMEVCNTR15_EL0,      ENCODE_SYSREG( 14,  9, 3, 3, 7) },
//  { SREG_GROUP_SPR, NVMM_AARCH64_SPR_PMEVCNTR16_EL0,      ENCODE_SYSREG( 14, 10, 3, 3, 0) },
//  { SREG_GROUP_SPR, NVMM_AARCH64_SPR_PMEVCNTR17_EL0,      ENCODE_SYSREG( 14, 10, 3, 3, 1) },
//  { SREG_GROUP_SPR, NVMM_AARCH64_SPR_PMEVCNTR18_EL0,      ENCODE_SYSREG( 14, 10, 3, 3, 2) },
//  { SREG_GROUP_SPR, NVMM_AARCH64_SPR_PMEVCNTR19_EL0,      ENCODE_SYSREG( 14, 10, 3, 3, 3) },
//  { SREG_GROUP_SPR, NVMM_AARCH64_SPR_PMEVCNTR1_EL0,       ENCODE_SYSREG( 14,  8, 3, 3, 1) },
//  { SREG_GROUP_SPR, NVMM_AARCH64_SPR_PMEVCNTR20_EL0,      ENCODE_SYSREG( 14, 10, 3, 3, 4) },
//  { SREG_GROUP_SPR, NVMM_AARCH64_SPR_PMEVCNTR21_EL0,      ENCODE_SYSREG( 14, 10, 3, 3, 5) },
//  { SREG_GROUP_SPR, NVMM_AARCH64_SPR_PMEVCNTR22_EL0,      ENCODE_SYSREG( 14, 10, 3, 3, 6) },
//  { SREG_GROUP_SPR, NVMM_AARCH64_SPR_PMEVCNTR23_EL0,      ENCODE_SYSREG( 14, 10, 3, 3, 7) },
//  { SREG_GROUP_SPR, NVMM_AARCH64_SPR_PMEVCNTR24_EL0,      ENCODE_SYSREG( 14, 11, 3, 3, 0) },
//  { SREG_GROUP_SPR, NVMM_AARCH64_SPR_PMEVCNTR25_EL0,      ENCODE_SYSREG( 14, 11, 3, 3, 1) },
//  { SREG_GROUP_SPR, NVMM_AARCH64_SPR_PMEVCNTR26_EL0,      ENCODE_SYSREG( 14, 11, 3, 3, 2) },
//  { SREG_GROUP_SPR, NVMM_AARCH64_SPR_PMEVCNTR27_EL0,      ENCODE_SYSREG( 14, 11, 3, 3, 3) },
//  { SREG_GROUP_SPR, NVMM_AARCH64_SPR_PMEVCNTR28_EL0,      ENCODE_SYSREG( 14, 11, 3, 3, 4) },
//  { SREG_GROUP_SPR, NVMM_AARCH64_SPR_PMEVCNTR29_EL0,      ENCODE_SYSREG( 14, 11, 3, 3, 5) },
//  { SREG_GROUP_SPR, NVMM_AARCH64_SPR_PMEVCNTR2_EL0,       ENCODE_SYSREG( 14,  8, 3, 3, 2) },
//  { SREG_GROUP_SPR, NVMM_AARCH64_SPR_PMEVCNTR30_EL0,      ENCODE_SYSREG( 14, 11, 3, 3, 6) },
//  { SREG_GROUP_SPR, NVMM_AARCH64_SPR_PMEVCNTR3_EL0,       ENCODE_SYSREG( 14,  8, 3, 3, 3) },
//  { SREG_GROUP_SPR, NVMM_AARCH64_SPR_PMEVCNTR4_EL0,       ENCODE_SYSREG( 14,  8, 3, 3, 4) },
//  { SREG_GROUP_SPR, NVMM_AARCH64_SPR_PMEVCNTR5_EL0,       ENCODE_SYSREG( 14,  8, 3, 3, 5) },
//  { SREG_GROUP_SPR, NVMM_AARCH64_SPR_PMEVCNTR6_EL0,       ENCODE_SYSREG( 14,  8, 3, 3, 6) },
//  { SREG_GROUP_SPR, NVMM_AARCH64_SPR_PMEVCNTR7_EL0,       ENCODE_SYSREG( 14,  8, 3, 3, 7) },
//  { SREG_GROUP_SPR, NVMM_AARCH64_SPR_PMEVCNTR8_EL0,       ENCODE_SYSREG( 14,  9, 3, 3, 0) },
//  { SREG_GROUP_SPR, NVMM_AARCH64_SPR_PMEVCNTR9_EL0,       ENCODE_SYSREG( 14,  9, 3, 3, 1) },
//  { SREG_GROUP_SPR, NVMM_AARCH64_SPR_PMEVTYPER0_EL0,      ENCODE_SYSREG( 14, 12, 3, 3, 0) },
//  { SREG_GROUP_SPR, NVMM_AARCH64_SPR_PMEVTYPER10_EL0,     ENCODE_SYSREG( 14, 13, 3, 3, 2) },
//  { SREG_GROUP_SPR, NVMM_AARCH64_SPR_PMEVTYPER11_EL0,     ENCODE_SYSREG( 14, 13, 3, 3, 3) },
//  { SREG_GROUP_SPR, NVMM_AARCH64_SPR_PMEVTYPER12_EL0,     ENCODE_SYSREG( 14, 13, 3, 3, 4) },
//  { SREG_GROUP_SPR, NVMM_AARCH64_SPR_PMEVTYPER13_EL0,     ENCODE_SYSREG( 14, 13, 3, 3, 5) },
//  { SREG_GROUP_SPR, NVMM_AARCH64_SPR_PMEVTYPER14_EL0,     ENCODE_SYSREG( 14, 13, 3, 3, 6) },
//  { SREG_GROUP_SPR, NVMM_AARCH64_SPR_PMEVTYPER15_EL0,     ENCODE_SYSREG( 14, 13, 3, 3, 7) },
//  { SREG_GROUP_SPR, NVMM_AARCH64_SPR_PMEVTYPER16_EL0,     ENCODE_SYSREG( 14, 14, 3, 3, 0) },
//  { SREG_GROUP_SPR, NVMM_AARCH64_SPR_PMEVTYPER17_EL0,     ENCODE_SYSREG( 14, 14, 3, 3, 1) },
//  { SREG_GROUP_SPR, NVMM_AARCH64_SPR_PMEVTYPER18_EL0,     ENCODE_SYSREG( 14, 14, 3, 3, 2) },
//  { SREG_GROUP_SPR, NVMM_AARCH64_SPR_PMEVTYPER19_EL0,     ENCODE_SYSREG( 14, 14, 3, 3, 3) },
//  { SREG_GROUP_SPR, NVMM_AARCH64_SPR_PMEVTYPER1_EL0,      ENCODE_SYSREG( 14, 12, 3, 3, 1) },
//  { SREG_GROUP_SPR, NVMM_AARCH64_SPR_PMEVTYPER20_EL0,     ENCODE_SYSREG( 14, 14, 3, 3, 4) },
//  { SREG_GROUP_SPR, NVMM_AARCH64_SPR_PMEVTYPER21_EL0,     ENCODE_SYSREG( 14, 14, 3, 3, 5) },
//  { SREG_GROUP_SPR, NVMM_AARCH64_SPR_PMEVTYPER22_EL0,     ENCODE_SYSREG( 14, 14, 3, 3, 6) },
//  { SREG_GROUP_SPR, NVMM_AARCH64_SPR_PMEVTYPER23_EL0,     ENCODE_SYSREG( 14, 14, 3, 3, 7) },
//  { SREG_GROUP_SPR, NVMM_AARCH64_SPR_PMEVTYPER24_EL0,     ENCODE_SYSREG( 14, 15, 3, 3, 0) },
//  { SREG_GROUP_SPR, NVMM_AARCH64_SPR_PMEVTYPER25_EL0,     ENCODE_SYSREG( 14, 15, 3, 3, 1) },
//  { SREG_GROUP_SPR, NVMM_AARCH64_SPR_PMEVTYPER26_EL0,     ENCODE_SYSREG( 14, 15, 3, 3, 2) },
//  { SREG_GROUP_SPR, NVMM_AARCH64_SPR_PMEVTYPER27_EL0,     ENCODE_SYSREG( 14, 15, 3, 3, 3) },
//  { SREG_GROUP_SPR, NVMM_AARCH64_SPR_PMEVTYPER28_EL0,     ENCODE_SYSREG( 14, 15, 3, 3, 4) },
//  { SREG_GROUP_SPR, NVMM_AARCH64_SPR_PMEVTYPER29_EL0,     ENCODE_SYSREG( 14, 15, 3, 3, 5) },
//  { SREG_GROUP_SPR, NVMM_AARCH64_SPR_PMEVTYPER2_EL0,      ENCODE_SYSREG( 14, 12, 3, 3, 2) },
//  { SREG_GROUP_SPR, NVMM_AARCH64_SPR_PMEVTYPER30_EL0,     ENCODE_SYSREG( 14, 15, 3, 3, 6) },
//  { SREG_GROUP_SPR, NVMM_AARCH64_SPR_PMEVTYPER3_EL0,      ENCODE_SYSREG( 14, 12, 3, 3, 3) },
//  { SREG_GROUP_SPR, NVMM_AARCH64_SPR_PMEVTYPER4_EL0,      ENCODE_SYSREG( 14, 12, 3, 3, 4) },
//  { SREG_GROUP_SPR, NVMM_AARCH64_SPR_PMEVTYPER5_EL0,      ENCODE_SYSREG( 14, 12, 3, 3, 5) },
//  { SREG_GROUP_SPR, NVMM_AARCH64_SPR_PMEVTYPER6_EL0,      ENCODE_SYSREG( 14, 12, 3, 3, 6) },
//  { SREG_GROUP_SPR, NVMM_AARCH64_SPR_PMEVTYPER7_EL0,      ENCODE_SYSREG( 14, 12, 3, 3, 7) },
//  { SREG_GROUP_SPR, NVMM_AARCH64_SPR_PMEVTYPER8_EL0,      ENCODE_SYSREG( 14, 13, 3, 3, 0) },
//  { SREG_GROUP_SPR, NVMM_AARCH64_SPR_PMEVTYPER9_EL0,      ENCODE_SYSREG( 14, 13, 3, 3, 1) },
//  { SREG_GROUP_SPR, NVMM_AARCH64_SPR_PMINTENCLR_EL1,      ENCODE_SYSREG(  9, 14, 3, 0, 2) },
//  { SREG_GROUP_SPR, NVMM_AARCH64_SPR_PMINTENSET_EL1,      ENCODE_SYSREG(  9, 14, 3, 0, 1) },
//  { SREG_GROUP_SPR, NVMM_AARCH64_SPR_PMOVSCLR_EL0,        ENCODE_SYSREG(  9, 12, 3, 3, 3) },
//  { SREG_GROUP_SPR, NVMM_AARCH64_SPR_PMOVSSET_EL0,        ENCODE_SYSREG(  9, 14, 3, 3, 3) },
//  { SREG_GROUP_SPR, NVMM_AARCH64_SPR_PMSELR_EL0,          ENCODE_SYSREG(  9, 12, 3, 3, 5) },
//  { SREG_GROUP_SPR, NVMM_AARCH64_SPR_PMSWINC_EL0,         ENCODE_SYSREG(  9, 12, 3, 3, 4) },
//  { SREG_GROUP_SPR, NVMM_AARCH64_SPR_PMUSERENR_EL0,       ENCODE_SYSREG(  9, 14, 3, 3, 0) },
//  { SREG_GROUP_SPR, NVMM_AARCH64_SPR_PMXEVCNTR_EL0,       ENCODE_SYSREG(  9, 13, 3, 3, 2) },
//  { SREG_GROUP_SPR, NVMM_AARCH64_SPR_PMXEVTYPER_EL0,      ENCODE_SYSREG(  9, 13, 3, 3, 1) },
//  { SREG_GROUP_SPR, NVMM_AARCH64_SPR_REVIDR_EL1,          ENCODE_SYSREG(  0,  0, 3, 0, 6) },
//  { SREG_GROUP_SPR, NVMM_AARCH64_SPR_RGSR_EL1,            ENCODE_SYSREG(  1,  0, 3, 0, 5) },
//  { SREG_GROUP_SPR, NVMM_AARCH64_SPR_RMR_EL1,             ENCODE_SYSREG( 12,  0, 3, 0, 2) },
//  { SREG_GROUP_SPR, NVMM_AARCH64_SPR_RNDRRS,              ENCODE_SYSREG(  2,  4, 3, 3, 1) },
//  { SREG_GROUP_SPR, NVMM_AARCH64_SPR_RNDR,                ENCODE_SYSREG(  2,  4, 3, 3, 0) },
//  { SREG_GROUP_SPR, NVMM_AARCH64_SPR_RVBAR_EL1,           ENCODE_SYSREG( 12,  0, 3, 0, 1) },
    { SREG_GROUP_SPR, NVMM_AARCH64_SPR_SCTLR_EL1,           ENCODE_SYSREG(  1,  0, 3, 0, 0) },
//  { SREG_GROUP_SPR, NVMM_AARCH64_SPR_SCXTNUM_EL0,         ENCODE_SYSREG( 13,  0, 3, 3, 7) },
//  { SREG_GROUP_SPR, NVMM_AARCH64_SPR_SCXTNUM_EL1,         ENCODE_SYSREG( 13,  0, 3, 0, 7) },
//  { SREG_GROUP_SPR, NVMM_AARCH64_SPR_SPSEL,               ENCODE_SYSREG(  4,  2, 3, 0, 0) },
//  { SREG_GROUP_SPR, NVMM_AARCH64_SPR_SPSR_ABT,            ENCODE_SYSREG(  4,  3, 3, 4, 1) },
//  { SREG_GROUP_SPR, NVMM_AARCH64_SPR_SPSR_EL1,            ENCODE_SYSREG(  4,  0, 3, 0, 0) },
//  { SREG_GROUP_SPR, NVMM_AARCH64_SPR_SPSR_FIQ,            ENCODE_SYSREG(  4,  3, 3, 4, 3) },
//  { SREG_GROUP_SPR, NVMM_AARCH64_SPR_SPSR_IRQ,            ENCODE_SYSREG(  4,  3, 3, 4, 0) },
//  { SREG_GROUP_SPR, NVMM_AARCH64_SPR_SPSR_UND,            ENCODE_SYSREG(  4,  3, 3, 4, 2) },
    { SREG_GROUP_SPR, NVMM_AARCH64_SPR_SP_EL0,              ENCODE_SYSREG(  4,  1, 3, 0, 0) },
    { SREG_GROUP_SPR, NVMM_AARCH64_SPR_SP_EL1,              ENCODE_SYSREG(  4,  1, 3, 4, 0) },
//  { SREG_GROUP_SPR, NVMM_AARCH64_SPR_SSBS,                ENCODE_SYSREG(  4,  2, 3, 3, 6) },
//  { SREG_GROUP_SPR, NVMM_AARCH64_SPR_TCO,                 ENCODE_SYSREG(  4,  2, 3, 3, 7) },
    { SREG_GROUP_SPR, NVMM_AARCH64_SPR_TCR_EL1,             ENCODE_SYSREG(  2,  0, 3, 0, 2) },
//  { SREG_GROUP_SPR, NVMM_AARCH64_SPR_TEECR32_EL1,         ENCODE_SYSREG(  0,  0, 2, 2, 0) },
//  { SREG_GROUP_SPR, NVMM_AARCH64_SPR_TEEHBR32_EL1,        ENCODE_SYSREG(  1,  0, 2, 2, 0) },
//  { SREG_GROUP_SPR, NVMM_AARCH64_SPR_TFSRE0_EL1,          ENCODE_SYSREG(  5,  6, 3, 0, 1) },
//  { SREG_GROUP_SPR, NVMM_AARCH64_SPR_TFSR_EL1,            ENCODE_SYSREG(  5,  6, 3, 0, 0) },
    { SREG_GROUP_SPR, NVMM_AARCH64_SPR_TPIDRRO_EL0,         ENCODE_SYSREG( 13,  0, 3, 3, 3) },
    { SREG_GROUP_SPR, NVMM_AARCH64_SPR_TPIDR_EL0,           ENCODE_SYSREG( 13,  0, 3, 3, 2) },
    { SREG_GROUP_SPR, NVMM_AARCH64_SPR_TPIDR_EL1,           ENCODE_SYSREG( 13,  0, 3, 0, 4) },
    { SREG_GROUP_SPR, NVMM_AARCH64_SPR_TTBR0_EL1,           ENCODE_SYSREG(  2,  0, 3, 0, 0) },
    { SREG_GROUP_SPR, NVMM_AARCH64_SPR_TTBR1_EL1,           ENCODE_SYSREG(  2,  0, 3, 0, 1) },
//  { SREG_GROUP_SPR, NVMM_AARCH64_SPR_UAO,                 ENCODE_SYSREG(  4,  2, 3, 0, 4) },
    { SREG_GROUP_SPR, NVMM_AARCH64_SPR_VBAR_EL1,            ENCODE_SYSREG( 12,  0, 3, 0, 0) },
};

/* QEMU -> NVMM */
void
nvmm_set_registers(CPUState *cpu)
{
    ARMCPU *arm_cpu = ARM_CPU(cpu);
    CPUArchState *env = &arm_cpu->env;
    struct nvmm_machine *mach = get_nvmm_mach();
    struct qemu_vcpu *qcpu = nvmm_get_qemu_vcpu(cpu);
    struct nvmm_vcpu *vcpu = &qcpu->vcpu;
    struct nvmm_aarch64_state *state = vcpu->state;
    int i, ret;

    assert(cpu_is_stopped(cpu) || qemu_cpu_is_self(cpu));

    /* GPRs */
    for (i = 0; i < 32; i++) {
        state->gprs[i] = env->xregs[i];
    }
    state->sprs[NVMM_AARCH64_SPR_SP_EL0] = env->sp_el[0];
    state->sprs[NVMM_AARCH64_SPR_SP_EL1] = env->sp_el[1];
    state->sprs[NVMM_AARCH64_SPR_PC] = env->pc;
    state->sprs[NVMM_AARCH64_SPR_SPSR_EL1] = pstate_read(env);

    /* FPRs */
    for (i = 0; i < 32; i++) {
        memcpy(&state->fprs[i], &env->vfp.zregs[i], sizeof(state->fprs[i]));
    }
    state->sprs[NVMM_AARCH64_SPR_FPCR] = vfp_get_fpcr(env);
    state->sprs[NVMM_AARCH64_SPR_FPSR] = vfp_get_fpsr(env);

    /* SPRs */
    assert(write_cpustate_to_list(arm_cpu, false));
    for (i = 0; i < ARRAY_SIZE(nvmm_sreg_match); i++) {
        if (nvmm_sreg_match[i].cp_idx == -1) {
            continue;
        }
        uint64_t val = arm_cpu->cpreg_values[nvmm_sreg_match[i].cp_idx];
        switch (nvmm_sreg_match[i].group) {
        case SREG_GROUP_SPR:
            state->sprs[nvmm_sreg_match[i].reg] = val;
            break;
        case SREG_GROUP_TID:
            state->tids[nvmm_sreg_match[i].reg] = val;
            break;
        default:
            assert(false);
        }
    }

    //XXXXXXX vtimer

    ret = nvmm_vcpu_setstate(mach, vcpu, NVMM_AARCH64_STATE_ALL);
    if (ret == -1) {
        error_report("NVMM: Failed to set virtual processor context,"
            " error=%d", errno);
    }
}

/* NVMM -> QEMU */
void
nvmm_get_registers(CPUState *cpu)
{
    ARMCPU *arm_cpu = ARM_CPU(cpu);
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

    /* GPRs */
    for (i = 0; i < 32; i++) {
        env->xregs[i] = state->gprs[i];
    }
    env->sp_el[0] = state->sprs[NVMM_AARCH64_SPR_SP_EL0];
    env->sp_el[1] = state->sprs[NVMM_AARCH64_SPR_SP_EL1];
    env->pc = state->sprs[NVMM_AARCH64_SPR_PC];
    pstate_write(env, state->sprs[NVMM_AARCH64_SPR_SPSR_EL1]);

    /* FPRs */
    for (i = 0; i < 32; i++) {
        memcpy(&env->vfp.zregs[i], &state->fprs[i], sizeof(state->fprs[i]));
    }
    vfp_set_fpcr(env, state->sprs[NVMM_AARCH64_SPR_FPCR]);
    vfp_set_fpsr(env, state->sprs[NVMM_AARCH64_SPR_FPSR]);

    /* SPRs */
    for (i = 0; i < ARRAY_SIZE(nvmm_sreg_match); i++) {
        if (nvmm_sreg_match[i].cp_idx == -1) {
            continue;
        }

        uint64_t val;
        switch (nvmm_sreg_match[i].group) {
        case SREG_GROUP_SPR:
            val = state->sprs[nvmm_sreg_match[i].reg];
            break;
        case SREG_GROUP_TID:
            val = state->tids[nvmm_sreg_match[i].reg];
            break;
        default:
            assert(false);
        }
        arm_cpu->cpreg_values[nvmm_sreg_match[i].cp_idx] = val;
    }
    assert(write_list_to_cpustate(arm_cpu));
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
	abort();

	return 0;
}

static void
nvmm_wfi(CPUState *cpu)
{
//    ARMCPU *arm_cpu = ARM_CPU(cpu);

    if (cpu->interrupt_request & (CPU_INTERRUPT_HARD | CPU_INTERRUPT_FIQ)) {
        /* Interrupt pending, no need to wait */
        return;
    }

//XXXXX
    return;

//    qatomic_mb_set(&cpu->thread_kicked, false);
//    qemu_mutex_unlock_iothread();
//    pselect(0, 0, 0, 0, NULL, &cpu->hvf->unblock_ipi_mask);
//    qemu_mutex_lock_iothread();
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

        case NVMM_VCPU_EXIT_MRS:
            error_report("XXXXXX: NOTYET: NVMM_VCPU_EXIT_MRS\n");
            abort();
            break;
        case NVMM_VCPU_EXIT_MSR:
            error_report("XXXXXX: NOTYET: NVMM_VCPU_EXIT_MSR\n");
            abort();
            break;
        case NVMM_VCPU_EXIT_HVC:
            error_report("XXXXXX: NOTYET: NVMM_VCPU_EXIT_HVC\n");
            abort();
            break;
        case NVMM_VCPU_EXIT_SMC:
            error_report("XXXXXX: NOTYET: NVMM_VCPU_EXIT_SMC\n");
            abort();
            break;
        case NVMM_VCPU_EXIT_WFI:
            nvmm_wfi(cpu);
            break;
        case NVMM_VCPU_EXIT_WFE:
            error_report("XXXXXX: NOTYET: NVMM_VCPU_EXIT_WFE\n");
            break;

        case NVMM_VCPU_EXIT_HALTED:
            ret = nvmm_handle_halted(mach, cpu, exit);
            break;
        case NVMM_VCPU_EXIT_SHUTDOWN:
            qemu_system_reset_request(SHUTDOWN_CAUSE_GUEST_RESET);
            cpu->exception_index = EXCP_INTERRUPT;
            ret = 1;
            break;
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

static void
nvmm_sreg_init(CPUState *cpu)
{
    ARMCPU *arm_cpu = ARM_CPU(cpu);
    CPUArchState *env = &arm_cpu->env;
    uint32_t sregs_match_len = ARRAY_SIZE(nvmm_sreg_match);
    uint32_t sregs_cnt = 0;

    env->aarch64 = true;
    asm volatile("mrs %0, cntfrq_el0" : "=r"(arm_cpu->gt_cntfrq_hz));

    /* Allocate enough space for our sysreg sync */
    arm_cpu->cpreg_indexes = g_renew(uint64_t, arm_cpu->cpreg_indexes,
                                     sregs_match_len);
    arm_cpu->cpreg_values = g_renew(uint64_t, arm_cpu->cpreg_values,
                                    sregs_match_len);
    arm_cpu->cpreg_vmstate_indexes = g_renew(uint64_t,
                                             arm_cpu->cpreg_vmstate_indexes,
                                             sregs_match_len);
    arm_cpu->cpreg_vmstate_values = g_renew(uint64_t,
                                            arm_cpu->cpreg_vmstate_values,
                                            sregs_match_len);

    memset(arm_cpu->cpreg_values, 0, sregs_match_len * sizeof(uint64_t));

    /* Populate cp list for all known sysregs */
    for (int i = 0; i < sregs_match_len; i++) {
        const ARMCPRegInfo *ri;
        uint32_t key = nvmm_sreg_match[i].key;

        ri = get_arm_cp_reginfo(arm_cpu->cp_regs, key);
        if (ri) {
// printf("group=%d, reg=%d\n", nvmm_sreg_match[i].group, nvmm_sreg_match[i].reg);
            assert(!(ri->type & ARM_CP_NO_RAW));
            nvmm_sreg_match[i].cp_idx = sregs_cnt;
            arm_cpu->cpreg_indexes[sregs_cnt++] = cpreg_to_kvm_id(key);
        } else {
            nvmm_sreg_match[i].cp_idx = -1;
        }
    }
    arm_cpu->cpreg_array_len = sregs_cnt;
    arm_cpu->cpreg_vmstate_array_len = sregs_cnt;

    assert(write_cpustate_to_list(arm_cpu, false));
}


typedef struct ARMHostCPUFeatures {
    ARMISARegisters isar;
    uint64_t features;
    uint64_t midr;
    uint32_t reset_sctlr;
    const char *dtb_compatible;
} ARMHostCPUFeatures;

static ARMHostCPUFeatures arm_host_cpu_features;

void
nvmm_arm_set_cpu_features_from_host(ARMCPU *cpu)
{
    if (arm_host_cpu_features.dtb_compatible == NULL) {
        char path[128];
        size_t len;
        uint64_t sysctlbuf[64];
        struct aarch64_sysctl_cpu_id *id =
            (struct aarch64_sysctl_cpu_id *)sysctlbuf;

        /*
         * XXX: It is necessary to get "machdep.cpuN.cpu_id" of all CPUs,
         * and select the minimum feature.
         */
        snprintf(path, sizeof(path), "machdep.cpu%d.cpu_id", 0);
        len = sizeof(sysctlbuf);
        memset(sysctlbuf, 0, len);
        if (sysctlbyname(path, id, &len, 0, 0) == -1) {
            error_report("couldn't get %s", path);
            return;
        }
        if (len != sizeof(struct aarch64_sysctl_cpu_id)) {
            error_report("Warning: kernel version bumped?");
            return;
        }

        arm_host_cpu_features.midr = id->ac_midr;
        arm_host_cpu_features.isar.id_aa64dfr0 = id->ac_aa64dfr0;
        arm_host_cpu_features.isar.id_aa64dfr1 = id->ac_aa64dfr1;
        arm_host_cpu_features.isar.id_aa64isar0 = id->ac_aa64isar0;
        arm_host_cpu_features.isar.id_aa64isar1 = id->ac_aa64isar1;
        arm_host_cpu_features.isar.id_aa64mmfr0 = id->ac_aa64mmfr0;
        arm_host_cpu_features.isar.id_aa64mmfr1 = id->ac_aa64mmfr1;
        arm_host_cpu_features.isar.id_aa64mmfr2 = id->ac_aa64mmfr2;
        arm_host_cpu_features.isar.id_aa64pfr0 = id->ac_aa64pfr0;
        arm_host_cpu_features.isar.id_aa64pfr1 = id->ac_aa64pfr1;
        arm_host_cpu_features.isar.id_aa64zfr0 = id->ac_aa64zfr0;
        arm_host_cpu_features.isar.mvfr0 = id->ac_mvfr0;
        arm_host_cpu_features.isar.mvfr1 = id->ac_mvfr1;
        arm_host_cpu_features.isar.mvfr2 = id->ac_mvfr2;

        arm_host_cpu_features.isar.id_aa64smfr0 = 0;
        arm_host_cpu_features.isar.reset_pmcr_el0 = 0;
        arm_host_cpu_features.reset_sctlr = SCTLR_RES1;

        arm_host_cpu_features.dtb_compatible = "arm,arm-v8";
        arm_host_cpu_features.features = (1ULL << ARM_FEATURE_V8) |
                                         (1ULL << ARM_FEATURE_NEON) |
                                         (1ULL << ARM_FEATURE_AARCH64) |
//                                       (1ULL << ARM_FEATURE_PMU) |
                                         (1ULL << ARM_FEATURE_GENERIC_TIMER);
    }

    /* arm_host_cpu_features to cpu */
    cpu->dtb_compatible = arm_host_cpu_features.dtb_compatible;
    cpu->isar = arm_host_cpu_features.isar;
    cpu->env.features = arm_host_cpu_features.features;
    cpu->midr = arm_host_cpu_features.midr;
    cpu->reset_sctlr = arm_host_cpu_features.reset_sctlr;
}

int
nvmm_init_vcpu(CPUState *cpu)
{
    struct nvmm_machine *mach = get_nvmm_mach();
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

    nvmm_sreg_init(cpu);

    /* Set CP_NO_RAW system registers on init */
    struct nvmm_aarch64_state *state = qcpu->vcpu.state;
    ARMCPU *arm_cpu = ARM_CPU(cpu);
    ret = nvmm_vcpu_getstate(mach, &qcpu->vcpu, NVMM_AARCH64_STATE_ALL);

    state->sprs[NVMM_AARCH64_SPR_MIDR_EL1] = arm_cpu->midr;
    state->sprs[NVMM_AARCH64_SPR_MPIDR_EL1] = (1U << 31) | arm_cpu->mp_affinity;

    CPUArchState *env = &arm_cpu->env;
    uint64_t pfr = state->tids[NVMM_AARCH64_TID_ID_AA64PFR0_EL1];
    pfr &= ~ID_AA64PFR0_EL1_GIC;
    pfr |= __SHIFTIN(env->gicv3state ? 1 : 0, ID_AA64PFR0_EL1_GIC);
    state->tids[NVMM_AARCH64_TID_ID_AA64PFR0_EL1] = pfr;

    /* We're limited to underlying hardware caps, override internal versions */
    arm_cpu->isar.id_aa64mmfr0 = state->tids[NVMM_AARCH64_TID_ID_AA64MMFR0_EL1];
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
