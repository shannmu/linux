// SPDX-License-Identifier: GPL-2.0
/*
 * Lazy DMA - DMA tracking and VM-exit based page management
 *
 * Copyright (C) 2025
 */

#include "asm/dma-mapping.h"
#include "linux/types.h"
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/miscdevice.h>
#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/io.h>
#include <linux/memblock.h>
#include <linux/dma-mapping.h>
#include <linux/dma-map-ops.h>
#include <linux/dma-direct.h>
#include <linux/pci.h>
#include <linux/atomic.h>
#include <linux/slab.h>
#include <linux/lazydma.h>
#include <asm/pgtable.h>

#define LAZYDMA_NAME "lazydma"

/* Lazy DMA global state */
struct lazydma_state {
	/* Tracking memory region */
	phys_addr_t track_phys_addr;
	size_t track_size;
	void *track_vaddr;

	/* Original DMA ops for hooking */
	const struct dma_map_ops *orig_dma_ops;

	/* Our hooked DMA ops */
	struct dma_map_ops hooked_dma_ops;

	bool initialized;
	spinlock_t lock;
} lazydma;

/* Configuration from kernel command line */
static phys_addr_t track_mem_addr __initdata;
static size_t track_mem_size __initdata;
// static phys_addr_t mmio_mem_addr __initdata;
// static size_t mmio_mem_size __initdata;

/*
 * Get PMD index from physical address
 */
static inline unsigned long phys_to_pmd_index(phys_addr_t phys)
{
	return phys >> PMD_SHIFT;
}

/*
 * Get tracking entry for a physical address
 */
static inline struct dma_tracking_entry *get_tracking_entry(phys_addr_t phys)
{
	unsigned long pmd_index = phys_to_pmd_index(phys);
	struct dma_tracking_entry *entries = lazydma.track_vaddr;

	/* Check bounds */
	if (pmd_index >=
	    (lazydma.track_size / sizeof(struct dma_tracking_entry))) {
		pr_warn_ratelimited("lazydma: PMD index %lu out of bounds\n",
				    pmd_index);
		return NULL;
	}

	return &entries[pmd_index];
}

/*
 * Check if present bit is set for a PMD
 */
static inline bool check_present_bit(struct dma_tracking_entry *entry)
{
	unsigned int val = atomic_read(&entry->val);
	return !!(val & (1U << 31)); /* present bit is bit 31 */
}

// /*
//  * Trigger VM-exit by writing PMD index to MMIO region
//  * Returns true if present bit was set after VM-exit
//  */
// static bool trigger_vmexit_for_pmd(unsigned long pmd_index)
// {
// 	struct dma_tracking_entry *entry;
// 	int timeout = 1000; /* Timeout iterations */
//
// 	if (!lazydma.mmio_vaddr) {
// 		pr_warn_ratelimited("lazydma: MMIO region not mapped\n");
// 		return false;
// 	}
//
// 	/* Write PMD index to MMIO region to trigger VM-exit */
// 	writel(pmd_index, lazydma.mmio_vaddr);
//
// 	/* Ensure write is flushed */
// 	wmb();
//
// 	/*
// 	 * After VM-exit, host will process this PMD and set the present bit.
// 	 * Poll the present bit to check if host has finished processing.
// 	 */
// 	entry = get_tracking_entry(pmd_index << PMD_SHIFT);
// 	if (!entry)
// 		return false;
//
// 	while (timeout-- > 0) {
// 		if (check_present_bit(entry))
// 			return true;
// 		cpu_relax();
// 	}
//
// 	pr_warn_ratelimited("lazydma: Timeout waiting for present bit on PMD %lu\n",
// 			    pmd_index);
// 	return false;
// }

static inline void __touch_mem(phys_addr_t phys, size_t size)
{
	if (unlikely(size == 0))
		return;

	/* 找到第一个有交集的 PMD 的起始物理地址 */
	phys_addr_t curr_phys = phys & PMD_MASK;

	/* 找到最后一个有交集的 PMD 的起始物理地址 */
	phys_addr_t last_phys = (phys + size - 1) & PMD_MASK;

	/* 遍历每一个涉及到的 PMD */
	do {
		/* * 1. __va() 将物理地址转为直接映射区虚拟地址 
		* 2. 强转为 (char *) 指向首字节
		* 3. READ_ONCE 确保触发实际的内存读取指令
		*/
		(void)READ_ONCE(*(char *)__va(curr_phys));

		/* 移动到下一个 PMD */
		curr_phys += PMD_SIZE;

	} while (curr_phys <= last_phys);
}

