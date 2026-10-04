/* Everything the kernel needs to know about the machine, discovered from
 * the device tree.  Nothing about either QEMU virt or LeapVM is hard-coded. */
#ifndef KERNEL_PLATFORM_H
#define KERNEL_PLATFORM_H
#include <stdint.h>
#include "fdt.h"

#define PLAT_MAX_MEM    4
#define PLAT_MAX_CPUS   8
#define PLAT_MAX_VIRTIO 32

enum { PSCI_NONE = 0, PSCI_HVC = 1, PSCI_SMC = 2 };

struct platform {
    char     model[48];
    int      nmem;
    uint64_t mem_base[PLAT_MAX_MEM], mem_size[PLAT_MAX_MEM];
    int      ncpu;
    uint64_t cpu_mpidr[PLAT_MAX_CPUS];
    int      psci;
    uint64_t gicd_base, gicd_size, gicr_base, gicr_size;
    uint32_t vtimer_intid;          /* virtual timer PPI as a GIC INTID */
    uint64_t uart_base;
    uint32_t uart_intid;
    uint64_t rtc_base;              /* PL031, 0 if absent */
    int      nvirtio;
    uint64_t virtio_base[PLAT_MAX_VIRTIO];
    uint32_t virtio_intid[PLAT_MAX_VIRTIO];
    char     bootargs[256];
};

/* Fills *pf.  Returns 0, or -1 with a reason in err when something the
 * kernel cannot run without (memory, GICv3, timer, UART) is missing. */
int platform_from_fdt(const struct fdt *f, struct platform *pf, char *err, unsigned errlen);

#endif
