// SPDX-License-Identifier: GPL-2.0
/*
 * Paravirtualized Scheduling - Guest 端实现
 *
 * Guest 内核读取 pv_sched_table 并进行智能调度决策
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/cpumask.h>
#include <linux/io.h>
#include <linux/memremap.h>
#include <linux/pv_sched_guest.h>
#include <linux/sched/topology.h>
#include <asm/kvm_para.h>

/* 全局 Guest info 实例 */
struct cps_guest_info cps_guest = {
	.enabled = false,
};
EXPORT_SYMBOL_GPL(cps_guest);

/* 缓存有效期（纳秒）：500us */
#define CPS_CACHE_VALID_NS	500000UL

/*
 * ============================================================================
 * 3.1 Guest 端初始化和映射
 * ============================================================================
 */

/**
 * cps_detect_host_support - 检测 Host 是否支持 pv_sched
 *
 * 返回值：true 表示支持，false 表示不支持
 */
static bool cps_detect_host_support(void)
{
	/* 检查是否在 KVM 虚拟机中运行 */
	if (!kvm_para_available())
		return false;

	/* 可以通过 CPUID 或其他方式检测，这里简化处理 */
	return true;
}

/**
 * cps_get_table_gpa - 通过 hypercall 获取 pv_sched_table 的 GPA
 *
 * 返回值：GPA，0 表示失败
 */
static u64 cps_get_table_gpa(void)
{
	unsigned long ret;

	/* 调用 hypercall */
	ret = kvm_hypercall0(KVM_HC_PV_SCHED_GET_TABLE_GPA);

	if (ret == 0) {
		pr_warn("pv_sched_guest: Host did not provide pv_sched_table\n");
		return 0;
	}

	return ret;
}

/**
 * cps_guest_init - Guest 启动时初始化
 *
 * 返回值：0 成功，负数表示错误
 */
int cps_guest_init(void)
{
	u64 gpa;
	void __iomem *base;
	struct cps_hdr __iomem *hdr;
	u32 version, nr_cpus;
	unsigned long offset;

	/* 检测 Host 支持 */
	if (!cps_detect_host_support()) {
		pr_info("pv_sched_guest: Host does not support pv_sched\n");
		return -ENODEV;
	}

	/* 获取 pv_sched_table 的 GPA */
	gpa = cps_get_table_gpa();
	if (!gpa)
		return -ENOENT;

	cps_guest.gpa = gpa;

	/* 映射到内核虚拟地址空间（只读） */
	base = memremap(gpa, CPS_DEFAULT_SIZE, MEMREMAP_WB);
	if (!base) {
		pr_err("pv_sched_guest: failed to map GPA 0x%llx\n", gpa);
		return -ENOMEM;
	}

	cps_guest.base = base;

	/* 验证 header */
	hdr = (struct cps_hdr __iomem *)base;
	version = READ_ONCE(hdr->version);
	nr_cpus = READ_ONCE(hdr->rows);

	if (version != CPS_UAPI_VERSION) {
		pr_err("pv_sched_guest: version mismatch: expected %u, got %u\n",
		       CPS_UAPI_VERSION, version);
		memunmap(base);
		return -EINVAL;
	}

	if (nr_cpus == 0 || nr_cpus > CPS_MAX_CPUS) {
		pr_err("pv_sched_guest: invalid nr_cpus: %u\n", nr_cpus);
		memunmap(base);
		return -EINVAL;
	}

	cps_guest.nr_cpus = nr_cpus;
	cps_guest.hdr = hdr;

	/* 初始化快速访问指针 */
	offset = READ_ONCE(hdr->bm_offset);
	cps_guest.bm_avail = (unsigned long __iomem *)(base + offset);
	offset += CPS_BITMAP_SIZE(nr_cpus);
	cps_guest.bm_migrating = (unsigned long __iomem *)(base + offset);
	offset += CPS_BITMAP_SIZE(nr_cpus);
	cps_guest.bm_rt = (unsigned long __iomem *)(base + offset);
	offset += CPS_BITMAP_SIZE(nr_cpus);
	cps_guest.bm_lowload = (unsigned long __iomem *)(base + offset);

	offset = READ_ONCE(hdr->rows_offset);
	cps_guest.rows = (struct cps_pcpu_row __iomem *)(base + offset);

	/* 分配本地缓存位图 */
	if (!zalloc_cpumask_var(&cps_guest.cached_avail, GFP_KERNEL))
		goto err_cached_avail;
	if (!zalloc_cpumask_var(&cps_guest.cached_lowload, GFP_KERNEL))
		goto err_cached_lowload;

	/* 初始化缓存参数 */
	cps_guest.cache_timestamp = 0;
	cps_guest.cache_valid_ns = CPS_CACHE_VALID_NS;

	/* 初始化统计 */
	cps_guest.nr_selects = 0;
	cps_guest.nr_epoch_conflicts = 0;
	cps_guest.nr_cache_hits = 0;
	cps_guest.nr_fallbacks = 0;

	/* 启用 pv_sched */
	cps_guest.enabled = true;

	pr_info("pv_sched_guest: initialized, GPA=0x%llx, %u CPUs\n",
		gpa, nr_cpus);

	return 0;

err_cached_lowload:
	free_cpumask_var(cps_guest.cached_avail);
err_cached_avail:
	memunmap(base);
	return -ENOMEM;
}