/*
 * Track DMA mapping for a physical address range
 */
static void track_dma_map(phys_addr_t phys, size_t size)
{
	phys_addr_t end = phys + size;
	phys_addr_t pmd_start, pmd_end;

	if (!lazydma.initialized)
		return;

	/* Align to PMD boundaries */
	pmd_start = phys & PMD_MASK;
	pmd_end = (end + PMD_SIZE - 1) & PMD_MASK;

	/* Track each PMD */
	for (phys_addr_t addr = pmd_start; addr < pmd_end; addr += PMD_SIZE) {
		struct dma_tracking_entry *entry = get_tracking_entry(addr);
		if (entry) {
			unsigned int old_val, new_val;
			int old_count;

			/* Atomically increment mapped_count (lower 31 bits) */
			do {
				// TODO: touch mem to try to trigger ept page fault.
				__touch_mem(phys, size);

				old_val = atomic_read(&entry->val);
				old_count = old_val &
					    0x7FFFFFFF; /* Get lower 31 bits */

				if (old_count >= 0x7FFFFFFF) {
					pr_warn_ratelimited(
						"lazydma: Mapped count overflow for PMD index %lu\n",
						phys_to_pmd_index(addr));
					break;
				}

				new_val =
					old_val +
					1; /* Increment count, preserve present bit */
			} while (atomic_cmpxchg(&entry->val, old_val,
						new_val) != old_val &&
				 (new_val & 0x80000000));
		}
	}
}

/*
 * Track DMA unmapping for a physical address range
 */
static void track_dma_unmap(phys_addr_t phys, size_t size)
{
	phys_addr_t end = phys + size;
	phys_addr_t pmd_start, pmd_end;

	if (!lazydma.initialized)
		return;

	/* Align to PMD boundaries */
	pmd_start = phys & PMD_MASK;
	pmd_end = (end + PMD_SIZE - 1) & PMD_MASK;

	/* Untrack each PMD */
	for (phys_addr_t addr = pmd_start; addr < pmd_end; addr += PMD_SIZE) {
		struct dma_tracking_entry *entry = get_tracking_entry(addr);
		if (entry) {
			unsigned int old_val, new_val;
			int old_count;

			/* Atomically decrement mapped_count (lower 31 bits) */
			do {
				old_val = atomic_read(&entry->val);
				old_count = old_val &
					    0x7FFFFFFF; /* Get lower 31 bits */

				if (old_count == 0) {
					pr_warn_ratelimited(
						"lazydma: Underflow on unmap for PMD index %lu\n",
						phys_to_pmd_index(addr));
					break;
				}

				new_val =
					old_val -
					1; /* Decrement count, preserve present bit */
			} while (atomic_cmpxchg(&entry->val, old_val,
						new_val) != old_val);
		}
	}
}

/*
 * Hooked DMA map_page operation
 */
static dma_addr_t lazydma_map_page(struct device *dev, struct page *page,
				   unsigned long offset, size_t size,
				   enum dma_data_direction dir,
				   unsigned long attrs)
{
	dma_addr_t dma_addr;
	phys_addr_t phys;

	/* Call original operation */
	if (lazydma.orig_dma_ops && lazydma.orig_dma_ops->map_page)
		dma_addr = lazydma.orig_dma_ops->map_page(dev, page, offset,
							  size, dir, attrs);
	else
		return DMA_MAPPING_ERROR;

	if (dma_addr == DMA_MAPPING_ERROR)
		return dma_addr;

	/* Track this DMA mapping */
	phys = page_to_phys(page) + offset;
	track_dma_map(phys, size);

	return dma_addr;
}

/*
 * Hooked DMA unmap_page operation
 */
static void lazydma_unmap_page(struct device *dev, dma_addr_t dma_addr,
			       size_t size, enum dma_data_direction dir,
			       unsigned long attrs)
{
	phys_addr_t phys = dma_to_phys(dev, dma_addr);

	/* Untrack this DMA mapping */
	track_dma_unmap(phys, size);

	/* Call original operation */
	if (lazydma.orig_dma_ops && lazydma.orig_dma_ops->unmap_page)
		lazydma.orig_dma_ops->unmap_page(dev, dma_addr, size, dir,
						 attrs);
}

/*
 * Hooked DMA map_sg operation
 */
