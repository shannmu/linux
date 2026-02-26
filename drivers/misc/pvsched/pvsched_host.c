#define pr_fmt(fmt) "pvsched_host: " fmt

#include "linux/bpf.h"
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/hashtable.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/sched.h>
#include <linux/kthread.h>
#include <linux/hrtimer.h>
#include <linux/uaccess.h>
#include <linux/btf.h>
#include <linux/btf_ids.h>
#include <linux/cpu.h>
#include <linux/vmalloc.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/sched/isolation.h>

#include "pvsched.h"

#define PVSCHED_START_ADDR 0x10000100000UL
#define PVSCHED_DEFAULT_INTERVAL_NS 100000000ULL
#define PVSCHED_MIN_BUDGET_PCT 1

/* ioctl 命令定义（同步写入 pvsched.h 供用户态使用）
 *
 * PVSCHED_INIT: arg = struct pvsched_init_args *，将该 VM 纳入全局配额计算
 * PVSCHED_EXIT: arg 忽略，将该 VM 移出全局配额计算
 */
#define PVSCHED_MAGIC 'P'
#define PVSCHED_INIT _IOW(PVSCHED_MAGIC, 0, struct pvsched_init_args)
#define PVSCHED_EXIT _IO(PVSCHED_MAGIC, 1)

/* entry 的运行状态，由 ioctl 驱动流转 */
enum pvsched_state {
	PVSCHED_STATE_IDLE, /* open 后尚未 INIT */
	PVSCHED_STATE_ACTIVE, /* INIT 成功，纳入全局配额计算 */
};

/* Host 私有的单 vCPU 追踪状态，仅 worker 线程读写 */
struct pvsched_vcpu_state {
	u64 last_seq;
	u64 cached_pressure;
};

struct pvsched_entry {
	pid_t tgid;

	/* 共享内存：单虚拟机对应单页，固定在 PVSCHED_START_ADDR */
	struct pvsched_shared_mem *kvirt_ptr;
	struct page *page;

	/* 运行状态，由 ioctl 写、release 读；entry->lock 保护 */
	enum pvsched_state state;
	spinlock_t lock;

	/* ioctl INIT 时传入，worker 线程用于限定循环范围 */
	u32 vcpu_num;

	/*
	 * vcpu_states: 随 entry kzalloc 一并清零，大小固定为
	 * PVSCHED_MAX_VCPU，仅 worker 线程读写，无需额外同步。
	 */
	struct pvsched_vcpu_state vcpu_states[PVSCHED_MAX_VCPU];

	struct hlist_node node;
	struct list_head free_node;
	struct rcu_head rcu;
};

struct pvsched_host_runtime {
	struct task_struct *worker_thread;
	struct hrtimer timer;
	ktime_t interval;
	/*
	 * 隔离 CPU 数量：在模块加载时由 isolcpus= 内核参数静态决定，
	 * 缓存于此，避免在 worker 每次触发时重复遍历所有在线 CPU。
	 */
	u32 isolated_cpus;
};

/* 全局哈希表：RCU 保护读端，mutex 保护写端（insert/delete） */
static DEFINE_HASHTABLE(pvsched_htable, 8);
static DEFINE_MUTEX(pvsched_lock);
static struct pvsched_host_runtime pvsched_rt;

/* /proc/pvsched_host 目录 */
static struct proc_dir_entry *pvsched_proc_dir;

/*
 * 使能开关：仅当内核命令行包含 pvsched_host=on 时才激活所有功能。
 * 未传入该参数时，模块加载/设备注册等均跳过，做到零开销。
 */
static bool pvsched_enabled __read_mostly;

static int __init pvsched_setup(char *str)
{
	if (str && strcmp(str, "on") == 0)
		pvsched_enabled = true;
	return 1;
}
early_param("pvsched_host", pvsched_setup);

/* -------------------------------------------------------------------------
 * 内部辅助：按 tgid 查找 entry
 * ---------------------------------------------------------------------- */

/* RCU 读端持有时使用 */
static struct pvsched_entry *pvsched_find_rcu(pid_t tgid)
{
	struct pvsched_entry *entry;

