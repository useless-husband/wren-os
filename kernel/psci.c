/* Power State Coordination Interface: how a guest asks the hypervisor (or
 * firmware) to start CPUs and power off.  QEMU virt and LeapVM both use HVC. */
#include "psci.h"
#include "platform.h"
#include "printk.h"
#include "arch.h"

#define PSCI_CPU_ON_64     0xc4000003u
#define PSCI_SYSTEM_OFF    0x84000008u

static int conduit;

void psci_init(int c) { conduit = c; }

static int64_t psci_call(uint64_t fn, uint64_t a1, uint64_t a2, uint64_t a3)
{
    register uint64_t x0 __asm__("x0") = fn;
    register uint64_t x1 __asm__("x1") = a1;
    register uint64_t x2 __asm__("x2") = a2;
    register uint64_t x3 __asm__("x3") = a3;
    if (conduit == PSCI_HVC)
        __asm__ volatile("hvc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x3) : "memory");
    else if (conduit == PSCI_SMC)
        __asm__ volatile("smc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x3) : "memory");
    else
        return -1;
    return (int64_t)x0;
}

int psci_cpu_on(uint64_t mpidr, paddr_t entry, uint64_t context)
{
    return (int)psci_call(PSCI_CPU_ON_64, mpidr, entry, context);
}

void psci_system_off(void)
{
    psci_call(PSCI_SYSTEM_OFF, 0, 0, 0);
    printk("psci: SYSTEM_OFF failed, halting\n");
    irq_off();
    for (;;) wfi();
}
