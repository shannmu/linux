/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _EVL_RISCV_ASM_THREAD_H
#define _EVL_RISCV_ASM_THREAD_H

static inline bool evl_is_breakpoint(int trapnr)
{
	return trapnr == RISCV_TRAP_BREAK;
}

#endif /* !_EVL_RISCV_ASM_THREAD_H */
