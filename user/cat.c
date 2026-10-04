#include "lib/ulib.h"

static int copy(int fd, const char *name)
{
    static char buf[8192];
    long n;
    while ((n = read(fd, buf, sizeof buf)) > 0)
        if (write_all(1, buf, (size_t)n) != n) {
            fprintf(2, "cat: write error\n");
            return 1;
        }
    if (n < 0) {
        fprintf(2, "cat: %s: %s\n", name, strerror((int)n));
        return 1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 2) exit(copy(0, "stdin"));
    int status = 0;
    for (int i = 1; i < argc; i++) {
        int fd = open(argv[i], O_RDONLY);
        if (fd < 0) {
            fprintf(2, "cat: %s: %s\n", argv[i], strerror(fd));
            status = 1;
            continue;
        }
        status |= copy(fd, argv[i]);
        close(fd);
    }
    exit(status);
}
