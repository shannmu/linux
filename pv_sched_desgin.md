# pv_sched 设计文档

## 1. 背景

在"**卫星地面站一体化系统**"中，地面控制中心通常同时运行两类关键系统：

1. **实时系统（RTOS）**：执行轨控指令、姿态控制、数据采集、时间同步等硬实时任务；
2. **业务系统（Linux 或通用 OS）**：处理遥测数据、进行图像分析、通信中继、数据库与云接入。

为了提升系统安全性与隔离性，工程上通常采用 **虚拟化（Virtualization）** 的方式，将实时域与业务域分区运行。
然而，现有方案多为 **静态分区（Static Partitioning）**：

- 在系统启动时固定分配物理核（pCPU）给 RTOS 与 Linux；
- RTOS 的核不能被调度到其他任务，Linux 不能使用这些核；
- 导致 **资源利用率低、业务弹性不足**。

在卫星地面站的场景下，任务负载具有明显的"时变性"：

- 卫星测控窗口开启时，实时任务负载上升；
- 非测控阶段，实时负载下降，而业务处理需求上升。

因此需要一种能够：

> **在保证实时调度确定性的前提下，实现资源弹性分配的混合虚拟化系统。**

本研究提出的 **CPS-based Mixed System Scheduling Architecture**
（基于协同半虚拟化调度的混合系统调度架构）
正是为解决此问题而设计。

## 2. 总体目标

| 目标         | 英文描述                                                     |
| ------------ | ------------------------------------------------------------ |
| **实时保证** | Maintain deterministic scheduling for RTOS without preemption or interference. |
| **资源弹性** | Allow Linux (business domain) VMs to use freed CPUs when RTOS load is low. |
| **多域隔离** | Ensure strong isolation between RT and Non-RT domains, with controlled resource reallocation. |

## 3. 系统架构

### 3.1 CPU 分区策略

系统将物理 CPU 分为两个区域：

- **Host Region**（最小化）：固定少量 pCPU（如 pCPU0-1）用于 Host 管理任务
- **VM Region**（弹性池）：其余 pCPU 用于 VM 调度，支持 RT VM 和 Linux VM 的弹性分配

**内核启动参数示例**：
```bash
isolcpus=domain,managed_irq 0-1   # Host 保留核心
nohz_full=2-31                     # VM Region 关闭 timer tick
rcu_nocbs=2-31                     # RCU callback 不在 VM Region 执行
irqaffinity=0-1                    # 中断只绑定到 Host Region
```

### 3.2 vCPU:pCPU 映射模型

系统中的 VM 根据实时性需求分为两类，采用不同的映射策略：

#### 3.2.1 RT VM：1:1 静态固定映射

- **特性**：每个 vCPU 严格绑定到一个专属 pCPU，关系固定不变
- **目的**：保证调度确定性，消除虚拟化调度抖动
- **实现**：
  - VM 创建时，Host 分配 N 个专属 pCPU（与 vCPU 数量一致）
  - 通过 `set_cpus_allowed_ptr` 将每个 vCPU 线程硬绑定到对应 pCPU
  - 这些 pCPU **不会**被其他任何 VM（包括其他 RT VM）调度使用
- **pv_sched_table**：RT VM **不需要感知** `pv_sched_table`，Host 在底层保证隔离性

**示例**：
```
RT-VM-A: 4 vCPU
  vCPU0 → pCPU8  (固定)
  vCPU1 → pCPU9  (固定)
  vCPU2 → pCPU10 (固定)
  vCPU3 → pCPU11 (固定)

pCPU8-11 被 RT-VM-A 独占，不会被调度到其他任何 vcpu 线程
```

#### 3.2.2 Linux VM：M:N 动态映射

- **特性**：M 个 vCPU 可以运行在 N 个 pCPU 上（M ≥ N），映射关系动态变化
- **目的**：充分利用可用 pCPU，根据负载智能调度
- **实现**：
  - VM 配置时可以创建较多 vCPU（如 16 个）
  - Host 根据资源可用性，动态分配 N 个 pCPU（如当前分配 8 个）
  - Guest 通过 `pv_sched_table` 感知可用的 pCPU 集合
  - Guest 调度器根据负载信息选择"最优 vCPU"（对应最优 pCPU）
- **弹性**：当 RT VM 释放 pCPU 时，N 可以增加；征用时，N 会减少

**示例**：
```
Linux-VM-B: 16 vCPU, 当前可用 8 pCPU
  vCPU0-15 → {pCPU12-19} (动态选择)

Guest 调度器根据 pv_sched_table 中的负载信息：
  - 任务 A 选择 vCPU3 (对应 pCPU15，当前负载低)
  - 任务 B 选择 vCPU7 (对应 pCPU19，NUMA 亲和性好)

当 pCPU14 被征用给新 RT VM：
  → Host 更新 pv_sched_table，清除 pCPU14 的 CPSF_AVAIL
  → Guest 下次调度时避开对应的 vCPU，使用其余 7 个 pCPU
```

### 3.3 RT VM 实时性保证

对 RT VM 的实时性可能产生影响的部分：

| 事件类型                        | 是否触发调度 | 说明                                    |
| ------------------------------- | ------------ | --------------------------------------- |
| Host 外部中断（IPI/NMI）        | ✅ 可能       | 若 pCPU 仍接收 Host IPI，会导致 VM-exit |
| Host 强制调度（signal/stop）    | ✅ 可能       | 比如管理员 `kill -STOP` QEMU            |
| Host timer tick (nohz_full off) | ✅ 可能       | 若没隔离，会中断 Guest                  |
| Host softirq / IRQ 绑定错误     | ✅ 可能       | 若 IRQ affinity 没设置好，会打断 Guest  |
| 内存访问 EPT violation          | ✅ 可能       | 如果 Guest 内存页不常驻                 |

