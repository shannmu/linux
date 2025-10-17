# CPS-based Mixed System Scheduling Architecture Design  
# 基于 CPS 的混合系统调度架构设计

**版本 Version:** v1.0  
**作者 Author:** [Your Name]  
**项目 Project:** 卫星地面站一体化虚拟化系统  
**日期 Date:** 2025-10  

---

## 1. 背景 Background

### 中文说明

在“**卫星地面站一体化系统**”中，地面控制中心通常同时运行两类关键系统：

1. **实时系统（RTOS）**：执行轨控指令、姿态控制、数据采集、时间同步等硬实时任务；
2. **业务系统（Linux 或通用 OS）**：处理遥测数据、进行图像分析、通信中继、数据库与云接入。

为了提升系统安全性与隔离性，工程上通常采用 **虚拟化（Virtualization）** 的方式，将实时域与业务域分区运行。  
然而，现有方案多为 **静态分区（Static Partitioning）**：  
- 在系统启动时固定分配物理核（pCPU）给 RTOS 与 Linux；  
- RTOS 的核不能被调度到其他任务，Linux 不能使用这些核；  
- 导致 **资源利用率低、业务弹性不足**。

在卫星地面站的场景下，任务负载具有明显的“时变性”：
- 卫星测控窗口开启时，实时任务负载上升；
- 非测控阶段，实时负载下降，而业务处理需求上升。  

因此需要一种能够：
> **在保证实时调度确定性的前提下，实现资源弹性分配的混合虚拟化系统。**

本研究提出的 **CPS-based Mixed System Scheduling Architecture**  
（基于协同半虚拟化调度的混合系统调度架构）  
正是为解决此问题而设计。

---

### English Version

In the **Integrated Satellite Ground Station System**, two domains coexist:

1. **Real-Time Domain (RTOS):**  
   Executes satellite attitude control, command dispatch, and telemetry data acquisition tasks with strict timing guarantees.
2. **Service Domain (Linux):**  
   Handles data processing, image analysis, database management, and ground network communication.

Virtualization provides a natural isolation boundary between these domains.  
However, most existing **real-time virtualization solutions** adopt **static partitioning**—  
each RTOS and Linux VM is permanently bound to a fixed set of physical CPUs (pCPUs).  

This design ensures determinism but sacrifices flexibility and utilization.  
During off-peak satellite communication phases, RTOS CPUs remain idle while Linux workloads may lack resources.

Hence, our goal is to design a **mixed virtualization architecture** that ensures:

> **Real-time determinism for RTOS, and dynamic resource elasticity for service systems.**

The proposed **CPS-based Mixed System Scheduling Architecture** achieves this through **zero-exit cooperative scheduling** between host and guest.

---

## 2. 总体目标 Overview

| 目标目标 | 英文描述 |
|-----------|-----------|
| **实时保证** | Maintain deterministic scheduling for RTOS without preemption or interference. |
| **资源弹性** | Allow Linux (business domain) VMs to use freed CPUs when RTOS load is low. |
| **零 VM-exit 协同** | Implement cross-layer cooperation via shared memory (Refer-Table) instead of virtio. |
| **多域隔离** | Ensure strong isolation between RT and Non-RT domains, with controlled resource reallocation. |

---

## 3. 系统架构 Architecture

### 3.1 整体架构图 Overview

```
+--------------------------------------------------------------+
|                   Host Linux Kernel (6.12)                   |
|--------------------------------------------------------------|
|  KVM Scheduler + CPS Backend                                 |
|   - 维护共享 Refer-Table（memfd）                            |
|   - 实现 RT 准入与回收（Elastic Partition）                  |
|   - 调整 vCPU/pCPU 亲和性、IRQ 隔离                          |
|--------------------------------------------------------------|
|  QEMU 8.2 + cps-ram Device                                   |
|   - 分配共享内存 memfd（64KiB）                              |
|   - 映射至 GPA=0x88000000                                    |
|   - 追加 cmdline: cps_refertable=0x88000000,0x10000          |
|   - 注册 /dev/cps 给 Host 后端                               |
+--------------------------------------------------------------+
|                     Guest 虚拟机层                           |
|--------------------------------------------------------------|
|  CPS Frontend (Guest Kernel Module)                          |
|   - early_memremap() 映射 Refer-Table                        |
|   - 提供 epoch 检查与行级读取 API                            |
|--------------------------------------------------------------|
|  Xenomai3 RTOS Guest                                         |
|   - 一次性读取 RT 核掩码                                     |
|   - 固定绑定、无迁移                                         |
|--------------------------------------------------------------|
|  Linux Guest                                                 |
|   - select_task_rq() → cps_select_cpu()                      |
|   - 避让 RT 区域，优先低负载/同 CG 核                        |
+--------------------------------------------------------------+
```

---

## 4. 核心设计 Design Details

### 4.1 共享内存结构（Refer-Table）

```c
struct cps_hdr {
    u32 version;    // 协议版本
    u32 rows;       // vCPU 数
    u32 row_size;   // sizeof(struct cps_row)
    u32 reserved0;
    volatile u32 epoch; // 奇偶序列：偶=稳定，奇=写入中
    u8 pad[60];
};

struct cps_row {
    u16 vcpu_id;
    u16 pcpu_id;
    u16 vcg_id;
    u8  rt_flag;    // 1=RT核, 0=普通核
    u8  pcpu_load;  // 0=低, 1=高
    u32 flags;
    u64 reserved[2];
};
```

---

## 5. 优势分析 Advantages

| 特性 | 说明 |
|------|------|
| **零 VM-exit 协同** | Guest 访问共享内存无需陷入 Host |
| **静态 + 动态混合** | 启动时静态划分，运行期动态调整 |
| **多域融合** | RTOS 与 Linux 并行、互不干扰 |
| **高隔离性** | 保证卫星测控任务的确定性与安全性 |
| **高弹性** | 业务系统可动态扩缩资源 |

---

## 6. 后续开发计划 TODO Features

- Host 端：pCPU 负载采样、准入/回收策略、IRQ 迁移  
- Guest 端：select_task_rq() 集成、负载与缓存组感知策略  
- 工具链：QMP 命令、cpsd 守护进程、cpsadm 管理工具  
- 实验：卫星测控场景仿真、Xenomai 延迟抖动分析、多 VM 并发性能测试

---

## 7. 总结 Summary

该架构针对卫星地面站多域融合需求，提出了**基于协同半虚拟化调度（CPS）的混合系统调度框架**。  
通过零 VM-exit 的共享内存协同机制，实现了实时系统的确定性与业务系统的资源弹性共存。  

---