	hash_for_each_possible_rcu(pvsched_htable, entry, node, tgid) {
		if (entry->tgid == tgid)
			return entry;
	}
	return NULL;
}

/* mutex 持有时使用 */
static struct pvsched_entry *pvsched_find_locked(pid_t tgid)
{
	struct pvsched_entry *entry;

	hash_for_each_possible(pvsched_htable, entry, node, tgid) {
		if (entry->tgid == tgid)
			return entry;
	}
	return NULL;
}

/* -------------------------------------------------------------------------
 * BPF kfunc 接口：调度路径调用，RCU 读端
 * ---------------------------------------------------------------------- */

__bpf_kfunc_start_defs();
__bpf_kfunc struct pvsched_shared_mem *bpf_pvsched_get_ptr(pid_t tgid)
{
	struct pvsched_entry *entry;

	entry = pvsched_find_rcu(tgid);
	return entry ? entry->kvirt_ptr : NULL;
}
__bpf_kfunc_end_defs();

BTF_KFUNCS_START(pvsched_kfunc_ids)
BTF_ID_FLAGS(func, bpf_pvsched_get_ptr)
BTF_KFUNCS_END(pvsched_kfunc_ids)

static const struct btf_kfunc_id_set pvsched_kfunc_set = {
	.owner = THIS_MODULE,
	.set = &pvsched_kfunc_ids,
};

/* -------------------------------------------------------------------------
 * 核心配额计算逻辑（全局 worker 线程调用）
 * ---------------------------------------------------------------------- */

static u64 pvsched_collect_total_pressure(void)
{
	struct pvsched_entry *entry;
	unsigned int bkt;
	u64 total_pressure = 0;

	hash_for_each_rcu(pvsched_htable, bkt, entry, node) {
		struct pvsched_shared_mem *shm;
		struct pvsched_vcpu_state *states;
		u32 vcpu_num, i;

		if (READ_ONCE(entry->state) != PVSCHED_STATE_ACTIVE)
			continue;

		vcpu_num = READ_ONCE(entry->vcpu_num);
		if (!vcpu_num || vcpu_num > PVSCHED_MAX_VCPU)
			continue;

		shm = entry->kvirt_ptr;
		states = entry->vcpu_states;

		for (i = 0; i < vcpu_num; i++) {
			struct pvsched_info *info = &shm->info[i];
			u64 cur_seq = atomic64_read(&info->update_seq);
			u64 cur_pressure = atomic64_read(&info->qos_pressure);

			if (cur_seq != states[i].last_seq) {
				states[i].cached_pressure = cur_pressure;
				states[i].last_seq = cur_seq;
			} else {
				states[i].cached_pressure =
					(states[i].cached_pressure * 15) >> 4;
			}

			total_pressure += states[i].cached_pressure;
		}
	}

	return total_pressure;
}

static void pvsched_distribute_budget(u64 total_pressure)
{
	struct pvsched_entry *entry;
	unsigned int bkt;
	u64 total_budget;
	u64 min_budget;
	u64 min_total_budget;
	u64 remaining_budget;
	u32 total_vcpus = 0;

	/* isolated_cpus 在模块加载时已缓存，直接读取 */
	total_budget = (u64)pvsched_rt.isolated_cpus * ktime_to_ns(pvsched_rt.interval);
	min_budget = ktime_to_ns(pvsched_rt.interval) * PVSCHED_MIN_BUDGET_PCT / 100;
	if (!min_budget)
		min_budget = 1;

	hash_for_each_rcu(pvsched_htable, bkt, entry, node) {
		u32 vcpu_num;

		if (READ_ONCE(entry->state) != PVSCHED_STATE_ACTIVE)
			continue;

		vcpu_num = READ_ONCE(entry->vcpu_num);
		if (!vcpu_num || vcpu_num > PVSCHED_MAX_VCPU)
			continue;

		total_vcpus += vcpu_num;
	}

	if (!total_vcpus)
		return;

	min_total_budget = min_budget * total_vcpus;
	if (min_total_budget > total_budget) {
		min_budget = total_budget / total_vcpus;
		min_total_budget = min_budget * total_vcpus;
	}

	remaining_budget = total_budget - min_total_budget;

	hash_for_each_rcu(pvsched_htable, bkt, entry, node) {
		struct pvsched_shared_mem *shm;
		struct pvsched_vcpu_state *states;
		u32 vcpu_num, i;

		if (READ_ONCE(entry->state) != PVSCHED_STATE_ACTIVE)
			continue;

		vcpu_num = READ_ONCE(entry->vcpu_num);
		if (!vcpu_num || vcpu_num > PVSCHED_MAX_VCPU)
			continue;

		shm = entry->kvirt_ptr;
		states = entry->vcpu_states;

		for (i = 0; i < vcpu_num; i++) {
			u64 budget = min_budget;

			if (remaining_budget) {
				if (total_pressure)
					budget += (states[i].cached_pressure *
						   remaining_budget) /
						  total_pressure;
				else
					budget += remaining_budget / total_vcpus;
			}
			atomic64_set(&shm->info[i].tokens, budget);
		}
	}
}

