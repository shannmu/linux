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
- [x] 共享内存的初始化，主要在于qos_pressure的初始化，新创建的虚拟机的qos_pressure为0
      会导致虚拟机无法分配到配额，我有以下两种方案
  - 初始化时给一个较低的qos值
  - CPU配额分配时，首先给每个vCPU一个最低配额，然后扣除掉所有的最低配额后，再使用QoS进行归一化分配剩余的配额
- [x] 现在的pvsched_host实现完成，现在我开始进行测试，所以需要一些调试信息
  - /proc/pvsched_host/ 接口，可以遍历当前模块中的hashtable，从而查看各种数据
  - 使用内核提供printk_ratelimted等接口，在init/exit, open/release/ioctl等生命周期管理处，补充DEBUG/WARNING等日志
- [x] 现在使用了太多pr_debug, 请使用pr_info,这样调试信息能看到，否则还需要我进行一些编译选项的配置
