/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
#ifndef _UAPI_LINUX_PV_SCHED_H
#define _UAPI_LINUX_PV_SCHED_H

/*
 * Paravirtualized Scheduling (pv_sched) UAPI
 *
 * 用于 Host-Guest 协同调度的共享数据结构定义
 *
 * 这个头文件定义了 pv_sched_table 的内存布局，该表由 Host 更新，
 * Guest 只读访问，用于获取 pCPU 的实时负载和可用性信息。
 */

#include <linux/types.h>

/*
 * CPS (Cooperative Paravirtualized Scheduling) 版本号
 * 每次 UAPI 结构变化时递增
 */
#define CPS_UAPI_VERSION	1

/*
 * pv_sched_table 的默认配置
 */
#define CPS_DEFAULT_GPA		0x88000000ULL	/* 默认 Guest 物理地址 */
#define CPS_DEFAULT_SIZE	(1ULL << 21)	/* 默认 2MB，支持最多 8192 pCPU */
#define CPS_MAX_CPUS		8192		/* 最大支持的 pCPU 数量 */

/*
 * pCPU 负载等级
 * Host 根据当前负载将每个 pCPU 分类到这 3 个等级之一
 */
enum cps_load {
	CPS_LOAD_LOW	= 0,	/* 低负载：CPU 使用率 < 30% */
	CPS_LOAD_MED	= 1,	/* 中负载：CPU 使用率 30-70% */
	CPS_LOAD_HIGH	= 2,	/* 高负载：CPU 使用率 > 70% */
};

/*
 * pCPU 状态标志位
 * 每个 pCPU 可以同时拥有多个标志
 */
enum cps_state_flags {
	CPSF_AVAIL		= (1 << 0),	/* 可用：当前可调度任务到此 pCPU */
	CPSF_MIGRATING		= (1 << 1),	/* 迁移中：正在迁移任务到其他 pCPU */
	CPSF_PINNED		= (1 << 2),	/* 已绑定：被 RT VM 独占 */
	CPSF_DO_NOT_USE		= (1 << 3),	/* 不可用：硬件故障或维护中 */
	CPSF_LIMITED_IO		= (1 << 4),	/* I/O 受限：适合计算密集型任务 */
};

/*
 * 能力特性位
 * 指示 pv_sched_table 支持哪些扩展功能
 */
#define CPS_FEAT_NUMA		(1ULL << 0)	/* 支持 NUMA 拓扑信息 */
#define CPS_FEAT_LLC		(1ULL << 1)	/* 支持 LLC 缓存拓扑 */
#define CPS_FEAT_THERMAL	(1ULL << 2)	/* 支持温度压力信息 */
#define CPS_FEAT_IRQ_PRESSURE	(1ULL << 3)	/* 支持中断压力信息 */
#define CPS_FEAT_CAPACITY	(1ULL << 4)	/* 支持 CPU capacity 信息 */

/*
 * pv_sched_table 头部结构
 *
 * 内存布局：
 * +-------------------+
 * | struct cps_hdr    | <- 页首，64 字节对齐
 * +-------------------+
 * | 位图区            | <- 偏移 bm_offset
 * |  - bm_avail       |    (每个位图 BITS_TO_LONGS(rows) * 8 字节)
 * |  - bm_migrating   |
 * |  - bm_rt          |
 * |  - bm_lowload     |
 * +-------------------+
 * | 行数组            | <- 偏移 rows_offset
 * | struct cps_pcpu_row[rows] |
 * +-------------------+
 * | 扩展区 (可选)     | <- 偏移 ext_offset (如果非 0)
 * +-------------------+
 */
struct cps_hdr {
	__u32 version;		/* CPS_UAPI_VERSION */
	__u16 rows;		/* 本 VM 可见的 pCPU 行数 N */
	__u16 row_size;		/* sizeof(struct cps_pcpu_row) */

