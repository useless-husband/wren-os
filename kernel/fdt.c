#include "fdt.h"
#include "../lib/kstring.h"

#define FDT_MAGIC      0xd00dfeedu
#define TOK_BEGIN_NODE 1u
#define TOK_END_NODE   2u
#define TOK_PROP       3u
#define TOK_NOP        4u
#define TOK_END        9u

uint32_t fdt_be32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

uint64_t fdt_cells(const uint8_t *p, int ncells)
{
    uint64_t v = 0;
    for (int i = 0; i < ncells; i++) v = v << 32 | fdt_be32(p + 4 * i);
    return v;
}

uint32_t fdt_totalsize(const void *blob)
{
    const uint8_t *b = blob;
    return fdt_be32(b) == FDT_MAGIC ? fdt_be32(b + 4) : 0;
}

const char *fdt_strerror(int err)
{
    switch (err) {
    case FDT_OK: return "ok";
    case FDT_ERR_MAGIC: return "bad magic";
    case FDT_ERR_HEADER: return "inconsistent header";
    case FDT_ERR_VERSION: return "unsupported version";
    case FDT_ERR_TRUNCATED: return "truncated block";
    case FDT_ERR_STRUCTURE: return "malformed structure block";
    case FDT_ERR_FULL: return "too many nodes or properties";
    default: return "unknown error";
    }
}

/* Is there a NUL in [p, end)?  Returns the string length or -1. */
static long bounded_strlen(const uint8_t *p, const uint8_t *end)
{
    for (const uint8_t *q = p; q < end; q++)
        if (*q == 0) return q - p;
    return -1;
}

int fdt_parse(struct fdt *f, const void *blob, size_t maxlen)
{
    const uint8_t *b = blob;
    memset(f, 0, sizeof *f);
    if (maxlen < 40) return FDT_ERR_HEADER;
    if (fdt_be32(b) != FDT_MAGIC) return FDT_ERR_MAGIC;
    uint32_t total = fdt_be32(b + 4), off_struct = fdt_be32(b + 8), off_strings = fdt_be32(b + 12);
    uint32_t off_rsv = fdt_be32(b + 16), version = fdt_be32(b + 20), last_comp = fdt_be32(b + 24);
    uint32_t size_strings = fdt_be32(b + 32), size_struct = fdt_be32(b + 36);
    if (version < 16 || last_comp > 17) return FDT_ERR_VERSION;
    if (total < 40 || total > maxlen) return FDT_ERR_HEADER;
    /* Every block must lie inside the blob; compare in 64 bits so sums cannot wrap. */
    if ((uint64_t)off_struct + size_struct > total || (uint64_t)off_strings + size_strings > total ||
        off_rsv < 40 || (uint64_t)off_rsv + 16 > total || (off_struct & 3) || (size_struct & 3) || (off_rsv & 7))
        return FDT_ERR_HEADER;
    f->blob = b;
    f->totalsize = total;

    for (uint32_t o = off_rsv;; o += 16) {
        if ((uint64_t)o + 16 > total) return FDT_ERR_TRUNCATED;
        uint64_t addr = fdt_cells(b + o, 2), size = fdt_cells(b + o + 8, 2);
        if (addr == 0 && size == 0) break;
        if (f->nrsv == FDT_MAX_RSV) return FDT_ERR_FULL;
        f->rsv_addr[f->nrsv] = addr;
        f->rsv_size[f->nrsv] = size;
        f->nrsv++;
    }

    const uint8_t *p = b + off_struct, *end = p + size_struct;
    const uint8_t *strs = b + off_strings, *strs_end = strs + size_strings;
    int stack[FDT_MAX_DEPTH];
    int depth = 0;
    bool done = false, children_started = false;
    while (!done) {
        if (end - p < 4) return FDT_ERR_TRUNCATED;
        uint32_t tok = fdt_be32(p);
        p += 4;
        switch (tok) {
        case TOK_BEGIN_NODE: {
            long n = bounded_strlen(p, end);
            if (n < 0) return FDT_ERR_TRUNCATED;
            if (depth == 0 && f->nnodes != 0) return FDT_ERR_STRUCTURE;   /* two roots */
            if (depth == FDT_MAX_DEPTH) return FDT_ERR_STRUCTURE;
            if (f->nnodes == FDT_MAX_NODES) return FDT_ERR_FULL;
            struct fdt_node *nd = &f->nodes[f->nnodes];
            nd->name = (const char *)p;
            nd->parent = depth ? stack[depth - 1] : -1;
            nd->depth = depth;
            nd->first_prop = f->nprops;
            nd->nprops = 0;
            stack[depth++] = f->nnodes++;
            children_started = false;
            uint64_t adv = ((uint64_t)n + 1 + 3) & ~3ull;
            if (adv > (uint64_t)(end - p)) return FDT_ERR_TRUNCATED;
            p += adv;
            break;
        }
        case TOK_END_NODE:
            if (depth == 0) return FDT_ERR_STRUCTURE;
            depth--;
            children_started = true;   /* the parent's properties are over */
            break;
        case TOK_PROP: {
            if (end - p < 8) return FDT_ERR_TRUNCATED;
            uint32_t len = fdt_be32(p), nameoff = fdt_be32(p + 4);
            p += 8;
            if (depth == 0 || children_started) return FDT_ERR_STRUCTURE;
            if (len > (uint64_t)(end - p)) return FDT_ERR_TRUNCATED;
            if (nameoff >= size_strings || bounded_strlen(strs + nameoff, strs_end) < 0)
                return FDT_ERR_TRUNCATED;
            if (f->nprops == FDT_MAX_PROPS) return FDT_ERR_FULL;
            struct fdt_prop *pr = &f->props[f->nprops++];
            pr->name = (const char *)(strs + nameoff);
            pr->data = p;
            pr->len = len;
            f->nodes[stack[depth - 1]].nprops++;
            uint64_t adv = ((uint64_t)len + 3) & ~3ull;
            if (adv > (uint64_t)(end - p)) return FDT_ERR_TRUNCATED;
            p += adv;
            break;
        }
        case TOK_NOP:
            break;
        case TOK_END:
            if (depth != 0 || f->nnodes == 0) return FDT_ERR_STRUCTURE;
            done = true;
            break;
        default:
            return FDT_ERR_STRUCTURE;
        }
    }
    return FDT_OK;
}