static void pvsched_recalc_all_vms(void)
{
	u64 total_pressure;

	rcu_read_lock();
	total_pressure = pvsched_collect_total_pressure();
	pvsched_distribute_budget(total_pressure);
	rcu_read_unlock();
}

/* -------------------------------------------------------------------------
 * /proc/pvsched_host/status — 全局运行时信息
 * ---------------------------------------------------------------------- */

static int pvsched_proc_status_show(struct seq_file *m, void *v)
{
	seq_printf(m, "isolated_cpus: %u\n", pvsched_rt.isolated_cpus);
	seq_printf(m, "interval_ns:  %llu\n", ktime_to_ns(pvsched_rt.interval));
	seq_printf(m, "worker_alive: %d\n",
		   pvsched_rt.worker_thread != NULL ? 1 : 0);
	return 0;
}

static int pvsched_proc_status_open(struct inode *inode, struct file *file)
{
	return single_open(file, pvsched_proc_status_show, NULL);
}

static const struct proc_ops pvsched_proc_status_ops = {
	.proc_open    = pvsched_proc_status_open,
	.proc_read    = seq_read,
	.proc_lseek   = seq_lseek,
	.proc_release = single_release,
};

/* -------------------------------------------------------------------------
 * /proc/pvsched_host/vms — 每个 VM 的摘要（每行一个 VM）
 * ---------------------------------------------------------------------- */

static int pvsched_proc_vms_show(struct seq_file *m, void *v)
{
	struct pvsched_entry *entry;
	unsigned int bkt;

	seq_printf(m, "%-10s %-8s %s\n", "tgid", "state", "vcpu_num");
	seq_puts(m, "-----------------------------\n");

	rcu_read_lock();
	hash_for_each_rcu(pvsched_htable, bkt, entry, node) {
		enum pvsched_state state = READ_ONCE(entry->state);

		seq_printf(m, "%-10d %-8s %u\n",
			   entry->tgid,
			   state == PVSCHED_STATE_ACTIVE ? "ACTIVE" : "IDLE",
			   READ_ONCE(entry->vcpu_num));
	}
	rcu_read_unlock();

	return 0;
}

static int pvsched_proc_vms_open(struct inode *inode, struct file *file)
{
	return single_open(file, pvsched_proc_vms_show, NULL);
}

static const struct proc_ops pvsched_proc_vms_ops = {
	.proc_open    = pvsched_proc_vms_open,
	.proc_read    = seq_read,
	.proc_lseek   = seq_lseek,
	.proc_release = single_release,
};

/* -------------------------------------------------------------------------
 * /proc/pvsched_host/vcpus — 每个 vCPU 的调度数据（每行一个 vCPU）
 * ---------------------------------------------------------------------- */