/**
 * cps_guest_cleanup - Guest 关闭时清理
 */
void cps_guest_cleanup(void)
{
	if (!cps_guest.enabled)
		return;

	cps_guest.enabled = false;

	if (cps_guest.cached_lowload)
		free_cpumask_var(cps_guest.cached_lowload);
	if (cps_guest.cached_avail)
		free_cpumask_var(cps_guest.cached_avail);

	if (cps_guest.base)
		memunmap((void __iomem *)cps_guest.base);

	pr_info("pv_sched_guest: cleaned up\n");
}

/*
 * ============================================================================
 * 3.2 Epoch 安全读取
 * ============================================================================
 */

/**
 * cps_read_row_safe - 安全读取单行
 * @cpu: pCPU ID
 * @out: 输出缓冲区
 *
 * 返回值：0 成功，-EAGAIN epoch 冲突，-EINVAL 参数无效
 */
int cps_read_row_safe(int cpu, struct cps_pcpu_row *out)
{
	u32 epoch_start, epoch_end;
	struct cps_pcpu_row __iomem *row;
	int retry;

	if (!cps_guest.enabled)
		return -ENODEV;

	if (cpu < 0 || cpu >= cps_guest.nr_cpus)
		return -EINVAL;

	if (!out)
		return -EINVAL;

	row = &cps_guest.rows[cpu];

	/* 最多重试 3 次 */
	for (retry = 0; retry < 3; retry++) {
		/* 开始读取 */
		epoch_start = cps_read_epoch_start(cps_guest.hdr);
		if (epoch_start == (u32)-1) {
			/* 正在写入，稍后重试 */
			cpu_relax();
			continue;
		}

		/* 读取数据 */
		out->pcpu_id = READ_ONCE(row->pcpu_id);
		out->load = READ_ONCE(row->load);
		out->flags = READ_ONCE(row->flags);
		out->irq_pressure = READ_ONCE(row->irq_pressure);
		out->thermal_pr = READ_ONCE(row->thermal_pr);
		out->capacity_pc = READ_ONCE(row->capacity_pc);
		out->numa_id = READ_ONCE(row->numa_id);
		out->llc_id = READ_ONCE(row->llc_id);
		out->util_avg = READ_ONCE(row->util_avg);
		out->runnable_avg = READ_ONCE(row->runnable_avg);

		/* 验证 epoch */
		if (cps_read_epoch_end(cps_guest.hdr, epoch_start))
			return 0;  /* 成功 */

		/* epoch 冲突，重试 */
		cps_guest.nr_epoch_conflicts++;
		cpu_relax();
	}

	/* 重试次数用尽 */
	return -EAGAIN;
}
EXPORT_SYMBOL_GPL(cps_read_row_safe);

/*
 * ============================================================================
 * 3.3 位图快速筛选
 * ============================================================================
 */

/**
 * cps_read_bitmap_safe - 安全读取位图
 * @bm_src: 源位图（在共享内存中）
 * @out: 输出 CPU 掩码
 *
 * 返回值：0 成功，-EAGAIN epoch 冲突
 */
static int cps_read_bitmap_safe(unsigned long __iomem *bm_src, cpumask_t *out)
{
	u32 epoch_start;
	int nr_longs = BITS_TO_LONGS(cps_guest.nr_cpus);
	int i;

	/* 开始读取 */
	epoch_start = cps_read_epoch_start(cps_guest.hdr);
	if (epoch_start == (u32)-1)
		return -EAGAIN;

	/* 读取位图 */
	for (i = 0; i < nr_longs; i++)
		((unsigned long *)out)[i] = READ_ONCE(bm_src[i]);

	/* 验证 epoch */
	if (!cps_read_epoch_end(cps_guest.hdr, epoch_start)) {
		cps_guest.nr_epoch_conflicts++;
		return -EAGAIN;
	}

	return 0;
}

