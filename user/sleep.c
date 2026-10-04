#include "lib/ulib.h"

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(2, "usage: sleep milliseconds\n");
        exit(2);
    }
    exit(sleep(atol(argv[1])) < 0);
}
