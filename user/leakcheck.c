/* leakcheck prog [args...]: run prog with the malloc leak checker.
 *
 * Sets the PER_LEAKCHECK personality flag, which survives exec and is
 * inherited by children, then execs prog.  The user library's exit()
 * sees the flag and prints the leak report on fd 2 before the process
 * ends (user/lib/malloc.c). */
#include "lib/ulib.h"
#include <wren/syscall.h>

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(2, "usage: leakcheck program [args...]\n");
        exit(2);
    }
    char path[128];
    if (strchr(argv[1], '/')) strlcpy(path, argv[1], sizeof path);
    else snprintf(path, sizeof path, "/bin/%s", argv[1]);
    long r = personality(PER_LEAKCHECK);
    if (r >= 0) r = exec(path, argv + 1);
    fprintf(2, "leakcheck: %s: %s\n", argv[1], strerror((int)r));
    _exit(1);                        /* not exit(): this program has nothing to report */
}