static int pvsched_proc_vcpus_show(struct seq_file *m, void *v)
{
	struct pvsched_entry *entry;
	unsigned int bkt;

	seq_printf(m, "%-10s %-8s %-16s %-16s %-16s %-16s\n",
		   "tgid", "vcpu_id", "qos_pressure", "cached_pressure",
		   "update_seq", "tokens");
	seq_puts(m, "----------------------------------------------------------------------"
		    "----------\n");

	rcu_read_lock();
	hash_for_each_rcu(pvsched_htable, bkt, entry, node) {
		struct pvsched_shared_mem *shm;
		u32 vcpu_num, i;

		if (READ_ONCE(entry->state) != PVSCHED_STATE_ACTIVE)
			continue;

		vcpu_num = READ_ONCE(entry->vcpu_num);
		if (!vcpu_num || vcpu_num > PVSCHED_MAX_VCPU)
			continue;

		shm = entry->kvirt_ptr;

		for (i = 0; i < vcpu_num; i++) {
			seq_printf(m,
				   "%-10d %-8u %-16lld %-16llu %-16lld %-16lld\n",
				   entry->tgid,
				   i,
				   atomic64_read(&shm->info[i].qos_pressure),
				   entry->vcpu_states[i].cached_pressure,
				   atomic64_read(&shm->info[i].update_seq),
				   atomic64_read(&shm->info[i].tokens));
		}
	}
	rcu_read_unlock();

	return 0;
}

static int pvsched_proc_vcpus_open(struct inode *inode, struct file *file)
{
	return single_open(file, pvsched_proc_vcpus_show, NULL);
}

static const struct proc_ops pvsched_proc_vcpus_ops = {
	.proc_open    = pvsched_proc_vcpus_open,
	.proc_read    = seq_read,
	.proc_lseek   = seq_lseek,
	.proc_release = single_release,
};

/* -------------------------------------------------------------------------
 * 定时器回调：中断上下文，仅唤醒 worker 线程
 * ---------------------------------------------------------------------- */

static enum hrtimer_restart pvsched_timer_callback(struct hrtimer *timer)
{
	wake_up_process(pvsched_rt.worker_thread);
	hrtimer_forward_now(timer, pvsched_rt.interval);
	return HRTIMER_RESTART;
}

/* -------------------------------------------------------------------------
 * worker 线程：周期性执行配额计算
 * ---------------------------------------------------------------------- */

static int pvsched_worker_fn(void *data)
{
	pr_info("global worker started\n");

	while (!kthread_should_stop()) {
		set_current_state(TASK_INTERRUPTIBLE);
		schedule();

		if (kthread_should_stop())
			break;

		pvsched_recalc_all_vms();
	}

	__set_current_state(TASK_RUNNING);
	pr_info("global worker exiting\n");
	return 0;
}

/* -------------------------------------------------------------------------
 * 运行时启停：模块生命周期绑定的单 worker/single hrtimer
 * ---------------------------------------------------------------------- */

