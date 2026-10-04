#include "elf.h"
#include "../lib/kstring.h"

#define PT_LOAD 1
#define PF_X 1
#define PF_W 2
#define PF_R 4
#define EM_AARCH64 183
#define ET_EXEC 2
#define PAGE 4096u

/* Little-endian field readers: the buffer has no alignment guarantee. */
static uint64_t rd(const uint8_t *p, int n)
{
    uint64_t v = 0;
    for (int i = n - 1; i >= 0; i--) v = v << 8 | p[i];
    return v;
}

const char *elf_strerror(int err)
{
    switch (err) {
    case ELF_OK: return "ok";
    case ELF_ERR_SHORT: return "headers truncated";
    case ELF_ERR_MAGIC: return "not an ELF file";
    case ELF_ERR_CLASS: return "not an ELF64 AArch64 executable";
    case ELF_ERR_PHDR: return "bad program header table";
    case ELF_ERR_SEGMENT: return "bad segment";
    case ELF_ERR_OVERLAP: return "overlapping segments";
    case ELF_ERR_WX: return "writable+executable segment";
    case ELF_ERR_ENTRY: return "entry point outside executable code";
    default: return "unknown";
    }
}

int elf_parse(const uint8_t *h, size_t len, uint64_t file_size, uint64_t umin, uint64_t umax,
              struct elf_image *out)
{
    memset(out, 0, sizeof *out);
    if (len < 64) return ELF_ERR_SHORT;
    if (h[0] != 0x7f || h[1] != 'E' || h[2] != 'L' || h[3] != 'F') return ELF_ERR_MAGIC;
    if (h[4] != 2 || h[5] != 1 || h[6] != 1) return ELF_ERR_CLASS;          /* 64-bit, LE, version 1 */
    if (rd(h + 16, 2) != ET_EXEC || rd(h + 18, 2) != EM_AARCH64) return ELF_ERR_CLASS;
    uint64_t entry = rd(h + 24, 8), phoff = rd(h + 32, 8);
    uint64_t phentsize = rd(h + 54, 2), phnum = rd(h + 56, 2);
    if (phentsize != 56 || phnum == 0 || phnum > 32) return ELF_ERR_PHDR;
    if (phoff > len || phnum * 56 > len - phoff) return ELF_ERR_SHORT;

    for (uint64_t i = 0; i < phnum; i++) {
        const uint8_t *ph = h + phoff + i * 56;
        if (rd(ph, 4) != PT_LOAD) continue;
        uint32_t flags = (uint32_t)rd(ph + 4, 4);
        uint64_t off = rd(ph + 8, 8), va = rd(ph + 16, 8);
        uint64_t filesz = rd(ph + 32, 8), memsz = rd(ph + 40, 8);
        if (memsz == 0) continue;
        if (out->nseg == ELF_MAX_SEGS) return ELF_ERR_PHDR;
        if (filesz > memsz) return ELF_ERR_SEGMENT;
        if (va < umin || va >= umax || memsz > umax - va) return ELF_ERR_SEGMENT;   /* no wraparound */
        if (off > file_size || filesz > file_size - off) return ELF_ERR_SEGMENT;
        if ((va % PAGE) != (off % PAGE)) return ELF_ERR_SEGMENT;
        if ((flags & PF_W) && (flags & PF_X)) return ELF_ERR_WX;
        uint64_t lo = va & ~(uint64_t)(PAGE - 1), hi = (va + memsz + PAGE - 1) & ~(uint64_t)(PAGE - 1);
        for (int k = 0; k < out->nseg; k++) {   /* page granularity: segments may not share a page */
            uint64_t klo = out->seg[k].vaddr & ~(uint64_t)(PAGE - 1);
            uint64_t khi = (out->seg[k].vaddr + out->seg[k].memsz + PAGE - 1) & ~(uint64_t)(PAGE - 1);
            if (lo < khi && klo < hi) return ELF_ERR_OVERLAP;
        }
        struct elf_segment *s = &out->seg[out->nseg++];
        s->vaddr = va;
        s->memsz = memsz;
        s->offset = off;
        s->filesz = filesz;
        s->prot = (flags & PF_R ? 1 : 0) | (flags & PF_W ? 2 : 0) | (flags & PF_X ? 4 : 0);
        if (hi > out->end) out->end = hi;
    }
    if (out->nseg == 0) return ELF_ERR_PHDR;
    for (int k = 0; k < out->nseg; k++)
        if ((out->seg[k].prot & 4) && entry >= out->seg[k].vaddr &&
            entry - out->seg[k].vaddr < out->seg[k].memsz) {
            out->entry = entry;
            return ELF_OK;
        }
    return ELF_ERR_ENTRY;
}
