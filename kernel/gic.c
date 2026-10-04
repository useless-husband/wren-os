#include "gic.h"
#include "arch.h"
#include "cpu.h"
#include "memlayout.h"
#include "platform.h"
#include "printk.h"
#include "proc.h"
#include "spinlock.h"

/* Distributor */
#define GICD_CTLR       0x0000
#define GICD_TYPER      0x0004
#define GICD_IGROUPR    0x0080
#define GICD_ISENABLER  0x0100
#define GICD_ICENABLER  0x0180
#define GICD_ICPENDR    0x0280
#define GICD_IPRIORITYR 0x0400
#define GICD_ICFGR      0x0c00
#define GICD_IROUTER    0x6000
#define CTLR_RWP        (1u << 31)
#define CTLR_ARE        (1u << 4)
#define CTLR_GRP1       (1u << 1)

/* Redistributor: RD frame, then SGI frame 64 KiB later */
#define GICR_CTLR       0x0000
#define GICR_TYPER      0x0008
#define GICR_WAKER      0x0014
#define GICR_SGI        0x10000
#define GICR_IGROUPR0   (GICR_SGI + 0x0080)
#define GICR_ISENABLER0 (GICR_SGI + 0x0100)
#define GICR_ICENABLER0 (GICR_SGI + 0x0180)
#define GICR_IPRIORITYR (GICR_SGI + 0x0400)
#define GICR_ICFGR1     (GICR_SGI + 0x0c04)
#define WAKER_SLEEP     (1u << 1)
#define WAKER_ASLEEP    (1u << 2)

#define MAX_INTID 1020
#define PRIO_DEFAULT 0xa0

static void timer_tick(void *arg);
static uint64_t gicd, gicr_base, gicr_size;
static uint32_t nlines;
static struct { irq_handler_t fn; void *arg; } handlers[MAX_INTID];
static uint32_t vtimer_intid;

static void dist_wait(void)
{
    while (mmio_read32(gicd + GICD_CTLR) & CTLR_RWP) cpu_relax();
}

void gic_init(const struct platform *pf)
{
    gicd = (uint64_t)P2V(pf->gicd_base);
    gicr_base = (uint64_t)P2V(pf->gicr_base);
    gicr_size = pf->gicr_size;
    vtimer_intid = pf->vtimer_intid;
    if (vtimer_intid >= 32) panic("timer interrupt %u is not a PPI", vtimer_intid);
    handlers[vtimer_intid].fn = timer_tick;   /* before any CPU starts its timer */
    nlines = MIN(32 * ((mmio_read32(gicd + GICD_TYPER) & 0x1f) + 1), MAX_INTID);

    mmio_write32(gicd + GICD_CTLR, 0);
    dist_wait();
    for (uint32_t i = 32; i < nlines; i += 32) {
        mmio_write32(gicd + GICD_ICENABLER + i / 8, 0xffffffff);
        mmio_write32(gicd + GICD_ICPENDR + i / 8, 0xffffffff);
        mmio_write32(gicd + GICD_IGROUPR + i / 8, 0xffffffff);      /* all non-secure group 1 */
    }
    for (uint32_t i = 32; i < nlines; i += 4)
        mmio_write32(gicd + GICD_IPRIORITYR + i, PRIO_DEFAULT * 0x01010101u);
    for (uint32_t i = 32; i < nlines; i += 16)
        mmio_write32(gicd + GICD_ICFGR + i / 4, 0);                 /* level-triggered */
    dist_wait();
    mmio_write32(gicd + GICD_CTLR, CTLR_ARE | CTLR_GRP1);
    dist_wait();
}

/* Find this CPU's redistributor by matching GICR_TYPER's affinity. */
static uint64_t find_redist(uint64_t mpidr)
{
    uint32_t aff = (uint32_t)((mpidr & 0xffffff) | ((mpidr >> 32 & 0xff) << 24));
    for (uint64_t off = 0; off + 0x20000 <= gicr_size;) {
        uint64_t rd = gicr_base + off;
        uint64_t typer = mmio_read64(rd + GICR_TYPER);
        if ((uint32_t)(typer >> 32) == aff) return rd;
        if (typer & (1u << 4)) break;                        /* Last */
        off += (typer & (1u << 1)) ? 0x40000 : 0x20000;     /* VLPIS frames make it 256 KiB */
    }
    return 0;
}

