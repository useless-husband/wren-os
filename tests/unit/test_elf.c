/* kernel/elf.c on the host: a valid executable, one corruption per rule,
 * random fuzzing, and every real program in build/user/ (passed as args). */
#include "check.h"
#include <string.h>
#include "../../kernel/elf.h"

#define UMIN 0x10000ull
#define UMAX 0x3fffe00000ull

static uint8_t img[4096];

static void le(uint8_t *p, uint64_t v, int n) { for (int i = 0; i < n; i++) p[i] = (uint8_t)(v >> (8 * i)); }

/* Two segments: text R+X at 0x10000 (file 0..0x1800), data R+W at 0x12000. */
static void make_valid(void)
{
    memset(img, 0, sizeof img);
    memcpy(img, "\x7f" "ELF", 4);
    img[4] = 2; img[5] = 1; img[6] = 1;
    le(img + 16, 2, 2);        /* ET_EXEC */
    le(img + 18, 183, 2);      /* EM_AARCH64 */
    le(img + 20, 1, 4);
    le(img + 24, 0x10040, 8);  /* entry */
    le(img + 32, 64, 8);       /* phoff */
    le(img + 52, 64, 2);
    le(img + 54, 56, 2);
    le(img + 56, 2, 2);
    uint8_t *ph = img + 64;
    le(ph, 1, 4); le(ph + 4, 5, 4); le(ph + 8, 0, 8); le(ph + 16, 0x10000, 8);
    le(ph + 32, 0x1800, 8); le(ph + 40, 0x1800, 8);
    ph += 56;
    le(ph, 1, 4); le(ph + 4, 6, 4); le(ph + 8, 0x2000, 8); le(ph + 16, 0x12000, 8);
    le(ph + 32, 0x100, 8); le(ph + 40, 0x5000, 8);
}

static int parse(uint64_t file_size)
{
    struct elf_image im;
    return elf_parse(img, sizeof img, file_size, UMIN, UMAX, &im);
}

int main(int argc, char **argv)
{
    struct elf_image im;
    make_valid();
    CHECK_EQ(elf_parse(img, sizeof img, 0x3000, UMIN, UMAX, &im), ELF_OK);
    CHECK_EQ(im.nseg, 2);
    CHECK_EQ(im.entry, 0x10040);
    CHECK_EQ(im.end, 0x17000);
    CHECK_EQ(im.seg[0].prot, 5);
    CHECK_EQ(im.seg[1].prot, 3);

    struct { const char *what; int off, n; uint64_t val; uint64_t fsize; int want; } cases[] = {
        {"magic", 1, 1, 'X', 0x3000, ELF_ERR_MAGIC},
        {"32-bit class", 4, 1, 1, 0x3000, ELF_ERR_CLASS},
        {"big endian", 5, 1, 2, 0x3000, ELF_ERR_CLASS},
        {"shared object", 16, 2, 3, 0x3000, ELF_ERR_CLASS},
        {"x86-64", 18, 2, 62, 0x3000, ELF_ERR_CLASS},
        {"phentsize", 54, 2, 32, 0x3000, ELF_ERR_PHDR},
        {"no phdrs", 56, 2, 0, 0x3000, ELF_ERR_PHDR},
        {"phdrs past buffer", 32, 8, 4090, 0x3000, ELF_ERR_SHORT},
        {"phoff huge", 32, 8, 0xffffffffffffff00ull, 0x3000, ELF_ERR_SHORT},
        {"filesz > memsz", 64 + 56 + 32, 8, 0x6000, 0x9000, ELF_ERR_SEGMENT},
        {"below USER_MIN", 64 + 16, 8, 0x0, 0x3000, ELF_ERR_SEGMENT},
        {"wraps around", 64 + 56 + 40, 8, 0xffffffffffff0000ull, 0x3000, ELF_ERR_SEGMENT},
        {"past USER top", 64 + 56 + 16, 8, UMAX - 0x1000, 0x3000, ELF_ERR_SEGMENT},
        {"file data past EOF", 64 + 56 + 8, 8, 0x2f80, 0x3000, ELF_ERR_SEGMENT},
        {"offset/vaddr misaligned", 64 + 56 + 8, 8, 0x2100, 0x3000, ELF_ERR_SEGMENT},
        {"overlap", 64 + 56 + 16, 8, 0x11000, 0x3000, ELF_ERR_OVERLAP},
        {"writable+executable", 64 + 56 + 4, 4, 7, 0x3000, ELF_ERR_WX},
        {"entry in data", 24, 8, 0x12010, 0x3000, ELF_ERR_ENTRY},
        {"entry nowhere", 24, 8, 0x9999999, 0x3000, ELF_ERR_ENTRY},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        make_valid();
        le(img + cases[i].off, cases[i].val, cases[i].n);
        int r = parse(cases[i].fsize);
        if (r != cases[i].want)
            fprintf(stderr, "case '%s': got %d (%s), want %d\n", cases[i].what, r, elf_strerror(r), cases[i].want);
        CHECK_EQ(r, cases[i].want);
    }
    CHECK_EQ(elf_parse(img, 63, 0x3000, UMIN, UMAX, &im), ELF_ERR_SHORT);

    unsigned long long seed = 0xe1fe1full;
    rng_state = seed;
    int ok = 0;
    for (int it = 0; it < 50000; it++) {
        make_valid();
        for (int k = 0, m = 1 + (int)(rnd() % 3); k < m; k++) img[rnd() % 176] = (uint8_t)rnd();
        if (parse(rnd() % 0x10000) == ELF_OK) {
            ok++;
            elf_parse(img, sizeof img, 0x10000, UMIN, UMAX, &im);
            for (int s = 0; s < im.nseg; s++) {   /* anything accepted must be loadable */
                CHECK(im.seg[s].vaddr >= UMIN && im.seg[s].vaddr + im.seg[s].memsz <= UMAX);
                CHECK(im.seg[s].filesz <= im.seg[s].memsz);
                CHECK(!((im.seg[s].prot & 2) && (im.seg[s].prot & 4)));
            }
        }
    }
    printf("test_elf: fuzzed 50000 headers (seed %llx), %d accepted, all within bounds\n", seed, ok);

    for (int i = 1; i < argc; i++) {          /* the real programs */
        FILE *f = fopen(argv[i], "rb");
        if (!f) continue;
        memset(img, 0, sizeof img);
        size_t n = fread(img, 1, sizeof img, f);
        fseek(f, 0, SEEK_END);
        long size = ftell(f);
        fclose(f);
        int r = elf_parse(img, n, (uint64_t)size, UMIN, UMAX, &im);
        if (r != ELF_OK) fprintf(stderr, "%s: %s\n", argv[i], elf_strerror(r));
        CHECK_EQ(r, ELF_OK);
        CHECK_EQ(im.entry, 0x10000);
    }
    if (argc > 1) printf("test_elf: %d real programs validated\n", argc - 1);
    return check_summary("test_elf");
}