**实时保证措施**：

1. **CPU 隔离**：通过上述内核参数彻底隔离 VM Region
2. **内存常驻**：RT VM 使用 pinned memory，避免 page fault
3. **设备直通**：没有 virtio；所有设备通过 VFIO / passthrough
4. **最小化 hypercall**：RT VM 避免使用 hypercall

### 3.4 RT VM vCPU Pinning 多层隔离机制

为实现 RT VM 的 **1:1 静态固定映射** 并确保 pCPU 独占，需要在多个层面实施隔离。单纯使用 `set_cpus_allowed_ptr()` 是不够的。

#### 3.4.1 为什么需要多层隔离？

`set_cpus_allowed_ptr()` 仅设置线程的 CPU 亲和性（软限制），存在以下问题：

| 问题 | 描述 | 后果 |
|------|------|------|
| **无法独占 CPU** | 其他 Host 任务仍可能在该 pCPU 上运行 | RT vCPU 被 ksoftirqd/kworker 抢占 |
| **无法阻止中断** | 硬件中断仍会路由到该 pCPU | RT vCPU 被中断处理打断 |
| **缺少反向隔离** | 其他 VM 的调度器不知道该 pCPU 被占用 | 其他 Linux VM 的 vCPU 可能被调度到此 pCPU |
| **无法防止抢占** | 如果优先级不够高，仍可能被抢占 | RT vCPU 调度延迟不确定 |

#### 3.4.2 多层隔离架构

```
┌─────────────────────────────────────────────────────────────┐
│                     隔离层级（由底层到上层）                 │
├─────────────────────────────────────────────────────────────┤
│ 第 1 层：内核启动参数（CPU 分区）                           │
│   - isolcpus, nohz_full, rcu_nocbs, irqaffinity              │
│   - 作用：将 VM Region 从 Host 调度域中隔离                 │
│   - 防止：Host 调度器、timer tick、RCU、中断干扰            │
├─────────────────────────────────────────────────────────────┤
│ 第 2 层：cgroup cpuset 隔离（可选但推荐）                   │
│   - 将 RT VM 进程组限制到特定 pCPU 集合                     │
│   - 作用：进程组级别的 CPU 使用边界                         │
│   - 防止：其他 VM 的 vCPU 线程使用 RT 的 pCPU               │
├─────────────────────────────────────────────────────────────┤
│ 第 3 层：set_cpus_allowed_ptr() 精细绑定                    │
│   - 将单个 vCPU 线程绑定到单个 pCPU                         │
│   - 作用：实现 1:1 映射                                      │
│   - 防止：vCPU 线程在多个 pCPU 间漂移                       │
├─────────────────────────────────────────────────────────────┤
│ 第 4 层：SCHED_FIFO 实时调度策略                            │
│   - 提升 vCPU 线程优先级到最高                              │
│   - 作用：确保 vCPU 线程不被抢占                            │
│   - 防止：vCPU 被普通任务抢占                               │
├─────────────────────────────────────────────────────────────┤
│ 第 5 层：cps_allocate_rt_cpus() 全局协调                    │
│   - 更新全局状态表 + 通知所有 Linux VM                      │
│   - 作用：跨 VM 的资源协调                                  │
│   - 防止：其他 Linux VM 的调度器选择这些 pCPU               │
└─────────────────────────────────────────────────────────────┘
```

#### 3.4.3 详细实现

##### 第 1 层：内核启动参数（CPU 分区）

```bash
# 假设系统有 32 个 pCPU (0-31)
isolcpus=domain,managed_irq 0-1   # pCPU 0-1 给 Host 保留
nohz_full=2-31                     # pCPU 2-31 关闭 timer tick
rcu_nocbs=2-31                     # RCU callback 不在这些 CPU 执行
irqaffinity=0-1                    # 中断只绑定到 pCPU 0-1
```

**效果**：
- `isolcpus=domain,managed_irq 0-1`：pCPU 0-1 从调度域中隔离，普通任务不会被调度到这些 CPU
- `nohz_full=2-31`：pCPU 2-31 进入 "tickless" 模式，timer tick 不会打断运行的任务
- `rcu_nocbs=2-31`：RCU callback 在 pCPU 0-1 执行，不打断 pCPU 2-31 的任务
- `irqaffinity=0-1`：硬件中断只路由到 pCPU 0-1

**作用**：将 pCPU 2-31 打造成 "VM Region"，Host 的调度器和中断默认不会使用这些 CPU。

##### 第 2 层：cgroup cpuset 隔离

```bash
# 创建 RT VM 专属的 cgroup
mkdir /sys/fs/cgroup/rt_vm_1
echo "8-11" > /sys/fs/cgroup/rt_vm_1/cpuset.cpus
echo "exclusive" > /sys/fs/cgroup/rt_vm_1/cpuset.cpus.partition
echo 0 > /sys/fs/cgroup/rt_vm_1/cpuset.mems

# 将 RT VM 的所有进程放入这个 cgroup
echo $QEMU_PID > /sys/fs/cgroup/rt_vm_1/cgroup.procs
```

**效果**：
- QEMU 进程及其所有子线程（vCPU 线程）只能在 pCPU 8-11 上运行
- `exclusive` 模式防止其他 cgroup 使用这些 CPU

##### 第 3-5 层：KVM 中的实现

###### 用户空间（QEMU）创建 RT VM

