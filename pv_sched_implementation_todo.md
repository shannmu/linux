# pv_sched 实现 TODO

## 实现策略

在同一个 Linux kernel 中实现 Host 和 Guest 功能：
- **Host 功能**：作为 KVM/虚拟化 Host，管理全局 pCPU 状态，更新 pv_sched_table
- **Guest 功能**：作为 VM 内部的调度器扩展，读取 pv_sched_table 进行智能调度

## 第一阶段：基础设施（UAPI 和数据结构）

### 1.1 创建 UAPI 头文件
**文件**: `include/uapi/linux/pv_sched.h`

**内容**:
- [ ] 定义 `CPS_UAPI_VERSION`, `CPS_DEFAULT_GPA`, `CPS_DEFAULT_SIZE`
- [ ] 定义 `enum cps_load`（LOW/MED/HIGH）
- [ ] 定义 `enum cps_state_flags`（AVAIL/MIGRATING/PINNED/DO_NOT_USE/LIMITED_IO）
- [ ] 定义 `struct cps_hdr`（header，包含 version/rows/epoch/offsets）
- [ ] 定义 `struct cps_pcpu_row`（每个 pCPU 的信息行）

**依赖**: 无

**预计时间**: 30 分钟

---

### 1.2 创建 Host 端内部数据结构
**文件**: `include/linux/pv_sched_host.h`

**内容**:
- [ ] 定义 `enum cps_pcpu_owner_type`（NONE/HOST/RT_VM/LINUX_POOL）
- [ ] 定义 `enum cps_pcpu_state`（FREE/ALLOCATED/MIGRATING）
- [ ] 定义 `struct cps_pcpu_slot`（全局状态表的每个槽位）
- [ ] 定义 `struct cps_global_state`（全局状态表，包含 spinlock + pcpu_state[] + 位图）
- [ ] 定义 `struct cps_vm_table`（per-VM 的 pv_sched_table 管理结构）

**依赖**: 1.1

**预计时间**: 30 分钟

---

### 1.3 创建 Guest 端内部数据结构
**文件**: `include/linux/pv_sched_guest.h`

**内容**:
- [ ] 定义 `struct cps_guest_info`（Guest 侧的 pv_sched_table 映射信息）
- [ ] 定义辅助宏：`cps_get_bitmap()`, `cps_get_row()` 等
- [ ] 定义 epoch 读取辅助函数原型

**依赖**: 1.1

**预计时间**: 20 分钟

---

## 第二阶段：Host 端核心实现

### 2.1 实现全局状态管理
**文件**: `kernel/sched/pv_sched_host.c`

**功能**:
- [ ] 定义全局变量 `static struct cps_global_state cps_global;`
- [ ] 实现 `cps_global_init(void)` - 系统启动时初始化全局状态表
  - [ ] 初始化 spinlock
  - [ ] 将所有 pCPU 标记为 `LINUX_POOL` 状态（暂时跳过 Host Region 的硬编码）
  - [ ] 初始化位图
- [ ] 实现 `cps_query_pcpu_state(int cpu, struct cps_pcpu_slot *out)` - 查询 pCPU 状态

**依赖**: 1.2

**预计时间**: 1 小时

**测试点**:
- 启动后检查全局状态表是否正确初始化
- 通过 debugfs 暴露状态表供调试

---

### 2.2 实现 per-VM pv_sched_table 管理
**文件**: `kernel/sched/pv_sched_host.c`（续）

**功能**:
- [ ] 实现 `cps_vm_table_alloc(struct kvm *kvm, int nr_cpus)` - 为 VM 分配 pv_sched_table
  - [ ] 分配共享内存（使用 `alloc_pages()` 或 `vmalloc()`）
  - [ ] 初始化 `struct cps_hdr`（version/rows/offsets/epoch）
  - [ ] 初始化位图区
  - [ ] 初始化行数组（填充 pCPU 拓扑信息：NUMA/LLC）
- [ ] 实现 `cps_vm_table_free(struct cps_vm_table *table)` - 释放 pv_sched_table
- [ ] 实现 `cps_vm_table_map_to_guest(struct cps_vm_table *table, u64 gpa)` - 将表映射到 Guest 物理地址空间

