/* Boot: kmain() on the CPU the loader started, secondary_main() on the
 * others (started through PSCI CPU_ON). */
#include "types.h"
#include "arch.h"
#include "bio.h"
#include "blk.h"
#include "console.h"
#include "cpu.h"
#include "fdt.h"
#include "file.h"
#include "gic.h"
#include "kalloc.h"
#include "memlayout.h"
#include "platform.h"
#include "printk.h"
#include "proc.h"
#include "psci.h"
#include "stats.h"
#include "vm.h"

struct cpu cpus[NCPU];
int ncpu_online;
uint64_t kimage_voffset;
uint64_t kstats[NSTATS];
struct platform plat;
static struct fdt boot_fdt;

extern char _start_image[], _end[], secondary_entry[];

static void uart_irq(void *arg)
{
    (void)arg;
    uart_intr();
}

static void start_secondaries(void)
{
    int n = 1;
    for (int i = 0; i < plat.ncpu && n < NCPU; i++) {
        uint64_t mpidr = plat.cpu_mpidr[i];
        if (mpidr == cpus[0].mpidr) continue;
        struct cpu *c = &cpus[n];
        c->id = n;
        c->mpidr = mpidr;
        int r = psci_cpu_on(mpidr, V2P(secondary_entry), (uint64_t)n);
        if (r != 0) {
            printk("cpu%d (mpidr %lx): CPU_ON failed (%d)\n", n, mpidr, r);
            continue;
        }
        uint64_t deadline = timer_count() + timer_freq();          /* 1 second */
        while (!__atomic_load_n(&c->online, __ATOMIC_ACQUIRE) && timer_count() < deadline) cpu_relax();
        if (!c->online) {
            printk("cpu%d (mpidr %lx): did not come up\n", n, mpidr);
            continue;
        }
        ncpu_online = ++n;
    }
}

void kmain(paddr_t dtb_pa, paddr_t load_pa)
{
    cpus[0].id = 0;
    cpus[0].mpidr = read_sysreg(mpidr_el1) & 0xff00ffffffUL;
    write_sysreg(tpidr_el1, &cpus[0]);
    ncpu_online = 1;

    const void *dtb = P2V(dtb_pa);
    uint32_t dtb_size = fdt_totalsize(dtb);
    char err[96];
    if (dtb_size == 0 || dtb_size > (2u << 20) || fdt_parse(&boot_fdt, dtb, dtb_size) != FDT_OK ||
        platform_from_fdt(&boot_fdt, &plat, err, sizeof err) != 0)
        for (;;) wfi();          /* no usable device tree: we cannot even find the UART */
    uart_init(plat.uart_base);
    printk("\nwren-os: booting on \"%s\", image at %p, %d cpu(s) in the device tree\n", plat.model,
           (void *)load_pa, plat.ncpu);
    psci_init(plat.psci);

    paddr_t img_end = load_pa + (uint64_t)(_end - _start_image);
    kalloc_init(&plat, &boot_fdt, load_pa, img_end, dtb_pa, dtb_size);
    kvm_init(&plat, load_pa);
    kvm_install();
    printk("memory: %lu MiB free of %lu MiB, kernel %lu KiB\n", pages_free_count() >> 8,
           (plat.mem_size[0] >> 20), (img_end - load_pa) >> 10);

    gic_init(&plat);
    if (gic_cpu_init() != 0) panic("no GIC redistributor for cpu0");
    console_init();
    gic_enable(plat.uart_intid, uart_irq, NULL);
    file_init();
    bio_init();
    proc_init();
    if (blk_init(&plat) != 0) panic("no virtio-blk disk found (QEMU: -drive ... -device virtio-blk-device)");
    user_init();

    start_secondaries();
    timer_cpu_init();
    printk("smp: %d cpu(s) online\n", ncpu_online);
    scheduler();
}

void secondary_main(int id)
{
    struct cpu *c = &cpus[id];
    write_sysreg(tpidr_el1, c);
    kvm_install();
    if (gic_cpu_init() != 0) {
        printk("cpu%d: no GIC redistributor\n", id);
        for (;;) wfi();
    }
    timer_cpu_init();
    __atomic_store_n(&c->online, true, __ATOMIC_RELEASE);
    scheduler();
}