```c
// QEMU 代码（简化）
int create_rt_vm(int nr_vcpus, int *pcpu_ids)
{
    int kvm_fd = open("/dev/kvm", O_RDWR);
    int vm_fd = ioctl(kvm_fd, KVM_CREATE_VM, 0);

    // 新增的 ioctl：标记这是一个 RT VM
    struct kvm_rt_vm_config config = {
        .nr_vcpus = nr_vcpus,
        .pcpu_ids = pcpu_ids,  // [8, 9, 10, 11]
    };
    ioctl(vm_fd, KVM_SET_RT_VM_CONFIG, &config);

    // 创建 vCPU（会自动绑定到对应的 pCPU）
    for (int i = 0; i < nr_vcpus; i++) {
        int vcpu_fd = ioctl(vm_fd, KVM_CREATE_VCPU, i);
    }

    return vm_fd;
}
```

###### KVM 处理 RT VM 配置（新增 ioctl）

```c
// virt/kvm/kvm_main.c
long kvm_vm_ioctl(struct file *filp, unsigned int ioctl, unsigned long arg)
{
    struct kvm *kvm = filp->private_data;

    switch (ioctl) {
    case KVM_SET_RT_VM_CONFIG: {
        struct kvm_rt_vm_config config;
        if (copy_from_user(&config, argp, sizeof(config)))
            return -EFAULT;

        // 1. 标记 KVM 为 RT 模式
        kvm->is_rt_vm = true;
        kvm->nr_rt_vcpus = config.nr_vcpus;
        kvm->rt_pcpu_ids = kmalloc_array(config.nr_vcpus,
                                          sizeof(int), GFP_KERNEL);
        memcpy(kvm->rt_pcpu_ids, config.pcpu_ids,
               config.nr_vcpus * sizeof(int));

        // 2. 调用 cps_allocate_rt_cpus()（第 5 层）
        cpumask_t mask;
        cpumask_clear(&mask);
        for (int i = 0; i < config.nr_vcpus; i++)
            cpumask_set_cpu(config.pcpu_ids[i], &mask);

        int ret = cps_allocate_rt_cpus(kvm->vm_id, &mask);
        if (ret < 0) {
            // 资源冲突，创建失败
            kfree(kvm->rt_pcpu_ids);
            return ret;
        }

        return 0;
    }
    // ...
    }
}
```

###### KVM 创建 RT vCPU 时绑定（第 3-4 层）

```c
// arch/x86/kvm/x86.c
int kvm_arch_vcpu_create(struct kvm_vcpu *vcpu)
{
    // ... 常规初始化

    // 如果是 RT VM，执行绑定
    if (vcpu->kvm->is_rt_vm) {
        int vcpu_id = vcpu->vcpu_id;
        int pcpu_id = vcpu->kvm->rt_pcpu_ids[vcpu_id];

        // 第 3 层：CPU 亲和性绑定
        cpumask_t mask;
        cpumask_clear(&mask);
        cpumask_set_cpu(pcpu_id, &mask);
        set_cpus_allowed_ptr(current, &mask);  // current = vCPU 线程

        // 第 4 层：设置实时调度策略
        struct sched_param param = { .sched_priority = MAX_RT_PRIO - 1 };
        sched_setscheduler(current, SCHED_FIFO, &param);

        // 防止 vCPU 被迁移（额外保险）
        vcpu->cpu_pinned = true;
        vcpu->pinned_pcpu = pcpu_id;

        pr_info("RT vCPU %d pinned to pCPU %d\n", vcpu_id, pcpu_id);
    }

    return 0;
}
```

###### 防止 RT vCPU 被迁移

```c
// virt/kvm/kvm_main.c
static void vcpu_load_on_cpu(struct kvm_vcpu *vcpu, int cpu)
{
    // RT vCPU 不允许跨 CPU 迁移
    if (vcpu->cpu_pinned && cpu != vcpu->pinned_pcpu) {
        WARN_ONCE(1, "RT vCPU %d attempted migration from pCPU %d to %d\n",
                  vcpu->vcpu_id, vcpu->pinned_pcpu, cpu);
        return;  // 拒绝迁移
    }

    // ... 正常的 vcpu_load 逻辑
}
```

#### 3.4.4 隔离层级对比

| 隔离层级 | 实现方式 | 防止的干扰 | 是否必需 |
|---------|---------|-----------|---------|
| 内核参数 | isolcpus/nohz_full/rcu_nocbs/irqaffinity | Host 调度器、tick、RCU、中断 | ✅ 必需 |
| cgroup cpuset | cpuset.cpus + exclusive | 其他进程组使用这些 CPU | 🟡 推荐 |
| set_cpus_allowed_ptr | 线程级亲和性 | vCPU 线程跨 CPU 漂移 | ✅ 必需 |
| SCHED_FIFO | 实时调度策略 | vCPU 被普通任务抢占 | ✅ 必需 |
| cps_allocate_rt_cpus | 全局状态表 + pv_sched_table | 其他 VM 的调度器选择这些 pCPU | ✅ 必需 |

#### 3.4.5 完整的创建流程

```
用户空间（QEMU）
    │
    │ ioctl(KVM_SET_RT_VM_CONFIG)
    ▼
Host Kernel (KVM)
    │
    ├─► 调用 cps_allocate_rt_cpus()  ◄─ 第 5 层
    │      │
    │      ├─► 检查资源可用性
    │      ├─► 启动优雅迁移（通知 Linux VM）
    │      ├─► 更新全局状态表
    │      └─► 更新所有 Linux VM 的 pv_sched_table
    │
    ├─► ioctl(KVM_CREATE_VCPU)
    │      │
    │      ├─► set_cpus_allowed_ptr()  ◄─ 第 3 层
    │      ├─► sched_setscheduler(SCHED_FIFO)  ◄─ 第 4 层
    │      └─► vcpu->cpu_pinned = true
    │
    └─► RT vCPU 运行在专属 pCPU 上（完全隔离）
```