**依赖**: 2.1

**预计时间**: 2 小时

**测试点**:
- 创建虚拟表，检查内存布局是否符合设计文档
- 验证 epoch 机制

---

### 2.3 实现 pCPU 分配和释放（RT VM）
**文件**: `kernel/sched/pv_sched_host.c`（续）

**功能**:
- [ ] 实现 `cps_allocate_rt_cpus(u32 vm_id, const cpumask_t *request_cpus)`
  - [ ] 步骤 1: 检查资源可用性（持有 spinlock）
  - [ ] 步骤 2: 标记为 MIGRATING 状态
  - [ ] 步骤 3: 调用 `cps_update_linux_vms_for_migration()`（设置所有 Linux VM 的 MIGRATING 标志）
  - [ ] 步骤 4: 等待迁移完成或超时（`cps_wait_migration_done()`）
  - [ ] 步骤 5: 更新全局状态为 ALLOCATED（持有 spinlock）
  - [ ] 步骤 6: 调用 `cps_update_linux_vms_for_rt_allocation()`（清除 AVAIL，设置 RT 标志）
- [ ] 实现 `cps_release_rt_cpus(u32 vm_id)`
  - [ ] 查找该 VM 的所有 pCPU
  - [ ] 更新全局状态为 LINUX_POOL/FREE
  - [ ] 调用 `cps_update_linux_vms_for_release()`（设置 AVAIL，清除 RT 标志）

**依赖**: 2.2

**预计时间**: 3 小时

**测试点**:
- 模拟 RT VM 创建/销毁，检查全局状态转换是否正确
- 验证位图同步更新

---

### 2.4 实现 pv_sched_table 更新（周期性 + 触发式）
**文件**: `kernel/sched/pv_sched_host.c`（续）

**功能**:
- [ ] 实现 `cps_update_vm_table(struct cps_vm_table *table)` - 周期性更新单个 VM 的表
  - [ ] epoch++ (odd)
  - [ ] 更新行信息：load/irq_pressure/thermal_pr/capacity_pc
  - [ ] 更新位图：bm_lowload
  - [ ] epoch++ (even)
- [ ] 实现 `cps_update_linux_vms_for_migration(cpumask_t *cpus, bool set)` - 设置/清除 MIGRATING 标志
- [ ] 实现 `cps_update_linux_vms_for_rt_allocation(cpumask_t *cpus)` - RT 分配后更新
- [ ] 实现 `cps_update_linux_vms_for_release(cpumask_t *cpus)` - RT 释放后更新
- [ ] 创建定时器或工作队列，周期性（1-5ms）调用 `cps_update_vm_table()` 更新所有 Linux VM

**依赖**: 2.3

**预计时间**: 2 小时

**测试点**:
- 检查周期性更新是否正常工作
- 验证 epoch 机制在并发读写下的一致性

---

### 2.5 实现 KVM 集成（Linux VM）
**文件**: `arch/x86/kvm/x86.c` 或 `virt/kvm/kvm_main.c`

**功能**:
- [ ] 在 `kvm_arch_vcpu_create()` 中为 Linux VM 分配 pv_sched_table（调用 `cps_vm_table_alloc()`）
- [ ] 在 `kvm_arch_vcpu_destroy()` 中释放 pv_sched_table（调用 `cps_vm_table_free()`）
- [ ] 实现新的 hypercall `KVM_HC_PV_SCHED_GET_TABLE_GPA`，允许 Guest 查询 pv_sched_table 的 GPA
  - [ ] 在 `kvm_emulate_hypercall()` 中添加分支处理
  - [ ] 返回该 VM 对应的 pv_sched_table 的 GPA

**依赖**: 2.4

**预计时间**: 1.5 小时

**测试点**:
- 创建 Linux VM，检查 pv_sched_table 是否正确分配和映射
- 从 QEMU 或 Guest 使用 hypercall 验证 GPA 获取成功

---

