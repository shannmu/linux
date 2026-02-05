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

#include "pvsched.h"

#define PVSCHED_START_ADDR 0x10000000000UL
#define PVSCHED_MAX_VCPU 16

/* ioctl 命令定义（同步写入 pvsched.h 供用户态使用）
 *
 * PVSCHED_INIT: arg = struct pvsched_init_args *，启动定时器和 worker 线程
 * PVSCHED_EXIT: arg 忽略，停止定时器和 worker 线程
 */
#define PVSCHED_MAGIC 'P'
#define PVSCHED_INIT _IOW(PVSCHED_MAGIC, 0, struct pvsched_init_args)
#define PVSCHED_EXIT _IO(PVSCHED_MAGIC, 1)

/* entry 的运行状态，由 ioctl 驱动流转 */
enum pvsched_state {
	PVSCHED_STATE_IDLE, /* open 后尚未 INIT */
	PVSCHED_STATE_ACTIVE, /* INIT 成功，timer/worker 运行中 */
	PVSCHED_STATE_STOPPED, /* EXIT 后，等待 release 清理 */
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

	/* worker 线程与定时器，仅在 ACTIVE 状态下运行 */
	struct task_struct *worker_thread;
	struct hrtimer timer;
	ktime_t interval;

	/*
	 * vcpu_states: 随 entry kzalloc 一并清零，大小固定为
	 * PVSCHED_MAX_VCPU，仅 worker 线程读写，无需额外同步。
	 */
	struct pvsched_vcpu_state vcpu_states[PVSCHED_MAX_VCPU];

	struct hlist_node node;
	struct rcu_head rcu;
};

/* 全局哈希表：RCU 保护读端，mutex 保护写端（insert/delete） */
static DEFINE_HASHTABLE(pvsched_htable, 8);
static DEFINE_MUTEX(pvsched_lock);

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
 * 核心配额计算逻辑（仅 worker 线程调用）
 * ---------------------------------------------------------------------- */

static void pvsched_recalc_logic(struct pvsched_entry *entry)
{
	struct pvsched_shared_mem *shm = entry->kvirt_ptr;
	struct pvsched_vcpu_state *states = entry->vcpu_states;
	u32 vcpu_num = entry->vcpu_num;
	u64 total_pressure = 0;

	/* 1. 采集阶段 */
	for (u32 i = 0; i < vcpu_num; i++) {
		struct pvsched_info *info = &shm->info[i];
		u64 cur_seq = atomic64_read(&info->update_seq);
		u64 cur_pressure = atomic64_read(&info->qos_pressure);

		if (cur_seq != states[i].last_seq) {
			/* Guest 推送了新数据，直接采用 */
			states[i].cached_pressure = cur_pressure;
			states[i].last_seq = cur_seq;
		} else {
			/*
			 * Guest 没有更新 seq，执行缓慢衰减（每 tick 衰减 1/16），
			 * 防止已崩溃或静默的 Guest 持续占用高配额。
			 */
			states[i].cached_pressure =
				(states[i].cached_pressure * 15) >> 4;
		}

		total_pressure += states[i].cached_pressure;
	}

	/* 2. 计算与分发阶段 */
	if (total_pressure == 0)
		total_pressure = 1; /* 避免除零 */

	u64 total_budget = 100000000ULL; /* 总预算 100 ms（单位 ns） */

	for (u32 i = 0; i < vcpu_num; i++) {
		u64 budget = (states[i].cached_pressure * total_budget) /
			     total_pressure;
		atomic64_set(&shm->info[i].tokens, budget);
	}
}

/* -------------------------------------------------------------------------
 * 定时器回调：中断上下文，仅唤醒 worker 线程
 * ---------------------------------------------------------------------- */

static enum hrtimer_restart pvsched_timer_callback(struct hrtimer *timer)
{
	struct pvsched_entry *entry =
		container_of(timer, struct pvsched_entry, timer);

	wake_up_process(entry->worker_thread);
	hrtimer_forward_now(timer, entry->interval);
	return HRTIMER_RESTART;
}

/* -------------------------------------------------------------------------
 * worker 线程：周期性执行配额计算
 * ---------------------------------------------------------------------- */

static int pvsched_worker_fn(void *data)
{
	struct pvsched_entry *entry = data;

	pr_info("pvsched_host: worker started, tgid=%d vcpu_num=%u\n",
		entry->tgid, entry->vcpu_num);

	while (!kthread_should_stop()) {
		set_current_state(TASK_INTERRUPTIBLE);
		schedule();

		if (kthread_should_stop())
			break;

		pvsched_recalc_logic(entry);
	}

	__set_current_state(TASK_RUNNING);
	pr_info("pvsched_host: worker exiting, tgid=%d\n", entry->tgid);
	return 0;
}

/* -------------------------------------------------------------------------
 * ioctl 辅助：启停 timer 和 worker（spinlock 外调用，操作可能阻塞）
 * ---------------------------------------------------------------------- */

static int pvsched_do_init(struct pvsched_entry *entry, u32 vcpu_num,
			   u64 interval_ns)
{
	if (vcpu_num == 0 || vcpu_num > PVSCHED_MAX_VCPU)
		return -EINVAL;

	if (interval_ns == 0)
		return -EINVAL;

	entry->vcpu_num = vcpu_num;
	entry->interval = ns_to_ktime(interval_ns);

	entry->worker_thread = kthread_run(pvsched_worker_fn, entry,
					   "pvsched/%d", entry->tgid);
	if (IS_ERR(entry->worker_thread)) {
		int ret = PTR_ERR(entry->worker_thread);
		entry->worker_thread = NULL;
		return ret;
	}

	hrtimer_start(&entry->timer, entry->interval, HRTIMER_MODE_REL);
	return 0;
}

