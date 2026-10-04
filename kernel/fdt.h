/* Flattened device tree (DTB) reader.
 *
 * fdt_parse() makes one validating pass over the blob and records every node
 * and property in flat arrays; the query functions then work on those
 * arrays.  Nothing here allocates memory or trusts the blob: every offset and
 * length is checked against the header before use, so a corrupt blob yields
 * an error code, never an out-of-bounds read.  The file is free of kernel
 * dependencies and is also compiled into the host unit tests.
 *
 * Format reference: Devicetree Specification v0.4, chapter 5. */
#ifndef KERNEL_FDT_H
#define KERNEL_FDT_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define FDT_MAX_NODES 256
#define FDT_MAX_PROPS 1536
#define FDT_MAX_DEPTH 16
#define FDT_MAX_RSV   16

enum {
    FDT_OK = 0,
    FDT_ERR_MAGIC = -1,     /* not a DTB */
    FDT_ERR_HEADER = -2,    /* header fields inconsistent or out of range */
    FDT_ERR_VERSION = -3,
    FDT_ERR_TRUNCATED = -4, /* a token/name/property runs past its block */
    FDT_ERR_STRUCTURE = -5, /* unbalanced or unknown tokens */
    FDT_ERR_FULL = -6,      /* more nodes/properties than we have room for */
};

struct fdt_node {
    const char *name;       /* "name@unit"; the root is "" */
    int parent;             /* index, -1 for the root */
    int depth;
    int first_prop;         /* properties precede children, so they are contiguous */
    int nprops;
};

struct fdt_prop {
    const char *name;
    const uint8_t *data;
    uint32_t len;
};

struct fdt {
    const uint8_t *blob;
    uint32_t totalsize;
    int nnodes, nprops, nrsv;
    struct fdt_node nodes[FDT_MAX_NODES];
    struct fdt_prop props[FDT_MAX_PROPS];
    uint64_t rsv_addr[FDT_MAX_RSV], rsv_size[FDT_MAX_RSV];   /* /memreserve/ entries */
};

int fdt_parse(struct fdt *f, const void *blob, size_t maxlen);
const char *fdt_strerror(int err);
uint32_t fdt_totalsize(const void *blob);   /* 0 if the magic is wrong */

/* Big-endian cell readers. */
uint32_t fdt_be32(const uint8_t *p);
uint64_t fdt_cells(const uint8_t *p, int ncells);   /* 1 or 2 cells */

const struct fdt_prop *fdt_getprop(const struct fdt *f, int node, const char *name);
bool fdt_prop_u32(const struct fdt *f, int node, const char *name, uint32_t *out);
const char *fdt_prop_str(const struct fdt *f, int node, const char *name);  /* NUL-checked */

/* Node lookup.  Paths are absolute ("/cpus/cpu@0"); a component without a
 * unit address matches any unit address ("/chosen", "/psci"). */
int fdt_path(const struct fdt *f, const char *path);
bool fdt_compatible(const struct fdt *f, int node, const char *compat);
int fdt_next_compatible(const struct fdt *f, int after, const char *compat);  /* after = -1 to start */
int fdt_next_child(const struct fdt *f, int parent, int after);

/* reg = <addr size>... decoded with the parent's #address-cells/#size-cells. */
int fdt_addr_cells(const struct fdt *f, int node);   /* cells for this node's children */
int fdt_size_cells(const struct fdt *f, int node);
bool fdt_reg(const struct fdt *f, int node, int index, uint64_t *addr, uint64_t *size);

#endif
