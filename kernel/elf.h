/* ELF64 executable validation.  Pure function, also built into the host
 * unit tests: every field that the loader later trusts is checked here. */
#ifndef KERNEL_ELF_H
#define KERNEL_ELF_H
#include <stddef.h>
#include <stdint.h>

#define ELF_MAX_SEGS 8

struct elf_segment {
    uint64_t vaddr, memsz, offset, filesz;
    int      prot;           /* VM_R | VM_W | VM_X */
};

struct elf_image {
    uint64_t entry;
    uint64_t end;            /* highest vaddr + memsz, page aligned: the heap starts here */
    int      nseg;
    struct elf_segment seg[ELF_MAX_SEGS];
};

enum {
    ELF_OK = 0,
    ELF_ERR_SHORT = -1,      /* headers do not fit in what we were given */
    ELF_ERR_MAGIC = -2,
    ELF_ERR_CLASS = -3,      /* not ELF64 little-endian AArch64 executable */
    ELF_ERR_PHDR = -4,       /* program header table malformed */
    ELF_ERR_SEGMENT = -5,    /* a segment is out of bounds or inconsistent */
    ELF_ERR_OVERLAP = -6,
    ELF_ERR_WX = -7,         /* writable and executable */
    ELF_ERR_ENTRY = -8,      /* entry point not inside an executable segment */
};

/* hdr: the first `len` bytes of the file; file_size: whole file length.
 * Loadable segments must lie within [umin, umax). */
int elf_parse(const uint8_t *hdr, size_t len, uint64_t file_size, uint64_t umin, uint64_t umax,
              struct elf_image *out);
const char *elf_strerror(int err);

#endif