#### 3.4.6 与 Linux VM 的对比

| 特性 | RT VM | Linux VM |
|------|-------|----------|
| vCPU:pCPU 映射 | 1:1 静态固定 | M:N 动态 |
| 需要 pv_sched_table | ❌ 不需要 | ✅ 需要 |
| CPU 亲和性 | 硬绑定（set_cpus_allowed_ptr + SCHED_FIFO） | 动态调整 |
| 调度策略 | SCHED_FIFO（实时） | SCHED_NORMAL（CFS） |
| pCPU 独占性 | ✅ 完全独占 | ❌ 共享池 |
| 隔离层级 | 5 层（全部必需） | 不需要 |
| 创建方式 | KVM_SET_RT_VM_CONFIG ioctl | 常规 KVM API |

## 4. pv_sched_table 设计

### 4.1 数据结构定义

`pv_sched_table` 是 Host 与 Linux Guest 之间的共享内存表，用于传递 pCPU 负载信息和可用性状态。

```c
/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
#ifndef _UAPI_LINUX_CPS_H
#define _UAPI_LINUX_CPS_H

#include <linux/types.h>

#define CPS_UAPI_VERSION   2
#define CPS_DEFAULT_GPA    0x88000000ULL
#define CPS_DEFAULT_SIZE   0x00010000ULL /* 64 KiB */

/* --- 标准化压力/状态枚举（可按需扩展） --- */
enum cps_load : __u8 {
    CPS_LOAD_LOW  = 0,
    CPS_LOAD_MED  = 1,
    CPS_LOAD_HIGH = 2,
};

enum cps_state_flags {
    CPSF_AVAIL        = 1u << 0, /* 此 pCPU 对本 VM 可用 */
    CPSF_MIGRATING    = 1u << 1, /* Host 正在调整该 pCPU 的归属 */
    CPSF_PINNED       = 1u << 2, /* 固定给 RT vCPU */
    CPSF_DO_NOT_USE   = 1u << 3, /* 暂不可用（降温/维护/异常） */
    CPSF_LIMITED_IO   = 1u << 4, /* 该核有设备直通/IO 压力 */
};

struct cps_hdr {
    __u32 version;       /* CPS_UAPI_VERSION */
    __u16 rows;          /* 本 VM 可见的 pCPU 行数 N */
    __u16 row_size;      /* sizeof(struct cps_pcpu_row) */

    __u32 bm_offset;     /* 位图区相对页首偏移（字节） */
    __u32 rows_offset;   /* 行数组相对页首偏移（字节） */
    __u32 ext_offset;    /* 扩展区相对页首偏移（字节），0=无 */

    volatile __u32 epoch;/* 奇偶序列：偶=稳定，奇=写入中 */
    __u32 reserved0;

    __u64 features;      /* 能力位：bit0=have_numa, bit1=have_llc, ... */
    __u64 rsvd_hdr[3];
} __attribute__((aligned(64)));

/* 每行 = 一个 pCPU 的快照（对齐到 64B，避免伪共享） */
struct cps_pcpu_row {
    __u16 pcpu_id;       /* 物理 CPU 号（全局） */
    __u16 numa_id;       /* NUMA 节点 */
    __u16 llc_id;        /* Last-level cache / LLC group id */
    __u16 cg_id;         /* Cache group */

    __u8  rt_flag;       /* 1=该 pCPU 当前属于 RT pool */
    __u8  load;          /* enum cps_load：LOW/MED/HIGH */
    __u8  irq_pressure;  /* 中断压力 (0..100) */
    __u8  thermal_pr;    /* 热压 (0..100) */

    __u8  capacity_pc;   /* 容量缩放（0..100，反映频点/降频） */
    __u8  runnable;      /* Host 观测 runnable tasks */
    __u8  steal_pc;      /* steal time 百分比 */
    __u8  rsv_byte;

    __u32 state;         /* enum cps_state_flags 位或 */

    __u16 hint_best_vcpu;/* 建议映射到此 pCPU 的 vCPU */
    __u16 hint_weight;   /* 权重（0..65535） */

    __u64 rsvd_row[3];   /* 预留扩展位 */
} __attribute__((aligned(64)));

#endif /* _UAPI_LINUX_CPS_H */
```

### 4.2 内存布局

