// SPDX-License-Identifier: GPL-2.0
/*
 * Paravirtualized Scheduling - Host 端实现
 *
 * 管理全局 pCPU 状态和 per-VM pv_sched_table
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/mm.h>
#include <linux/vmalloc.h>
#include <linux/cpumask.h>
#include <linux/sched.h>
#include <linux/hrtimer.h>
#include <linux/ktime.h>
#include <linux/pv_sched_host.h>
#include <linux/sched/topology.h>
#include <linux/sched/loadavg.h>

/* 全局状态表 */
static struct cps_global_state cps_global;

/* VM 表链表（用于周期性更新所有 VM） */
static LIST_HEAD(cps_vm_tables);
static DEFINE_SPINLOCK(cps_vm_tables_lock);

/* 默认配置 */
#define CPS_DEFAULT_UPDATE_PERIOD_MS	2	/* 默认 2ms 更新一次 */
#define CPS_DEFAULT_MIGRATION_TIMEOUT_MS 100	/* 默认迁移超时 100ms */

/*
 * ============================================================================
 * 2.1 全局状态管理
 * ============================================================================
 */

/**
 * cps_global_init - 初始化全局 pCPU 状态表
 *
 * 在系统启动时调用，初始化所有数据结构
 */
int cps_global_init(void)
{
	int cpu;

	spin_lock_init(&cps_global.lock);

	/* 分配位图 */
	if (!zalloc_cpumask_var(&cps_global.bm_free, GFP_KERNEL))
		goto err_free;
	if (!zalloc_cpumask_var(&cps_global.bm_host, GFP_KERNEL))
		goto err_host;
	if (!zalloc_cpumask_var(&cps_global.bm_rt, GFP_KERNEL))
		goto err_rt;
	if (!zalloc_cpumask_var(&cps_global.bm_linux_pool, GFP_KERNEL))
		goto err_linux;

	/* 初始化所有 pCPU 为 LINUX_POOL 状态（暂时跳过 Host Region 划分） */
	for_each_possible_cpu(cpu) {
		cps_global.pcpu_state[cpu].owner_type = CPS_OWNER_LINUX_POOL;
		cps_global.pcpu_state[cpu].owner_id = 0;
		cps_global.pcpu_state[cpu].state = CPS_STATE_ALLOCATED;
		cps_global.pcpu_state[cpu].timestamp = ktime_get_ns();

		cpumask_set_cpu(cpu, cps_global.bm_linux_pool);
	}

	/* 更新统计 */
	cps_global.nr_free = 0;
	cps_global.nr_host = 0;
	cps_global.nr_rt = 0;
	cps_global.nr_linux_pool = num_possible_cpus();

	pr_info("pv_sched: global state initialized, %u CPUs in Linux pool\n",
		cps_global.nr_linux_pool);

	return 0;

err_linux:
	free_cpumask_var(cps_global.bm_rt);
err_rt:
	free_cpumask_var(cps_global.bm_host);
err_host:
	free_cpumask_var(cps_global.bm_free);
err_free:
	return -ENOMEM;
}

/**
 * cps_global_cleanup - 清理全局状态
 */
void cps_global_cleanup(void)
{
	free_cpumask_var(cps_global.bm_linux_pool);
	free_cpumask_var(cps_global.bm_rt);
	free_cpumask_var(cps_global.bm_host);
	free_cpumask_var(cps_global.bm_free);

	pr_info("pv_sched: global state cleaned up\n");
}

/**
 * cps_query_pcpu_state - 查询 pCPU 状态
 * @cpu: pCPU ID
 * @out: 输出缓冲区
 *
 * 返回值：0 成功，负数表示错误
 */