/**
 * cps_get_avail_cpus - 获取可用 pCPU 位图
 * @out: 输出 CPU 掩码
 *
 * 返回值：0 成功，负数表示错误
 */
int cps_get_avail_cpus(cpumask_t *out)
{
	int ret;
	int retry;

	if (!cps_guest.enabled)
		return -ENODEV;

	if (!out)
		return -EINVAL;

	/* 最多重试 3 次 */
	for (retry = 0; retry < 3; retry++) {
		ret = cps_read_bitmap_safe(cps_guest.bm_avail, out);
		if (ret == 0)
			return 0;

		cpu_relax();
	}

	return ret;
}
EXPORT_SYMBOL_GPL(cps_get_avail_cpus);

/**
 * cps_filter_rt_cpus - 从掩码中过滤掉 RT 独占的 pCPU
 * @mask: 要过滤的 CPU 掩码（会被修改）
 *
 * 返回值：0 成功，负数表示错误
 */
int cps_filter_rt_cpus(cpumask_t *mask)
{
	cpumask_var_t rt_cpus;
	int ret;
	int retry;

	if (!cps_guest.enabled)
		return -ENODEV;

	if (!mask)
		return -EINVAL;

	if (!zalloc_cpumask_var(&rt_cpus, GFP_ATOMIC))
		return -ENOMEM;

	/* 读取 RT 位图 */
	for (retry = 0; retry < 3; retry++) {
		ret = cps_read_bitmap_safe(cps_guest.bm_rt, rt_cpus);
		if (ret == 0)
			break;
		cpu_relax();
	}

	if (ret) {
		free_cpumask_var(rt_cpus);
		return ret;
	}

	/* 从 mask 中移除 RT CPU */
	cpumask_andnot(mask, mask, rt_cpus);

	free_cpumask_var(rt_cpus);
	return 0;
}
EXPORT_SYMBOL_GPL(cps_filter_rt_cpus);

/**
 * cps_filter_migrating_cpus - 从掩码中过滤掉正在迁移的 pCPU
 * @mask: 要过滤的 CPU 掩码（会被修改）
 *
 * 返回值：0 成功，负数表示错误
 */
int cps_filter_migrating_cpus(cpumask_t *mask)
{
	cpumask_var_t migrating_cpus;
	int ret;
	int retry;

	if (!cps_guest.enabled)
		return -ENODEV;

	if (!mask)
		return -EINVAL;

	if (!zalloc_cpumask_var(&migrating_cpus, GFP_ATOMIC))
		return -ENOMEM;

	/* 读取 MIGRATING 位图 */
	for (retry = 0; retry < 3; retry++) {
		ret = cps_read_bitmap_safe(cps_guest.bm_migrating, migrating_cpus);
		if (ret == 0)
			break;
		cpu_relax();
	}

	if (ret) {
		free_cpumask_var(migrating_cpus);
		return ret;
	}

	/* 从 mask 中移除正在迁移的 CPU */
	cpumask_andnot(mask, mask, migrating_cpus);

	free_cpumask_var(migrating_cpus);
	return 0;
}
EXPORT_SYMBOL_GPL(cps_filter_migrating_cpus);

/**
 * cps_filter_lowload_cpus - 只保留低负载的 pCPU
 * @mask: 要过滤的 CPU 掩码（会被修改）
 *
 * 返回值：0 成功，负数表示错误
 */
int cps_filter_lowload_cpus(cpumask_t *mask)
{
	cpumask_var_t lowload_cpus;
	int ret;
	int retry;

	if (!cps_guest.enabled)
		return -ENODEV;

	if (!mask)
		return -EINVAL;

	if (!zalloc_cpumask_var(&lowload_cpus, GFP_ATOMIC))
		return -ENOMEM;

	/* 读取 LOWLOAD 位图 */
	for (retry = 0; retry < 3; retry++) {
		ret = cps_read_bitmap_safe(cps_guest.bm_lowload, lowload_cpus);
		if (ret == 0)
			break;
		cpu_relax();
	}

	if (ret) {
		free_cpumask_var(lowload_cpus);
		return ret;
	}

	/* 只保留低负载的 CPU */
	cpumask_and(mask, mask, lowload_cpus);

	free_cpumask_var(lowload_cpus);
	return 0;
}
EXPORT_SYMBOL_GPL(cps_filter_lowload_cpus);

