#ifndef WREN_STAT_H
#define WREN_STAT_H
#include <stdint.h>

#define T_DIR    1
#define T_FILE   2
#define T_DEVICE 3

struct stat {
    uint32_t dev;
    uint32_t ino;
    uint16_t type;
    uint16_t nlink;
    uint32_t size;
};

#endif
