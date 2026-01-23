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

#define MAX_VM_NUMS 256

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

void lazydma_map(phys_addr_t gpa, size_t size);

void lazydma_unmap(phys_addr_t gpa, size_t size);

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
