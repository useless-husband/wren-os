#ifndef LIB_CRC32C_H
#define LIB_CRC32C_H
#include <stddef.h>
#include <stdint.h>

/* Start with crc = 0; feed buffers in order.  crc32c_update(0, "123456789", 9) == 0xe3069283. */
uint32_t crc32c_update(uint32_t crc, const void *buf, size_t len);

#endif