int gic_cpu_init(void)
{
    struct cpu *c = mycpu();
    uint64_t rd = find_redist(c->mpidr);
    if (!rd) return -1;
    c->gicr_rd = rd;
    mmio_write32(rd + GICR_WAKER, mmio_read32(rd + GICR_WAKER) & ~WAKER_SLEEP);
    while (mmio_read32(rd + GICR_WAKER) & WAKER_ASLEEP) cpu_relax();

    mmio_write32(rd + GICR_ICENABLER0, 0xffffffff);
    mmio_write32(rd + GICR_IGROUPR0, 0xffffffff);
    for (int i = 0; i < 32; i += 4) mmio_write32(rd + GICR_IPRIORITYR + i, PRIO_DEFAULT * 0x01010101u);
    mmio_write32(rd + GICR_ISENABLER0, (1u << SGI_WAKEUP) | (1u << SGI_STOP) | (1u << vtimer_intid));

    write_icc(ICC_SRE_EL1, read_icc(ICC_SRE_EL1) | 1);   /* system-register interface */
    isb();
    write_icc(ICC_PMR_EL1, 0xff);
    write_icc(ICC_BPR1_EL1, 0);
    write_icc(ICC_CTLR_EL1, 0);                           /* EOI drops priority and deactivates */
    write_icc(ICC_IGRPEN1_EL1, 1);
    isb();
    return 0;
}

void gic_enable(uint32_t intid, irq_handler_t h, void *arg)
{
    if (intid >= MAX_INTID) panic("gic_enable: intid %u", intid);
    handlers[intid].fn = h;
    handlers[intid].arg = arg;
    if (intid < 32) return;   /* private interrupts are enabled per CPU */
    mmio_write64(gicd + GICD_IROUTER + 8 * intid, cpus[0].mpidr & 0xff00ffffffUL);
    mmio_write32(gicd + GICD_ISENABLER + (intid / 32) * 4, 1u << (intid % 32));
}

static void stop_this_cpu(void *arg)
{
    (void)arg;
    irq_off();
    for (;;) wfi();
}

void stop_other_cpus(void)
{
    if (!gicd) return;
    for (int i = 0; i < ncpu_online; i++)
        if (mycpu() && i != mycpu()->id) gic_send_sgi(i, SGI_STOP);
}

void gic_send_sgi(int cpu, int sgi)
{
    uint64_t m = cpus[cpu].mpidr;
    uint64_t v = ((uint64_t)sgi << 24) | (1u << (m & 0xf)) | (((m >> 8) & 0xff) << 16) |
                 (((m >> 16) & 0xff) << 32) | (((m >> 32) & 0xff) << 48) | (((m & 0xff) >> 4) << 44);
    dsb_ishst();
    write_icc(ICC_SGI1R_EL1, v);
    isb();
}

/* ---- virtual timer ---- */

volatile uint64_t ticks;
struct spinlock tick_lock = SPINLOCK_INIT("ticks");
static uint64_t tick_interval;

static void timer_tick(void *arg)
{
    (void)arg;
    write_sysreg(cntv_tval_el0, tick_interval);
    struct cpu *c = mycpu();
    c->ticks++;
    if (c->id == 0) {
        spin_lock(&tick_lock);
        ticks++;
        proc_wakeup((void *)&ticks);
        spin_unlock(&tick_lock);
    }
    if (c->proc) proc_tick();
}

void timer_cpu_init(void)
{
    tick_interval = timer_freq() / HZ;
    write_sysreg(cntkctl_el1, 1 << 1);                /* EL0VCTEN: user code may read the counter */
    write_sysreg(cntv_tval_el0, tick_interval);
    write_sysreg(cntv_ctl_el0, 1);                    /* enable, unmasked */
    isb();
}

uint64_t uptime_ms(void)
{
    return timer_count() * 1000 / timer_freq();
}

void gic_handle_irq(void)
{
    for (;;) {
        uint64_t iar = read_icc(ICC_IAR1_EL1);
        uint32_t intid = iar & 0xffffff;
        if (intid >= MAX_INTID) return;              /* 1023: spurious, nothing pending */
        if (intid == SGI_STOP) stop_this_cpu(NULL);
        if (handlers[intid].fn) handlers[intid].fn(handlers[intid].arg);
        write_icc(ICC_EOIR1_EL1, iar);
    }
}