### 2.6 实现 RT VM 多层隔离机制
**文件**: `include/uapi/linux/kvm.h`, `arch/x86/kvm/x86.c`, `virt/kvm/kvm_main.c`

**功能**:

#### 2.6.1 定义 UAPI 结构和 ioctl
**文件**: `include/uapi/linux/kvm.h`
- [ ] 定义 `struct kvm_rt_vm_config`
  ```c
  struct kvm_rt_vm_config {
      __u32 vm_id;           /* RT VM 的全局 ID */
      __u32 nr_vcpus;        /* vCPU 数量 */
      __u32 pcpu_ids[256];   /* vCPU i 绑定的 pCPU ID */
  };
  ```
- [ ] 定义新的 ioctl `KVM_SET_RT_VM_CONFIG`
  ```c
  #define KVM_SET_RT_VM_CONFIG _IOW(KVMIO, 0xXX, struct kvm_rt_vm_config)
  ```

#### 2.6.2 实现 ioctl 处理函数
**文件**: `virt/kvm/kvm_main.c`
- [ ] 实现 `kvm_vm_ioctl_set_rt_config(struct kvm *kvm, struct kvm_rt_vm_config *cfg)`
  - [ ] 验证配置参数（nr_vcpus, pcpu_ids 有效性）
  - [ ] 调用 `cps_allocate_rt_cpus(cfg->vm_id, pcpu_mask)` 申请 pCPU 资源
  - [ ] 如果分配成功，保存配置到 `kvm->rt_config`
  - [ ] 设置 `kvm->is_rt_vm = true` 标志
- [ ] 在 `kvm_vm_ioctl()` 中添加 `KVM_SET_RT_VM_CONFIG` 分支

#### 2.6.3 实现 RT vCPU 创建时的 5 层隔离
**文件**: `arch/x86/kvm/x86.c`
- [ ] 在 `kvm_arch_vcpu_create()` 中添加 RT VM 特殊处理
  - [ ] **第 3 层：CPU 亲和性绑定**
    - [ ] 调用 `set_cpus_allowed_ptr(current, pcpu_mask)` 将 vCPU 线程绑定到指定 pCPU
  - [ ] **第 4 层：实时调度策略**
    - [ ] 调用 `sched_setscheduler(current, SCHED_FIFO, &param)` 设置 SCHED_FIFO
    - [ ] 设置优先级为 `MAX_RT_PRIO - 1`
  - [ ] **防止迁移标记**
    - [ ] 设置 `vcpu->cpu_pinned = true`
    - [ ] 设置 `vcpu->pinned_pcpu = pcpu_id`
- [ ] 在 `vcpu_load_on_cpu()` 中添加迁移检查
  ```c
  if (vcpu->cpu_pinned && cpu != vcpu->pinned_pcpu) {
      pr_warn("RT vCPU %d migration blocked: %d -> %d\n",
              vcpu->vcpu_id, vcpu->pinned_pcpu, cpu);
      return -EINVAL;
  }
  ```

#### 2.6.4 实现 RT VM 销毁时的资源释放
**文件**: `virt/kvm/kvm_main.c`
- [ ] 在 `kvm_destroy_vm()` 中添加 RT VM 检查
  - [ ] 如果 `kvm->is_rt_vm == true`，调用 `cps_release_rt_cpus(kvm->rt_config.vm_id)`
  - [ ] 释放 pCPU 资源，更新全局状态表和所有 Linux VM 的 pv_sched_table

#### 2.6.5 添加 RT VM 配置字段到 struct kvm
**文件**: `include/linux/kvm_host.h`
- [ ] 在 `struct kvm` 中添加字段
  ```c
  struct kvm {
      // ... 现有字段
      bool is_rt_vm;                      /* 是否为 RT VM */
      struct kvm_rt_vm_config rt_config;  /* RT VM 配置 */
  };
  ```
- [ ] 在 `struct kvm_vcpu` 中添加字段
  ```c
  struct kvm_vcpu {
      // ... 现有字段
      bool cpu_pinned;     /* 是否已绑定到 pCPU */
      int pinned_pcpu;     /* 绑定的 pCPU ID */
  };
  ```

