#include "asm/pgtable_64_types.h"
#include "linux/kvm_types.h"
#include "linux/lockdep.h"
#include "linux/mutex.h"
#include "linux/mutex_types.h"
#include "linux/rcupdate.h"
#include <linux/mmu_notifier.h>
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
#include <linux/vmalloc.h>


#include "linux/lazydma.h"

#ifdef CONFIG_LAZYDMA_PTE_MODE
	#define LAZYDMA_SHIFT    PAGE_SHIFT
	#define LAZYDMA_SIZE     PAGE_SIZE
	/* PTE 模式下的步进 */
	#define LAZYDMA_WALK_STEP PAGE_SIZE
#else
	#define LAZYDMA_SHIFT    PMD_SHIFT
	#define LAZYDMA_SIZE     PMD_SIZE
	/* PMD 模式下的步进 */
	#define LAZYDMA_WALK_STEP PMD_SIZE
#endif


/* * 注意：reclaim_pages 在 mm/vmscan.c 中定义。
 * 如果它在你的内核源码中是 static 的，你需要去 mm/vmscan.c 去掉 static，
 * 或者在 mm/internal.h 中找到它的声明。
 * 这里我们手动声明一下。
 */
extern unsigned long reclaim_pages(struct list_head *folio_list);
extern bool folio_isolate_lru(struct folio *folio);

/* 你的自定义接口 */
extern bool pmd_check_io(pmd_t *pmd);
extern bool pte_check_io(pte_t *pte);

#define DEVICE_NAME "lazydma"

struct lazydma_ctx {
	atomic_t used; /* 0: 空闲, 1: 占用 */
	int id; /* 调试用的 ID */

	struct task_struct *thread;
	struct mm_struct *target_mm;
	struct dma_tracking_entry *entrys;
	wait_queue_head_t wq;
	struct mmu_notifier mn;
	bool stop_req;

	struct mutex mt_lock;
	struct __rcu lazydma_mem_table *mem_table;
};

static struct lazydma_ctx vm_ctx_pool[MAX_VM_NUMS];


/* --------------------------------------------------------------------------
 * 页表状态检查 (is_mapped)
 * -------------------------------------------------------------------------- */

/* 通用 Helper：获取 PMD */
static pmd_t *get_pmd_fast(struct mm_struct *mm, unsigned long addr)
{
	pgd_t *pgd;
	p4d_t *p4d;
	pud_t *pud;

	pgd = pgd_offset(mm, addr);
	if (pgd_none(*pgd) || pgd_bad(*pgd)) return NULL;

	p4d = p4d_offset(pgd, addr);
	if (p4d_none(*p4d) || p4d_bad(*p4d)) return NULL;

	pud = pud_offset(p4d, addr);
	if (pud_none(*pud) || pud_bad(*pud)) return NULL;

	return pmd_offset(pud, addr);
}

#ifndef CONFIG_LAZYDMA_PTE_MODE
/* PMD 模式检查 */
static bool is_target_mapped(struct mm_struct *mm, unsigned long addr)
{
	pmd_t *pmd = get_pmd_fast(mm, addr);
	
	if (!pmd) return false;

	/* 检查 PMD 是否存在且不是 swap entry */
	if (pmd_present(*pmd) && !is_swap_pmd(*pmd)) {
		return true;
	}
	return false;
}
#else
/* PTE 模式检查 */
static bool is_target_mapped(struct mm_struct *mm, unsigned long addr)
{
	pmd_t *pmd;
	pte_t *pte;
	spinlock_t *ptl;
	bool is_present = false;

	pmd = get_pmd_fast(mm, addr);
	if (!pmd || pmd_none(*pmd) || pmd_bad(*pmd)) 
		return false;

	/* * 注意：如果是 THP (Huge Page)，但在 PTE 模式下被当作 4K 处理，
	 * 这里需要额外逻辑。通常如果配置了 PTE 模式，应尽量避免 THP 
	 * 或者在这里拆分处理。简化起见，我们假设是标准的 PTE 映射。
	 */
	if (pmd_trans_huge(*pmd)) {
		/* 如果是透明大页，且我们在 PTE 模式，视为已映射 */
		return true; 
	}

	/* 获取 PTE */
	pte = pte_offset_map_lock(mm, pmd, addr, &ptl);
	if (!pte) return false;

	if (pte_present(*pte))
		is_present = true;

	pte_unmap_unlock(pte, ptl);
	return is_present;
}
#endif

