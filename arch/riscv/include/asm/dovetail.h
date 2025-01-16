/*
 * SPDX-License-Identifier: GPL-2.0
 *
 * Copyright (C) 2024 shanmu.
 */
#ifndef _ASM_RISCV_DOVETAIL_H
#define _ASM_RISCV_DOVETAIL_H

// FIXME: USE `arch/riscv/include/asm/csr.h`
/* RISCV traps */
#define RISCV_TRAP_INSN_MISALIGNED		0
#define RISCV_TRAP_INSN_FAULT	1
#define RISCV_TRAP_INSN_ILLEGAL	2
#define RISCV_TRAP_BREAK		3
#define RISCV_TRAP_LOAD_MISALIGNED	4
#define RISCV_TRAP_LOAD_FAULT		5
#define RISCV_TRAP_STORE_MISALIGNED	6
#define RISCV_TRAP_STORE_FAULT		7
#define RISCV_TRAP_ECALL_S		8
#define RISCV_TRAP_UNKNOWN		9
#define RISCV_TRAP_ECALL_M		10
#define RISCV_TRAP_PAGE_FAULT		11


#if !defined(__ASSEMBLY__) && defined(CONFIG_DOVETAIL)

static inline void arch_dovetail_exec_prepare(void)
{ }

static inline
void arch_dovetail_switch_prepare(bool leave_inband)
{ }

static inline
void arch_dovetail_switch_finish(bool enter_inband)
{ }

#define arch_dovetail_is_prctl(__nr)	\
	((__nr) == __NR_prctl)

#endif	/* !__ASSEMBLY__ && CONFIG_DOVETAIL */

#endif /* _ASM_RISCV_DOVETAIL_H */
