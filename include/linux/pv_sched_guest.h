/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_PV_SCHED_GUEST_H
#define _LINUX_PV_SCHED_GUEST_H

/*
 * Paravirtualized Scheduling - Guest 端内部数据结构
 *
 * 用于 Guest 内核读取 pv_sched_table 并进行智能调度决策
 */

#include <linux/types.h>
#include <linux/cpumask.h>
#include <uapi/linux/pv_sched.h>

/*
 * Guest 侧的 pv_sched_table 信息
 *
 * 在 Guest 启动时映射 Host 提供的共享内存页，只读访问
 */
struct cps_guest_info {
	/* 基本信息 */
	bool enabled;				/* 是否启用 pv_sched */
	u32 nr_cpus;				/* 可见的 pCPU 数量 */
	u64 gpa;				/* Guest 物理地址 */
	void __iomem *base;			/* 映射的内核虚拟地址 */

	/* 快速访问指针（指向 base 内的各个区域） */
	struct cps_hdr __iomem *hdr;		/* 头部（包含 epoch） */
	unsigned long __iomem *bm_avail;	/* 位图：可用 */
	unsigned long __iomem *bm_migrating;	/* 位图：迁移中 */
	unsigned long __iomem *bm_rt;		/* 位图：RT 独占 */
	unsigned long __iomem *bm_lowload;	/* 位图：低负载 */
	struct cps_pcpu_row __iomem *rows;	/* 行数组 */

	/* 本地缓存（减少远程访问） */
	cpumask_var_t cached_avail;		/* 缓存的可用 CPU 掩码 */
	cpumask_var_t cached_lowload;		/* 缓存的低负载 CPU 掩码 */
	u64 cache_timestamp;			/* 缓存时间戳（ns） */
	u32 cache_valid_ns;			/* 缓存有效期（ns），默认 500us */

	/* 统计信息 */
	u64 nr_selects;				/* 总选择次数 */
	u64 nr_epoch_conflicts;			/* epoch 冲突次数 */
	u64 nr_cache_hits;			/* 缓存命中次数 */
	u64 nr_fallbacks;			/* fallback 到原生调度器次数 */
};

/* 全局 Guest info 实例 */
extern struct cps_guest_info cps_guest;

/*
 * Guest 端初始化和清理
 */
int cps_guest_init(void);
void cps_guest_cleanup(void);

/*
 * Epoch 安全读取协议
 *
 * 使用方式：
 *   u32 epoch;
 *   do {
 *       epoch = cps_read_epoch_start(hdr);
 *       if (epoch == (u32)-1)
 *           return -EAGAIN;  // 正在写入
 *       // 读取数据...
 *   } while (!cps_read_epoch_end(hdr, epoch));
 */

/**
 * cps_read_epoch_start - 开始读取前检查 epoch
 * @hdr: pv_sched_table 头部
 *
 * 返回值：
 *   epoch 值（偶数）：可以开始读取
 *   (u32)-1：正在写入中，建议重试
 */
static inline u32 cps_read_epoch_start(struct cps_hdr __iomem *hdr)
{
	u32 epoch;

	/* 使用 READ_ONCE 防止编译器优化 */
	epoch = READ_ONCE(hdr->epoch);

	/* 如果是奇数，表示正在写入 */
	if (epoch & 1)
		return (u32)-1;

	/* 内存屏障：确保后续读取不会重排到 epoch 读取之前 */
	smp_rmb();

	return epoch;
}

/**
 * cps_read_epoch_end - 读取后验证 epoch
 * @hdr: pv_sched_table 头部
 * @epoch_start: 开始时读取的 epoch 值
 *
 * 返回值：
 *   true: 数据一致，可以使用
 *   false: 数据可能被修改，需要重试
 */
