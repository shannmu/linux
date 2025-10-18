// SPDX-License-Identifier: GPL-2.0
/*
 * KVM paravirtualized scheduling support
 *
 * KVM 与 pv_sched 的集成层
 */

#include <linux/kvm_host.h>
#include <linux/pv_sched_host.h>
#include <linux/slab.h>
#include <asm/kvm_para.h>

/* 全局 VM ID 计数器 */
static atomic_t cps_vm_id_counter = ATOMIC_INIT(1);

/**
 * kvm_pv_sched_init_vm - 为 Linux VM 初始化 pv_sched
 * @kvm: KVM 实例
 *
 * 返回值：0 成功，负数表示错误
 */
int kvm_pv_sched_init_vm(struct kvm *kvm)
{
	struct cps_vm_table *table;
	int nr_cpus;
	u32 vm_id;
	int ret;

	/* RT VM 不需要 pv_sched_table */
	if (kvm->is_rt_vm)
		return 0;

	/* 分配 VM ID */
	vm_id = atomic_inc_return(&cps_vm_id_counter);
	kvm->pv_sched_vm_id = vm_id;

	/* 使用系统的 CPU 数量（可以根据需求调整） */
	nr_cpus = num_possible_cpus();

	/* 分配 pv_sched_table */
	table = cps_vm_table_alloc(vm_id, nr_cpus);
	if (!table) {
		pr_err("kvm_pv_sched: failed to allocate table for VM %u\n", vm_id);
		return -ENOMEM;
	}

	kvm->pv_sched_table = table;

	/* 默认映射到 CPS_DEFAULT_GPA */
	ret = cps_vm_table_map_to_guest(table, CPS_DEFAULT_GPA);
	if (ret) {
		pr_err("kvm_pv_sched: failed to map table to guest: %d\n", ret);
		cps_vm_table_free(table);
		kvm->pv_sched_table = NULL;
		return ret;
	}

	/* 启动周期性更新 */
	ret = cps_vm_table_start_updates(table);
	if (ret) {
		pr_err("kvm_pv_sched: failed to start updates: %d\n", ret);
		cps_vm_table_free(table);
		kvm->pv_sched_table = NULL;
		return ret;
	}

	pr_info("kvm_pv_sched: initialized for VM %u (%d CPUs)\n", vm_id, nr_cpus);

	return 0;
}

/**
 * kvm_pv_sched_destroy_vm - 清理 VM 的 pv_sched
 * @kvm: KVM 实例
 */
void kvm_pv_sched_destroy_vm(struct kvm *kvm)
{
	/* Linux VM: 释放 pv_sched_table */
	if (kvm->pv_sched_table) {
		pr_info("kvm_pv_sched: destroying Linux VM %u\n", kvm->pv_sched_vm_id);
		cps_vm_table_free(kvm->pv_sched_table);
		kvm->pv_sched_table = NULL;
	}

	/* RT VM: 释放分配的 pCPU 资源 */
	if (kvm->is_rt_vm) {
		int ret;

		pr_info("kvm_pv_sched: destroying RT VM %u, releasing %u CPUs\n",
			kvm->pv_sched_vm_id, kvm->rt_config.nr_vcpus);

		ret = cps_release_rt_cpus(kvm->pv_sched_vm_id);
		if (ret)
			pr_err("kvm_pv_sched: failed to release RT CPUs: %d\n", ret);

		kvm->is_rt_vm = false;
	}
}

/**
 * kvm_pv_sched_handle_hypercall - 处理 pv_sched 相关的 hypercall
 * @vcpu: vCPU
 * @nr: hypercall 号
 * @a0-a3: hypercall 参数
 * @ret: 返回值
 *
 * 返回值：true 表示已处理，false 表示未处理
 */