```
pv_sched_table 内存结构（64 KiB 示例，32 个 pCPU）：

┌─────────────────────────────────────────────────────────────┐
│ Offset 0x0000: struct cps_hdr (128 bytes, cache-aligned)   │
├─────────────────────────────────────────────────────────────┤
│ Offset 0x0080: 位图区（Bitmap Section）                     │
│   bm_avail     [0x0080 - 0x0084)   4 bytes (32 bits)       │
│   bm_rt        [0x0084 - 0x0088)   4 bytes                 │
│   bm_lowload   [0x0088 - 0x008C)   4 bytes                 │
│   bm_migrating [0x008C - 0x0090)   4 bytes                 │
│   bm_llc0      [0x0090 - 0x0094)   4 bytes (可选)          │
│   bm_llc1      [0x0094 - 0x0098)   4 bytes (可选)          │
│   bm_numa0     [0x0098 - 0x009C)   4 bytes (可选)          │
│   bm_numa1     [0x009C - 0x00A0)   4 bytes (可选)          │
│   ... (预留更多位图)                                        │
├─────────────────────────────────────────────────────────────┤
│ Offset 0x0400: 行数组（Row Array）                          │
│   row[0]       [0x0400 - 0x0440)  64 bytes (pCPU 0)        │
│   row[1]       [0x0440 - 0x0480)  64 bytes (pCPU 1)        │
│   ...                                                       │
│   row[31]      [0x0BC0 - 0x0C00)  64 bytes (pCPU 31)       │
├─────────────────────────────────────────────────────────────┤
│ Offset 0x1000: 扩展区（Extension，可选）                    │
│   - NUMA 距离矩阵                                           │
│   - LLC 拓扑信息                                            │
│   - 带宽/能耗统计                                           │
└─────────────────────────────────────────────────────────────┘

计算公式：
  bitmap_size = round_up(rows, 64) / 8  （字节数）
  对于 32 个 pCPU: bitmap_size = round_up(32, 64) / 8 = 8 bytes
  对于 128 个 pCPU: bitmap_size = round_up(128, 64) / 8 = 16 bytes
```

### 4.3 位图语义

| 位图名称      | 位含义                          | 更新时机                        |
|---------------|--------------------------------|---------------------------------|
| `bm_avail`    | 1 = 该 pCPU 对本 VM 可用        | RT VM 创建/销毁时               |
| `bm_rt`       | 1 = 该 pCPU 当前属于 RT 池      | RT VM 创建/销毁时               |
| `bm_lowload`  | 1 = 负载 LOW/MED（可接受负载）  | 周期性更新（1-5ms）             |
| `bm_migrating`| 1 = 正在迁移中（优雅迁移阶段）  | 征用 pCPU 时设置，完成后清除    |
| `bm_llc[k]`   | 1 = 该 pCPU 属于 LLC group k    | VM 创建时初始化（拓扑静态）     |
| `bm_numa[n]`  | 1 = 该 pCPU 属于 NUMA node n    | VM 创建时初始化（拓扑静态）     |

### 4.4 访问协议（Epoch 机制）

`pv_sched_table` 使用 **epoch 机制** 保证无锁一致性：

#### Host 写入过程

```c
hdr->epoch++;          // odd - 开始写入
smp_wmb();
// 更新位图 + 更新部分或全部行
smp_wmb();
hdr->epoch++;          // even - 写入完成
```

#### Guest 读取过程

```c
do {
    u32 e1 = READ_ONCE(hdr->epoch);
    if (e1 & 1) continue;    // 等待写入完成
    smp_rmb();
    // 使用位图先筛：如 bm_avail & ~bm_rt & bm_lowload
    // 再少量查行，读取 pcpu_id/numa_id/llc_id/load/flags 等
    smp_rmb();
    u32 e2 = READ_ONCE(hdr->epoch);
} while (e1 != e2 || (e2 & 1));
```

**特性**：
- Host 更新期间（epoch 奇数），Guest 只等待
- Guest 读取期间，不会锁 Host
- Host 写入快，Guest 读取无阻塞 → 完全 lock-free

## 5. Host 端全局状态管理

### 5.1 数据结构设计

Host 内核维护全局的 pCPU 分配状态表，用于协调多个 VM 间的资源分配。

```c
/* Host 端全局 pCPU 状态管理 */

enum cps_pcpu_owner_type {
    CPS_OWNER_NONE = 0,      /* 未分配 / Host 保留 */
    CPS_OWNER_HOST,          /* Host 管理核心（如 isolcpus=0-1） */
    CPS_OWNER_RT_VM,         /* 属于某个 RT VM */
    CPS_OWNER_LINUX_POOL,    /* Linux VM 共享池 */
};

enum cps_pcpu_state {
    CPS_PCPU_FREE = 0,       /* 空闲，可分配 */
    CPS_PCPU_ALLOCATED,      /* 已分配给某个 VM */
    CPS_PCPU_MIGRATING,      /* 正在迁移中（征用前的优雅迁移） */
};

struct cps_pcpu_slot {
    enum cps_pcpu_owner_type owner_type;
    u32 owner_id;            /* VM ID（RT VM 时有效，0 表示共享池） */
    enum cps_pcpu_state state;

    /* 统计和调试信息 */
    u64 last_update_ns;      /* 上次状态更新时间 */
    u32 migration_timeout_ms;/* 迁移超时时间（MIGRATING 状态时有效） */
    u32 reserved;
};

/* 全局状态表 */
struct cps_global_state {
    spinlock_t lock;         /* 保护整个状态表 */
    struct cps_pcpu_slot pcpu_state[NR_CPUS];

    /* 快速查询位图（可选优化） */
    unsigned long bm_rt_occupied[BITS_TO_LONGS(NR_CPUS)];   /* RT VM 占用 */
    unsigned long bm_linux_pool[BITS_TO_LONGS(NR_CPUS)];    /* Linux 池 */
    unsigned long bm_migrating[BITS_TO_LONGS(NR_CPUS)];     /* 迁移中 */
};

static struct cps_global_state cps_global;
```

### 5.2 pCPU 状态转换图