**依赖**: 2.3（依赖 `cps_allocate_rt_cpus` 和 `cps_release_rt_cpus`）

**预计时间**: 4 小时

**测试点**:
- 使用 QEMU 调用 `KVM_SET_RT_VM_CONFIG` 创建 RT VM
- 验证 vCPU 线程已绑定到指定 pCPU（通过 `/proc/<pid>/status` 查看 Cpus_allowed_list）
- 验证 vCPU 线程的调度策略为 SCHED_FIFO（通过 `chrt -p <pid>` 查看）
- 验证全局状态表中对应 pCPU 已标记为 `RT_VM` 且不可用
- 验证所有 Linux VM 的 pv_sched_table 中对应行的 AVAIL 标志已清除
- 测试 RT VM 销毁后 pCPU 资源是否正确释放

---

## 第三阶段：Guest 端核心实现

### 3.1 实现 Guest 端初始化和映射
**文件**: `arch/x86/kernel/pv_sched_guest.c`

**功能**:
- [ ] 实现 `cps_guest_init()` - Guest 启动时初始化
  - [ ] 通过 hypercall 或 CPUID 检测 Host 是否支持 pv_sched
  - [ ] 获取 pv_sched_table 的 GPA（通过 hypercall 或固定地址 `CPS_DEFAULT_GPA`）
  - [ ] 使用 `ioremap()` 或 `memremap()` 映射到 Guest 内核虚拟地址空间
  - [ ] 验证 header（version/rows）
  - [ ] 初始化 `struct cps_guest_info`
- [ ] 实现 `cps_guest_cleanup()` - Guest 关闭时清理

**依赖**: 1.3

**预计时间**: 1.5 小时

**测试点**:
- Guest 启动后检查 pv_sched_table 是否成功映射
- 读取 header 验证数据正确性

---

### 3.2 实现 epoch 安全读取
**文件**: `arch/x86/kernel/pv_sched_guest.c`（续）

**功能**:
- [ ] 实现 `cps_read_epoch_start(struct cps_hdr *hdr, u32 *epoch_out)` - 开始读取前检查 epoch
- [ ] 实现 `cps_read_epoch_end(struct cps_hdr *hdr, u32 epoch_start)` - 读取后验证 epoch
- [ ] 实现 `cps_read_row_safe(struct cps_hdr *hdr, int cpu, struct cps_pcpu_row *out)` - 安全读取单行

**依赖**: 3.1

**预计时间**: 1 小时

**测试点**:
- 在 Host 快速更新时，验证 Guest 读取的一致性
- 测试 epoch 冲突重试机制

---

### 3.3 实现位图快速筛选
**文件**: `arch/x86/kernel/pv_sched_guest.c`（续）

**功能**:
- [ ] 实现 `cps_get_avail_cpus(struct cps_guest_info *info, cpumask_t *out)` - 获取可用 pCPU 位图
- [ ] 实现 `cps_filter_rt_cpus(cpumask_t *mask, struct cps_guest_info *info)` - 过滤掉 RT pCPU
- [ ] 实现 `cps_filter_migrating_cpus(cpumask_t *mask, struct cps_guest_info *info)` - 过滤掉正在迁移的 pCPU
- [ ] 实现 `cps_filter_lowload_cpus(cpumask_t *mask, struct cps_guest_info *info)` - 筛选低负载 pCPU

**依赖**: 3.2

**预计时间**: 1.5 小时

**测试点**:
- 验证位图操作的正确性和性能
- 测试边界情况（全部可用/全部不可用）

---

### 3.4 实现 CPU 选择算法
**文件**: `arch/x86/kernel/pv_sched_guest.c`（续）

