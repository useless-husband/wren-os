#include "types.h"
#include "arch.h"
#include "cpu.h"
#include "fdt.h"
#include "platform.h"
#include "printk.h"
#include "memlayout.h"

struct cpu cpus[NCPU];
int ncpu_online;
uint64_t kimage_voffset;
struct platform plat;
static struct fdt boot_fdt;

void kmain(paddr_t dtb_pa, paddr_t load_pa)
{
    cpus[0].id = 0;
    cpus[0].mpidr = read_sysreg(mpidr_el1) & 0xff00ffffffUL;
    write_sysreg(tpidr_el1, &cpus[0]);

    const void *dtb = P2V(dtb_pa);
    uint32_t dtb_size = fdt_totalsize(dtb);
    char err[96];
    int r = fdt_parse(&boot_fdt, dtb, dtb_size ? dtb_size : 64);
    if (r != FDT_OK || platform_from_fdt(&boot_fdt, &plat, err, sizeof err) != 0)
        for (;;) wfi();          /* no UART address known: nothing we can print */
    uart_init(plat.uart_base);
    printk("\nwren-os booting on \"%s\" at EL%lu, image at %p\n", plat.model,
           (read_sysreg(CurrentEL) >> 2) & 3, (void *)load_pa);
    for (int i = 0; i < plat.nmem; i++)
        printk("  memory %p-%p\n", (void *)plat.mem_base[i], (void *)(plat.mem_base[i] + plat.mem_size[i]));
    printk("  %d cpus, psci %s, gicd %p gicr %p, vtimer intid %u\n", plat.ncpu,
           plat.psci == PSCI_HVC ? "hvc" : plat.psci == PSCI_SMC ? "smc" : "none",
           (void *)plat.gicd_base, (void *)plat.gicr_base, plat.vtimer_intid);
    printk("  uart %p intid %u, %d virtio-mmio slots, bootargs \"%s\"\n", (void *)plat.uart_base,
           plat.uart_intid, plat.nvirtio, plat.bootargs);
    panic("stage 1 done");
}

void secondary_main(int id) { (void)id; for (;;) wfi(); }
