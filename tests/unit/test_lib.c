/* lib/: CRC-32C vectors, the printf formatter, string routines. */
#include "check.h"
#include <string.h>
#include "../../lib/crc32c.h"
#include "../../lib/fmt.h"

static const char *fmt(const char *f, ...)
{
    static char buf[256];
    va_list ap;
    va_start(ap, f);
    vsnformat(buf, sizeof buf, f, ap);
    va_end(ap);
    return buf;
}

int main(void)
{
    /* CRC-32C check value (RFC 3720 / iSCSI) and incremental use */
    CHECK_EQ(crc32c_update(0, "123456789", 9), 0xe3069283u);
    CHECK_EQ(crc32c_update(crc32c_update(0, "1234", 4), "56789", 5), 0xe3069283u);
    CHECK_EQ(crc32c_update(0, "", 0), 0);
    unsigned char zeros[32] = {0};
    CHECK_EQ(crc32c_update(0, zeros, 32), 0x8a9136aau);

    CHECK(!strcmp(fmt("%d %i %u", -42, 7, 3000000000u), "-42 7 3000000000"));
    CHECK(!strcmp(fmt("%x %X %o", 255, 255, 8), "ff FF 10"));
    CHECK(!strcmp(fmt("%ld %lu %lx", -1L, 18446744073709551615UL, 0xdeadbeefcafeUL),
                  "-1 18446744073709551615 deadbeefcafe"));
    CHECK(!strcmp(fmt("[%5d][%-5d][%05d][%-05d]", 42, 42, 42, 42), "[   42][42   ][00042][42   ]"));
    CHECK(!strcmp(fmt("[%05d]", -42), "[-0042]"));
    CHECK(!strcmp(fmt("[%8s][%-8s][%.3s]", "ab", "ab", "abcdef"), "[      ab][ab      ][abc]"));
    CHECK(!strcmp(fmt("%*d|%-*d|", 4, 1, 3, 2), "   1|2  |"));
    CHECK(!strcmp(fmt("%c%c%%", 'o', 'k'), "ok%"));
    CHECK(!strcmp(fmt("%s", (char *)0), "(null)"));
    CHECK(!strcmp(fmt("%p", (void *)0x1234), "0x1234"));
    CHECK(!strcmp(fmt("%ld", (long)(-9223372036854775807L - 1)), "-9223372036854775808"));
    CHECK(!strcmp(fmt("%q"), "%q"));                     /* unknown conversions are printed */

    char small[5];
    CHECK_EQ(snformat(small, sizeof small, "%s", "abcdefgh"), 8);   /* returns the full length */
    CHECK(!strcmp(small, "abcd"));                       /* and truncates safely */
    return check_summary("test_lib");
}
