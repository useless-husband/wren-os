#include "lib/ulib.h"

int main(int argc, char **argv)
{
    int i = 1;
    bool newline = true;
    if (argc > 1 && strcmp(argv[1], "-n") == 0) {
        newline = false;
        i++;
    }
    char buf[512];
    int n = 0;
    for (; i < argc; i++) {
        n += snprintf(buf + n, sizeof buf - (size_t)n, "%s%s", argv[i], i + 1 < argc ? " " : "");
        if (n >= (int)sizeof buf - 1) n = (int)sizeof buf - 2;
    }
    if (newline) buf[n++] = '\n';
    write_all(1, buf, (size_t)n);
    exit(0);
}