bool kvm_pv_sched_handle_hypercall(struct kvm_vcpu *vcpu, unsigned long nr,
				    unsigned long a0, unsigned long a1,
				    unsigned long a2, unsigned long a3,
				    unsigned long *ret)
{
	struct kvm *kvm = vcpu->kvm;

	switch (nr) {
	case KVM_HC_PV_SCHED_GET_TABLE_GPA:
		/*
		 * Hypercall: 获取 pv_sched_table 的 GPA
		 * 参数：无
		 * 返回：GPA（64 位）
		 */
		if (!kvm->pv_sched_table) {
			*ret = 0;  /* 没有 pv_sched_table */
			return true;
		}

		*ret = kvm->pv_sched_table->gpa;
		return true;

	default:
		return false;  /* 未处理 */
	}
}

/**
 * kvm_pv_sched_get_page_pa - 获取 pv_sched_table 指定页的物理地址
 * @kvm: KVM 实例
 * @page_idx: 页索引
 *
 * 返回值：物理地址，失败返回 0
 *
 * 这个函数供 KVM 的内存管理层使用，用于建立 EPT 映射
 */
phys_addr_t kvm_pv_sched_get_page_pa(struct kvm *kvm, u32 page_idx)
{
	struct cps_vm_table *table = kvm->pv_sched_table;

	if (!table)
		return 0;

	if (page_idx >= table->nr_pages)
		return 0;

	return page_to_phys(table->pages[page_idx]);
}

/**
 * kvm_pv_sched_set_rt_vm_config - 设置 RT VM 配置（ioctl 处理）
 * @kvm: KVM 实例
 * @cfg: RT VM 配置
 *
 * 返回值：0 成功，负数表示错误
 */
int kvm_pv_sched_set_rt_vm_config(struct kvm *kvm, struct kvm_rt_vm_config *cfg)
{
	cpumask_var_t request_cpus;
	int i;
	int ret;

	if (!cfg)
		return -EINVAL;

	/* 验证参数 */
	if (cfg->nr_vcpus == 0 || cfg->nr_vcpus > KVM_PV_SCHED_MAX_RT_VCPUS)
		return -EINVAL;

	/* RT VM 不能有 pv_sched_table */
	if (kvm->pv_sched_table) {
		pr_err("kvm_pv_sched: cannot set RT config on Linux VM\n");
		return -EINVAL;
	}

	/* 不能重复配置 */
	if (kvm->is_rt_vm) {
		pr_warn("kvm_pv_sched: RT VM already configured\n");
		return -EEXIST;
	}

	/* 分配 CPU 掩码 */
	if (!zalloc_cpumask_var(&request_cpus, GFP_KERNEL))
		return -ENOMEM;

	/* 验证并构造 pCPU 掩码 */
	for (i = 0; i < cfg->nr_vcpus; i++) {
		int pcpu_id = cfg->pcpu_ids[i];

		if (pcpu_id < 0 || pcpu_id >= nr_cpu_ids) {
			pr_err("kvm_pv_sched: invalid pCPU ID %d for vCPU %d\n",
			       pcpu_id, i);
			ret = -EINVAL;
			goto out_free;
		}

		if (!cpu_possible(pcpu_id)) {
			pr_err("kvm_pv_sched: pCPU %d is not possible\n", pcpu_id);
			ret = -EINVAL;
			goto out_free;
		}

		/* 检查是否有重复 */
		if (cpumask_test_cpu(pcpu_id, request_cpus)) {
			pr_err("kvm_pv_sched: duplicate pCPU ID %d\n", pcpu_id);
			ret = -EINVAL;
			goto out_free;
		}

		cpumask_set_cpu(pcpu_id, request_cpus);
	}

	/* 调用全局分配函数（第 5 层：全局协调） */
	ret = cps_allocate_rt_cpus(cfg->vm_id, request_cpus, 0);
	if (ret) {
		pr_err("kvm_pv_sched: failed to allocate RT CPUs: %d\n", ret);
		goto out_free;
	}

	/* 保存配置 */
	memcpy(&kvm->rt_config, cfg, sizeof(*cfg));
	kvm->is_rt_vm = true;
	kvm->pv_sched_vm_id = cfg->vm_id;

	pr_info("kvm_pv_sched: configured RT VM %u with %u vCPUs\n",
		cfg->vm_id, cfg->nr_vcpus);

	ret = 0;

out_free:
	free_cpumask_var(request_cpus);
	return ret;
}