static int lazydma_map_sg(struct device *dev, struct scatterlist *sg, int nents,
			  enum dma_data_direction dir, unsigned long attrs)
{
	int ret;
	struct scatterlist *s;
	int i;

	/* Call original operation */
	if (lazydma.orig_dma_ops && lazydma.orig_dma_ops->map_sg)
		ret = lazydma.orig_dma_ops->map_sg(dev, sg, nents, dir, attrs);
	else
		return 0;

	if (ret <= 0)
		return ret;

	/* Track each mapped segment */
	for_each_sg(sg, s, ret, i) {
		phys_addr_t phys = sg_phys(s);
		track_dma_map(phys, s->length);
	}

	return ret;
}

/*
 * Hooked DMA unmap_sg operation
 */
static void lazydma_unmap_sg(struct device *dev, struct scatterlist *sg,
			     int nents, enum dma_data_direction dir,
			     unsigned long attrs)
{
	struct scatterlist *s;
	int i;

	/* Untrack each mapped segment */
	for_each_sg(sg, s, nents, i) {
		if (s->dma_length) {
			phys_addr_t phys = sg_phys(s);
			track_dma_unmap(phys, s->dma_length);
		}
	}

	/* Call original operation */
	if (lazydma.orig_dma_ops && lazydma.orig_dma_ops->unmap_sg)
		lazydma.orig_dma_ops->unmap_sg(dev, sg, nents, dir, attrs);
}

/*
 * Hooked DMA map_resource operation (for MMIO)
 */
static dma_addr_t lazydma_map_resource(struct device *dev,
				       phys_addr_t phys_addr, size_t size,
				       enum dma_data_direction dir,
				       unsigned long attrs)
{
	dma_addr_t dma_addr;

	/* Call original operation */
	if (lazydma.orig_dma_ops && lazydma.orig_dma_ops->map_resource)
		dma_addr = lazydma.orig_dma_ops->map_resource(dev, phys_addr,
							      size, dir, attrs);
	else
		return DMA_MAPPING_ERROR;

	if (dma_addr == DMA_MAPPING_ERROR)
		return dma_addr;

	/* Track this MMIO DMA mapping */
	track_dma_map(phys_addr, size);

	return dma_addr;
}

/*
 * Hooked DMA unmap_resource operation (for MMIO)
 */
static void lazydma_unmap_resource(struct device *dev, dma_addr_t dma_addr,
				   size_t size, enum dma_data_direction dir,
				   unsigned long attrs)
{
	phys_addr_t phys = dma_to_phys(dev, dma_addr);

	/* Untrack this MMIO DMA mapping */
	track_dma_unmap(phys, size);

	/* Call original operation */
	if (lazydma.orig_dma_ops && lazydma.orig_dma_ops->unmap_resource)
		lazydma.orig_dma_ops->unmap_resource(dev, dma_addr, size, dir,
						     attrs);
}

/*
 * Hook DMA operations - copy original ops and override map/unmap functions
 */
static void hook_dma_ops(void)
{
	/* Get original DMA ops from architecture */
	lazydma.orig_dma_ops = get_arch_dma_ops();

	if (!lazydma.orig_dma_ops) {
		pr_warn("lazydma: No original DMA ops found, will use direct mapping\n");
		/* In this case, we can still hook at the generic DMA layer */
		return;
	}

	/* Copy original DMA ops and hook map/unmap */
	memcpy(&lazydma.hooked_dma_ops, lazydma.orig_dma_ops,
	       sizeof(struct dma_map_ops));

	/* Hook page mapping operations */
	lazydma.hooked_dma_ops.map_page = lazydma_map_page;
	lazydma.hooked_dma_ops.unmap_page = lazydma_unmap_page;

	/* Hook scatter-gather operations */
	lazydma.hooked_dma_ops.map_sg = lazydma_map_sg;
	lazydma.hooked_dma_ops.unmap_sg = lazydma_unmap_sg;

	/* Hook resource mapping operations (for MMIO) */
	lazydma.hooked_dma_ops.map_resource = lazydma_map_resource;
	lazydma.hooked_dma_ops.unmap_resource = lazydma_unmap_resource;

	/*
	 * TODO: Apply hooked ops to devices
	 *
	 * For PCI devices, we could iterate over all PCI devices and set:
	 *   set_dma_ops(&pdev->dev, &lazydma.hooked_dma_ops);
	 *
	 * Or we could hook at the architecture level by replacing the
	 * global dma_ops (arch/x86/kernel/pci-dma.c)
	 */

	pr_info("lazydma: DMA ops hooks prepared (not yet applied to devices)\n");
}

