#ifndef _UAPI_PVSCHED_H
#define _UAPI_PVSCHED_H

/* vCPU 数量上限，内核与用户态共用 */
#define PVSCHED_MAX_VCPU 16
/*
 * 定点数缩放比例：将 0.0 - 1.0 映射为 0 - 1024
 * 0.5 表示为 512，1.0 表示为 1024
 */
#define PVSCHED_PRESSURE_SCALE 1024
#define PVSCHED_MAGIC 'P'

#ifndef __bpf__
#ifdef __KERNEL__
#include <linux/types.h>
#include <linux/ioctl.h>
#else
#include <stdint.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#endif

/* -------------------------------------------------------------------------
 * ioctl 接口
 * ---------------------------------------------------------------------- */

/**
 * struct pvsched_init_args - PVSCHED_INIT ioctl 的用户态参数
 * @vcpu_num:    该虚拟机的 vCPU 数量，范围 [1, PVSCHED_MAX_VCPU]
 * @_pad:        显式填充，保证 interval_ns 8 字节对齐，禁止直接使用
 * @interval_ns: 配额计算的控制窗口长度，单位纳秒，必须大于 0
 */
struct pvsched_init_args {
	uint32_t vcpu_num;
	uint32_t _pad;
	uint64_t interval_ns;
};

#define PVSCHED_INIT _IOW(PVSCHED_MAGIC, 0, struct pvsched_init_args)
#define PVSCHED_EXIT _IO(PVSCHED_MAGIC, 1)

/* -------------------------------------------------------------------------
 * 共享内存数据结构
 * ---------------------------------------------------------------------- */

/**
 * struct pvsched_info - 单个 vCPU 的调度元数据
 *
 * @qos_pressure: QoS 压力值，范围 [0, PVSCHED_PRESSURE_SCALE]
 *                计算方法：(uint64_t)(real_value * PVSCHED_PRESSURE_SCALE)
 * @update_seq:   单调递增序列号，Guest 每次更新 qos_pressure 后递增，
 *                Host 通过比较前后值判断 Guest 是否推送了新数据
 * @tokens:       Host 分配的剩余 CPU 配额，单位纳秒，由 Host 写入，
 *                Guest 和 scx BPF 程序只读
 */
struct pvsched_info {
#ifdef __KERNEL__
	atomic64_t qos_pressure;
	atomic64_t update_seq;
	atomic64_t tokens;
#else
	_Atomic uint64_t qos_pressure;
	_Atomic uint64_t update_seq;
	_Atomic int64_t tokens;
#endif
};

/**
 * struct pvsched_shared_mem - 单个虚拟机的共享内存区域布局
 *
 * 整个结构体固定在虚拟机地址空间的 1TB（0x10000000000）处，
 * 大小不超过一个 4KB 页（支持最多 PVSCHED_MAX_VCPU 个 vCPU）。
 *
 * epoch 用于保证 Guest 写入数据时的一致性快照：
 *   写端（Guest/QEMU）：
 *     1. 原子递增 epoch（变为奇数）
 *     2. 写入 info 数组数据
 *     3. 原子递增 epoch（变为偶数）
 *   读端（Host/scx）：
 *     1. 读取 epoch1
 *     2. 读取 info 数组数据
 *     3. 读取 epoch2
 *     4. epoch1 == epoch2 且为偶数时，数据有效
 *
 * @epoch:    版本计数器，偶数表示数据完整，奇数表示写入进行中
 * @tgid:     对应 QEMU 进程的线程组 ID
 * @vcpu_num: 该虚拟机分配的 vCPU 数量，写入后不再变更
 * @info:     各 vCPU 的调度元数据数组，长度由 vcpu_num 决定
 */
struct pvsched_shared_mem {
#ifdef __KERNEL__
	atomic_t epoch;
#else
	_Atomic uint32_t epoch;
#endif
	pid_t tgid;
	uint32_t vcpu_num;

	struct pvsched_info info[0];
};

#endif /* !__bpf__ */

#endif /* _UAPI_PVSCHED_H */