	__u32 bm_offset;	/* 位图区相对页首偏移（字节） */
	__u32 rows_offset;	/* 行数组相对页首偏移（字节） */
	__u32 ext_offset;	/* 扩展区相对页首偏移（字节），0=无 */

	/*
	 * Epoch 序列号（用于无锁一致性协议）
	 * - 偶数：数据稳定，可以安全读取
	 * - 奇数：正在更新中，读取可能不一致
	 *
	 * Guest 读取流程：
	 * 1. epoch_start = READ_ONCE(hdr->epoch)
	 * 2. if (epoch_start & 1) retry;  // 奇数，正在写入
	 * 3. 读取数据
	 * 4. epoch_end = READ_ONCE(hdr->epoch)
	 * 5. if (epoch_start != epoch_end) retry;  // 数据可能被更新
	 */
	volatile __u32 epoch;
	__u32 reserved0;

	__u64 features;		/* 能力位：CPS_FEAT_* */
	__u64 rsvd_hdr[3];	/* 保留字段，用于未来扩展 */
} __attribute__((aligned(64)));

/*
 * 单个 pCPU 的信息行
 *
 * Host 周期性（1-5ms）更新这些字段，Guest 只读访问
 */
struct cps_pcpu_row {
	__u32 pcpu_id;		/* 物理 CPU ID */
	__u8  load;		/* 负载等级：enum cps_load */
	__u8  flags;		/* 状态标志：enum cps_state_flags */
	__u8  irq_pressure;	/* 中断压力：0-100（百分比） */
	__u8  thermal_pr;	/* 温度压力：0-100（百分比） */

	__u16 capacity_pc;	/* CPU capacity：0-1000（千分比） */
	__u16 numa_id;		/* NUMA 节点 ID */
	__u32 llc_id;		/* Last Level Cache ID */

	/*
	 * 负载详细信息（可选）
	 * 如果 features & CPS_FEAT_CAPACITY，这些字段有效
	 */
	__u32 util_avg;		/* 平均利用率：0-1024 (PELT) */
	__u32 runnable_avg;	/* 可运行任务平均值：0-1024 */

	__u32 reserved[2];	/* 保留字段 */
} __attribute__((aligned(32)));

/*
 * 位图区的布局
 *
 * 位图区包含 4 个位图，每个位图有 rows 位（向上对齐到 long）
 * 位图按顺序排列：
 *   1. bm_avail:     CPSF_AVAIL 标志的快速查找
 *   2. bm_migrating: CPSF_MIGRATING 标志的快速查找
 *   3. bm_rt:        CPSF_PINNED 标志的快速查找（RT VM 独占）
 *   4. bm_lowload:   load == CPS_LOAD_LOW 的快速查找
 *
 * 使用位图可以加速 Guest 侧的 CPU 筛选，达到 O(1) 复杂度
 */

/*
 * 辅助宏：计算位图大小（字节数）
 * @nr_cpus: pCPU 数量
 */
#define CPS_BITMAP_SIZE(nr_cpus) \
	(((nr_cpus) + 63) / 64 * 8)  /* 向上对齐到 8 字节 (64 位) */

/*
 * 辅助宏：计算整个位图区大小（4 个位图）
 */
#define CPS_BITMAP_AREA_SIZE(nr_cpus) \
	(CPS_BITMAP_SIZE(nr_cpus) * 4)

/*
 * 辅助宏：计算行数组大小
 */
#define CPS_ROWS_AREA_SIZE(nr_cpus) \
	(sizeof(struct cps_pcpu_row) * (nr_cpus))

/*
 * 辅助宏：计算整个 pv_sched_table 所需的最小大小
 */
#define CPS_TABLE_SIZE(nr_cpus) \
	(sizeof(struct cps_hdr) + \
	 CPS_BITMAP_AREA_SIZE(nr_cpus) + \
	 CPS_ROWS_AREA_SIZE(nr_cpus))

#endif /* _UAPI_LINUX_PV_SCHED_H */