/**
 * cps_get_candidate_cpus - 获取候选 CPU 集合（组合筛选）
 * @out: 输出 CPU 掩码
 * @prefer_lowload: 是否优先选择低负载 CPU
 *
 * 返回值：0 成功，负数表示错误
 */
int cps_get_candidate_cpus(cpumask_t *out, bool prefer_lowload)
{
	int ret;

	if (!cps_guest.enabled)
		return -ENODEV;

	if (!out)
		return -EINVAL;

	/* 步骤 1: 获取可用 CPU */
	ret = cps_get_avail_cpus(out);
	if (ret)
		return ret;

	/* 步骤 2: 过滤 RT CPU */
	ret = cps_filter_rt_cpus(out);
	if (ret)
		return ret;

	/* 步骤 3: 过滤正在迁移的 CPU */
	ret = cps_filter_migrating_cpus(out);
	if (ret)
		return ret;

	/* 步骤 4: 如果需要，只保留低负载 CPU */
	if (prefer_lowload) {
		ret = cps_filter_lowload_cpus(out);
		/* 如果低负载过滤后为空，忽略错误（使用之前的候选集） */
		if (ret == 0 && cpumask_empty(out)) {
			/* 重新获取不带 lowload 过滤的候选集 */
			ret = cps_get_avail_cpus(out);
			if (ret)
				return ret;
			ret = cps_filter_rt_cpus(out);
			if (ret)
				return ret;
			ret = cps_filter_migrating_cpus(out);
		}
	}

	return ret;
}
EXPORT_SYMBOL_GPL(cps_get_candidate_cpus);

/*
 * ============================================================================
 * 3.4 CPU 选择算法
 * ============================================================================
 */

/**
 * cps_score_cpu - 为 CPU 打分
 * @cpu: 要评分的 CPU
 * @prev_cpu: 任务之前运行的 CPU
 * @row: CPU 的信息行
 *
 * 返回值：分数（越高越好），负数表示错误
 *
 * 评分标准：
 * - 同 NUMA 节点：+1000
 * - 同 LLC：+500
 * - 低负载：+300
 * - 中负载：+100
 * - capacity 高：+capacity_pc
 * - 中断压力低：+（100 - irq_pressure）
 */
static int cps_score_cpu(int cpu, int prev_cpu, struct cps_pcpu_row *row)
{
	int score = 0;

	/* NUMA 亲和性 */
	if (cpu_possible(cpu) && cpu_possible(prev_cpu)) {
		if (cpu_to_node(cpu) == cpu_to_node(prev_cpu))
			score += 1000;
	}

	/* LLC 亲和性 */
	if (cpu_possible(cpu) && cpu_possible(prev_cpu)) {
		if (topology_core_id(cpu) == topology_core_id(prev_cpu))
			score += 500;
	}

	/* 负载因素 */
	switch (row->load) {
	case CPS_LOAD_LOW:
		score += 300;
		break;
	case CPS_LOAD_MED:
		score += 100;
		break;
	case CPS_LOAD_HIGH:
		score += 0;
		break;
	}

	/* Capacity 因素 */
	score += row->capacity_pc;

	/* 中断压力因素（反向） */
	score += (100 - row->irq_pressure);

	/* 温度压力因素（反向） */
	score += (100 - row->thermal_pr) / 2;

	return score;
}

/**
 * cps_select_cpu - 主 CPU 选择函数
 * @p: 要调度的任务
 * @prev_cpu: 任务之前运行的 CPU
 * @wake_flags: 唤醒标志
 *
 * 返回值：选中的 CPU ID，-1 表示 fallback
 */
