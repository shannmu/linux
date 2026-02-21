// SPDX-License-Identifier: GPL-2.0
/*
 * pvsched_guest.c - PV Scheduler Guest-side driver
 *
 * Responsibilities:
 *  1. Parse kernel cmdline parameter "pvsched_shared_mem=<addr>,<size>" via
 *     early_param, then ioremap the reserved physical memory region into a
 *     struct pvsched_shared_mem pointer used as shared memory between Guest
 *     kernel and Host.
 *
 *  2. Expose a /dev/pvsched_guest character device with open, release and mmap
 *     file operations so that userspace can directly access the shared memory.
 *
 * Cmdline format:
 *   pvsched_shared_mem=<RES_ADDR>,<RES_SIZE> memmap=<RES_SIZE>\$<RES_ADDR>
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/mm.h>
#include <linux/io.h>
#include <linux/memremap.h>
#include <linux/uaccess.h>
#include <linux/slab.h>
#include <linux/string.h>

#include "pvsched.h"

#define PVSCHED_GUEST_DEV_NAME "pvsched_guest"

/* --------------------------------------------------------------------------
 * Shared memory descriptor parsed from the kernel command line.
 * -------------------------------------------------------------------------- */

static phys_addr_t pvsched_res_addr; /* physical base address */
static phys_addr_t pvsched_res_size; /* size in bytes         */

/* Virtual mapping of the shared memory region (set during module init). */
static struct pvsched_shared_mem *pvsched_shm;

/* --------------------------------------------------------------------------
 * early_param handler
 *
 * Parses:  pvsched_shared_mem=<hex/dec addr>,<hex/dec size>
 * -------------------------------------------------------------------------- */

static int __init pvsched_shared_mem_setup(char *str)
{
	char *size_str;

	if (!str || !*str) {
		pr_err("pvsched_guest: missing argument for pvsched_shared_mem\n");
		return -EINVAL;
	}

	/* Split on ',' to get address and size tokens. */
	size_str = strchr(str, ',');
	if (!size_str) {
		pr_err("pvsched_guest: invalid format, expected <addr>,<size>\n");
		return -EINVAL;
	}
	*size_str = '\0';
	size_str++;

	pvsched_res_addr = (phys_addr_t)memparse(str, NULL);
	pvsched_res_size = (phys_addr_t)memparse(size_str, NULL);

	if (!pvsched_res_addr || !pvsched_res_size) {
		pr_err("pvsched_guest: invalid addr=0x%llx or size=0x%llx\n",
		       (unsigned long long)pvsched_res_addr,
		       (unsigned long long)pvsched_res_size);
		return -EINVAL;
	}

	pr_info("pvsched_guest: parsed shared mem addr=0x%llx size=0x%llx\n",
		(unsigned long long)pvsched_res_addr,
		(unsigned long long)pvsched_res_size);

	return 0;
}
early_param("pvsched_shared_mem", pvsched_shared_mem_setup);

/* --------------------------------------------------------------------------
 * Character device file operations
 * -------------------------------------------------------------------------- */

static int pvsched_guest_open(struct inode *inode, struct file *filp)
{
	if (!pvsched_shm) {
		pr_err("pvsched_guest: shared memory not available\n");
		return -ENXIO;
	}

	/* Store the physical parameters in private_data for mmap. */
	filp->private_data = NULL; /* no per-fd state needed */
	return 0;
}

static int pvsched_guest_release(struct inode *inode, struct file *filp)
{
	return 0;
}

/**
 * pvsched_guest_mmap - map the shared memory region into user address space.
 *
 * The userspace process receives a direct window onto the physical pages
 * that are also mapped by the kernel as pvsched_shm.  No copy occurs;
 * both the kernel and userspace see the same data.
 */
static int pvsched_guest_mmap(struct file *filp, struct vm_area_struct *vma)
{
	unsigned long vsize = vma->vm_end - vma->vm_start;
	unsigned long psize = (unsigned long)pvsched_res_size;
	unsigned long pfn;

	if (!pvsched_shm) {
		pr_err("pvsched_guest: mmap called but shared mem not ready\n");
		return -ENXIO;
	}

	/* Only allow mapping the exact size (or smaller). */
	if (vsize > psize) {
		pr_err("pvsched_guest: mmap size 0x%lx exceeds reserved size 0x%lx\n",
		       vsize, psize);
		return -EINVAL;
	}

	/*
	* Keep the default cached (Write-Back) page protection so that the
	* user-space mapping is consistent with the kernel-side memremap(WB)
	* mapping of the same physical pages.
	*
	* Mixing cache attributes on the same physical page (e.g. WB in the
	* kernel and UC/WC in userspace) causes MTRRs/PAT conflicts on x86
	* and is architecturally undefined on most other platforms.
	*
	* If the kernel mapping is ever changed to MEMREMAP_WC, change this
	* line to:  vma->vm_page_prot = pgprot_writecombine(vma->vm_page_prot);
	*/
	vma->vm_page_prot = vm_get_page_prot(vma->vm_flags);

	/* vm_pgoff allows the user to map at an offset inside the region. */
	pfn = (pvsched_res_addr >> PAGE_SHIFT) + vma->vm_pgoff;

	if (remap_pfn_range(vma, vma->vm_start, pfn, vsize,
			    vma->vm_page_prot)) {
		pr_err("pvsched_guest: remap_pfn_range failed\n");
		return -EAGAIN;
	}

	return 0;
}

