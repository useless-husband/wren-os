/* exec: replace the current program with an ELF executable from the file
 * system.  The new address space is built completely before the old one is
 * touched, so a failed exec returns an error to an intact caller. */
#include "syscall.h"
#include "elf.h"
#include "fs.h"
#include "kalloc.h"
#include "log.h"
#include "memlayout.h"
#include "printk.h"
#include "proc.h"
#include "vm.h"
#include <wren/errno.h>
#include "../lib/kstring.h"

/* Copy segment s of ip into freshly allocated pages of vm. */
static int load_segment(struct vmspace *vm, struct inode *ip, const struct elf_segment *s)
{
    vaddr_t end = s->vaddr + s->memsz, fend = s->vaddr + s->filesz;
    for (vaddr_t page = ROUNDDOWN(s->vaddr, PAGE_SIZE); page < end; page += PAGE_SIZE) {
        paddr_t pa = page_alloc();
        if (!pa) return -ENOMEM;
        vaddr_t lo = MAX(page, s->vaddr), hi = MIN(page + PAGE_SIZE, fend);
        if (lo < hi) {
            uint32_t n = (uint32_t)(hi - lo);
            uint64_t kdst = (uint64_t)P2V(pa) + (lo - page);
            if (readi(ip, false, kdst, (uint32_t)(s->offset + (lo - s->vaddr)), n) != (int)n) {
                page_put(pa);
                return -ENOEXEC;
            }
        }
        int r = vm_map(vm, page, pa, s->prot);
        if (r < 0) {
            page_put(pa);
            return r;
        }
    }
    return 0;
}

int proc_exec(const char *path, char *const argv[], int argc)
{
    struct proc *p = myproc();
    struct vmspace nvm = {0};
    struct elf_image img;
    uint8_t *hdr = kmalloc(PAGE_SIZE);
    if (!hdr) return -ENOMEM;

    log_begin_op();
    struct inode *ip = namei(path);
    if (!ip) {
        log_end_op();
        kfree(hdr);
        return -ENOENT;
    }
    ilock(ip);
    int r = ip->type == DI_FILE ? 0 : -EACCES;
    if (r == 0) {
        int n = readi(ip, false, (uint64_t)hdr, 0, PAGE_SIZE);
        if (n < 0) r = n;
        else if (elf_parse(hdr, (size_t)n, ip->size, USER_MIN, USTACK_LOW - (2UL << 20), &img) != ELF_OK)
            r = -ENOEXEC;
    }
    if (r == 0) r = vm_create(&nvm);
    for (int i = 0; r == 0 && i < img.nseg; i++) r = load_segment(&nvm, ip, &img.seg[i]);
    iunlockput(ip);
    log_end_op();
    kfree(hdr);
    if (r < 0) goto fail;

    for (int i = 0; i < img.nseg; i++)
        if (img.seg[i].prot & VM_X) vm_sync_icache(&nvm, img.seg[i].vaddr, img.seg[i].memsz);
    nvm.heap_start = nvm.brk = img.end;

    /* Stack: argument strings at the top, then the argv array, 16-byte aligned. */
    uint64_t sp = USER_TOP, uargv[MAXARG + 1];
    for (int i = argc - 1; i >= 0; i--) {
        size_t len = strlen(argv[i]) + 1;
        sp -= len;
        if (copyout(&nvm, sp, argv[i], len) < 0) {
            r = -ENOMEM;
            goto fail;
        }
        uargv[i] = sp;
    }
    uargv[argc] = 0;
    sp = ROUNDDOWN(sp - (uint64_t)(argc + 1) * sizeof(uint64_t), 16);
    if (copyout(&nvm, sp, uargv, (size_t)(argc + 1) * sizeof(uint64_t)) < 0) {
        r = -ENOMEM;
        goto fail;
    }

    /* Point of no return: switch to the new image. */
    const char *base = strrchr(path, '/');
    strlcpy(p->name, base ? base + 1 : path, sizeof p->name);
    struct vmspace old = p->vm;
    p->vm = nvm;
    vm_activate(&p->vm);
    vm_destroy(&old);
    memset(p->tf, 0, sizeof *p->tf);
    p->tf->elr = img.entry;
    p->tf->sp = sp;
    p->tf->x[1] = sp;                    /* argv; x0 = argc is the return value */
    p->tf->spsr = 0;
    memset(&p->fp, 0, sizeof p->fp);     /* the new program starts with clean FP state */
    fpu_restore(&p->fp);
    return argc;

fail:
    if (nvm.root) vm_destroy(&nvm);
    return r;
}

SYSCALL(exec)
{
    char path[MAXPATH];
    int r = fetch_path(tf, 0, path);
    if (r < 0) return r;
    /* Gather argv into one page: pointers first, then the strings. */
    char *page = kmalloc(PAGE_SIZE);
    if (!page) return -ENOMEM;
    char **kargv = (char **)page;
    char *strs = page + (MAXARG + 1) * sizeof(char *);
    size_t room = PAGE_SIZE - (MAXARG + 1) * sizeof(char *);
    uint64_t uargv = arg(tf, 1);
    int argc = 0;
    for (;; argc++) {
        uint64_t uarg;
        if (argc > MAXARG) {
            r = -E2BIG;
            break;
        }
        if (copyin(&myproc()->vm, &uarg, uargv + (uint64_t)argc * 8, 8) < 0) {
            r = -EFAULT;
            break;
        }
        if (!uarg) break;
        if (argc == MAXARG) {
            r = -E2BIG;
            break;
        }
        int c = copyinstr(&myproc()->vm, strs, uarg, room);
        if (c < 0) {
            r = c == -ENAMETOOLONG ? -E2BIG : c;
            break;
        }
        kargv[argc] = strs;
        size_t used = strlen(strs) + 1;
        strs += used;
        room -= used;
    }
    if (r == 0) r = proc_exec(path, kargv, argc);
    kfree(page);
    return r;
}
