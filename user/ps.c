#include "lib/ulib.h"

int main(void)
{
    static struct procinfo rows[64];
    int n = procinfo(rows, 64);
    if (n < 0) {
        fprintf(2, "ps: %s\n", strerror(n));
        exit(1);
    }
    static const char *states[] = {"?", "runnable", "running", "sleeping", "zombie"};
    printf("  PID  PPID STATE     CPU    MEM(KiB)  TICKS NAME\n");
    for (int i = 0; i < n; i++)
        printf("%5d %5d %-9s %3d %11lu %6lu %s\n", rows[i].pid, rows[i].ppid,
               states[rows[i].state >= 1 && rows[i].state <= 4 ? rows[i].state : 0], rows[i].cpu,
               rows[i].mem / 1024, rows[i].ticks, rows[i].name);
    exit(0);
}
