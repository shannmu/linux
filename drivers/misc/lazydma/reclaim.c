#include "linux/lazydma.h"
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/miscdevice.h>
#include <linux/fs.h>
#include <linux/kthread.h>
#include <linux/mm.h>
#include <linux/swap.h>
#include <linux/pagewalk.h>
#include <linux/sched/mm.h>
#include <linux/mm_inline.h> /* for folio_isolate_lru if inline */
#include <linux/ioctl.h>
#include <linux/types.h>

#define MY_IOC_MAGIC 'q'

// 传递的数据结构
struct lazydma_shm_config {
	__u64 addr; // 用户态虚拟地址 (HVA / vaddr)
	__u64 size; // 共享内存大小 (bytes)
	__u64 padding; // 保留字段，用于字节对齐或未来扩展
};

// 定义 IOCTL 命令：_IOW 表示用户态向内核写数据
#define LAZYDMA_SET_SHM_CONFIG _IOW(MY_IOC_MAGIC, 1, struct lazydma_shm_config)

/* * 注意：reclaim_pages 在 mm/vmscan.c 中定义。
 * 如果它在你的内核源码中是 static 的，你需要去 mm/vmscan.c 去掉 static，
 * 或者在 mm/internal.h 中找到它的声明。
 * 这里我们手动声明一下。
 */
extern unsigned long reclaim_pages(struct list_head *folio_list);

/* 你的自定义接口 */
extern bool pmd_check_io(pmd_t *pmd);

#define DEVICE_NAME "lazydma"

struct lazydma_ctx {
	atomic_t used; /* 0: 空闲, 1: 占用 */
	int id; /* 调试用的 ID */

	struct task_struct *thread;
	struct mm_struct *target_mm;
	struct dma_tracking_entry *entrys;
	wait_queue_head_t wq;
	bool stop_req;
};

static struct lazydma_ctx vm_ctx_pool[MAX_VM_NUMS];

/*
 * 页表遍历回调 (PMD Level)
 */
static int lazydma_pmd_entry(pmd_t *pmd, unsigned long addr, unsigned long next,
			     struct mm_walk *walk)
{
	struct list_head *folio_list = walk->private;
	struct page *page;
	struct folio *folio;

	/* 1. 基础有效性检查 */
	if (!pmd_present(*pmd) || pmd_none(*pmd))
		return 0;

	/* 2. 调用你的业务逻辑：判断是否被 DMA 占用 */
	/* * 注意：此时一定要确保 pmd_check_io 内部不要睡眠，
	* 或者如果需要睡眠，确保锁的顺序是安全的。
	* 此时我们持有 mmap_read_lock。
	*/
	if (!pmd_check_io(pmd)) {
		return 0; /* 正在使用，不可回收 */
	}

	/* 3. 获取 struct page */
	page = pmd_page(*pmd);
	if (!page)
		return 0;

	/* 转换为 folio (Kernel 6.12 标准) */
	folio = page_folio(page);

	/* * 4. 尝试获取 Folio 引用
	* 如果 folio refcount 为 0，说明已经在释放中，跳过
	*/
	if (!folio_try_get(folio))
		return 0;

	/* * 5. 核心步骤：隔离 LRU
	* 只有在 LRU 链表上的页面才能被 Swap。
	* folio_isolate_lru 会将页面移出全局 LRU，变为私有状态。
	*/
	if (folio_isolate_lru(folio)) {
		/* 隔离成功，加入本地列表，准备交给 reclaim_pages */
		list_add_tail(&folio->lru, folio_list);
	} else {
		/* 隔离失败 (可能被锁住、不在 LRU 上等)，释放引用 */
		folio_put(folio);
	}

	return 0;
}

static const struct mm_walk_ops lazydma_walk_ops = {
	.pmd_entry = lazydma_pmd_entry,
	/* 不需要 pte_entry，因为你只关注 pmd 级别的大页或内存块 */
};

/*
 * 扫描与回收主逻辑
 */
static void lazydma_scan_mm(struct mm_struct *mm)
{
	struct list_head folio_list;
	unsigned long nr_reclaimed;

	INIT_LIST_HEAD(&folio_list);

	/* * 1. 遍历页表
	* 必须持有 mmap_read_lock 以防止 VMA 树在遍历时改变
	*/
	mmap_read_lock(mm);

	/* * 建议优化：如果你知道 Guest 内存的具体 VA 范围，
	* 将 0, TASK_SIZE 替换为 start, end 效率会高非常多
	*/
	walk_page_range(mm, 0, TASK_SIZE, &lazydma_walk_ops, &folio_list);

	mmap_read_unlock(mm);

	/* * 2. 执行回收
	* reclaim_pages 是这一步的魔法。它会：
	* - 遍历列表中的 folio
	* - 尝试 add_to_swap (分配 ZRAM slot)
	* - 触发 pageout (写出数据)
	* - 释放页面 (如果写出成功)
	* - 处理失败的页面 (put back to LRU)
	*/
	if (!list_empty(&folio_list)) {
		nr_reclaimed = reclaim_pages(&folio_list);
		/* * reclaim_pages 会自动处理 list 中 folio 的 put/free。
		* 我们不需要在这里手动做 list_del 或 folio_put。
		*/
		if (nr_reclaimed > 0)
			pr_debug("lazydma: Reclaimed %lu folios\n",
				 nr_reclaimed);
	}
}

