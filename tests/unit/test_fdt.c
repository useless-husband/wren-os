/* kernel/fdt.c and kernel/platform.c on the host.
 *
 *   test_fdt [qemu.dtb]
 *
 * 1. A tree written by a small FDT writer below that mirrors exactly what
 *    LeapVM's src/fdt.c emits; platform_from_fdt must find LeapVM's devices.
 * 2. If given, the DTB QEMU itself generates for `virt,gic-version=3`
 *    (made at test time with -machine dumpdtb=...); must find QEMU's devices.
 * 3. Malformed blobs: specific corruptions must be rejected with the right
 *    error, and 20000 random mutations must never crash the parser (the
 *    test is built with AddressSanitizer and UBSan).
 */
#include "check.h"
#include <stdint.h>
#include <string.h>
#include "../../kernel/fdt.h"
#include "../../kernel/platform.h"

/* ---- a tiny FDT writer ---- */
static uint8_t st[16384], strs[2048], blob[32768];
static size_t st_len, strs_len;

static void put32(uint32_t v)
{
    st[st_len++] = (uint8_t)(v >> 24);
    st[st_len++] = (uint8_t)(v >> 16);
    st[st_len++] = (uint8_t)(v >> 8);
    st[st_len++] = (uint8_t)v;
}

static void put_bytes(const void *p, size_t n)
{
    if (n) memcpy(st + st_len, p, n);   /* empty properties pass p == NULL */
    st_len += n;
    while (st_len & 3) st[st_len++] = 0;
}

static uint32_t str_off(const char *s)
{
    for (size_t i = 0; i < strs_len; i += strlen((char *)strs + i) + 1)
        if (!strcmp((char *)strs + i, s)) return (uint32_t)i;
    size_t n = strlen(s) + 1;
    memcpy(strs + strs_len, s, n);
    strs_len += n;
    return (uint32_t)(strs_len - n);
}

static void begin(const char *name) { put32(1); put_bytes(name, strlen(name) + 1); }
static void end(void) { put32(2); }
static void prop(const char *name, const void *d, size_t n) { put32(3); put32((uint32_t)n); put32(str_off(name)); put_bytes(d, n); }
static void prop_str(const char *name, const char *s) { prop(name, s, strlen(s) + 1); }
static void prop_cells(const char *name, int n, const uint32_t *c)
{
    uint8_t b[64];
    for (int i = 0; i < n; i++) {
        b[4 * i] = (uint8_t)(c[i] >> 24);
        b[4 * i + 1] = (uint8_t)(c[i] >> 16);
        b[4 * i + 2] = (uint8_t)(c[i] >> 8);
        b[4 * i + 3] = (uint8_t)c[i];
    }
    prop(name, b, (size_t)n * 4);
}
#define CELLS(name, ...) do { uint32_t c_[] = {__VA_ARGS__}; prop_cells(name, sizeof c_ / 4, c_); } while (0)

static void be32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }

static size_t finish(void)
{
    put32(9);
    uint32_t off_rsv = 40, off_st = off_rsv + 16, off_str = off_st + (uint32_t)st_len;
    uint32_t total = off_str + (uint32_t)strs_len;
    memset(blob, 0, sizeof blob);
    uint32_t hdr[10] = {0xd00dfeed, total, off_st, off_str, off_rsv, 17, 16, 0, (uint32_t)strs_len, (uint32_t)st_len};
    for (int i = 0; i < 10; i++) be32(blob + 4 * i, hdr[i]);
    memcpy(blob + off_st, st, st_len);
    memcpy(blob + off_str, strs, strs_len);
    return total;
}

/* The same nodes and properties as LeapVM's build_fdt() with 4 CPUs,
 * 256 MiB, a disk and no network. */