#define LAZYDMA_INVALID_ADDR (~0ULL)

static inline gpa_t hva_to_gpa(struct lazydma_mem_table *mem_table, hva_t hva)
{
	int i;
	struct lazydma_memory_region *reg;

	if (!mem_table)
		return LAZYDMA_INVALID_ADDR;

	/* 线性遍历 Memtable */
	for (i = 0; i < mem_table->nregions; i++) {
		reg = &mem_table->regions[i];

		/* 判断 hva 是否在当前 region 区间内: [start, start + size) */
		if (hva >= reg->userspace_addr && 
			hva < (reg->userspace_addr + reg->size)) {
			
			/* 计算偏移量并加上 GPA 基地址 */
			return reg->guest_phys_addr + (hva - reg->userspace_addr);
		}
	}

	return LAZYDMA_INVALID_ADDR;
}

static inline hva_t gpa_to_hva(struct lazydma_mem_table *mem_table, gpa_t gpa)
{
	int i;
	struct lazydma_memory_region *reg;

	if (!mem_table)
		return LAZYDMA_INVALID_ADDR;

	for (i = 0; i < mem_table->nregions; i++) {
		reg = &mem_table->regions[i];

		/* 判断 gpa 是否在当前 region 区间内 */
		if (gpa >= reg->guest_phys_addr && 
			gpa < (reg->guest_phys_addr + reg->size)) {
			
			/* 计算偏移量并加上 HVA 基地址 */
			return reg->userspace_addr + (gpa - reg->guest_phys_addr);
		}
	}

	return LAZYDMA_INVALID_ADDR;
}


static struct dma_tracking_entry* get_entry(struct lazydma_ctx *ctx, hva_t addr)
{
	struct lazydma_mem_table *mem_table = NULL;
	struct dma_tracking_entry *entrys = ctx->entrys;
	gpa_t gpa;
	unsigned int index;

	rcu_read_lock();
	mem_table = rcu_dereference(ctx->mem_table);
	gpa = hva_to_gpa(mem_table, addr);
	index = gpa >> PMD_SHIFT;
	rcu_read_unlock();

	return &entrys[index];
}

static int
lazydma_mn_invalidate_range_start(struct mmu_notifier *mn,
				  const struct mmu_notifier_range *range)
{
	struct lazydma_ctx *ctx = container_of(mn, struct lazydma_ctx, mn);
	unsigned long addr;

	/* 步进改为 LAZYDMA_WALK_STEP (PMD_SIZE 或 PAGE_SIZE) */
	for (addr = range->start; addr < range->end; addr += LAZYDMA_WALK_STEP) {
		/* 对齐地址，防止非对齐的 range start */
		unsigned long aligned_addr = addr & ~(LAZYDMA_SIZE - 1);
		struct dma_tracking_entry *entry = get_entry(ctx, aligned_addr);
		if (entry) {
			atomic_fetch_and(0x7fffffff, &entry->val);
		}
	}
	return 0;
}

static void
lazydma_mn_invalidate_range_end(struct mmu_notifier *mn,
				const struct mmu_notifier_range *range)
{
	struct lazydma_ctx *ctx = container_of(mn, struct lazydma_ctx, mn);
	unsigned long addr;

	for (addr = range->start; addr < range->end; addr += LAZYDMA_WALK_STEP) {
		unsigned long aligned_addr = addr & ~(LAZYDMA_SIZE - 1);
		
		/* 使用适配模式的 is_target_mapped */
		// TODO: 确认这个逻辑可以保障事务性
		if (is_target_mapped(range->mm, aligned_addr)) {
			struct dma_tracking_entry *entry =
				get_entry(ctx, aligned_addr);
			if (entry && entry->present == 0) {
				entry->present = 1;
				wmb();
			}
		}
	}
}

static const struct mmu_notifier_ops lazydma_mn_ops = {
	.invalidate_range_start = lazydma_mn_invalidate_range_start,
	.invalidate_range_end = lazydma_mn_invalidate_range_end,
};

// TODO: neet to 确定参数, 参数用于index到ctx
void lazydma_notify_page_fault(void)
{

}

/* --------------------------------------------------------------------------
 * Page Walk & Reclaim 逻辑
 * -------------------------------------------------------------------------- */

