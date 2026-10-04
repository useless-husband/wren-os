#include "platform.h"
#include "../lib/kstring.h"
#include "../lib/fmt.h"

/* Decode the index-th interrupt of a node as a GIC INTID.  GIC bindings use
 * 3 cells: <type number flags>, type 0 = SPI (INTID 32+n), 1 = PPI (16+n). */
static bool irq_intid(const struct fdt *f, int node, int index, uint32_t *intid)
{
    const struct fdt_prop *p = fdt_getprop(f, node, "interrupts");
    if (!p || (uint64_t)(index + 1) * 12 > p->len) return false;
    const uint8_t *d = p->data + 12 * index;
    uint32_t type = fdt_be32(d), num = fdt_be32(d + 4);
    if (type == 0 && num < 988) *intid = 32 + num;
    else if (type == 1 && num < 16) *intid = 16 + num;
    else return false;
    return true;
}

static int fail(char *err, unsigned errlen, const char *what)
{
    snformat(err, errlen, "device tree: %s", what);
    return -1;
}

static int find_uart(const struct fdt *f)
{
    int chosen = fdt_path(f, "/chosen");
    const struct fdt_prop *sp = fdt_getprop(f, chosen, "stdout-path");
    if (sp && sp->len > 1 && sp->len < 128 && sp->data[sp->len - 1] == 0) {
        char path[128];
        memcpy(path, sp->data, sp->len);
        char *colon = strchr(path, ':');     /* "/pl011@9000000:115200n8" */
        if (colon) *colon = 0;
        int n = fdt_path(f, path);
        if (n >= 0 && fdt_compatible(f, n, "arm,pl011")) return n;
    }
    return fdt_next_compatible(f, -1, "arm,pl011");
}

int platform_from_fdt(const struct fdt *f, struct platform *pf, char *err, unsigned errlen)
{
    memset(pf, 0, sizeof *pf);
    const char *model = fdt_prop_str(f, 0, "model");
    strlcpy(pf->model, model ? model : "unknown", sizeof pf->model);

    for (int i = 1; i < f->nnodes; i++) {
        const char *dt = fdt_prop_str(f, i, "device_type");
        if (!dt || strcmp(dt, "memory") != 0) continue;
        uint64_t a, s;
        for (int r = 0; fdt_reg(f, i, r, &a, &s) && pf->nmem < PLAT_MAX_MEM; r++) {
            if (s == 0) continue;
            pf->mem_base[pf->nmem] = a;
            pf->mem_size[pf->nmem] = s;
            pf->nmem++;
        }
    }
    if (pf->nmem == 0) return fail(err, errlen, "no memory node");

    int cpus = fdt_path(f, "/cpus");
    if (cpus < 0) return fail(err, errlen, "no /cpus");
    for (int c = fdt_next_child(f, cpus, -1); c >= 0; c = fdt_next_child(f, cpus, c)) {
        const char *dt = fdt_prop_str(f, c, "device_type");
        uint64_t mpidr, unused;
        if (!dt || strcmp(dt, "cpu") != 0 || !fdt_reg(f, c, 0, &mpidr, &unused)) continue;
        if (pf->ncpu < PLAT_MAX_CPUS) pf->cpu_mpidr[pf->ncpu++] = mpidr;
    }
    if (pf->ncpu == 0) return fail(err, errlen, "no cpu nodes");

    int psci = fdt_next_compatible(f, -1, "arm,psci-1.0");
    if (psci < 0) psci = fdt_next_compatible(f, -1, "arm,psci-0.2");
    if (psci >= 0) {
        const char *m = fdt_prop_str(f, psci, "method");
        if (m && strcmp(m, "hvc") == 0) pf->psci = PSCI_HVC;
        else if (m && strcmp(m, "smc") == 0) pf->psci = PSCI_SMC;
    }

    int gic = fdt_next_compatible(f, -1, "arm,gic-v3");
    if (gic < 0) return fail(err, errlen, "no GICv3 (QEMU needs -machine gic-version=3)");
    if (!fdt_reg(f, gic, 0, &pf->gicd_base, &pf->gicd_size) ||
        !fdt_reg(f, gic, 1, &pf->gicr_base, &pf->gicr_size))
        return fail(err, errlen, "GICv3 node without distributor/redistributor regions");

    int timer = fdt_next_compatible(f, -1, "arm,armv8-timer");
    if (timer < 0 || !irq_intid(f, timer, 2, &pf->vtimer_intid))   /* order: sec, phys, virt, hyp */
        return fail(err, errlen, "no architected timer");

    int uart = find_uart(f);
    if (uart < 0 || !fdt_reg(f, uart, 0, &pf->uart_base, &(uint64_t){0}) ||
        !irq_intid(f, uart, 0, &pf->uart_intid))
        return fail(err, errlen, "no PL011 UART");

    int rtc = fdt_next_compatible(f, -1, "arm,pl031");
    uint64_t rs;
    if (rtc < 0 || !fdt_reg(f, rtc, 0, &pf->rtc_base, &rs)) pf->rtc_base = 0;

    for (int v = fdt_next_compatible(f, -1, "virtio,mmio"); v >= 0 && pf->nvirtio < PLAT_MAX_VIRTIO;
         v = fdt_next_compatible(f, v, "virtio,mmio")) {
        uint64_t base, size;
        uint32_t intid;
        if (!fdt_reg(f, v, 0, &base, &size) || !irq_intid(f, v, 0, &intid)) continue;
        pf->virtio_base[pf->nvirtio] = base;
        pf->virtio_intid[pf->nvirtio] = intid;
        pf->nvirtio++;
    }

    const char *args = fdt_prop_str(f, fdt_path(f, "/chosen"), "bootargs");
    strlcpy(pf->bootargs, args ? args : "", sizeof pf->bootargs);
    return 0;
}