int cps_query_pcpu_state(int cpu, struct cps_pcpu_slot *out)
{
	unsigned long flags;

	if (cpu < 0 || cpu >= NR_CPUS)
		return -EINVAL;

	if (!out)
		return -EINVAL;

	spin_lock_irqsave(&cps_global.lock, flags);
	*out = cps_global.pcpu_state[cpu];
	spin_unlock_irqrestore(&cps_global.lock, flags);

	return 0;
}
EXPORT_SYMBOL_GPL(cps_query_pcpu_state);

/**
 * cps_get_pcpu_bitmap - 获取指定类型的 pCPU 位图
 * @out: 输出 CPU 掩码
 * @owner_type: 所有者类型
 */
void cps_get_pcpu_bitmap(cpumask_t *out, enum cps_pcpu_owner_type owner_type)
{
	unsigned long flags;

	if (!out)
		return;

	spin_lock_irqsave(&cps_global.lock, flags);

	switch (owner_type) {
	case CPS_OWNER_NONE:
		cpumask_copy(out, cps_global.bm_free);
		break;
	case CPS_OWNER_HOST:
		cpumask_copy(out, cps_global.bm_host);
		break;
	case CPS_OWNER_RT_VM:
		cpumask_copy(out, cps_global.bm_rt);
		break;
	case CPS_OWNER_LINUX_POOL:
		cpumask_copy(out, cps_global.bm_linux_pool);
		break;
	default:
		cpumask_clear(out);
	}

	spin_unlock_irqrestore(&cps_global.lock, flags);
}
EXPORT_SYMBOL_GPL(cps_get_pcpu_bitmap);

/*
 * ============================================================================
 * 2.2 per-VM pv_sched_table 管理
 * ============================================================================
 */

/**
 * cps_vm_table_alloc - 为 VM 分配 pv_sched_table
 * @vm_id: VM ID
 * @nr_cpus: 该 VM 可见的 pCPU 数量
 *
 * 返回值：成功返回 table 指针，失败返回 NULL
 */
struct cps_vm_table *cps_vm_table_alloc(u32 vm_id, int nr_cpus)
{
	struct cps_vm_table *table;
	size_t table_size;
	size_t hdr_size, bm_size, rows_size;
	unsigned long offset;
	int order;
	int i;

	if (nr_cpus <= 0 || nr_cpus > CPS_MAX_CPUS)
		return NULL;

	/* 分配管理结构 */
	table = kzalloc(sizeof(*table), GFP_KERNEL);
	if (!table)
		return NULL;

	table->vm_id = vm_id;
	table->nr_cpus = nr_cpus;

	/* 计算各区域大小 */
	hdr_size = sizeof(struct cps_hdr);
	bm_size = CPS_BITMAP_AREA_SIZE(nr_cpus);	/* 4 个位图 */
	rows_size = CPS_ROWS_AREA_SIZE(nr_cpus);

	table_size = hdr_size + bm_size + rows_size;

	/* 向上对齐到页边界 */
	table_size = PAGE_ALIGN(table_size);
	table->nr_pages = table_size >> PAGE_SHIFT;

	/* 分配物理页（使用 alloc_pages 以便映射到 Guest） */
	order = get_order(table_size);
	table->pages = kcalloc(table->nr_pages, sizeof(struct page *), GFP_KERNEL);
	if (!table->pages)
		goto err_pages;

	/* 分配连续的物理页 */
	for (i = 0; i < table->nr_pages; i++) {
		table->pages[i] = alloc_page(GFP_KERNEL | __GFP_ZERO);
		if (!table->pages[i])
			goto err_alloc_pages;
	}

	/* 映射到内核虚拟地址空间 */
	table->base = vmap(table->pages, table->nr_pages, VM_MAP, PAGE_KERNEL);
	if (!table->base)
		goto err_vmap;

	/* 初始化快速访问指针 */
	offset = 0;

	table->hdr = (struct cps_hdr *)(table->base + offset);
	offset += hdr_size;

	table->bm_avail = (unsigned long *)(table->base + offset);
	offset += CPS_BITMAP_SIZE(nr_cpus);