/* * 提取公共逻辑：尝试隔离并添加 Folio 到列表 
 */
static void lazydma_isolate_and_add(struct page *page, struct list_head *folio_list)
{
	struct folio *folio;

	if (!page) return;
	
	folio = page_folio(page);
	if (!folio_try_get(folio))
		return;

	if (folio_isolate_lru(folio)) {
		list_add_tail(&folio->lru, folio_list);
	} else {
		folio_put(folio);
	}
}

/* PMD 模式回调 (Huge Page) */
static int lazydma_pmd_entry(pmd_t *pmd, unsigned long addr, unsigned long next,
			     struct mm_walk *walk)
{
	struct list_head *folio_list = walk->private;

	if (!pmd_present(*pmd) || pmd_none(*pmd))
		return 0;

	/* 业务 Check */
	if (!pmd_check_io(pmd))
		return 0;

	lazydma_isolate_and_add(pmd_page(*pmd), folio_list);
	return 0;
}

/* PTE 模式回调 (4K Page) */
static int lazydma_pte_entry(pte_t *pte, unsigned long addr, unsigned long next,
			     struct mm_walk *walk)
{
	struct list_head *folio_list = walk->private;
	struct page *page;

	if (!pte_present(*pte))
		return 0;

	/* 业务 Check (你需要实现 pte_check_io) */
	if (!pte_check_io(pte))
		return 0;

	page = vm_normal_page(walk->vma, addr, *pte);
	if (!page)
		return 0;

	lazydma_isolate_and_add(page, folio_list);
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

	/* 3. 获取 mm */
	if (!current->mm) {
		atomic_set(&ctx->used, 0); /* 回滚：释放槽位 */
		return -EINVAL;
	}

	mmget(current->mm);
	ctx->target_mm = current->mm;

	init_waitqueue_head(&ctx->wq);
	ctx->mn.ops = &lazydma_mn_ops;
	mmu_notifier_register(&ctx->mn, ctx->target_mm);

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

	/* 线程结束，释放 mm 引用 */
	mmu_notifier_unregister(&ctx->mn, ctx->target_mm);
	mmput(ctx->target_mm);

	pr_info("lazydma: Released device for VM %d\n", ctx->id);

	/* 清空关联，防止 UAF */
	file->private_data = NULL;

	/* 最后：原子地归还槽位 */
	atomic_set(&ctx->used, 0);
	return 0;
}

static int update_lazydma_mem_table(struct lazydma_ctx *ctx, struct lazydma_mem_table *new, u64 full_size)
{
	int ret = 0;

	struct lazydma_mem_table *old = NULL;
	struct lazydma_mem_table *tmp = vzalloc(full_size);
	memcpy(tmp, new, full_size);

	mutex_lock(&ctx->mt_lock);
	old = rcu_dereference_protected(ctx->mem_table, lock_is_held(&ctx->mt_lock));
	rcu_assign_pointer(ctx->mem_table, tmp);
	mutex_unlock(&ctx->mt_lock);

	synchronize_rcu();

	vfree(old);

	if (ctx->entrys == NULL) {
			
	}
	return ret;
}

static long lazydma_ioctl(struct file *filp, unsigned int cmd,
			  unsigned long arg)
{
	struct lazydma_ctx *ctx = filp->private_data;

	if (!ctx)
		return -ENODEV;

	switch (cmd) {
	case LAZYDMA_SET_MEMTABLE:
		struct lazydma_mem_table header;
		struct lazydma_mem_table *full_table;
		u64 full_size;

		/* 1. 先读头部，获取 nregions */
		if (copy_from_user(&header, (void __user *)arg, sizeof(header)))
			return -EFAULT;

		/* 2. 分配内核内存读取整个表 */
		full_size = sizeof(header) + header.nregions * sizeof(struct lazydma_memory_region);
		full_table = vmalloc(full_size);
		if (!full_table) return -ENOMEM;

		if (copy_from_user(full_table, (void __user *)arg, full_size)) {
			vfree(full_table);
			return -EFAULT;
		}

		/* 3. 更新 ctx 中的内存映射表 */
		/* * 注意：这里需要并发保护 (mutex)。
		* 建议：释放旧的 map，建立新的 map。
		* 遍历 full_table->regions[i]，保存 {gpa, hva, size} 到内核结构体中。
		*/
		update_lazydma_mem_table(ctx, full_table, full_size);

		vfree(full_table);
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