const struct fdt_prop *fdt_getprop(const struct fdt *f, int node, const char *name)
{
    if (node < 0 || node >= f->nnodes) return NULL;
    const struct fdt_node *n = &f->nodes[node];
    for (int i = 0; i < n->nprops; i++) {
        const struct fdt_prop *p = &f->props[n->first_prop + i];
        if (strcmp(p->name, name) == 0) return p;
    }
    return NULL;
}

bool fdt_prop_u32(const struct fdt *f, int node, const char *name, uint32_t *out)
{
    const struct fdt_prop *p = fdt_getprop(f, node, name);
    if (!p || p->len != 4) return false;
    *out = fdt_be32(p->data);
    return true;
}

const char *fdt_prop_str(const struct fdt *f, int node, const char *name)
{
    const struct fdt_prop *p = fdt_getprop(f, node, name);
    if (!p || p->len == 0 || p->data[p->len - 1] != 0) return NULL;
    return (const char *)p->data;
}

/* Does node name "name@unit" match path component [c, c+len)? */
static bool name_matches(const char *name, const char *c, size_t len)
{
    if (strncmp(name, c, len) != 0) return false;
    if (name[len] == 0) return true;
    if (name[len] != '@') return false;
    for (size_t i = 0; i < len; i++)   /* "cpu@1" must match exactly; "cpu" matches any unit */
        if (c[i] == '@') return false;
    return true;
}

int fdt_path(const struct fdt *f, const char *path)
{
    if (f->nnodes == 0 || path[0] != '/') return -1;
    int node = 0;
    const char *c = path + 1;
    while (*c) {
        const char *slash = strchr(c, '/');
        size_t len = slash ? (size_t)(slash - c) : strlen(c);
        int found = -1;
        for (int ch = fdt_next_child(f, node, -1); ch >= 0; ch = fdt_next_child(f, node, ch))
            if (name_matches(f->nodes[ch].name, c, len)) { found = ch; break; }
        if (found < 0) return -1;
        node = found;
        if (!slash) break;
        c = slash + 1;
    }
    return node;
}

bool fdt_compatible(const struct fdt *f, int node, const char *compat)
{
    const struct fdt_prop *p = fdt_getprop(f, node, "compatible");
    if (!p) return false;
    size_t want = strlen(compat) + 1;
    /* A string list: "a\0b\0".  Compare each NUL-terminated entry. */
    for (uint32_t i = 0; i < p->len;) {
        size_t n = strnlen((const char *)p->data + i, p->len - i);
        if (n + 1 == want && memcmp(p->data + i, compat, want) == 0) return true;
        i += (uint32_t)n + 1;
    }
    return false;
}

int fdt_next_compatible(const struct fdt *f, int after, const char *compat)
{
    for (int i = after + 1; i < f->nnodes; i++)
        if (fdt_compatible(f, i, compat)) return i;
    return -1;
}

int fdt_next_child(const struct fdt *f, int parent, int after)
{
    for (int i = (after < 0 ? parent : after) + 1; i < f->nnodes; i++) {
        if (f->nodes[i].depth <= f->nodes[parent].depth) return -1;   /* left the subtree */
        if (f->nodes[i].parent == parent) return i;
    }
    return -1;
}

/* Spec defaults: #address-cells 2, #size-cells 1 when absent. */
int fdt_addr_cells(const struct fdt *f, int node)
{
    uint32_t v;
    return fdt_prop_u32(f, node, "#address-cells", &v) && v >= 1 && v <= 2 ? (int)v : 2;
}

int fdt_size_cells(const struct fdt *f, int node)
{
    uint32_t v;
    if (!fdt_prop_u32(f, node, "#size-cells", &v)) return 1;
    return v <= 2 ? (int)v : 1;
}

bool fdt_reg(const struct fdt *f, int node, int index, uint64_t *addr, uint64_t *size)
{
    if (node <= 0 || node >= f->nnodes || index < 0) return false;
    int parent = f->nodes[node].parent;
    int ac = fdt_addr_cells(f, parent), sc = fdt_size_cells(f, parent);
    const struct fdt_prop *p = fdt_getprop(f, node, "reg");
    uint32_t stride = 4u * (uint32_t)(ac + sc);
    if (!p || stride == 0 || (uint64_t)(index + 1) * stride > p->len) return false;
    const uint8_t *d = p->data + (uint32_t)index * stride;
    *addr = fdt_cells(d, ac);
    *size = sc ? fdt_cells(d + 4 * ac, sc) : 0;
    return true;
}