static size_t leapvm_like_dtb(void)
{
    st_len = strs_len = 0;
    begin("");
    prop_str("compatible", "linux,dummy-virt");
    prop_str("model", "LeapVM");
    CELLS("#address-cells", 2);
    CELLS("#size-cells", 2);
    CELLS("interrupt-parent", 1);
    begin("chosen");
    prop_str("bootargs", "console=ttyAMA0");
    prop_str("stdout-path", "/pl011@c000000");
    end();
    begin("memory@40000000");
    prop_str("device_type", "memory");
    CELLS("reg", 0, 0x40000000, 0, 0x10000000);
    end();
    begin("cpus");
    CELLS("#address-cells", 1);
    CELLS("#size-cells", 0);
    for (int i = 0; i < 4; i++) {
        char name[16];
        snprintf(name, sizeof name, "cpu@%d", i);
        begin(name);
        prop_str("device_type", "cpu");
        prop_str("compatible", "arm,arm-v8");
        CELLS("reg", (uint32_t)i);
        prop_str("enable-method", "psci");
        end();
    }
    end();
    begin("psci");
    prop("compatible", "arm,psci-1.0\0arm,psci-0.2", sizeof "arm,psci-1.0\0arm,psci-0.2");
    prop_str("method", "hvc");
    end();
    begin("intc@8000000");
    prop_str("compatible", "arm,gic-v3");
    CELLS("#interrupt-cells", 3);
    prop("interrupt-controller", NULL, 0);
    CELLS("reg", 0, 0x08000000, 0, 0x10000, 0, 0x0a000000, 0, 0x2000000);
    CELLS("phandle", 1);
    end();
    begin("timer");
    prop_str("compatible", "arm,armv8-timer");
    CELLS("interrupts", 1, 13, 4, 1, 14, 4, 1, 11, 4, 1, 10, 4);
    end();
    begin("pl011@c000000");
    prop("compatible", "arm,pl011\0arm,primecell", sizeof "arm,pl011\0arm,primecell");
    CELLS("reg", 0, 0x0c000000, 0, 0x1000);
    CELLS("interrupts", 0, 1, 4);
    end();
    begin("rtc@c010000");
    prop("compatible", "arm,pl031\0arm,primecell", sizeof "arm,pl031\0arm,primecell");
    CELLS("reg", 0, 0x0c010000, 0, 0x1000);
    CELLS("interrupts", 0, 2, 4);
    end();
    begin("virtio_mmio@d000000");
    prop_str("compatible", "virtio,mmio");
    CELLS("reg", 0, 0x0d000000, 0, 0x200);
    CELLS("interrupts", 0, 16, 4);
    prop("dma-coherent", NULL, 0);
    end();
    end();
    return finish();
}

static struct fdt f;
static struct platform pf;

static void test_leapvm(void)
{
    size_t n = leapvm_like_dtb();
    CHECK_EQ(fdt_parse(&f, blob, n), FDT_OK);
    char err[96];
    CHECK_EQ(platform_from_fdt(&f, &pf, err, sizeof err), 0);
    CHECK(!strcmp(pf.model, "LeapVM"));
    CHECK_EQ(pf.nmem, 1);
    CHECK_EQ(pf.mem_base[0], 0x40000000);
    CHECK_EQ(pf.mem_size[0], 0x10000000);
    CHECK_EQ(pf.ncpu, 4);
    CHECK_EQ(pf.cpu_mpidr[3], 3);
    CHECK_EQ(pf.psci, PSCI_HVC);
    CHECK_EQ(pf.gicd_base, 0x08000000);
    CHECK_EQ(pf.gicr_base, 0x0a000000);
    CHECK_EQ(pf.gicr_size, 0x2000000);
    CHECK_EQ(pf.vtimer_intid, 27);
    CHECK_EQ(pf.uart_base, 0x0c000000);
    CHECK_EQ(pf.uart_intid, 33);
    CHECK_EQ(pf.rtc_base, 0x0c010000);
    CHECK_EQ(pf.nvirtio, 1);
    CHECK_EQ(pf.virtio_base[0], 0x0d000000);
    CHECK_EQ(pf.virtio_intid[0], 48);
    CHECK(!strcmp(pf.bootargs, "console=ttyAMA0"));
    CHECK_EQ(fdt_path(&f, "/cpus/cpu@2") >= 0, 1);
    CHECK_EQ(fdt_path(&f, "/cpus/cpu@9"), -1);
    CHECK_EQ(fdt_path(&f, "/chosen/nope"), -1);
    CHECK_EQ(fdt_path(&f, "/psci") >= 0, 1);
}