	table->bm_migrating = (unsigned long *)(table->base + offset);
	offset += CPS_BITMAP_SIZE(nr_cpus);

	table->bm_rt = (unsigned long *)(table->base + offset);
	offset += CPS_BITMAP_SIZE(nr_cpus);

	table->bm_lowload = (unsigned long *)(table->base + offset);
	offset += CPS_BITMAP_SIZE(nr_cpus);

	table->rows = (struct cps_pcpu_row *)(table->base + offset);

	/* 初始化头部 */
	table->hdr->version = CPS_UAPI_VERSION;
	table->hdr->rows = nr_cpus;
	table->hdr->row_size = sizeof(struct cps_pcpu_row);
	table->hdr->bm_offset = (unsigned long)table->bm_avail - (unsigned long)table->base;
	table->hdr->rows_offset = (unsigned long)table->rows - (unsigned long)table->base;
	table->hdr->ext_offset = 0;  /* 暂不支持扩展区 */
	table->hdr->epoch = 0;  /* 初始为偶数（稳定状态） */
	table->hdr->features = CPS_FEAT_NUMA | CPS_FEAT_LLC | CPS_FEAT_THERMAL |
			       CPS_FEAT_IRQ_PRESSURE | CPS_FEAT_CAPACITY;

	/* 初始化行数组（填充拓扑信息） */
	for (i = 0; i < nr_cpus; i++) {
		struct cps_pcpu_row *row = &table->rows[i];

		row->pcpu_id = i;
		row->load = CPS_LOAD_LOW;
		row->flags = CPSF_AVAIL;  /* 默认全部可用 */
		row->irq_pressure = 0;
		row->thermal_pr = 0;
		row->capacity_pc = 1000;  /* 默认满 capacity */

		/* 填充 NUMA 和 LLC 信息 */
		if (cpu_possible(i)) {
			row->numa_id = cpu_to_node(i);
			/* LLC ID 可以从 topology_core_id 获取（简化版） */
			row->llc_id = topology_core_id(i);
		} else {
			row->numa_id = 0;
			row->llc_id = 0;
		}

		row->util_avg = 0;
		row->runnable_avg = 0;
	}

