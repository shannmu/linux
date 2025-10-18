/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ARCH_X86_KVM_PV_SCHED_H
#define _ARCH_X86_KVM_PV_SCHED_H

#include <linux/types.h>

struct kvm;
struct kvm_vcpu;

/* VM 生命周期管理 */
int kvm_pv_sched_init_vm(struct kvm *kvm);
void kvm_pv_sched_destroy_vm(struct kvm *kvm);

/* Hypercall 处理 */
bool kvm_pv_sched_handle_hypercall(struct kvm_vcpu *vcpu, unsigned long nr,
				    unsigned long a0, unsigned long a1,
				    unsigned long a2, unsigned long a3,
				    unsigned long *ret);

/* 内存管理 */
phys_addr_t kvm_pv_sched_get_page_pa(struct kvm *kvm, u32 page_idx);

/* RT VM 配置 */
int kvm_pv_sched_set_rt_vm_config(struct kvm *kvm, struct kvm_rt_vm_config *cfg);

#endif /* _ARCH_X86_KVM_PV_SCHED_H */