static int pvsched_runtime_start(void)
{
	pvsched_rt.worker_thread = kthread_run(pvsched_worker_fn, NULL,
					       "pvsched/host");
	if (IS_ERR(pvsched_rt.worker_thread)) {
		int ret = PTR_ERR(pvsched_rt.worker_thread);

		pvsched_rt.worker_thread = NULL;
		pr_err("failed to create worker thread: %d\n", ret);
		return ret;
	}

	pvsched_rt.interval = ns_to_ktime(PVSCHED_DEFAULT_INTERVAL_NS);

	/*
	 * 缓存隔离 CPU 数量：isolcpus= 由内核启动参数静态决定，
	 * 模块加载时计算一次即可，无需在 worker 每次触发时重复遍历。
	 * 若未配置任何隔离 CPU，则回退到在线 CPU 总数。
	 */
	{
		u32 count = 0;
		int cpu;

		for_each_online_cpu(cpu) {
			if (!housekeeping_test_cpu(cpu, HK_TYPE_DOMAIN))
				count++;
		}
		pvsched_rt.isolated_cpus = count ? count : num_online_cpus();
	}

	hrtimer_init(&pvsched_rt.timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	pvsched_rt.timer.function = pvsched_timer_callback;
	hrtimer_start(&pvsched_rt.timer, pvsched_rt.interval, HRTIMER_MODE_REL);

	pr_info("runtime started, interval_ns=%llu, isolated_cpus=%u\n",
		PVSCHED_DEFAULT_INTERVAL_NS, pvsched_rt.isolated_cpus);
	return 0;
}

static void pvsched_runtime_stop(void)
{
	hrtimer_cancel(&pvsched_rt.timer);

	if (pvsched_rt.worker_thread) {
		kthread_stop(pvsched_rt.worker_thread);
		pvsched_rt.worker_thread = NULL;
	}

	pr_info("runtime stopped\n");
}

/* -------------------------------------------------------------------------
 * 驱动生命周期管理
 * ---------------------------------------------------------------------- */

static int pvsched_open(struct inode *inode, struct file *file)
{
	pid_t curr_tgid = task_tgid_vnr(current);
	struct pvsched_entry *entry;
	int ret;

	pr_info("open: tgid=%d\n", curr_tgid);

	entry = kzalloc(sizeof(*entry), GFP_KERNEL);
	if (!entry) {
		pr_warn_ratelimited("open: tgid=%d alloc entry failed\n",
				    curr_tgid);
		return -ENOMEM;
	}

	entry->tgid = curr_tgid;
	entry->state = PVSCHED_STATE_IDLE;
	spin_lock_init(&entry->lock);
	INIT_LIST_HEAD(&entry->free_node);
	/* vcpu_states 随 kzalloc 清零；vcpu_num 由 PVSCHED_INIT 写入 */

	/* 全局发布：检查重复后加入哈希表 */
	mutex_lock(&pvsched_lock);
	if (pvsched_find_locked(curr_tgid)) {
		pr_warn_ratelimited("open: tgid=%d already registered\n",
				    curr_tgid);
		ret = -EBUSY;
		mutex_unlock(&pvsched_lock);
		goto err_free;
	}

	hash_add_rcu(pvsched_htable, &entry->node, entry->tgid);
	mutex_unlock(&pvsched_lock);

	file->private_data = entry;

	pr_info("open: tgid=%d registered\n", curr_tgid);
	return 0;

err_free:
	kfree(entry);
	return ret;
}

static int pvsched_release(struct inode *inode, struct file *file)
{
	pid_t curr_tgid = task_tgid_vnr(current);
	struct pvsched_entry *entry = NULL;
	struct pvsched_entry *pos;
	struct hlist_node *tmp;

	pr_info("release: tgid=%d\n", curr_tgid);

	/* 第一步：锁内摘表，不执行任何可能阻塞的操作 */
	mutex_lock(&pvsched_lock);
	hash_for_each_possible_safe(pvsched_htable, pos, tmp, node, curr_tgid) {
		if (pos->tgid == curr_tgid) {
			hash_del_rcu(&pos->node);
			entry = pos;
			break;
		}
	}
	mutex_unlock(&pvsched_lock);

	if (!entry) {
		pr_warn_ratelimited("release: tgid=%d not found in table\n",
				    curr_tgid);
		return 0;
	}

	/*
	 * 第二步：等待所有 RCU 读端（bpf_pvsched_get_ptr / worker）退出临界区，
	 * 之后方可安全释放 entry 内存。
	 */
	synchronize_rcu();

	/* 第三步：释放资源 */
	if (entry->kvirt_ptr)
		vunmap(entry->kvirt_ptr);
	if (entry->page)       /* INIT 后未 EXIT 直接关闭 */
		put_page(entry->page);
	kfree(entry);

	pr_info("release: tgid=%d unregistered\n", curr_tgid);
	return 0;
}

static long pvsched_ioctl(struct file *file, unsigned int cmd,
			  unsigned long arg)
{
	pid_t curr_tgid = task_tgid_vnr(current);
	struct pvsched_entry *entry;
	unsigned long flags;
	int ret = 0;

	rcu_read_lock();
	entry = pvsched_find_rcu(curr_tgid);
	rcu_read_unlock();

	if (!entry) {
		pr_warn_ratelimited("ioctl: tgid=%d not found, cmd=%u\n",
				    curr_tgid, cmd);
		return -ENOENT;
	}

	switch (cmd) {
	case PVSCHED_INIT: {
		struct pvsched_init_args uargs;
		struct pvsched_shared_mem *shm;
		struct page *page = NULL;
		void *kvirt;
		int pin_ret;
		u32 i;

		if (copy_from_user(&uargs, (void __user *)arg, sizeof(uargs)))
			return -EFAULT;
		if (!uargs.vcpu_num || uargs.vcpu_num > PVSCHED_MAX_VCPU) {
			pr_warn_ratelimited("ioctl INIT: tgid=%d invalid vcpu_num=%u\n",
					    curr_tgid, uargs.vcpu_num);
			return -EINVAL;
		}
		if (!uargs.interval_ns) {
			pr_warn_ratelimited("ioctl INIT: tgid=%d zero interval_ns\n",
					    curr_tgid);
			return -EINVAL;
		}
		if (!uargs.shm_hva || !PAGE_ALIGNED(uargs.shm_hva)) {
			pr_warn_ratelimited("ioctl INIT: tgid=%d invalid shm_hva=0x%llx\n",
					    curr_tgid, uargs.shm_hva);
			return -EINVAL;
		}

		pr_info("ioctl INIT: tgid=%d vcpu_num=%u interval_ns=%llu shm_hva=0x%llx\n",
			 curr_tgid, uargs.vcpu_num, uargs.interval_ns,
			 uargs.shm_hva);

		/* pin QEMU 的 HVA 对应的页 */
		pin_ret = get_user_pages_fast(uargs.shm_hva, 1, FOLL_WRITE, &page);
		if (pin_ret != 1) {
			pr_warn_ratelimited("ioctl INIT: tgid=%d pin shm page failed\n",
					    curr_tgid);
			return -EFAULT;
		}

		kvirt = vmap(&page, 1, VM_MAP, PAGE_KERNEL);
		if (!kvirt) {
			put_page(page);
			pr_warn_ratelimited("ioctl INIT: tgid=%d vmap failed\n",
					    curr_tgid);
			return -ENOMEM;
		}

		spin_lock_irqsave(&entry->lock, flags);
		if (entry->state != PVSCHED_STATE_IDLE) {
			spin_unlock_irqrestore(&entry->lock, flags);
			vunmap(kvirt);
			put_page(page);
			pr_warn_ratelimited("ioctl INIT: tgid=%d not in IDLE state\n",
					    curr_tgid);
			return -EBUSY;
		}
		entry->page = page;
		entry->kvirt_ptr = kvirt;
		entry->vcpu_num = uargs.vcpu_num;
		entry->state = PVSCHED_STATE_ACTIVE;
		spin_unlock_irqrestore(&entry->lock, flags);

		shm = entry->kvirt_ptr;
		memset(shm, 0, PAGE_SIZE);
		/*
		 * tgid 和 vcpu_num 无需完整原子操作，理由如下：
		 * 1. 这两个字段在整个生命周期内仅被写入一次（此处）；
		 * 2. host worker 通过 entry->vcpu_num 获取 vCPU 数量，
		 *    不直接读取 shm->vcpu_num，故不存在与 worker 的竞争；
		 * 3. guest 在 QEMU 的 ioctl 系统调用返回后才能感知共享内存，
		 *    系统调用返回自带完整内存序（full barrier），保证 guest
		 *    看到的数据已经完整写入；
		 * 使用 WRITE_ONCE() 防止编译器将 32 位写操作拆分为多次
		 * 小写（byte/halfword），同时向读者明确这是有意的单次写入。
		 */
		WRITE_ONCE(shm->tgid, entry->tgid);
		WRITE_ONCE(shm->vcpu_num, uargs.vcpu_num);
		for (i = 0; i < uargs.vcpu_num; i++)
			atomic64_set(&shm->info[i].tokens, 0);

		pr_info("ioctl INIT: tgid=%d activated\n", curr_tgid);
		break;
	}

	case PVSCHED_EXIT: {
		struct page *old_page;
		struct pvsched_shared_mem *shm;
		u32 vcpu_num, i;

		pr_info("ioctl EXIT: tgid=%d\n", curr_tgid);

		spin_lock_irqsave(&entry->lock, flags);
		if (entry->state != PVSCHED_STATE_ACTIVE) {
			spin_unlock_irqrestore(&entry->lock, flags);
			pr_warn_ratelimited("ioctl EXIT: tgid=%d not in ACTIVE state\n",
					    curr_tgid);
			return -EINVAL;
		}
		vcpu_num = entry->vcpu_num;
		old_page = entry->page;
		shm = entry->kvirt_ptr;
		entry->vcpu_num = 0;
		entry->state = PVSCHED_STATE_IDLE;
		entry->page = NULL;
		entry->kvirt_ptr = NULL;
		spin_unlock_irqrestore(&entry->lock, flags);

		/* 等待 worker 退出当前 RCU 临界区，确保不再访问 shm */
		synchronize_rcu();

		for (i = 0; i < vcpu_num; i++)
			atomic64_set(&shm->info[i].tokens, 0);

		vunmap(shm);
		put_page(old_page);

		pr_info("ioctl EXIT: tgid=%d deactivated\n", curr_tgid);
		break;
	}

	default:
		pr_warn_ratelimited("ioctl: tgid=%d unknown cmd=%u\n",
				    curr_tgid, cmd);
		ret = -ENOTTY;
	}

	return ret;
}

static const struct file_operations pvsched_fops = {
	.owner = THIS_MODULE,
	.open = pvsched_open,
	.release = pvsched_release,
	.unlocked_ioctl = pvsched_ioctl,
};

static struct miscdevice pvsched_misc = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "pvsched_host",
	.fops = &pvsched_fops,
};