```
pCPU 状态转换：

  ┌─────────────┐
  │ CPS_OWNER_  │ (系统启动时初始化)
  │   LINUX_    │
  │    POOL     │
  │  (FREE)     │
  └──────┬──────┘
         │
         │ RT VM 创建请求
         ▼
  ┌─────────────┐
  │ CPS_OWNER_  │ (优雅迁移阶段)
  │   LINUX_    │
  │    POOL     │
  │(MIGRATING)  │
  └──────┬──────┘
         │
         │ 迁移完成/超时
         ▼
  ┌─────────────┐
  │ CPS_OWNER_  │ (RT VM 运行中)
  │   RT_VM     │
  │(ALLOCATED)  │
  └──────┬──────┘
         │
         │ RT VM 销毁
         ▼
  ┌─────────────┐
  │ CPS_OWNER_  │ (释放回共享池)
  │   LINUX_    │
  │    POOL     │
  │  (FREE)     │
  └─────────────┘
```

### 5.3 同步机制

- **Spinlock 保护**：所有对 `cps_global` 的读写都必须持有 `cps_global.lock`
- **关键区最小化**：只在修改状态时加锁，更新 VM 的 `pv_sched_table` 在锁外进行
- **位图加速**：使用 `bm_rt_occupied` 等位图快速筛选可用 pCPU，避免遍历

## 6. pCPU 弹性调整流程

### 6.1 场景一：创建 RT VM（征用 pCPU）

当需要创建新的 RT VM 时，需要从 Linux VM 共享池中征用 pCPU。

**流程**：

#### 步骤 1：资源检查

```c
- 检查全局 pcpu_state[]，筛选出 OWNER_LINUX_POOL 且 FREE 的 pCPU
- 验证数量是否满足 RT VM 的 vCPU 需求
- 检查是否与现有 RT VM 冲突（RT VM 间不能共享 pCPU）
```

#### 步骤 2：启动优雅迁移

```c
spin_lock(&cps_global.lock);
for_each_cpu(cpu, request_cpus) {
    pcpu_state[cpu].state = CPS_PCPU_MIGRATING;
    pcpu_state[cpu].migration_timeout_ms = 100;
    set_bit(cpu, bm_migrating);
}
spin_unlock(&cps_global.lock);
```

#### 步骤 3：更新所有 Linux VM 的 pv_sched_table

```c
for_each_linux_vm(vm) {
    epoch++;  // odd
    smp_wmb();

    // 设置 MIGRATING 标志，清除 AVAIL
    for_each_cpu(cpu, request_cpus) {
        row->state |= CPSF_MIGRATING;
        row->state &= ~CPSF_AVAIL;
    }
    clear_bits(vm->bm_avail, request_cpus);
    set_bits(vm->bm_migrating, request_cpus);

    smp_wmb();
    epoch++;  // even
}
```

#### 步骤 4：等待 Guest 迁移完成

```c
- Guest Linux 看到 CPSF_MIGRATING 标志后，主动迁移这些 pCPU 上的任务
- Host 轮询或等待超时（如 100ms）
- 超时后强制继续（Guest 调度器会自动适应）
```

#### 步骤 5：分配给 RT VM

```c
spin_lock(&cps_global.lock);
for_each_cpu(cpu, request_cpus) {
    pcpu_state[cpu].owner_type = CPS_OWNER_RT_VM;
    pcpu_state[cpu].owner_id = rt_vm_id;
    pcpu_state[cpu].state = CPS_PCPU_ALLOCATED;
}
set_bits(bm_rt_occupied, request_cpus);
clear_bits(bm_linux_pool, request_cpus);
clear_bits(bm_migrating, request_cpus);
spin_unlock(&cps_global.lock);

// 创建 RT VM，硬绑定 vCPU 线程到 pCPU (set_cpus_allowed_ptr)
```

#### 步骤 6：更新所有 Linux VM 的 pv_sched_table（最终状态）

```c
for_each_linux_vm(vm) {
    epoch++;
    smp_wmb();

    for_each_cpu(cpu, request_cpus) {
        row->state &= ~CPSF_MIGRATING;  // 清除迁移标志
        row->rt_flag = 1;               // 标记为 RT
    }
    clear_bits(vm->bm_migrating, request_cpus);
    set_bits(vm->bm_rt, request_cpus);

    smp_wmb();
    epoch++;
}
```

### 6.2 场景二：销毁 RT VM（释放 pCPU）

当 RT VM 关闭或缩容时，释放专属 pCPU 到 Linux 共享池。

**流程**：

#### 步骤 1：停止 RT VM

```c
- 停止所有 vCPU 线程
- 确保这些 pCPU 上没有 RT 任务在运行
```

#### 步骤 2：更新全局状态

```c
spin_lock(&cps_global.lock);
for_each_cpu(cpu, rt_vm->cpus) {
    pcpu_state[cpu].owner_type = CPS_OWNER_LINUX_POOL;
    pcpu_state[cpu].owner_id = 0;
    pcpu_state[cpu].state = CPS_PCPU_FREE;
}
clear_bits(bm_rt_occupied, rt_vm->cpus);
set_bits(bm_linux_pool, rt_vm->cpus);
spin_unlock(&cps_global.lock);
```

#### 步骤 3：更新所有 Linux VM 的 pv_sched_table

```c
for_each_linux_vm(vm) {
    epoch++;
    smp_wmb();

    for_each_cpu(cpu, released_cpus) {
        row->state |= CPSF_AVAIL;       // 设置可用
        row->rt_flag = 0;               // 清除 RT 标志
        row->load = CPS_LOAD_LOW;       // 重置负载
    }
    set_bits(vm->bm_avail, released_cpus);
    clear_bits(vm->bm_rt, released_cpus);

    smp_wmb();
    epoch++;
}
```

#### 步骤 4：Linux Guest 自动发现

```c
- Guest 调度器在下次调度时（几毫秒内）发现新 pCPU 可用
- 开始将任务调度到这些 pCPU 对应的 vCPU 上
```