**功能**:
- [ ] 实现 `cps_select_cpu(struct task_struct *p, int prev_cpu, cpumask_t *candidates)` - 主选择函数
  - [ ] 步骤 1: 使用位图快速构造候选集（bm_avail & ~bm_rt & bm_lowload & ~bm_migrating）
  - [ ] 步骤 2: 如果候选集为空，放宽条件（移除 lowload 限制）
  - [ ] 步骤 3: 在候选集中评分选择最优 pCPU
    - [ ] 考虑 NUMA 距离（与 prev_cpu 同 NUMA 优先）
    - [ ] 考虑 LLC 共享（与 prev_cpu 同 LLC 优先）
    - [ ] 考虑负载（load 低优先）
    - [ ] 考虑中断/热压（irq_pressure/thermal_pr 低优先）
    - [ ] 考虑 capacity（capacity_pc 高优先）
  - [ ] 步骤 4: 将选中的 pCPU 映射到对应的 vCPU 返回

**依赖**: 3.3

**预计时间**: 3 小时

**测试点**:
- 在不同负载场景下验证选择的合理性
- 性能测试：选择函数的延迟（应 < 10us）

---

### 3.5 集成到 CFS 调度器
**文件**: `kernel/sched/fair.c`

**功能**:
- [ ] 在 `select_task_rq_fair()` 开头添加 pv_sched 检查
  ```c
  if (cps_guest_enabled()) {
      int cpu = cps_select_cpu(p, prev_cpu, cpu_active_mask);
      if (cpu >= 0)
          return cpu;
      // fallback to original logic
  }
  ```
- [ ] 添加 sysctl 开关 `kernel.sched_pv_sched_enabled` 控制是否启用

**依赖**: 3.4

**预计时间**: 1 小时

**测试点**:
- 在 Guest 中运行负载，观察任务是否被调度到合适的 CPU
- 对比启用/禁用 pv_sched 的性能差异

---

### 3.6 实现 MIGRATING 响应机制
**文件**: `kernel/sched/core.c` 或 `kernel/sched/fair.c`

**功能**:
- [ ] 创建 workqueue 或内核线程，定期检查 bm_migrating 位图
- [ ] 实现 `cps_handle_migration()` - 主动迁移任务
  - [ ] 读取 bm_migrating 位图
  - [ ] 对于每个正在迁移的 pCPU，调用 `migrate_tasks()` 或触发负载均衡
- [ ] 在 `select_task_rq_fair()` 中避免选择 MIGRATING 的 CPU

**依赖**: 3.5

**预计时间**: 2 小时

**测试点**:
- 模拟 Host 发起迁移，验证 Guest 是否主动迁移任务
- 测试迁移的延迟（应在 100ms 超时内完成）

---

## 第四阶段：调试和测试设施

### 4.1 Host 端 debugfs 接口
**文件**: `kernel/sched/pv_sched_debug.c`

**功能**:
- [ ] 创建 `/sys/kernel/debug/pv_sched/global_state` - 显示全局状态表
- [ ] 创建 `/sys/kernel/debug/pv_sched/vm_tables/` - 显示每个 VM 的 pv_sched_table
- [ ] 创建 `/sys/kernel/debug/pv_sched/allocate_rt` - 手动触发 RT 分配（用于测试）
- [ ] 创建 `/sys/kernel/debug/pv_sched/release_rt` - 手动触发 RT 释放（用于测试）

**依赖**: 2.5

**预计时间**: 1.5 小时

---

### 4.2 Guest 端 debugfs 接口
**文件**: `arch/x86/kernel/pv_sched_guest.c`（续）

**功能**:
- [ ] 创建 `/sys/kernel/debug/pv_sched_guest/table_dump` - dump 当前 pv_sched_table
- [ ] 创建 `/sys/kernel/debug/pv_sched_guest/stats` - 统计信息（选择次数/命中率/失败次数）
- [ ] 创建 `/sys/kernel/debug/pv_sched_guest/epoch_conflicts` - epoch 冲突统计

**依赖**: 3.6

**预计时间**: 1 小时

---

### 4.3 性能测试工具
**文件**: `tools/testing/selftests/pv_sched/`

**功能**:
- [ ] 编写 Host 侧测试脚本：模拟 RT VM 创建/销毁，验证状态转换
- [ ] 编写 Guest 侧测试脚本：测试 CPU 选择的合理性和性能
- [ ] 编写压力测试：并发更新 + 并发读取，验证 epoch 机制
- [ ] 编写延迟测试：测量 `cps_select_cpu()` 的平均/最大延迟