static const struct file_operations pvsched_guest_fops = {
	.owner = THIS_MODULE,
	.open = pvsched_guest_open,
	.release = pvsched_guest_release,
	.mmap = pvsched_guest_mmap,
};

static struct miscdevice pvsched_guest_miscdev = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = PVSCHED_GUEST_DEV_NAME,
	.fops = &pvsched_guest_fops,
	.mode = 0666,
};

/* --------------------------------------------------------------------------
 * Module init / exit
 * -------------------------------------------------------------------------- */

static int __init pvsched_guest_init(void)
{
	int ret;

	/* Validate that the early_param handler found valid parameters. */
	if (!pvsched_res_addr || !pvsched_res_size) {
		pr_warn("pvsched_guest: no valid pvsched_shared_mem= on cmdline, "
			"driver inactive\n");
		return -ENODEV;
	}

	/* Sanity: size must be large enough to hold the shared memory layout. */
	if (pvsched_res_size < sizeof(struct pvsched_shared_mem)) {
		pr_err("pvsched_guest: reserved size 0x%llx < sizeof(pvsched_shared_mem)=%zu\n",
		       (unsigned long long)pvsched_res_size,
		       sizeof(struct pvsched_shared_mem));
		return -EINVAL;
	}

	/*
	* Map the reserved physical region into kernel virtual address space.
	*
	* memremap() is the correct interface for mapping system RAM that has
	* been reserved via "memmap=<size>$<addr>" on the kernel cmdline.
	* Unlike the ioremap() family (which targets MMIO), memremap() knows
	* the region is ordinary memory and sets up the mapping accordingly.
	*
	* MEMREMAP_WB  - Write-Back cached, normal memory semantics.
	*                Best choice when both Guest and Host access the region
	*                purely via CPU loads/stores (no DMA involved).
	*
	* If the hypervisor interface requires weaker ordering, replace with:
	*   MEMREMAP_WC  - Write-Combine  (relaxed ordering, good for bulk writes)
	*   MEMREMAP_WT  - Write-Through
	*
	* The matching release call is memunmap(), not iounmap().
	*/
	pvsched_shm = (struct pvsched_shared_mem *)memremap(
		pvsched_res_addr, pvsched_res_size, MEMREMAP_WB);
	if (!pvsched_shm) {
		pr_err("pvsched_guest: memremap(0x%llx, 0x%llx, WB) failed\n",
		       (unsigned long long)pvsched_res_addr,
		       (unsigned long long)pvsched_res_size);
		return -ENOMEM;
	}

	pr_info("pvsched_guest: shared mem mapped to virt %p (phys=0x%llx, size=0x%llx)\n",
		pvsched_shm, (unsigned long long)pvsched_res_addr,
		(unsigned long long)pvsched_res_size);

	/* Register the misc character device. */
	ret = misc_register(&pvsched_guest_miscdev);
	if (ret) {
		pr_err("pvsched_guest: misc_register failed (%d)\n", ret);
		memunmap(pvsched_shm);
		pvsched_shm = NULL;
		return ret;
	}

	pr_info("pvsched_guest: /dev/%s registered (minor=%d)\n",
		PVSCHED_GUEST_DEV_NAME, pvsched_guest_miscdev.minor);

	return 0;
}

static void __exit pvsched_guest_exit(void)
{
	misc_deregister(&pvsched_guest_miscdev);

	if (pvsched_shm) {
		memunmap(pvsched_shm);
		pvsched_shm = NULL;
	}

	pr_info("pvsched_guest: unloaded\n");
}

module_init(pvsched_guest_init);
module_exit(pvsched_guest_exit);

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Your Name <your@email>");
MODULE_DESCRIPTION(
	"PV Scheduler guest-side driver: shared memory + /dev/pvsched_guest");
