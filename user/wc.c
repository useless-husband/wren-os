#include "lib/ulib.h"

static long tl, tw, tc;

static int count(int fd, const char *name)
{
    static char buf[4096];
    long lines = 0, words = 0, bytes = 0, n;
    bool inword = false;
    while ((n = read(fd, buf, sizeof buf)) > 0) {
        bytes += n;
        for (long i = 0; i < n; i++) {
            if (buf[i] == '\n') lines++;
            if (isspace_c(buf[i])) inword = false;
            else if (!inword) {
                inword = true;
                words++;
            }
        }
    }
    if (n < 0) {
        fprintf(2, "wc: %s: %s\n", name, strerror((int)n));
        return 1;
    }
    printf("%7ld %7ld %7ld %s\n", lines, words, bytes, name);
    tl += lines;
    tw += words;
    tc += bytes;
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 2) exit(count(0, ""));
    int status = 0;
    for (int i = 1; i < argc; i++) {
        int fd = open(argv[i], O_RDONLY);
        if (fd < 0) {
            fprintf(2, "wc: %s: %s\n", argv[i], strerror(fd));
            status = 1;
            continue;
        }
        status |= count(fd, argv[i]);
        close(fd);
    }
    if (argc > 2) printf("%7ld %7ld %7ld total\n", tl, tw, tc);
    exit(status);
}
