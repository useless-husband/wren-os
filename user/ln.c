#include "lib/ulib.h"

int main(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(2, "usage: ln old new\n");
        exit(2);
    }
    int r = link(argv[1], argv[2]);
    if (r < 0) fprintf(2, "ln: %s\n", strerror(r));
    exit(r < 0);
}