### 6.3 场景三：周期性负载更新

Host 以固定周期（建议 1-5ms）更新各 pCPU 的负载信息。

**流程**：

#### 步骤 1：采集负载数据

```c
for_each_cpu(cpu, linux_pool) {
    load = calculate_load(cpu);              // LOW/MED/HIGH
    irq_pressure = get_irq_pressure(cpu);    // 0-100
    thermal_pr = get_thermal_pressure(cpu);  // 0-100
    capacity = get_cpu_capacity(cpu);        // 0-100 (频率缩放)
}
```

#### 步骤 2：更新所有 Linux VM 的 pv_sched_table

```c
for_each_linux_vm(vm) {
    epoch++;
    smp_wmb();

    for_each_avail_cpu(vm, cpu) {
        row->load = load;
        row->irq_pressure = irq_pressure;
        row->thermal_pr = thermal_pr;
        row->capacity_pc = capacity;
    }
    update_bitmap(vm->bm_lowload, based_on_load_threshold);

    smp_wmb();
    epoch++;
}
```

#### 步骤 3：Guest 使用最新信息

```c
- Guest 调度器在每次任务唤醒/负载均衡时读取 pv_sched_table
- 根据最新负载选择最优 vCPU/pCPU
- 避开高负载、高温、高中断压力的核心
```

## 7. 优雅迁移协议

当 Host 需要征用 pCPU 给新 RT VM 时，使用 `CPSF_MIGRATING` 标志与 Guest 协作完成任务迁移。

### 7.1 Guest 视角的 pCPU 状态机

```
  ┌──────────┐
  │  AVAIL   │ (正常可用，可调度)
  └────┬─────┘
       │
       │ Host 设置 MIGRATING
       ▼
  ┌──────────┐
  │ MIGRATING│ (迁移中，降权重，避免新调度)
  └────┬─────┘
       │ Guest 主动迁移任务
       │
       │ Host 清除 AVAIL
       ▼
  ┌──────────┐
  │ RT / 不可│ (已被 RT VM 占用，Guest 完全避开)
  │   用      │
  └──────────┘
```

### 7.2 超时处理

- **合理超时**：建议 100ms，足够 Guest 完成几轮调度周期（典型调度延迟 < 10ms）
- **超时后行为**：
  - Host 强制收回 pCPU（Guest 调度器会在下次调度时自动适应）
  - 不会导致 Guest 崩溃或任务丢失（Linux 调度器容错性强）
  - 可能短暂影响 Guest 性能（部分任务被迫迁移）

### 7.3 Guest 最佳实践

为了配合优雅迁移，Guest 调度器应：

1. **定期检查 MIGRATING 标志**（在 `select_task_rq` 路径）
2. **主动触发负载均衡**（看到 MIGRATING 时，调用 `migrate_tasks`）
3. **避免新任务调度到 MIGRATING CPU**（在候选集筛选时排除）
4. **可选：暴露统计信息**（如通过共享内存告知 Host 迁移进度）

## 8. Guest 端实现

### 8.1 Linux VM 调度器集成

#### 数据源
per-VM `pv_sched_table`（pCPU rows + 位图）

#### 入口点
- **侵入式**：在 `ttwu_select_cpu()` / `select_task_rq_fair()` 前置 `cps_select_cpu()`
- **低侵入**：`sched_ext` BPF 调度器基于只读映射表做策略

#### 挑选流程

1. 快速构造候选集：`C = bm_avail & ~bm_rt & bm_lowload`
2. 若 `C` 为空 → 放宽到 `bm_avail & ~bm_rt` 或退回原生
3. 在 `C` 中按 `(numa距离、llc_id、capacity_pc、irq_pressure、thermal_pr)` 评分
4. 选分数最高的 pCPU → 映射到本 VM 的某个 vCPU（如优先 `hint_best_vcpu`） → 返回对应 **vCPU** 给调度器

> 关键点：**Guest 永远只"挑 vCPU"**，但"评分/偏好"来自 **pCPU rows**。
> 选择的 vCPU 最终在 Host 硬边界之内运行（Host 通过 vCPU 线程亲和把 vCPU 限定到这张表的可用 pCPU 集）。

### 8.2 代码示例

#### Guest 快速筛选可用 pCPU

```c
/* Guest 侧（调度路径） */
int cps_select_cpu_fast(struct task_struct *p)
{
    struct cps_hdr *hdr = this_vm->pv_table;
    unsigned long *bm_avail, *bm_rt, *bm_lowload, *bm_migrating;
    unsigned long candidates[BITS_TO_LONGS(NR_CPUS)];
    u32 e1, e2;
    int cpu;

    do {
        e1 = READ_ONCE(hdr->epoch);
        if (e1 & 1) continue;
        smp_rmb();

        /* 读取位图（偏移计算） */
        bm_avail = (unsigned long *)((char *)hdr + hdr->bm_offset);
        bm_rt = bm_avail + BITS_TO_LONGS(hdr->rows);
        bm_lowload = bm_rt + BITS_TO_LONGS(hdr->rows);
        bm_migrating = bm_lowload + BITS_TO_LONGS(hdr->rows);

        /* 1. 构造候选集：可用 & 非RT & 低负载 & 非迁移中 */
        bitmap_and(candidates, bm_avail, bm_lowload, hdr->rows);
        bitmap_andnot(candidates, candidates, bm_rt, hdr->rows);
        bitmap_andnot(candidates, candidates, bm_migrating, hdr->rows);

        /* 2. 如果候选集为空，放宽条件 */
        if (bitmap_empty(candidates, hdr->rows)) {
            bitmap_and(candidates, bm_avail, cpu_online_mask, hdr->rows);
            bitmap_andnot(candidates, candidates, bm_rt, hdr->rows);
            bitmap_andnot(candidates, candidates, bm_migrating, hdr->rows);
        }

        smp_rmb();
        e2 = READ_ONCE(hdr->epoch);
    } while (e1 != e2 || (e2 & 1));

    /* 3. 在候选集中选择最优 */
    if (bitmap_empty(candidates, hdr->rows))
        return -1;  // 无可用 CPU

    /* 进一步根据行信息（NUMA/LLC/load）精细选择 */
    return select_best_from_bitmap(candidates, hdr, p);
}
```