static inline bool cps_read_epoch_end(struct cps_hdr __iomem *hdr, u32 epoch_start)
{
	u32 epoch_end;

	/* 内存屏障：确保前面的读取不会重排到 epoch 读取之后 */
	smp_rmb();

	epoch_end = READ_ONCE(hdr->epoch);

	/* 如果 epoch 没变，说明数据一致 */
	return epoch_start == epoch_end;
}

/**
 * cps_read_row_safe - 安全读取单行
 * @cpu: pCPU ID
 * @out: 输出缓冲区
 *
 * 返回值：
 *   0: 成功
 *   -EAGAIN: epoch 冲突，建议重试
 *   -EINVAL: 参数无效
 */
int cps_read_row_safe(int cpu, struct cps_pcpu_row *out);

/*
 * 位图快速筛选
 *
 * 这些函数使用位图加速 CPU 筛选，达到 O(1) 时间复杂度
 */

/**
 * cps_get_avail_cpus - 获取可用 pCPU 位图
 * @out: 输出 CPU 掩码
 *
 * 返回值：0 成功，负数表示错误
 */
int cps_get_avail_cpus(cpumask_t *out);

/**
 * cps_filter_rt_cpus - 从掩码中过滤掉 RT 独占的 pCPU
 * @mask: 要过滤的 CPU 掩码（会被修改）
 *
 * 返回值：0 成功，负数表示错误
 */
int cps_filter_rt_cpus(cpumask_t *mask);

/**
 * cps_filter_migrating_cpus - 从掩码中过滤掉正在迁移的 pCPU
 * @mask: 要过滤的 CPU 掩码（会被修改）
 *
 * 返回值：0 成功，负数表示错误
 */
int cps_filter_migrating_cpus(cpumask_t *mask);

/**
 * cps_filter_lowload_cpus - 只保留低负载的 pCPU
 * @mask: 要过滤的 CPU 掩码（会被修改）
 *
 * 返回值：0 成功，负数表示错误
 */
int cps_filter_lowload_cpus(cpumask_t *mask);

/**
 * cps_get_candidate_cpus - 获取候选 CPU 集合（组合筛选）
 * @out: 输出 CPU 掩码
 * @prefer_lowload: 是否优先选择低负载 CPU
 *
 * 等价于：bm_avail & ~bm_rt & ~bm_migrating [& bm_lowload]
 *
 * 返回值：0 成功，负数表示错误
 */
int cps_get_candidate_cpus(cpumask_t *out, bool prefer_lowload);

/*
 * CPU 选择算法
 */

/**
 * cps_select_cpu - 主 CPU 选择函数
 * @p: 要调度的任务
 * @prev_cpu: 任务之前运行的 CPU
 * @wake_flags: 唤醒标志（WF_SYNC 等）
 *
 * 返回值：
 *   >= 0: 选中的 CPU ID
 *   -1: 没有合适的 CPU，fallback 到原生调度器
 */
int cps_select_cpu(struct task_struct *p, int prev_cpu, int wake_flags);

/**
 * cps_guest_enabled - 检查 pv_sched 是否启用
 *
 * 这是一个轻量级检查，用于在调度热路径快速判断
 */
static inline bool cps_guest_enabled(void)
{
	return cps_guest.enabled;
}

/*
 * MIGRATING 响应机制
 */

/**
 * cps_check_migration - 检查是否有 CPU 正在迁移
 *
 * 如果检测到 MIGRATING 标志，会主动触发任务迁移
 * 这个函数应该在后台定期调用（如 1ms 间隔）
 */
void cps_check_migration(void);

/*
 * 调试和统计
 */
void cps_guest_print_stats(void);
void cps_guest_reset_stats(void);

#ifdef CONFIG_DEBUG_FS
int cps_guest_debugfs_init(void);
void cps_guest_debugfs_cleanup(void);
#else
static inline int cps_guest_debugfs_init(void) { return 0; }
static inline void cps_guest_debugfs_cleanup(void) { }
#endif

#endif /* _LINUX_PV_SCHED_GUEST_H */