	/* 初始化定时器 */
	hrtimer_init(&table->update_timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	table->update_period_ns = CPS_DEFAULT_UPDATE_PERIOD_MS * NSEC_PER_MSEC;
	atomic_set(&table->enabled, 0);

	table->nr_updates = 0;
	table->last_update_ns = 0;
	table->gpa = 0;  /* 稍后通过 map_to_guest 设置 */

	pr_info("pv_sched: allocated table for VM %u, %d CPUs, %u pages, size=%zu\n",
		vm_id, nr_cpus, table->nr_pages, table_size);

	return table;

err_vmap:
err_alloc_pages:
	for (i = 0; i < table->nr_pages; i++) {
		if (table->pages[i])
			__free_page(table->pages[i]);
	}
	kfree(table->pages);
err_pages:
	kfree(table);
	return NULL;
}
EXPORT_SYMBOL_GPL(cps_vm_table_alloc);

/**
 * cps_vm_table_free - 释放 VM 的 pv_sched_table
 * @table: 要释放的表
 */
void cps_vm_table_free(struct cps_vm_table *table)
{
	int i;

	if (!table)
		return;

	/* 停止更新定时器 */
	cps_vm_table_stop_updates(table);

	/* 释放映射 */
	if (table->base)
		vunmap(table->base);

	/* 释放物理页 */
	if (table->pages) {
		for (i = 0; i < table->nr_pages; i++) {
			if (table->pages[i])
				__free_page(table->pages[i]);
		}
		kfree(table->pages);
	}

	pr_info("pv_sched: freed table for VM %u\n", table->vm_id);

	kfree(table);
}
EXPORT_SYMBOL_GPL(cps_vm_table_free);

/**
 * cps_vm_table_map_to_guest - 将 pv_sched_table 映射到 Guest 物理地址空间
 * @table: VM 表
 * @gpa: Guest 物理地址
 *
 * 注意：实际的 EPT 映射由 KVM 层处理，这里只记录 GPA
 */
int cps_vm_table_map_to_guest(struct cps_vm_table *table, u64 gpa)
{
	if (!table)
		return -EINVAL;

	table->gpa = gpa;

	pr_info("pv_sched: VM %u table mapped to GPA 0x%llx\n",
		table->vm_id, gpa);

	return 0;
}
EXPORT_SYMBOL_GPL(cps_vm_table_map_to_guest);

/*
 * ============================================================================
 * 2.3 RT VM 的 pCPU 分配和释放
 * ============================================================================
 */

/**
 * cps_allocate_rt_cpus - 为 RT VM 分配 pCPU
 * @vm_id: RT VM ID
 * @request_cpus: 请求的 pCPU 集合
 * @timeout_ms: 迁移超时时间（0 = 使用默认值）
 *
 * 返回值：0 成功，负数表示错误
 */
int cps_allocate_rt_cpus(u32 vm_id, const cpumask_t *request_cpus, u32 timeout_ms)
{
	unsigned long flags;
	int cpu;
	int ret = 0;

	if (!request_cpus || cpumask_empty(request_cpus))
		return -EINVAL;

	if (timeout_ms == 0)
		timeout_ms = CPS_DEFAULT_MIGRATION_TIMEOUT_MS;

	spin_lock_irqsave(&cps_global.lock, flags);

	/* 步骤 1: 检查资源可用性 */
	for_each_cpu(cpu, request_cpus) {
		struct cps_pcpu_slot *slot = &cps_global.pcpu_state[cpu];

		/* 只能从 LINUX_POOL 中分配 */
		if (slot->owner_type != CPS_OWNER_LINUX_POOL) {
			pr_warn("pv_sched: CPU %d not in Linux pool (owner=%d)\n",
				cpu, slot->owner_type);
			ret = -EBUSY;
			goto out_unlock;
		}
	}

	/* 步骤 2: 标记为 MIGRATING 状态 */
	for_each_cpu(cpu, request_cpus) {
		struct cps_pcpu_slot *slot = &cps_global.pcpu_state[cpu];

		slot->state = CPS_STATE_MIGRATING;
		slot->timestamp = ktime_get_ns();
	}

	spin_unlock_irqrestore(&cps_global.lock, flags);

	/* 步骤 3: 通知所有 Linux VM 开始迁移 */
	ret = cps_update_linux_vms_for_migration(request_cpus, true);
	if (ret) {
		pr_err("pv_sched: failed to set MIGRATING flag: %d\n", ret);
		goto rollback;
	}

	/* 步骤 4: 等待迁移完成 */
	ret = cps_wait_migration_done(request_cpus, timeout_ms);
	if (ret) {
		pr_warn("pv_sched: migration timeout or error: %d\n", ret);
		/* 继续执行，不回滚 */
	}

	/* 步骤 5: 更新全局状态为 ALLOCATED */
	spin_lock_irqsave(&cps_global.lock, flags);

	for_each_cpu(cpu, request_cpus) {
		struct cps_pcpu_slot *slot = &cps_global.pcpu_state[cpu];

		slot->owner_type = CPS_OWNER_RT_VM;
		slot->owner_id = vm_id;
		slot->state = CPS_STATE_ALLOCATED;
		slot->timestamp = ktime_get_ns();

		/* 更新位图 */
		cpumask_clear_cpu(cpu, cps_global.bm_linux_pool);
		cpumask_set_cpu(cpu, cps_global.bm_rt);
	}

	/* 更新统计 */
	cps_global.nr_rt += cpumask_weight(request_cpus);
	cps_global.nr_linux_pool -= cpumask_weight(request_cpus);

	spin_unlock_irqrestore(&cps_global.lock, flags);

	/* 步骤 6: 通知所有 Linux VM 更新（清除 AVAIL，设置 PINNED） */
	ret = cps_update_linux_vms_for_rt_allocation(request_cpus);
	if (ret)
		pr_warn("pv_sched: failed to update VMs after RT allocation: %d\n", ret);

	pr_info("pv_sched: allocated %d CPUs for RT VM %u\n",
		cpumask_weight(request_cpus), vm_id);

	return 0;

rollback:
	spin_lock_irqsave(&cps_global.lock, flags);
	for_each_cpu(cpu, request_cpus) {
		struct cps_pcpu_slot *slot = &cps_global.pcpu_state[cpu];
		slot->state = CPS_STATE_ALLOCATED;
	}
out_unlock:
	spin_unlock_irqrestore(&cps_global.lock, flags);
	return ret;
}
EXPORT_SYMBOL_GPL(cps_allocate_rt_cpus);

/**
 * cps_release_rt_cpus - 释放 RT VM 的 pCPU
 * @vm_id: RT VM ID
 *
 * 返回值：0 成功，负数表示错误
 */
int cps_release_rt_cpus(u32 vm_id)
{
	unsigned long flags;
	cpumask_var_t release_cpus;
	int cpu;
	int ret;

	if (!zalloc_cpumask_var(&release_cpus, GFP_KERNEL))
		return -ENOMEM;

	spin_lock_irqsave(&cps_global.lock, flags);

	/* 查找该 VM 的所有 pCPU */
	for_each_possible_cpu(cpu) {
		struct cps_pcpu_slot *slot = &cps_global.pcpu_state[cpu];

		if (slot->owner_type == CPS_OWNER_RT_VM &&
		    slot->owner_id == vm_id) {
			cpumask_set_cpu(cpu, release_cpus);

			/* 更新状态 */
			slot->owner_type = CPS_OWNER_LINUX_POOL;
			slot->owner_id = 0;
			slot->state = CPS_STATE_ALLOCATED;
			slot->timestamp = ktime_get_ns();

			/* 更新位图 */
			cpumask_clear_cpu(cpu, cps_global.bm_rt);
			cpumask_set_cpu(cpu, cps_global.bm_linux_pool);
		}
	}

	/* 更新统计 */
	cps_global.nr_rt -= cpumask_weight(release_cpus);
	cps_global.nr_linux_pool += cpumask_weight(release_cpus);

	spin_unlock_irqrestore(&cps_global.lock, flags);

	/* 通知所有 Linux VM 更新（设置 AVAIL，清除 PINNED） */
	ret = cps_update_linux_vms_for_release(release_cpus);
	if (ret)
		pr_warn("pv_sched: failed to update VMs after RT release: %d\n", ret);

	pr_info("pv_sched: released %d CPUs from RT VM %u\n",
		cpumask_weight(release_cpus), vm_id);

	free_cpumask_var(release_cpus);
	return 0;
}
EXPORT_SYMBOL_GPL(cps_release_rt_cpus);

/*
 * ============================================================================
 * 2.4 pv_sched_table 更新（周期性 + 触发式）
 * ============================================================================
 */

/**
 * cps_get_cpu_load - 获取 CPU 当前负载等级
 * @cpu: CPU ID
 *
 * 返回值：CPS_LOAD_LOW/MED/HIGH
 */
static enum cps_load cps_get_cpu_load(int cpu)
{
	struct rq *rq;
	unsigned long util;