/*
 * Kthread 线程函数
 */
static int lazydma_worker(void *data)
{
	struct lazydma_ctx *ctx = data;

	pr_info("lazydma: Worker started for mm %p\n", ctx->target_mm);

	while (!kthread_should_stop() && !ctx->stop_req) {
		/* 执行业务 */
		lazydma_scan_mm(ctx->target_mm);

		/* * 休眠逻辑
		* 这里设置为 1 秒扫描一次，可根据需求调整
		*/
		wait_event_interruptible_timeout(
			ctx->wq, kthread_should_stop() || ctx->stop_req, HZ);
	}

	/* 线程结束，释放 mm 引用 */
	mmput(ctx->target_mm);
	return 0;
}

/*
 * Char Device Open
 */
static int lazydma_open(struct inode *inode, struct file *file)
{
	struct lazydma_ctx *ctx = NULL;
	int i;

	/* 1. 遍历数组寻找空闲槽位 */
	for (i = 0; i < MAX_VM_NUMS; i++) {
		/* CAS 操作：如果 used 为 0，则置为 1 并返回 true */
		if (atomic_cmpxchg(&vm_ctx_pool[i].used, 0, 1) == 0) {
			ctx = &vm_ctx_pool[i];
			ctx->id = i;
			break;
		}
	}

	if (!ctx) {
		pr_err("lazydma: Max VM limit reached (%d)\n", MAX_VM_NUMS);
		return -EBUSY;
	}

	/* 2. 初始化上下文 (清除旧数据) */
	/* 注意：atomic_t used 已经在上面置为 1 了，不要 memset 覆盖它 */
	ctx->stop_req = false;
	ctx->thread = NULL;
	ctx->target_mm = NULL;
	ctx->entrys = NULL;
	init_waitqueue_head(&ctx->wq);

	/* 3. 获取 mm */
	if (!current->mm) {
		atomic_set(&ctx->used, 0); /* 回滚：释放槽位 */
		return -EINVAL;
	}

	mmget(current->mm);
	ctx->target_mm = current->mm;

	/* 4. 启动线程 */
	ctx->thread = kthread_run(lazydma_worker, ctx, "lazydma/%d", ctx->id);
	if (IS_ERR(ctx->thread)) {
		int ret = PTR_ERR(ctx->thread);
		mmput(ctx->target_mm);
		atomic_set(&ctx->used, 0); /* 回滚 */
		return ret;
	}

	/* 5. 关键：将 ctx 保存到 file->private_data */
	file->private_data = ctx;

	pr_info("lazydma: Opened device for VM %d\n", ctx->id);
	return 0;
}

/*
 * Char Device Release
 */
static int lazydma_release(struct inode *inode, struct file *file)
{
	/* 从 private_data 取回当前进程的上下文 */
	struct lazydma_ctx *ctx = file->private_data;

	if (!ctx)
		return 0;

	/* 停止业务 */
	ctx->stop_req = true;
	wake_up_interruptible(&ctx->wq);

	if (ctx->thread) {
		kthread_stop(ctx->thread);
		ctx->thread = NULL;
	}

	/* * 注意：mmput 已经在 worker 线程退出前调用了，这里不需要再调。
	* 除非 kthread_run 失败的路径（已经在 open 里处理了）。
	*/

	pr_info("lazydma: Released device for VM %d\n", ctx->id);

	/* 清空关联，防止 UAF */
	file->private_data = NULL;

	/* 最后：原子地归还槽位 */
	atomic_set(&ctx->used, 0);
	return 0;
}

static long lazydma_ioctl(struct file *filp, unsigned int cmd,
			  unsigned long arg)
{
	struct lazydma_ctx *ctx = filp->private_data;
	struct lazydma_shm_config config;

	if (!ctx)
		return -ENODEV;

	switch (cmd) {
	case LAZYDMA_SET_SHM_CONFIG:
		if (copy_from_user(&config, (void __user *)arg,
				   sizeof(config))) {
			return -EFAULT;
		}
		pr_info("lazydma: VM %d config set: addr 0x%llx size %llu\n",
			ctx->id, config.addr, config.size);

		/* TODO: 这里可以将 config 保存到 ctx 中，
		* 供 lazydma_scan_mm 使用以缩小扫描范围 
		*/
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static const struct file_operations lazydma_fops = {
	.owner = THIS_MODULE,
	.open = lazydma_open,
	.release = lazydma_release,
	.unlocked_ioctl = lazydma_ioctl,
};

static struct miscdevice lazydma_misc = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = DEVICE_NAME,
	.fops = &lazydma_fops,
};

static int __init lazydma_init(void)
{
	return misc_register(&lazydma_misc);
}

static void __exit lazydma_exit(void)
{
	misc_deregister(&lazydma_misc);
}

module_init(lazydma_init);
module_exit(lazydma_exit);
