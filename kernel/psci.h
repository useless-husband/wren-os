#ifndef KERNEL_PSCI_H
#define KERNEL_PSCI_H
#include "types.h"

void psci_init(int conduit);   /* PSCI_HVC or PSCI_SMC from the device tree */
int  psci_cpu_on(uint64_t mpidr, paddr_t entry, uint64_t context);
NORETURN void psci_system_off(void);

#endif
