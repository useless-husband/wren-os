/* A conservative mark-sweep garbage collector for C programs, in the style
 * of the Boehm-Demers-Weiser collector (much simplified).
 *
 *   struct node *n = gc_malloc(sizeof *n);   // zeroed; never freed by hand
 *
 * The collector cannot know which words are pointers, so it treats every
 * aligned word in the roots (registers, stack, globals) and in reachable
 * objects as a possible pointer.  A word that points into a live object
 * keeps that object alive.  See docs/GC.md for the design and its limits.
 *
 * Two layers: the program's interface (gc_malloc and friends, gc_wren.c),
 * and the collector core below it (gc.c), which knows nothing about
 * wren-os and is unit-tested on the host with explicit roots. */
#ifndef GC_H
#define GC_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct gc_stats {
    uint64_t collections;
    uint64_t allocated_objects, allocated_bytes;   /* since the program started */
    uint64_t freed_objects, freed_bytes;           /* reclaimed, since the program started */
    uint64_t live_objects, live_bytes;             /* survivors of the last collection */
    uint64_t heap_pages;                           /* pages holding objects now */
    uint64_t peak_pages;                           /* arena high-water mark, in pages */
    uint64_t mark_overflows;                       /* mark-stack overflows (each forces a rescan) */
    uint64_t last_pause_ns, max_pause_ns, total_pause_ns;   /* gc_wren.c only */
};

/* ---- the program's interface (gc_wren.c) ---- */
void *gc_malloc(size_t n);            /* zeroed, 16-byte aligned; NULL when the arena is full */
void  gc_collect(void);               /* full collection now */
void  gc_get_stats(struct gc_stats *st);
bool  gc_is_allocated(const void *p); /* is p the start of a live object?  (for tests) */
void  gc_set_interior(bool on);       /* interior-pointer recognition; on by default */

/* ---- the collector core (gc.c) ---- */
#define GC_PAGE       4096
#define GC_NCLASS     16
#define GC_MAX_SMALL  2048            /* larger objects get whole pages of their own */
#define GC_NONE       0xffffffffu

enum { GP_FREE, GP_SMALL, GP_LARGE, GP_CONT };

/* One descriptor per arena page.  Small-object pages hold nobj objects of
 * one size class; bit i of alloc/mark belongs to object i.  A large object
 * is a GP_LARGE page followed by run-1 GP_CONT pages (alloc/mark bit 0 of
 * the head page). */
struct gc_page {
    uint8_t  kind;
    uint8_t  cls;
    uint16_t size;          /* GP_SMALL: object size */
    uint16_t nobj;          /* GP_SMALL: objects in the page */
    uint32_t run;           /* GP_LARGE: pages; GP_CONT: distance to the head; free-run head: length */
    uint32_t next;          /* free-run head: next run, or GC_NONE */
    uint64_t alloc[4];
    uint64_t mark[4];
};

struct gc_heap {
    struct gc_page *pages;
    uintptr_t      *mstack;             /* mark stack: object start addresses */
    size_t          mcap, msp;
    bool            overflow;           /* an object was marked but could not be pushed */
    bool            interior;           /* recognise pointers into the middle of objects */
    uintptr_t       base;               /* first object page, page aligned */
    uint32_t        npages;             /* arena capacity */
    uint32_t        top;                /* pages [0, top) have been handed out at some point */
    uint32_t        free_runs;          /* free page runs below top, address order */
    uint8_t         cls_of[GC_MAX_SMALL / 16 + 1];
    void           *freelist[GC_NCLASS];
    size_t          trigger_min;        /* collect after max(trigger_min, live_bytes) new bytes */
    size_t          since;              /* bytes allocated since the last collection */
    struct gc_stats st;
};

/* Lay the heap out in [mem, mem + bytes) (descriptors, mark stack, pages).
 * mem must be page aligned.  Returns the number of object pages, 0 if too small. */
uint32_t gc_heap_init(struct gc_heap *h, void *mem, size_t bytes, size_t mstack_entries);
void    *gc_heap_alloc(struct gc_heap *h, size_t n);
bool     gc_heap_want_collect(const struct gc_heap *h);

/* A collection: mark every root range, finish marking, sweep. */
void     gc_heap_mark_range(struct gc_heap *h, const void *lo, const void *hi);
void     gc_heap_mark_finish(struct gc_heap *h);
void     gc_heap_sweep(struct gc_heap *h);

/* The object that word w would keep alive under the current interior
 * policy (its start address), or 0. */
uintptr_t gc_heap_find(const struct gc_heap *h, uintptr_t w);
bool      gc_heap_is_allocated(const struct gc_heap *h, const void *p);
size_t    gc_class_size(int cls);

#endif
