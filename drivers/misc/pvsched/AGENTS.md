## BackGround
pvsched_host项目用于通过共享内存与guest进行QoS通信，根据guest内反馈的QoS信息，进行vCPU粒度的CPU配额调度
具体设计
- 在内核模块init时，创建hrtimer，并以一定时间间隔根据各个vCPU的QoS压力，进行CPU配额的计算并写入共享内存
- 其他项目中存在一个scx调度器，通过读取配额，只针对vCPU进行CPU调度
- NOTE:你无需考虑宿主机线程影响的影响

# TODO List
- [x] 现在的pvsched_host实现有问题
  - 现在每个QEMU启动都会初始化对应虚拟机的共享内存和通过ioctl(PVSCHED_INIT)创建hrtimer和线程
    可是，hrtimer和线程不应该是QEMU粒度的，而是一个宿主机对应的一个，所以，需要你修复这个问题
  - 修复路径
    - 首先是hrtimer和配额分配线程的生命周期需要和宿主机该内核模块绑定，QEMU打开/dev/pvsched_host，只负责添加共享内存到管理数据结构和初始化该共享内存
    - 配额计算的算法，需要修改
      - 每次hrtimer触发配额调度时，需要遍历所有的虚拟机（被管理的）的vCPU的QoS，进行所有vCPU的QoS归一化
        - 每个vCPU被分配到CPU配额为 pCPU_{NUM} * interval * QoS归一化权重