static void pvsched_proc_init(void)
{
	pvsched_proc_dir = proc_mkdir("pvsched_host", NULL);
	if (!pvsched_proc_dir) {
		pr_warn("failed to create /proc/pvsched_host\n");
		return;
	}

	if (!proc_create("status", 0444, pvsched_proc_dir,
			 &pvsched_proc_status_ops))
		pr_warn("failed to create /proc/pvsched_host/status\n");

	if (!proc_create("vms", 0444, pvsched_proc_dir,
			 &pvsched_proc_vms_ops))
		pr_warn("failed to create /proc/pvsched_host/vms\n");

	if (!proc_create("vcpus", 0444, pvsched_proc_dir,
			 &pvsched_proc_vcpus_ops))
		pr_warn("failed to create /proc/pvsched_host/vcpus\n");
}

static int __init pvsched_init(void)
{
	int ret;

	if (!pvsched_enabled) {
		pr_info("disabled (pass 'pvsched_host=on' on the kernel cmdline to enable)\n");
		return 0;
	}

	pr_info("module loading, interval_ns=%llu, online_cpus=%u\n",
		PVSCHED_DEFAULT_INTERVAL_NS, num_online_cpus());

	ret = register_btf_kfunc_id_set(BPF_PROG_TYPE_STRUCT_OPS,
					&pvsched_kfunc_set);
	if (ret) {
		pr_err("register kfunc set failed: %d\n", ret);
		return ret;
	}

	ret = misc_register(&pvsched_misc);
	if (ret) {
		pr_err("misc_register failed: %d\n", ret);
		return ret;
	}

	ret = pvsched_runtime_start();
	if (ret) {
		pr_err("runtime start failed: %d\n", ret);
		misc_deregister(&pvsched_misc);
		return ret;
	}

	pvsched_proc_init();

	pr_info("module loaded\n");
	return 0;
}

static void __exit pvsched_exit(void)
{
	struct pvsched_entry *entry, *n;
	struct hlist_node *tmp;
	LIST_HEAD(free_list);
	int bkt;

	if (!pvsched_enabled)
		return;

	pr_info("module unloading\n");

	if (pvsched_proc_dir)
		remove_proc_subtree("pvsched_host", NULL);

	pvsched_runtime_stop();
	misc_deregister(&pvsched_misc);

	mutex_lock(&pvsched_lock);
	hash_for_each_safe(pvsched_htable, bkt, tmp, entry, node) {
		hash_del_rcu(&entry->node);
		list_add(&entry->free_node, &free_list);
	}
	mutex_unlock(&pvsched_lock);

	synchronize_rcu();

	list_for_each_entry_safe(entry, n, &free_list, free_node) {
		if (entry->kvirt_ptr)
			vunmap(entry->kvirt_ptr);
		if (entry->page)
			put_page(entry->page);
		kfree(entry);
	}

	pr_info("module unloaded\n");
}

module_init(pvsched_init);
module_exit(pvsched_exit);

MODULE_LICENSE("GPL");