static void test_qemu(const char *path)
{
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        printf("test_fdt: no QEMU DTB at %s, skipping that part\n", path);
        return;
    }
    static uint8_t q[1 << 20];
    size_t n = fread(q, 1, sizeof q, fp);
    fclose(fp);
    CHECK_EQ(fdt_parse(&f, q, n), FDT_OK);
    char err[96] = "";
    CHECK_EQ(platform_from_fdt(&f, &pf, err, sizeof err), 0);
    if (err[0]) fprintf(stderr, "%s\n", err);
    CHECK_EQ(pf.mem_base[0], 0x40000000);
    CHECK_EQ(pf.mem_size[0], 256u << 20);
    CHECK_EQ(pf.ncpu, 4);
    CHECK_EQ(pf.psci, PSCI_HVC);
    CHECK_EQ(pf.gicd_base, 0x08000000);
    CHECK_EQ(pf.gicr_base, 0x080a0000);
    CHECK_EQ(pf.uart_base, 0x09000000);
    CHECK_EQ(pf.uart_intid, 33);
    CHECK_EQ(pf.vtimer_intid, 27);
    CHECK_EQ(pf.nvirtio, 32);
    CHECK_EQ(pf.rtc_base, 0x09010000);
    printf("test_fdt: QEMU virt DTB: %d nodes, %d properties\n", f.nnodes, f.nprops);
}

static void test_malformed(void)
{
    size_t n = leapvm_like_dtb();
    static uint8_t b[32768];
    memcpy(b, blob, n);
    b[0] ^= 1;
    CHECK_EQ(fdt_parse(&f, b, n), FDT_ERR_MAGIC);
    memcpy(b, blob, n);
    CHECK_EQ(fdt_parse(&f, b, n - 1), FDT_ERR_HEADER);          /* totalsize beyond buffer */
    CHECK_EQ(fdt_parse(&f, b, 20), FDT_ERR_HEADER);
    memcpy(b, blob, n);
    be32(b + 20, 15);
    CHECK_EQ(fdt_parse(&f, b, n), FDT_ERR_VERSION);
    memcpy(b, blob, n);
    be32(b + 36, 0x7ffffff0);                                    /* size_dt_struct huge */
    CHECK_EQ(fdt_parse(&f, b, n), FDT_ERR_HEADER);
    memcpy(b, blob, n);
    be32(b + 12, 0xfffffff0);                                    /* strings offset wraps */
    CHECK_EQ(fdt_parse(&f, b, n), FDT_ERR_HEADER);
    /* unbalanced: drop the final END_NODE by turning it into a NOP */
    memcpy(b, blob, n);
    uint32_t off_st = 56, size_st = (uint32_t)st_len;
    be32(b + off_st + size_st - 8, 4);
    CHECK_EQ(fdt_parse(&f, b, n), FDT_ERR_STRUCTURE);
    /* a property name offset outside the strings block */
    memcpy(b, blob, n);
    for (uint32_t o = off_st; o + 12 < off_st + size_st; o += 4)
        if (b[o] == 0 && b[o + 1] == 0 && b[o + 2] == 0 && b[o + 3] == 3) {
            be32(b + o + 8, 0x10000);
            break;
        }
    CHECK_EQ(fdt_parse(&f, b, n), FDT_ERR_TRUNCATED);

    /* fuzz: random bit flips, byte overwrites and truncations */
    unsigned long long seed = 0x5eedf00dull;
    rng_state = seed;
    printf("test_fdt: fuzzing 20000 mutations, seed %llx\n", seed);
    int accepted = 0;
    for (int it = 0; it < 20000; it++) {
        memcpy(b, blob, n);
        int muts = 1 + (int)(rnd() % 4);
        for (int k = 0; k < muts; k++) {
            size_t at = rnd() % n;
            if (rnd() % 2) b[at] ^= (uint8_t)(1u << (rnd() % 8));
            else b[at] = (uint8_t)rnd();
        }
        size_t len = rnd() % 8 ? n : rnd() % n;
        if (fdt_parse(&f, b, len) == FDT_OK) {
            accepted++;
            char err[96];
            platform_from_fdt(&f, &pf, err, sizeof err);    /* must not crash either */
            for (int i = 0; i < f.nnodes; i++) {
                uint64_t a, s;
                fdt_reg(&f, i, 0, &a, &s);
                fdt_compatible(&f, i, "virtio,mmio");
            }
        }
    }
    printf("test_fdt: %d of 20000 mutated blobs still parsed (and were queried safely)\n", accepted);
}

int main(int argc, char **argv)
{
    test_leapvm();
    if (argc > 1) test_qemu(argv[1]);
    test_malformed();
    return check_summary("test_fdt");
}
