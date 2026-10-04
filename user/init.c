/* init: open the console as fds 0-2, then keep a shell running. */
#include "lib/ulib.h"

int main(void)
{
    if (open("/dev/console", O_RDWR) < 0) {
        mknod("/dev/console", 1);
        if (open("/dev/console", O_RDWR) < 0) exit(1);
    }
    dup(0);
    dup(0);
    for (;;) {
        printf("init: starting /bin/sh\n");
        int pid = fork();
        if (pid < 0) {
            printf("init: fork failed: %s\n", strerror(pid));
            sleep(1000);
            continue;
        }
        if (pid == 0) {
            char *argv[] = {"sh", 0};
            exec("/bin/sh", argv);
            printf("init: cannot exec /bin/sh\n");
            exit(1);
        }
        /* Reap everything; orphans are re-parented to us. */
        for (;;) {
            int status, w = wait(&status);
            if (w == pid || w < 0) break;
        }
    }
}