**依赖**: 4.2

**预计时间**: 3 小时

---

## 第五阶段：优化和完善

### 5.1 性能优化
- [ ] 使用 per-CPU 缓存减少 pv_sched_table 访问
- [ ] 优化位图操作（使用 SIMD 指令）
- [ ] 优化 epoch 机制（减少内存屏障开销）
- [ ] 考虑使用 RCU 替代部分 spinlock

**预计时间**: 4 小时

---

### 5.2 错误处理和容错
- [ ] 添加对 pv_sched_table 损坏的检测和恢复
- [ ] 添加对 epoch 长时间冲突的检测和 fallback
- [ ] 添加对 Host 停止更新的检测（heartbeat 机制）
- [ ] 完善所有错误路径的清理逻辑

**预计时间**: 2 小时

---

### 5.3 文档和注释
- [ ] 为所有公共函数添加详细注释（参数/返回值/副作用）
- [ ] 添加 `Documentation/virt/pv_sched.rst` 使用文档
- [ ] 更新 `MAINTAINERS` 文件

**预计时间**: 2 小时

---

## 总计预估时间

- **第一阶段**: 1.5 小时
- **第二阶段**: 12.5 小时（增加了 RT VM 多层隔离机制 2.6）
- **第三阶段**: 11 小时
- **第四阶段**: 5.5 小时
- **第五阶段**: 8 小时

**总计**: ~38.5 小时（约 4.8 个工作日）

---

## 实现顺序建议

建议按以下顺序实现，以便逐步验证功能：

1. **第一阶段全部** → 建立基础设施
2. **2.1 + 2.2** → Host 端基础功能，可以创建和管理表
3. **4.1** → 添加 debugfs，方便调试 Host 端
4. **3.1 + 3.2** → Guest 端基础功能，可以读取表
5. **4.2** → 添加 debugfs，方便调试 Guest 端
6. **2.3 + 2.4** → Host 端完整功能（分配/释放/更新）
7. **2.5** → KVM 集成（Linux VM）
8. **3.3 + 3.4 + 3.5** → Guest 端完整功能（选择算法）
9. **3.6** → 迁移响应
10. **2.6** → RT VM 多层隔离机制（在 Linux VM 功能验证后再实现）
11. **4.3** → 测试
12. **第五阶段** → 优化和完善

---

## RT VM 多层隔离的额外配置（非内核实现部分）

以下是 RT VM 5 层隔离机制中需要在**内核外部**完成的配置，这些不在本 TODO 的实现范围内，但需要在系统部署时配置：

### 第 1 层：内核启动参数（Host 端）
**配置位置**: Host 的 bootloader 配置（如 GRUB）

**必需参数**:
```bash
isolcpus=4-7           # 隔离 CPU 4-7 用于 RT VM
nohz_full=4-7          # 关闭 tick（减少中断）
rcu_nocbs=4-7          # RCU 回调转移到其他 CPU
irqaffinity=0-3        # 硬中断只在 CPU 0-3 处理
```

**说明**: 这些参数需要在 Host 启动时配置，确保指定的 pCPU 在 Host 侧就被隔离。

### 第 2 层：cgroup cpuset 隔离（Host 端）
**配置位置**: Host 的初始化脚本或 systemd service

**配置命令**:
```bash
# 创建 RT CPU 专用 cgroup
mkdir -p /sys/fs/cgroup/cpuset/rt_region
echo 4-7 > /sys/fs/cgroup/cpuset/rt_region/cpuset.cpus
echo 0 > /sys/fs/cgroup/cpuset/rt_region/cpuset.mems
echo 1 > /sys/fs/cgroup/cpuset/rt_region/cpuset.cpu_exclusive

# 限制 Host 其他进程只能使用 CPU 0-3
echo 0-3 > /sys/fs/cgroup/cpuset/cpuset.cpus
```

**说明**: 通过 cgroup cpuset 确保 Host 的其他进程不会被调度到 RT 专用的 CPU 上。