	if (!cpu_online(cpu))
		return CPS_LOAD_LOW;

	rq = cpu_rq(cpu);

	/* 使用 PELT 的 util_avg（范围 0-1024） */
	util = cpu_util(cpu);

	/* 转换为百分比，然后分级 */
	if (util < 307)  /* < 30% */
		return CPS_LOAD_LOW;
	else if (util < 717)  /* 30-70% */
		return CPS_LOAD_MED;
	else
		return CPS_LOAD_HIGH;
}

/**
 * cps_update_vm_table - 更新单个 VM 的 pv_sched_table
 * @table: VM 表
 *
 * 返回值：0 成功，负数表示错误
 */
int cps_update_vm_table(struct cps_vm_table *table)
{
	unsigned long flags;
	int cpu;
	struct cps_pcpu_slot slot;
	u32 epoch;

	if (!table || !table->base)
		return -EINVAL;

	/* 步骤 1: epoch++ (奇数，表示开始写入) */
	epoch = table->hdr->epoch;
	table->hdr->epoch = epoch + 1;
	smp_wmb();  /* 确保 epoch 更新对读者可见 */

	/* 步骤 2: 清空位图 */
	bitmap_zero((unsigned long *)table->bm_avail, table->nr_cpus);
	bitmap_zero((unsigned long *)table->bm_migrating, table->nr_cpus);
	bitmap_zero((unsigned long *)table->bm_rt, table->nr_cpus);
	bitmap_zero((unsigned long *)table->bm_lowload, table->nr_cpus);

	/* 步骤 3: 更新每一行 */
	for (cpu = 0; cpu < table->nr_cpus; cpu++) {
		struct cps_pcpu_row *row = &table->rows[cpu];

		/* 查询全局状态 */
		spin_lock_irqsave(&cps_global.lock, flags);
		slot = cps_global.pcpu_state[cpu];
		spin_unlock_irqrestore(&cps_global.lock, flags);

		/* 更新负载 */
		row->load = cps_get_cpu_load(cpu);

		/* 更新标志位 */
		row->flags = 0;

		if (slot.owner_type == CPS_OWNER_LINUX_POOL &&
		    slot.state == CPS_STATE_ALLOCATED)
			row->flags |= CPSF_AVAIL;

		if (slot.state == CPS_STATE_MIGRATING)
			row->flags |= CPSF_MIGRATING;

		if (slot.owner_type == CPS_OWNER_RT_VM)
			row->flags |= CPSF_PINNED;

		if (!cpu_online(cpu))
			row->flags |= CPSF_DO_NOT_USE;

		/* 更新压力信息（简化版，实际需要从硬件读取） */
		row->irq_pressure = 0;  /* TODO: 从 irqstat 计算 */
		row->thermal_pr = 0;    /* TODO: 从 thermal zone 读取 */

		/* 更新 capacity */
		if (cpu_online(cpu))
			row->capacity_pc = (arch_scale_cpu_capacity(cpu) * 1000) >> 10;
		else
			row->capacity_pc = 0;

		/* 更新 PELT 信息 */
		if (cpu_online(cpu)) {
			row->util_avg = cpu_util(cpu);
			row->runnable_avg = cpu_runnable(cpu);
		} else {
			row->util_avg = 0;
			row->runnable_avg = 0;
		}

		/* 更新位图 */
		if (row->flags & CPSF_AVAIL)
			set_bit(cpu, (unsigned long *)table->bm_avail);
		if (row->flags & CPSF_MIGRATING)
			set_bit(cpu, (unsigned long *)table->bm_migrating);
		if (row->flags & CPSF_PINNED)
			set_bit(cpu, (unsigned long *)table->bm_rt);
		if (row->load == CPS_LOAD_LOW)
			set_bit(cpu, (unsigned long *)table->bm_lowload);
	}

	/* 步骤 4: epoch++ (偶数，表示写入完成) */
	smp_wmb();
	table->hdr->epoch = epoch + 2;

	/* 更新统计 */
	table->nr_updates++;
	table->last_update_ns = ktime_get_ns();

	return 0;
}
EXPORT_SYMBOL_GPL(cps_update_vm_table);

/**
 * cps_update_linux_vms_for_migration - 设置/清除 MIGRATING 标志
 * @cpus: 要设置的 CPU 集合
 * @set: true=设置，false=清除
 *
 * 返回值：0 成功，负数表示错误
 */
int cps_update_linux_vms_for_migration(const cpumask_t *cpus, bool set)
{
	struct cps_vm_table *table;
	unsigned long flags;
	int cpu;

	/* 遍历所有 Linux VM 的表 */
	spin_lock_irqsave(&cps_vm_tables_lock, flags);

	list_for_each_entry(table, &cps_vm_tables, list) {
		u32 epoch = table->hdr->epoch;

		/* epoch++ (奇数) */
		table->hdr->epoch = epoch + 1;
		smp_wmb();

		/* 更新 MIGRATING 标志 */
		for_each_cpu(cpu, cpus) {
			if (cpu >= table->nr_cpus)
				continue;

			if (set) {
				table->rows[cpu].flags |= CPSF_MIGRATING;
				set_bit(cpu, (unsigned long *)table->bm_migrating);
			} else {
				table->rows[cpu].flags &= ~CPSF_MIGRATING;
				clear_bit(cpu, (unsigned long *)table->bm_migrating);
			}
		}

		/* epoch++ (偶数) */
		smp_wmb();
		table->hdr->epoch = epoch + 2;
	}

	spin_unlock_irqrestore(&cps_vm_tables_lock, flags);

	return 0;
}
EXPORT_SYMBOL_GPL(cps_update_linux_vms_for_migration);

/**
 * cps_update_linux_vms_for_rt_allocation - RT 分配后更新所有 VM
 * @cpus: 被 RT VM 占用的 CPU 集合
 *
 * 返回值：0 成功，负数表示错误
 */
int cps_update_linux_vms_for_rt_allocation(const cpumask_t *cpus)
{
	struct cps_vm_table *table;
	unsigned long flags;
	int cpu;

	spin_lock_irqsave(&cps_vm_tables_lock, flags);

	list_for_each_entry(table, &cps_vm_tables, list) {
		u32 epoch = table->hdr->epoch;

		table->hdr->epoch = epoch + 1;
		smp_wmb();

		for_each_cpu(cpu, cpus) {
			if (cpu >= table->nr_cpus)
				continue;

			/* 清除 AVAIL 和 MIGRATING，设置 PINNED */
			table->rows[cpu].flags &= ~(CPSF_AVAIL | CPSF_MIGRATING);
			table->rows[cpu].flags |= CPSF_PINNED;

			/* 更新位图 */
			clear_bit(cpu, (unsigned long *)table->bm_avail);
			clear_bit(cpu, (unsigned long *)table->bm_migrating);
			set_bit(cpu, (unsigned long *)table->bm_rt);
		}

		smp_wmb();
		table->hdr->epoch = epoch + 2;
	}

	spin_unlock_irqrestore(&cps_vm_tables_lock, flags);

	return 0;
}
EXPORT_SYMBOL_GPL(cps_update_linux_vms_for_rt_allocation);

/**
 * cps_update_linux_vms_for_release - RT 释放后更新所有 VM
 * @cpus: 被释放的 CPU 集合
 *
 * 返回值：0 成功，负数表示错误
 */
int cps_update_linux_vms_for_release(const cpumask_t *cpus)
{
	struct cps_vm_table *table;
	unsigned long flags;
	int cpu;

	spin_lock_irqsave(&cps_vm_tables_lock, flags);

	list_for_each_entry(table, &cps_vm_tables, list) {
		u32 epoch = table->hdr->epoch;

		table->hdr->epoch = epoch + 1;
		smp_wmb();

		for_each_cpu(cpu, cpus) {
			if (cpu >= table->nr_cpus)
				continue;

			/* 设置 AVAIL，清除 PINNED */
			table->rows[cpu].flags |= CPSF_AVAIL;
			table->rows[cpu].flags &= ~CPSF_PINNED;

			/* 更新位图 */
			set_bit(cpu, (unsigned long *)table->bm_avail);
			clear_bit(cpu, (unsigned long *)table->bm_rt);
		}

		smp_wmb();
		table->hdr->epoch = epoch + 2;
	}

	spin_unlock_irqrestore(&cps_vm_tables_lock, flags);

	return 0;
}
EXPORT_SYMBOL_GPL(cps_update_linux_vms_for_release);

/**
 * cps_wait_migration_done - 等待迁移完成
 * @cpus: 正在迁移的 CPU 集合
 * @timeout_ms: 超时时间（ms）
 *
 * 返回值：0 成功，-ETIMEDOUT 超时
 */
int cps_wait_migration_done(const cpumask_t *cpus, u32 timeout_ms)
{
	unsigned long timeout_jiffies = msecs_to_jiffies(timeout_ms);
	unsigned long start = jiffies;

	/*
	 * 简化实现：等待固定时间
	 * 实际应该检查 Guest 是否完成迁移（通过 hypercall 通知）
	 */
	msleep(timeout_ms);

	if (time_after(jiffies, start + timeout_jiffies))
		return -ETIMEDOUT;

	return 0;
}
EXPORT_SYMBOL_GPL(cps_wait_migration_done);

/**
 * cps_update_timer_callback - 定时器回调函数
 * @timer: hrtimer
 *
 * 返回值：HRTIMER_RESTART 继续运行
 */
static enum hrtimer_restart cps_update_timer_callback(struct hrtimer *timer)
{
	struct cps_vm_table *table = container_of(timer, struct cps_vm_table,
						   update_timer);