int cps_select_cpu(struct task_struct *p, int prev_cpu, int wake_flags)
{
	cpumask_var_t candidates;
	struct cps_pcpu_row row;
	int best_cpu = -1;
	int best_score = -1;
	int cpu;
	int ret;

	if (!cps_guest.enabled)
		return -1;

	/* 统计 */
	cps_guest.nr_selects++;

	/* 分配候选集掩码 */
	if (!zalloc_cpumask_var(&candidates, GFP_ATOMIC)) {
		cps_guest.nr_fallbacks++;
		return -1;
	}

	/* 步骤 1: 获取候选集（优先低负载） */
	ret = cps_get_candidate_cpus(candidates, true);
	if (ret) {
		cps_guest.nr_fallbacks++;
		free_cpumask_var(candidates);
		return -1;
	}

	/* 步骤 2: 如果候选集为空，fallback */
	if (cpumask_empty(candidates)) {
		cps_guest.nr_fallbacks++;
		free_cpumask_var(candidates);
		return -1;
	}

	/* 步骤 3: 在候选集中评分选择最优 CPU */
	for_each_cpu(cpu, candidates) {
		int score;

		/* 读取 CPU 信息 */
		ret = cps_read_row_safe(cpu, &row);
		if (ret)
			continue;  /* epoch 冲突，跳过 */

		/* 计算分数 */
		score = cps_score_cpu(cpu, prev_cpu, &row);

		/* 更新最优 CPU */
		if (score > best_score) {
			best_score = score;
			best_cpu = cpu;
		}
	}

	free_cpumask_var(candidates);

	/* 如果没找到合适的 CPU，fallback */
	if (best_cpu < 0) {
		cps_guest.nr_fallbacks++;
		return -1;
	}

	return best_cpu;
}
EXPORT_SYMBOL_GPL(cps_select_cpu);

/*
 * ============================================================================
 * 3.6 MIGRATING 响应机制
 * ============================================================================
 */

/**
 * cps_check_migration - 检查是否有 CPU 正在迁移
 *
 * 如果检测到 MIGRATING 标志，会主动触发任务迁移
 * 这个函数应该在后台定期调用（如 1ms 间隔）
 */
void cps_check_migration(void)
{
	cpumask_var_t migrating_cpus;
	int ret;
	int cpu;

	if (!cps_guest.enabled)
		return;

	if (!zalloc_cpumask_var(&migrating_cpus, GFP_KERNEL))
		return;

	/* 读取正在迁移的 CPU 集合 */
	ret = cps_read_bitmap_safe(cps_guest.bm_migrating, migrating_cpus);
	if (ret) {
		free_cpumask_var(migrating_cpus);
		return;
	}

	/* 如果没有正在迁移的 CPU，直接返回 */
	if (cpumask_empty(migrating_cpus)) {
		free_cpumask_var(migrating_cpus);
		return;
	}

	/*
	 * 主动迁移任务
	 *
	 * 这里简化实现：遍历正在迁移的 CPU，对每个 CPU 执行：
	 * 1. 找到运行在该 CPU 上的任务
	 * 2. 触发任务迁移
	 *
	 * 实际实现需要调用调度器的内部函数，如：
	 * - migrate_tasks()
	 * - kick_ilb() 触发 idle load balancing
	 * - 或者设置 TIF_NEED_RESCHED 标志
	 */
	for_each_cpu(cpu, migrating_cpus) {
		if (!cpu_online(cpu))
			continue;

		/*
		 * TODO: 调用调度器的迁移函数
		 * 通过RESCHEDLING_IPI向Linux VM发送调度请求, Linux VM所在pcpu跳过RT和Migrating标识的pcpu
		 * 例如：migrate_tasks(cpu) 或触发负载均衡
		 *
		 * 这里只是示例，实际需要调用内核内部 API
		 */
		pr_debug("pv_sched_guest: CPU %d is migrating, should move tasks\n", cpu);
	}

	free_cpumask_var(migrating_cpus);
}
EXPORT_SYMBOL_GPL(cps_check_migration);

/*
 * ============================================================================
 * 调试和统计
 * ============================================================================
 */

/**
 * cps_guest_print_stats - 打印统计信息
 */
void cps_guest_print_stats(void)
{
	if (!cps_guest.enabled) {
		pr_info("pv_sched_guest: not enabled\n");
		return;
	}

	pr_info("pv_sched_guest statistics:\n");
	pr_info("  Total selects:     %llu\n", cps_guest.nr_selects);
	pr_info("  Epoch conflicts:   %llu\n", cps_guest.nr_epoch_conflicts);
	pr_info("  Cache hits:        %llu\n", cps_guest.nr_cache_hits);
	pr_info("  Fallbacks:         %llu\n", cps_guest.nr_fallbacks);
}
EXPORT_SYMBOL_GPL(cps_guest_print_stats);

/**
 * cps_guest_reset_stats - 重置统计信息
 */
void cps_guest_reset_stats(void)
{
	cps_guest.nr_selects = 0;
	cps_guest.nr_epoch_conflicts = 0;
	cps_guest.nr_cache_hits = 0;
	cps_guest.nr_fallbacks = 0;
}
EXPORT_SYMBOL_GPL(cps_guest_reset_stats);