### 第 5 层：全局协调（由本 TODO 2.6 实现）
**实现位置**: 2.6.2 中的 `cps_allocate_rt_cpus()`

**说明**: 这一层由内核实现，通过 pv_sched_table 通知所有 Linux VM 避开 RT 专用的 pCPU。

---

## 关键问题和讨论点

### Q1: pv_sched_table 的内存分配方式
**选项 A**: 使用 `vmalloc()` 分配内核虚拟内存，通过 KVM 的内存映射机制暴露给 Guest
**选项 B**: 使用 `alloc_pages()` 分配物理页，直接映射到 Guest EPT

**建议**: 选项 B，性能更好且更符合虚拟化设计
**已确定**: ✓ 选项 B

### Q2: Guest 如何发现 pv_sched_table 的 GPA
**选项 A**: 固定地址 `CPS_DEFAULT_GPA = 0x88000000`
**选项 B**: 通过 hypercall 查询
**选项 C**: 通过 CPUID leaf 返回

**建议**: 选项 B，更灵活且安全
**已确定**: ✓ 选项 B（使用 hypercall `KVM_HC_PV_SCHED_GET_TABLE_GPA`）

### Q3: 周期性更新的实现方式
**选项 A**: 使用内核定时器（hrtimer）
**选项 B**: 使用工作队列（workqueue）
**选项 C**: 在调度器 tick 中更新

**建议**: 选项 A，可以精确控制更新间隔（1-5ms）
**已确定**: ✓ 选项 A（使用 hrtimer）

### Q4: 是否需要实现 sched_ext 集成
**当前**: 直接修改 `select_task_rq_fair()`
**可选**: 实现为 sched_ext BPF 调度器

**建议**: 第一版先直接修改 CFS，后续可以添加 sched_ext 支持

### Q5: RT VM 的 vCPU pinning 如何触发
**选项 A**: 通过新的 KVM ioctl（如 `KVM_SET_RT_VM_CONFIG`）
**选项 B**: 通过 QEMU 命令行参数自动触发
**选项 C**: 通过 Guest 内部的 hypercall 请求

**建议**: 选项 A，由 VMM（QEMU）控制更合理
**已确定**: ✓ 选项 A（已在 2.6 中定义 `KVM_SET_RT_VM_CONFIG` ioctl）

---

## 需要讨论的问题

以下是仍需要确定的设计细节：

1. **是否需要支持 pv_sched_table 的热更新（如增加 pCPU）？**
   - 当前设计：在 VM 创建时固定 pv_sched_table 大小
   - 可选扩展：支持动态添加/删除 pCPU 行

2. **Guest 侧是否需要通知 Host 迁移完成？**
   - 选项 A：Guest 完成迁移后通过 hypercall 通知 Host
   - 选项 B：仅依赖 Host 的超时机制（100ms）
   - 建议：选项 B 更简单，选项 A 可以减少延迟

3. **是否需要实现 RT VM 和 Linux VM 的优先级控制？**
   - 当前设计：RT VM 绝对优先（通过隔离实现）
   - 可选扩展：Linux VM 之间的优先级控制

4. **位图区的大小是否需要动态计算？**
   - 选项 A：固定支持最大 NR_CPUS（如 8192）
   - 选项 B：根据实际 pCPU 数量动态计算
   - 建议：选项 B，节省内存

5. **是否需要实现统计和性能监控（如 perf events）？**
   - 基础版本：仅通过 debugfs 提供统计信息（已在 4.1/4.2 中计划）
   - 扩展版本：集成到 perf subsystem，支持 `perf stat` 查看 pv_sched 指标

6. **RT VM 的 Layer 1/2 配置是否需要自动化？**
   - 选项 A：手动配置 bootloader 和 cgroup（当前方案）
   - 选项 B：提供脚本自动配置
   - 选项 C：集成到 QEMU 或 libvirt，自动设置隔离参数

---

## 更新历史

- **2025-10-18**: 添加 2.6 节 "RT VM 多层隔离机制"，详细说明 5 层隔离的内核实现部分（第 3/4/5 层），并补充 Layer 1/2 的外部配置说明。更新预计时间至 38.5 小时。