/*
 * Apply hooked DMA ops to a specific device
 */
int lazydma_hook_device(struct device *dev)
{
	if (!lazydma.initialized)
		return -ENODEV;

	set_dma_ops(dev, &lazydma.hooked_dma_ops);
	pr_info("lazydma: Hooked DMA ops for device %s\n", dev_name(dev));

	return 0;
}
EXPORT_SYMBOL_GPL(lazydma_hook_device);

/*
 * Restore original DMA ops to a specific device
 */
void lazydma_unhook_device(struct device *dev)
{
	if (!lazydma.initialized)
		return;

	set_dma_ops(dev, lazydma.orig_dma_ops);
	pr_info("lazydma: Unhooked DMA ops for device %s\n", dev_name(dev));
}
EXPORT_SYMBOL_GPL(lazydma_unhook_device);

/*
 * Parse kernel command line: lazydma_track=addr,size
 */
static int __init parse_lazydma_track(char *arg)
{
	char *p;

	if (!arg)
		return -EINVAL;

	track_mem_addr = memparse(arg, &p);
	if (*p == ',') {
		p++;
		track_mem_size = memparse(p, NULL);
	}

	pr_info("lazydma: Track memory: addr=0x%llx size=0x%lx\n",
		(unsigned long long)track_mem_addr, track_mem_size);

	return 0;
}
early_param("lazydma_track", parse_lazydma_track);

void lazydma_map(phys_addr_t gpa, size_t size)
{
	track_dma_map(gpa, size);
}

void lazydma_unmap(phys_addr_t gpa, size_t size)
{
	track_dma_unmap(gpa, size);
}

/*
 * Initialize lazy DMA subsystem
 */
static int __init lazydma_init(void)
{
	pr_info("lazydma: Initializing Lazy DMA subsystem\n");

	/* Check if we have valid configuration from command line */
	if (!track_mem_addr || !track_mem_size) {
		pr_err("lazydma: No tracking memory specified. Use lazydma_track=addr,size\n");
		return -EINVAL;
	}

	/* Initialize state */
	memset(&lazydma, 0, sizeof(lazydma));
	spin_lock_init(&lazydma.lock);

	lazydma.track_phys_addr = track_mem_addr;
	lazydma.track_size = track_mem_size;

	/* Map tracking memory region with memremap (cached, write-back) */
	lazydma.track_vaddr = memremap(lazydma.track_phys_addr,
				       lazydma.track_size, MEMREMAP_WB);
	if (!lazydma.track_vaddr) {
		pr_err("lazydma: Failed to map tracking memory\n");
		return -ENOMEM;
	}

	/* Initialize tracking entries to zero */
	memset(lazydma.track_vaddr, 0, lazydma.track_size);

	pr_info("lazydma: Tracking memory mapped at %p (phys: 0x%llx, size: 0x%lx)\n",
		lazydma.track_vaddr,
		(unsigned long long)lazydma.track_phys_addr,
		lazydma.track_size);
	pr_info("lazydma: Number of tracking entries: %lu (covers %lu MB)\n",
		lazydma.track_size / sizeof(struct dma_tracking_entry),
		(lazydma.track_size / sizeof(struct dma_tracking_entry)) * 2);

	/* Prepare DMA operation hooks */
	hook_dma_ops();

	lazydma.initialized = true;

	pr_info("lazydma: Device registered as /dev/%s\n", LAZYDMA_NAME);
	pr_info("lazydma: Initialization complete\n");
	pr_info("lazydma: To hook a device's DMA ops, call lazydma_hook_device()\n");

	return 0;
}

/*
 * Cleanup lazy DMA subsystem
 */
static void __exit lazydma_exit(void)
{
	pr_info("lazydma: Shutting down Lazy DMA subsystem\n");

	if (!lazydma.initialized)
		return;

	/* Mark as not initialized to stop tracking */
	lazydma.initialized = false;

	/* Unmap tracking memory region */
	if (lazydma.track_vaddr)
		memunmap(lazydma.track_vaddr);

	pr_info("lazydma: Shutdown complete\n");
}

module_init(lazydma_init);
module_exit(lazydma_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("shanmu");
MODULE_DESCRIPTION("Lazy DMA - DMA tracking with VM-exit support");
MODULE_VERSION("1.0");
