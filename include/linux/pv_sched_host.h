/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_PV_SCHED_HOST_H
#define _LINUX_PV_SCHED_HOST_H

/*
 * Paravirtualized Scheduling - Host 端内部数据结构
 *
 * 用于 KVM Host 端管理全局 pCPU 状态和 per-VM pv_sched_table
 */

#include <linux/types.h>
#include <linux/spinlock.h>
#include <linux/cpumask.h>
#include <linux/hrtimer.h>
#include <uapi/linux/pv_sched.h>

/*
 * pCPU 所有者类型
 * 用于全局状态表，标识每个 pCPU 当前属于哪个区域
 */
enum cps_pcpu_owner_type {
	CPS_OWNER_NONE		= 0,	/* 未分配 */
	CPS_OWNER_HOST		= 1,	/* 属于 Host（保留 Region） */
	CPS_OWNER_RT_VM		= 2,	/* 属于某个 RT VM（独占） */
	CPS_OWNER_LINUX_POOL	= 3,	/* 属于 Linux VM 池（共享） */
};

/*
 * pCPU 分配状态
 * 用于跟踪 pCPU 的分配流程（特别是 RT VM 的迁移流程）
 */
enum cps_pcpu_state {
	CPS_STATE_FREE		= 0,	/* 空闲：未被任何 VM 使用 */
	CPS_STATE_ALLOCATED	= 1,	/* 已分配：正常使用中 */
	CPS_STATE_MIGRATING	= 2,	/* 迁移中：等待 Guest 迁移完成 */
};

/*
 * 全局状态表的单个槽位
 * 每个 pCPU 对应一个槽位，记录其当前的所有者和状态
 */
struct cps_pcpu_slot {
	enum cps_pcpu_owner_type owner_type;	/* 所有者类型 */
	u32 owner_id;				/* 所有者 ID（RT VM ID 或 0） */
	enum cps_pcpu_state state;		/* 分配状态 */
	u64 timestamp;				/* 最后更新时间（ns） */
};

/*
 * 全局 pCPU 状态表
 *
 * 这是整个系统的中心数据结构，管理所有 pCPU 的分配和状态
 * 由 spinlock 保护，所有修改操作必须持锁
 */
struct cps_global_state {
	spinlock_t lock;			/* 保护下面的所有字段 */

	/* pCPU 状态数组 */
	struct cps_pcpu_slot pcpu_state[NR_CPUS];

	/* 快速查找位图（加速分配算法） */
	cpumask_var_t bm_free;			/* 空闲 pCPU */
	cpumask_var_t bm_host;			/* Host 保留的 pCPU */
	cpumask_var_t bm_rt;			/* RT VM 独占的 pCPU */
	cpumask_var_t bm_linux_pool;		/* Linux VM 池的 pCPU */

	/* 统计信息 */
	u32 nr_free;				/* 空闲 pCPU 数量 */
	u32 nr_host;				/* Host 保留数量 */
	u32 nr_rt;				/* RT VM 独占数量 */
	u32 nr_linux_pool;			/* Linux Pool 数量 */
};

/*
 * per-VM 的 pv_sched_table 管理结构
 *
 * 每个 Linux VM 都有一个独立的 pv_sched_table，Host 周期性更新
 */
struct cps_vm_table {
	/* 基本信息 */
	u32 vm_id;				/* VM ID（用于调试） */
	u32 nr_cpus;				/* 该 VM 可见的 pCPU 数量 */

	/* 共享内存 */
	void *base;				/* pv_sched_table 的内核虚拟地址 */
	struct page **pages;			/* 物理页数组 */
	u32 nr_pages;				/* 页数量 */
	u64 gpa;				/* 映射到 Guest 的物理地址 */

	/* 快速访问指针（指向 base 内的各个区域） */
	struct cps_hdr *hdr;			/* 头部 */
	unsigned long *bm_avail;		/* 位图：可用 */
	unsigned long *bm_migrating;		/* 位图：迁移中 */
	unsigned long *bm_rt;			/* 位图：RT 独占 */
	unsigned long *bm_lowload;		/* 位图：低负载 */
	struct cps_pcpu_row *rows;		/* 行数组 */

	/* 更新控制 */
	struct hrtimer update_timer;		/* 周期性更新定时器 */
	u64 update_period_ns;			/* 更新周期（ns），默认 2ms */
	atomic_t enabled;			/* 是否启用周期性更新 */

	/* 链表节点（用于全局 VM 表链表） */
	struct list_head list;			/* 链表节点 */

	/* 统计信息 */
	u64 nr_updates;				/* 总更新次数 */
	u64 last_update_ns;			/* 上次更新时间 */
};

/*
 * Host 端初始化和清理
 */
int cps_global_init(void);
void cps_global_cleanup(void);

/*
 * 全局状态查询
 */
int cps_query_pcpu_state(int cpu, struct cps_pcpu_slot *out);
void cps_get_pcpu_bitmap(cpumask_t *out, enum cps_pcpu_owner_type owner_type);

/*
 * per-VM 表管理
 */
struct cps_vm_table *cps_vm_table_alloc(u32 vm_id, int nr_cpus);
void cps_vm_table_free(struct cps_vm_table *table);
int cps_vm_table_map_to_guest(struct cps_vm_table *table, u64 gpa);

/*
 * RT VM 的 pCPU 分配和释放
 *
 * @vm_id: RT VM 的唯一标识符
 * @request_cpus: 请求的 pCPU 集合
 * @timeout_ms: 迁移超时时间（ms），0 = 使用默认值 100ms
 *
 * 返回值：
 *   0: 成功
 *   -EBUSY: pCPU 已被占用
 *   -ETIMEDOUT: 迁移超时
 *   -EINVAL: 参数无效
 */
int cps_allocate_rt_cpus(u32 vm_id, const cpumask_t *request_cpus, u32 timeout_ms);
int cps_release_rt_cpus(u32 vm_id);

/*
 * pv_sched_table 更新（周期性 + 触发式）
 */
int cps_update_vm_table(struct cps_vm_table *table);
int cps_update_linux_vms_for_migration(const cpumask_t *cpus, bool set);
int cps_update_linux_vms_for_rt_allocation(const cpumask_t *cpus);
int cps_update_linux_vms_for_release(const cpumask_t *cpus);

/*
 * 启动/停止周期性更新
 */
int cps_vm_table_start_updates(struct cps_vm_table *table);
void cps_vm_table_stop_updates(struct cps_vm_table *table);

/*
 * 迁移辅助函数
 */
int cps_wait_migration_done(const cpumask_t *cpus, u32 timeout_ms);

/*
 * 调试接口
 */
#ifdef CONFIG_DEBUG_FS
int cps_host_debugfs_init(void);
void cps_host_debugfs_cleanup(void);
#else
static inline int cps_host_debugfs_init(void) { return 0; }
static inline void cps_host_debugfs_cleanup(void) { }
#endif

#endif /* _LINUX_PV_SCHED_HOST_H */