## 9. Host 端实现

### 9.1 初始化（系统启动时）

```c
void cps_global_init(void)
{
    int cpu;

    spin_lock_init(&cps_global.lock);

    for_each_possible_cpu(cpu) {
        if (cpu < 2) {
            /* isolcpus=0-1，Host 保留 */
            cps_global.pcpu_state[cpu].owner_type = CPS_OWNER_HOST;
            cps_global.pcpu_state[cpu].state = CPS_PCPU_ALLOCATED;
        } else {
            /* 其他核心加入 Linux 共享池 */
            cps_global.pcpu_state[cpu].owner_type = CPS_OWNER_LINUX_POOL;
            cps_global.pcpu_state[cpu].state = CPS_PCPU_FREE;
            set_bit(cpu, cps_global.bm_linux_pool);
        }
    }
}
```

### 9.2 VM 创建时初始化 pv_sched_table

- QEMU `cps-ram` 分配 per-VM memfd（缺省 64KiB）
- Host CPS Backend：
  - 填充 `hdr.rows = 可见 pCPU 数`
  - 将"该 VM 的可见 pCPU 子集"映射为行（可按 pCPU id 升序）
  - 置位 `bm_avail`；其他位图按策略初始化
  - `epoch += 2` 结束写入

### 9.3 周期性更新

```c
/* Host 侧（周期性更新，1-5ms） */
void cps_update_vm_table(struct vm *linux_vm)
{
    struct cps_hdr *hdr = linux_vm->pv_table;
    unsigned long *bm_avail, *bm_lowload;
    int cpu;

    hdr->epoch++;  // odd
    smp_wmb();

    bm_avail = (unsigned long *)((char *)hdr + hdr->bm_offset);
    bm_lowload = bm_avail + 2 * BITS_TO_LONGS(hdr->rows);

    /* 更新行信息 */
    for_each_cpu(cpu, linux_vm->avail_cpus) {
        struct cps_pcpu_row *row = &hdr->rows[cpu];
        row->load = calculate_load(cpu);
        row->irq_pressure = get_irq_pressure(cpu);
        row->thermal_pr = get_thermal_pressure(cpu);

        /* 同步更新位图 */
        if (row->load <= CPS_LOAD_MED)
            set_bit(cpu, bm_lowload);
        else
            clear_bit(cpu, bm_lowload);
    }

    smp_wmb();
    hdr->epoch++;  // even
}
```

## 10. 性能分析

### 10.1 位图性能

| pCPU 数量 | 单个位图大小 | 8 个位图总大小 | 性能收益            |
|-----------|-------------|---------------|---------------------|
| 32        | 8 bytes     | 64 bytes      | 1-2 个 cache line   |
| 64        | 8 bytes     | 64 bytes      | 1-2 个 cache line   |
| 128       | 16 bytes    | 128 bytes     | 2-3 个 cache line   |
| 256       | 32 bytes    | 256 bytes     | 4-5 个 cache line   |

**优势**：
- 位图操作极快（位运算，CPU 直接支持）
- 避免遍历所有行（从 O(N) 降到 O(1) 筛选 + O(M) 精选，M << N）
- Cache 友好（位图集中存放，连续访问）

### 10.2 更新频率

- **周期性更新**：1-5ms（负载/温度/中断压力）
- **触发式更新**：RT VM 创建/销毁时
- **Epoch 切换开销**：< 1us（仅递增计数器 + 内存屏障）

## 11. 扩展性

### 11.1 扩展位图（可选）

对于复杂拓扑和高级调度策略，可添加：

```c
/* 扩展位图 */
- bm_same_llc_as[vcpu]:  与 vCPU 同 LLC 的 pCPU 集合
- bm_same_numa_as[vcpu]: 与 vCPU 同 NUMA 的 pCPU 集合
- bm_low_thermal:        温度正常的 pCPU（thermal_pr < 50）
- bm_low_irq:            中断压力低的 pCPU（irq_pressure < 30）
- bm_high_capacity:      高频/高性能的 pCPU（capacity_pc > 80）
```

### 11.2 扩展区（Extension）

在 `ext_offset` 指向的区域可以存放：
- NUMA 距离矩阵
- LLC 拓扑信息
- 带宽/能耗统计
- 历史负载数据

## 12. 总结

本设计通过以下机制实现了**实时性保证**与**资源弹性**的平衡：

1. **分离式架构**：RT VM 和 Linux VM 采用不同的映射策略
2. **共享内存表**：`pv_sched_table` 实现 Host-Guest 零开销通信
3. **优雅迁移**：`CPSF_MIGRATING` 标志协同完成任务迁移
4. **全局协调**：`cps_global_state` 管理多 VM 间的 pCPU 分配
5. **位图加速**：O(1) 时间复杂度筛选可用 pCPU

该架构适用于卫星地面站等**混合关键系统**，在保证实时任务确定性的同时，最大化资源利用率。