	if (!atomic_read(&table->enabled))
		return HRTIMER_NORESTART;

	/* 更新表 */
	cps_update_vm_table(table);

	/* 重新设置定时器 */
	hrtimer_forward_now(timer, ns_to_ktime(table->update_period_ns));

	return HRTIMER_RESTART;
}

/**
 * cps_vm_table_start_updates - 启动周期性更新
 * @table: VM 表
 *
 * 返回值：0 成功，负数表示错误
 */
int cps_vm_table_start_updates(struct cps_vm_table *table)
{
	unsigned long flags;

	if (!table)
		return -EINVAL;

	if (atomic_read(&table->enabled))
		return 0;  /* 已经启动 */

	/* 设置回调函数 */
	table->update_timer.function = cps_update_timer_callback;

	/* 启动定时器 */
	atomic_set(&table->enabled, 1);
	hrtimer_start(&table->update_timer,
		      ns_to_ktime(table->update_period_ns),
		      HRTIMER_MODE_REL);

	/* 添加到全局链表 */
	spin_lock_irqsave(&cps_vm_tables_lock, flags);
	list_add_tail(&table->list, &cps_vm_tables);
	spin_unlock_irqrestore(&cps_vm_tables_lock, flags);

	pr_info("pv_sched: started updates for VM %u (period=%llu ns)\n",
		table->vm_id, table->update_period_ns);

	return 0;
}
EXPORT_SYMBOL_GPL(cps_vm_table_start_updates);

/**
 * cps_vm_table_stop_updates - 停止周期性更新
 * @table: VM 表
 */
void cps_vm_table_stop_updates(struct cps_vm_table *table)
{
	unsigned long flags;

	if (!table)
		return;

	if (!atomic_read(&table->enabled))
		return;  /* 已经停止 */

	/* 停止定时器 */
	atomic_set(&table->enabled, 0);
	hrtimer_cancel(&table->update_timer);

	/* 从全局链表移除 */
	spin_lock_irqsave(&cps_vm_tables_lock, flags);
	list_del(&table->list);
	spin_unlock_irqrestore(&cps_vm_tables_lock, flags);

	pr_info("pv_sched: stopped updates for VM %u\n", table->vm_id);
}
EXPORT_SYMBOL_GPL(cps_vm_table_stop_updates);
