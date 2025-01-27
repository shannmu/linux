/* SPDX-License-Identifier: GPL-2.0
 *
 * IRQ Pipelining adapted from the ARM version.
 *
 * Copyright (C) 2024 Siemens AG
 * Author:       Tobias Schaffner <tobias.schaffner@siemens.com>.
 */

#ifndef _ASM_RISCV_IRQ_PIPELINE_H
#define _ASM_RISCV_IRQ_PIPELINE_H

#define OOB_NR_IPI		3


extern int ipi_virq_base;
extern int NR_CALL_FUNCTION_OOB_IPI;
extern int NR_RESCHEDULE_OOB_IPI;
extern int NR_TIMER_OOB_IPI;
extern int ipi_max;

#define CALL_FUNCTION_OOB_IPI (ipi_virq_base + NR_CALL_FUNCTION_OOB_IPI)
#define RESCHEDULE_OOB_IPI (ipi_virq_base + NR_RESCHEDULE_OOB_IPI)
#define TIMER_OOB_IPI (ipi_virq_base + NR_TIMER_OOB_IPI)
#define PIPELINED_IPI_MAX ipi_max

#ifdef CONFIG_IRQ_PIPELINE

#include <asm/ptrace.h>

/* NOTE: Any bit should be fine as long as we don't hit SR_SIE or SR_MIE. */
#define IRQMASK_i_POS   31

static inline notrace
unsigned long arch_irqs_virtual_to_native_flags(int stalled)
{
	if (!stalled)
		return SR_IE;
	return 0;
}

static inline notrace
unsigned long arch_irqs_native_to_virtual_flags(unsigned long flags)
{
	return (!!native_irqs_disabled_flags(flags)) << IRQMASK_i_POS;
}

static inline int arch_irqs_disabled_flags(unsigned long flags)
{
	return native_irqs_disabled_flags(flags);
}

static inline notrace void arch_local_irq_enable(void)
{
	barrier();
	inband_irq_enable();
}

static inline notrace void arch_local_irq_disable(void)
{
	inband_irq_disable();
	barrier();
}

static inline notrace unsigned long arch_local_save_flags(void)
{
	int stalled = inband_irqs_disabled();

	barrier();
	return arch_irqs_virtual_to_native_flags(stalled);
}

static inline notrace unsigned long arch_local_irq_save(void)
{
	int stalled = inband_irq_save();

	barrier();
	return arch_irqs_virtual_to_native_flags(stalled);
}

/* set interrupt enabled status */
static inline void arch_local_irq_restore(unsigned long flags)
{
	inband_irq_restore(arch_irqs_disabled_flags(flags));
	barrier();
}

static inline
void arch_save_timer_regs(struct pt_regs *dst, struct pt_regs *src)
{
	dst->epc = src->epc;
	dst->ra = src->ra;
	dst->sp = src->sp;
	dst->status = src->status;
	dst->cause = src->cause;
}

#else /* !CONFIG_IRQ_PIPELINE */

static inline unsigned long arch_local_irq_save(void)
{
	return native_irq_save();
}

static inline void arch_local_irq_enable(void)
{
	native_irq_enable();
}

static inline void arch_local_irq_disable(void)
{
	native_irq_disable();
}

static inline unsigned long arch_local_save_flags(void)
{
	return native_save_flags();
}

static inline void arch_local_irq_restore(unsigned long flags)
{
	native_irq_restore(flags);
}

static inline int arch_irqs_disabled_flags(unsigned long flags)
{
	return native_irqs_disabled_flags(flags);
}

#endif /* !CONFIG_IRQ_PIPELINE */

/* test hardware interrupt enable bit */
static inline int arch_irqs_disabled(void)
{
	return arch_irqs_disabled_flags(arch_local_save_flags());
}

struct pt_regs;

extern void (*handle_arch_irq)(struct pt_regs *);

static inline void arch_handle_irq_pipelined(struct pt_regs *regs)
{
	handle_arch_irq(regs);
}

static inline int arch_enable_oob_stage(void)
{
	return 0;
}

#endif /* _ASM_RISCV_IRQ_PIPELINE_H */
