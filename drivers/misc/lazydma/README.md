# Lazy DMA Driver

## Overview

The Lazy DMA driver provides DMA tracking and VM-exit based page management for virtualized environments. It tracks DMA mappings at PMD (2MB page) granularity and triggers VM-exits to coordinate with the hypervisor.

## Features

- **DMA Tracking**: Monitors DMA map/unmap operations at 2MB page granularity
- **VM-Exit Triggers**: Automatically triggers VM-exits when new pages are mapped
- **Atomic Operations**: Thread-safe tracking using atomic counters
- **Present Bit Checking**: Validates that host has processed VM-exits
- **Hook System**: Supports per-device DMA operation hooking

## Architecture

```
Guest Kernel                        Host/Hypervisor
    |                                      |
    |-- DMA map_page()                     |
    |   |-- track_dma_map()                |
    |       |-- mapped_count++             |
    |       |-- writel(pmd_index, mmio) -->|-- VM-exit handler
    |       |                              |   |-- Process PMD
    |       |                              |   |-- Set present bit
    |       |<-- VM-entry -------------------  |
    |       |-- Check present bit          |
    |                                      |
```

## Memory Layout

### Tracking Memory
- **Purpose**: Store DMA tracking entries
- **Format**: Array of 4-byte entries (one per PMD/2MB)
- **Entry Structure**:
  ```c
  struct dma_tracking_entry {
      unsigned int mapped_count : 31;  // Number of active mappings
      unsigned int present : 1;         // Set by host after processing
  };
  ```

### MMIO Memory
- **Purpose**: Trigger VM-exits
- **Usage**: Write PMD index to trigger VM-exit
- **Size**: Typically 4KB (one page)

## Kernel Configuration

### Build Configuration

1. Enable in kernel config:
   ```
   Device Drivers --->
       Misc devices --->
           <M> Lazy DMA tracking and VM-exit support
   ```

2. Or add to `.config`:
   ```
   CONFIG_LAZYDMA=m
   ```

### Kernel Command Line Parameters

Required parameters:

```bash
lazydma_track=<phys_addr>,<size>   # Tracking memory region
lazydma_mmio=<phys_addr>,<size>    # MMIO region for VM-exits
```

Example:
```bash
lazydma_track=0x100000000,0x1000000 lazydma_mmio=0x101000000,0x1000
```

This configures:
- 16MB tracking memory at 4GB (covers 8TB of physical memory)
- 4KB MMIO region at 4GB + 16MB

## Usage

### Loading the Module

```bash
modprobe lazydma
```

### Hooking a Device

From another kernel module:

```c
#include <linux/lazydma.h>
#include <linux/pci.h>

static int my_driver_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
    int ret;

    /* Hook DMA operations for this device */
    ret = lazydma_hook_device(&pdev->dev);
    if (ret) {
        dev_err(&pdev->dev, "Failed to hook lazydma: %d\n", ret);
        return ret;
    }

    /* ... rest of driver initialization ... */

    return 0;
}

static void my_driver_remove(struct pci_dev *pdev)
{
    /* Restore original DMA operations */
    lazydma_unhook_device(&pdev->dev);

    /* ... rest of driver cleanup ... */
}
```

### Device Node

The driver creates `/dev/lazydma` which can be used for:
- Runtime configuration (via ioctl - to be implemented)
- Debugging and monitoring

## Host/Hypervisor Integration

### VM-Exit Handler

The hypervisor must implement a VM-exit handler for MMIO writes:

```c
void handle_lazydma_vmexit(unsigned long pmd_index)
{
    /* 1. Pin the corresponding 2MB page */
    pin_guest_page(pmd_index << PMD_SHIFT, PMD_SIZE);

    /* 2. Update EPT/NPT mappings if needed */
    update_ept_mapping(pmd_index);

    /* 3. Set present bit in tracking entry */
    guest_tracking_entry[pmd_index].present = 1;

    /* 4. Resume guest */
}
```

### Memory Reservation

Reserve physical memory regions for tracking and MMIO:

```
# In QEMU command line or VM configuration
-object memory-backend-ram,id=lazydma_track,size=16M,host-nodes=0,policy=bind
-object memory-backend-ram,id=lazydma_mmio,size=4K,host-nodes=0,policy=bind
```

## Performance Considerations

### Overhead

- **First mapping**: VM-exit overhead (~1000-10000 cycles)
- **Subsequent mappings**: Only atomic increment (~10-50 cycles)
- **Unmapping**: Only atomic decrement (~10-50 cycles)

### Optimization Tips

1. **Use large pages**: Already optimized for 2MB pages
2. **Batch operations**: DMA operations on same PMD are cheap
3. **Pre-fault**: Consider pre-faulting frequently used memory

## Debugging

### Enable Debug Output

```bash
echo 8 > /proc/sys/kernel/printk  # Enable debug messages
dmesg | grep lazydma
```

### Check Device Status

```bash
cat /sys/kernel/debug/lazydma/stats  # (if debugfs support added)
```

### Common Issues

1. **Module fails to load**: Check kernel parameters are set
2. **VM-exit timeout**: Host handler may be slow or not implemented
3. **Wrong mappings**: Check memory regions don't overlap

## Example Use Cases

### 1. Live Migration with DMA

Track which pages are actively used by DMA devices to optimize live migration.

### 2. Memory Overcommitment

Allow hypervisor to swap out pages not actively used by DMA.

### 3. Security Isolation

Ensure DMA-capable devices only access pinned memory regions.

## Future Enhancements

- [ ] Add ioctl interface for runtime control
- [ ] Support for different page sizes (4KB, 1GB)
- [ ] Statistics and monitoring via debugfs
- [ ] Integration with VFIO for device pass-through
- [ ] Support for IOMMU-based tracking

## References

- Linux DMA API: Documentation/core-api/dma-api.rst
- KVM Virtualization: Documentation/virt/kvm/
- Intel VT-d Specification
- AMD-Vi Specification

## License

GPL-2.0

## Authors

Your Name <your.email@example.com>
