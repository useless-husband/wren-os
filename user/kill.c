#include "lib/ulib.h"

int main(int argc, char **argv)
{
    int status = 0;
    for (int i = 1; i < argc; i++) {
        int r = kill(atoi(argv[i]));
        if (r < 0) {
            fprintf(2, "kill: %s: %s\n", argv[i], strerror(r));
            status = 1;
        }
    }
    exit(status);
}
