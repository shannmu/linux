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
};

/* 全局哈希表：RCU 保护读端，mutex 保护写端（insert/delete） */
static DEFINE_HASHTABLE(pvsched_htable, 8);
static DEFINE_MUTEX(pvsched_lock);
static struct pvsched_host_runtime pvsched_rt;

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

	// FIXME: use ioslate cpus instead of online cpus
	total_budget = (u64)num_online_cpus() * ktime_to_ns(pvsched_rt.interval);
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
	pr_info("pvsched_host: global worker started\n");

	while (!kthread_should_stop()) {
		set_current_state(TASK_INTERRUPTIBLE);
		schedule();

		if (kthread_should_stop())
			break;

		pvsched_recalc_all_vms();
	}

	__set_current_state(TASK_RUNNING);
	pr_info("pvsched_host: global worker exiting\n");
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
		return ret;
	}

	pvsched_rt.interval = ns_to_ktime(PVSCHED_DEFAULT_INTERVAL_NS);
	hrtimer_init(&pvsched_rt.timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	pvsched_rt.timer.function = pvsched_timer_callback;
	hrtimer_start(&pvsched_rt.timer, pvsched_rt.interval, HRTIMER_MODE_REL);
	return 0;
}

static void pvsched_runtime_stop(void)
{
	hrtimer_cancel(&pvsched_rt.timer);

	if (pvsched_rt.worker_thread) {
		kthread_stop(pvsched_rt.worker_thread);
		pvsched_rt.worker_thread = NULL;
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

	// /* Pin 单页：整个共享内存结构固定在一个 4KB 页内 */
	// ret = get_user_pages_fast(PVSCHED_START_ADDR, 1, FOLL_WRITE,
	// 			  &entry->page);
	// if (ret != 1) {
	// 	ret = -EFAULT;
	// 	goto err_free_entry;
	// }
	//
	// entry->kvirt_ptr = page_address(entry->page);
	entry->tgid = curr_tgid;
	entry->state = PVSCHED_STATE_IDLE;
	spin_lock_init(&entry->lock);
	INIT_LIST_HEAD(&entry->free_node);
	/* vcpu_states 随 kzalloc 清零；vcpu_num 由 PVSCHED_INIT 写入 */

	// memset(entry->kvirt_ptr, 0, PAGE_SIZE);
	// entry->kvirt_ptr->tgid = curr_tgid;

	/* 全局发布：检查重复后加入哈希表 */
	mutex_lock(&pvsched_lock);
	if (pvsched_find_locked(curr_tgid)) {
		ret = -EBUSY;
		mutex_unlock(&pvsched_lock);
		goto err_free;
	}

	hash_add_rcu(pvsched_htable, &entry->node, entry->tgid);
	mutex_unlock(&pvsched_lock);

	file->private_data = entry;

	return 0;

err_free:
	kfree(entry);

// err_put_page:
// 	put_page(entry->page);
// err_free_entry:
// 	kfree(entry);
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
		struct pvsched_shared_mem *shm;
		struct page *page = NULL;
		void *kvirt;
		int pin_ret;
		u32 i;


		if (copy_from_user(&uargs, (void __user *)arg, sizeof(uargs)))
			return -EFAULT;
		if (!uargs.vcpu_num || uargs.vcpu_num > PVSCHED_MAX_VCPU)
			return -EINVAL;
		if (!uargs.interval_ns)
			return -EINVAL;
		if (!uargs.shm_hva || !PAGE_ALIGNED(uargs.shm_hva))
			return -EINVAL;

		/* pin QEMU 的 HVA 对应的页 */
		pin_ret = get_user_pages_fast(uargs.shm_hva, 1, FOLL_WRITE, &page);
		if (pin_ret != 1)
			return -EFAULT;

		kvirt = vmap(&page, 1, VM_MAP, PAGE_KERNEL);
		if (!kvirt) {
			put_page(page);
			return -ENOMEM;
		}
		spin_lock_irqsave(&entry->lock, flags);
		if (entry->state != PVSCHED_STATE_IDLE) {
			spin_unlock_irqrestore(&entry->lock, flags);
			vunmap(kvirt);
			put_page(page);
			return -EBUSY;
		}
		entry->page = page;
		entry->kvirt_ptr = kvirt;
		entry->vcpu_num = uargs.vcpu_num;
		entry->state = PVSCHED_STATE_ACTIVE;
		spin_unlock_irqrestore(&entry->lock, flags);

		shm = entry->kvirt_ptr;
		memset(shm, 0, PAGE_SIZE);
		// FIXME: atomic opeation
		shm->tgid     = entry->tgid;
		shm->vcpu_num = uargs.vcpu_num;
		for (i = 0; i < uargs.vcpu_num; i++)
			atomic64_set(&shm->info[i].tokens, 0);
		break;
	}

	case PVSCHED_EXIT:
	{
		struct page *old_page;
				struct pvsched_shared_mem *shm;
		u32 vcpu_num, i;

		spin_lock_irqsave(&entry->lock, flags);
		if (entry->state != PVSCHED_STATE_ACTIVE) {
			spin_unlock_irqrestore(&entry->lock, flags);
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
		break;
	}

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
	else {
		ret = pvsched_runtime_start();
		if (ret) {
			pr_err("pvsched_host: runtime start failed: %d\n", ret);
			misc_deregister(&pvsched_misc);
		}
	}

	return ret;
}

static void __exit pvsched_exit(void)
{
	struct pvsched_entry *entry, *n;
	struct hlist_node *tmp;
	LIST_HEAD(free_list);
	int bkt;

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
}

module_init(pvsched_init);
module_exit(pvsched_exit);

MODULE_LICENSE("GPL");
