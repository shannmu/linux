/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Lazy DMA - DMA tracking and VM-exit based page management
 *
 * Copyright (C) 2025
 */

#ifndef _LINUX_LAZYDMA_H
#define _LINUX_LAZYDMA_H

#include <linux/types.h>
#include <linux/device.h>

#ifdef CONFIG_LAZYDMA


#define MY_IOC_MAGIC 'q'

#define MAX_VM_NUMS 256

#define TRACKING_TABLE_ADDR 0x10000000000UL
#define TRACKING_TABLE_SIZE 0x200000UL

#ifdef CONFIG_LAZYDMA_HUGEPAGE

#define TRACKING_MEM_SIZE PMD_SIZE
#define TRACKING_MEM_SHIFT PMD_SHIFT
#define TRACKING_MEM_MASK PMD_MASK

#else

#define TRACKING_MEM_SIZE PAGE_SIZE
#define TRACKING_MEM_SHIFT PAGE_SHIFT
#define TRACKING_MEM_MASK PAGE_MASK

#endif

/**
 * lazydma_hook_device - Hook DMA operations for a specific device
 * @dev: Device to hook
 *
 * This function replaces the device's DMA operations with lazydma hooks
 * to enable DMA tracking. The original operations are preserved and called
 * by the hooks.
 *
 * Returns: 0 on success, negative error code on failure
 */
int lazydma_hook_device(struct device *dev);

/**
 * lazydma_unhook_device - Restore original DMA operations for a device
 * @dev: Device to unhook
 *
 * This function restores the device's original DMA operations, disabling
 * lazydma tracking for this device.
 */
void lazydma_unhook_device(struct device *dev);

/* Use kernel's PMD_SHIFT and PMD_SIZE definitions */
#ifndef PMD_SHIFT
#define PMD_SHIFT 21
#endif

#ifndef PMD_SIZE
#define PMD_SIZE (1UL << PMD_SHIFT)
#endif

#ifndef PMD_MASK
#define PMD_MASK (~(PMD_SIZE - 1))
#endif

/* DMA tracking entry - 4 bytes per PMD */
struct dma_tracking_entry {
	union {
		atomic_t val;
		struct {
			unsigned int mapped_count : 31;
			unsigned int present : 1;
		};
	};
} __packed;

struct lazydma_memory_region {
    __u64 userspace_addr;    /* HVA */
    __u64 guest_phys_addr;   /* GPA */
    __u64 size;              /* Size */
    __u64 flags;             /* 比如是否只读 */
};

/* 变长结构体，或者你分多次下发 */
struct lazydma_mem_table {
    __u32 nregions;
    __u32 padding;
    struct lazydma_memory_region regions[0]; /* 变长数组 */
};

#define LAZYDMA_SET_MEMTABLE _IOW(MY_IOC_MAGIC, 1, struct lazydma_mem_table)

void lazydma_map(phys_addr_t gpa, size_t size);

void lazydma_unmap(phys_addr_t gpa, size_t size);

void lazydma_notify_page_fault(void);

#else /* !CONFIG_LAZYDMA */

static inline int lazydma_hook_device(struct device *dev)
{
	return -ENODEV;
}

static inline void lazydma_unhook_device(struct device *dev)
{
}

#endif /* CONFIG_LAZYDMA */

#endif /* _LINUX_LAZYDMA_H */