static void pvsched_do_stop(struct pvsched_entry *entry)
{
	/* hrtimer_cancel 和 kthread_stop 对已停止的对象均安全（幂等） */
	hrtimer_cancel(&entry->timer);

	if (entry->worker_thread) {
		kthread_stop(entry->worker_thread);
		entry->worker_thread = NULL;
	}
}

/* -------------------------------------------------------------------------
 * 驱动生命周期管理
 * ---------------------------------------------------------------------- */

static int pvsched_open(struct inode *inode, struct file *file)
{
	pid_t curr_tgid = task_tgid_vnr(current);
	struct pvsched_entry *entry;
	int ret;

	entry = kzalloc(sizeof(*entry), GFP_KERNEL);
	if (!entry)
		return -ENOMEM;

	/* Pin 单页：整个共享内存结构固定在一个 4KB 页内 */
	ret = get_user_pages_fast(PVSCHED_START_ADDR, 1, FOLL_WRITE,
				  &entry->page);
	if (ret != 1) {
		ret = -EFAULT;
		goto err_free_entry;
	}

	entry->kvirt_ptr = page_address(entry->page);
	entry->tgid = curr_tgid;
	entry->state = PVSCHED_STATE_IDLE;
	spin_lock_init(&entry->lock);
	/* vcpu_states 随 kzalloc 清零；vcpu_num 由 PVSCHED_INIT 写入 */

	hrtimer_init(&entry->timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	entry->timer.function = pvsched_timer_callback;
	/* interval 由 PVSCHED_INIT 的 interval_ns 参数决定，此处不预设 */

	/* 全局发布：检查重复后加入哈希表 */
	mutex_lock(&pvsched_lock);

	if (pvsched_find_locked(curr_tgid)) {
		ret = -EBUSY;
		mutex_unlock(&pvsched_lock);
		goto err_put_page;
	}

	hash_add_rcu(pvsched_htable, &entry->node, entry->tgid);
	mutex_unlock(&pvsched_lock);

	return 0;

err_put_page:
	put_page(entry->page);
err_free_entry:
	kfree(entry);
	return ret;
}

static int pvsched_release(struct inode *inode, struct file *file)
{
	pid_t curr_tgid = task_tgid_vnr(current);
	struct pvsched_entry *entry = NULL;
	struct pvsched_entry *pos;
	struct hlist_node *tmp;

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

	if (!entry)
		return 0;

	/*
	 * 第二步：锁外停止 timer 和 worker。
	 * 若 PVSCHED_EXIT 已先行执行，pvsched_do_stop 的幂等性保证安全。
	 */
	pvsched_do_stop(entry);

	/*
	 * 第三步：等待所有 RCU 读端（bpf_pvsched_get_ptr）退出临界区，
	 * 之后方可安全释放 entry 内存。
	 */
	synchronize_rcu();

	/* 第四步：释放资源 */
	put_page(entry->page);
	kfree(entry);
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

	if (!entry)
		return -ENOENT;

	switch (cmd) {
	case PVSCHED_INIT: {
		struct pvsched_init_args uargs;

		if (copy_from_user(&uargs, (void __user *)arg, sizeof(uargs)))
			return -EFAULT;

		spin_lock_irqsave(&entry->lock, flags);
		if (entry->state != PVSCHED_STATE_IDLE) {
			spin_unlock_irqrestore(&entry->lock, flags);
			return -EBUSY;
		}
		/*
		 * 先将状态置为 ACTIVE，再在 spinlock 外执行可能阻塞的
		 * kthread_run / hrtimer_start。若 pvsched_do_init 失败，
		 * 回滚状态至 IDLE。
		 */
		entry->state = PVSCHED_STATE_ACTIVE;
		spin_unlock_irqrestore(&entry->lock, flags);

		ret = pvsched_do_init(entry, uargs.vcpu_num, uargs.interval_ns);
		if (ret) {
			spin_lock_irqsave(&entry->lock, flags);
			entry->state = PVSCHED_STATE_IDLE;
			spin_unlock_irqrestore(&entry->lock, flags);
		}
		break;
	}

	case PVSCHED_EXIT:
		spin_lock_irqsave(&entry->lock, flags);
		if (entry->state != PVSCHED_STATE_ACTIVE) {
			spin_unlock_irqrestore(&entry->lock, flags);
			return -EINVAL;
		}
		entry->state = PVSCHED_STATE_STOPPED;
		spin_unlock_irqrestore(&entry->lock, flags);

		/* spinlock 外执行可能阻塞的停止操作 */
		pvsched_do_stop(entry);
		break;

	default:
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

static int __init pvsched_init(void)
{
	int ret;

	ret = register_btf_kfunc_id_set(BPF_PROG_TYPE_STRUCT_OPS,
					&pvsched_kfunc_set);
	if (ret) {
		pr_err("pvsched_host: register kfunc set failed: %d\n", ret);
		return ret;
	}

	ret = misc_register(&pvsched_misc);
	if (ret)
		pr_err("pvsched_host: misc_register failed: %d\n", ret);

	return ret;
}

static void __exit pvsched_exit(void)
{
	misc_deregister(&pvsched_misc);
	/*
	 * 生产环境中此处应遍历哈希表，对所有残留 entry 执行与
	 * pvsched_release 相同的四步清理，确保模块卸载时不泄漏资源。
	 */
}

module_init(pvsched_init);
module_exit(pvsched_exit);

MODULE_LICENSE("GPL");
