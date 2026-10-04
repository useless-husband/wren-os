/* grep [-c] [-i] [-n] [-v] pattern [file...]
 * Patterns: literal characters, . (any), [set] [^set] [a-z], ^ and $
 * anchors, and the postfix operators * + ?. */
#include "lib/ulib.h"

static bool icase;

static int lower(int c) { return icase && c >= 'A' && c <= 'Z' ? c + 32 : c; }

/* Length of the single-character pattern item at p (a char, '.', or a class). */
static int item_len(const char *p)
{
    if (*p == '\\' && p[1]) return 2;
    if (*p != '[') return 1;
    const char *q = p + 1;
    if (*q == '^') q++;
    if (*q == ']') q++;
    while (*q && *q != ']') q++;
    return *q ? (int)(q - p + 1) : 1;
}

static bool item_matches(const char *p, int len, char c)
{
    if (*p == '.' && len == 1) return true;
    if (*p == '\\' && len == 2) return lower(p[1]) == lower(c);
    if (*p != '[' || len == 1) return lower(*p) == lower(c);
    const char *q = p + 1, *end = p + len - 1;
    bool neg = *q == '^';
    if (neg) q++;
    bool hit = false;
    for (; q < end; q++) {
        if (q + 2 < end && q[1] == '-') {
            if (lower(c) >= lower(q[0]) && lower(c) <= lower(q[2])) hit = true;
            q += 2;
        } else if (lower(*q) == lower(c)) {
            hit = true;
        }
    }
    return hit != neg;
}

/* Does pattern p match a prefix of text t? (backtracking) */
static bool match_here(const char *p, const char *t)
{
    if (!*p) return true;
    if (p[0] == '$' && !p[1]) return !*t;
    int len = item_len(p);
    char op = p[len];
    if (op == '*' || op == '+' || op == '?') {
        int max = op == '?' ? 1 : 1 << 30, min = op == '+' ? 1 : 0, n = 0;
        while (n < max && t[n] && item_matches(p, len, t[n])) n++;
        for (; n >= min; n--)                 /* longest first */
            if (match_here(p + len + 1, t + n)) return true;
        return false;
    }
    return *t && item_matches(p, len, *t) && match_here(p + len, t + 1);
}

static bool match(const char *p, const char *t)
{
    if (*p == '^') return match_here(p + 1, t);
    do {
        if (match_here(p, t)) return true;
    } while (*t++);
    return false;
}

static bool opt_c, opt_n, opt_v;

static long grep_fd(const char *pat, int fd, const char *name, bool show_name)
{
    char line[1024];
    long count = 0, lineno = 0;
    int n;
    while ((n = readline(fd, line, sizeof line)) > 0) {
        lineno++;
        if (line[n - 1] == '\n') line[n - 1] = 0;
        if (match(pat, line) == opt_v) continue;
        count++;
        if (opt_c) continue;
        if (show_name) printf("%s:", name);
        if (opt_n) printf("%ld:", lineno);
        printf("%s\n", line);
    }
    if (opt_c) {
        if (show_name) printf("%s:", name);
        printf("%ld\n", count);
    }
    return count;
}

int main(int argc, char **argv)
{
    int i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++)
        for (const char *o = argv[i] + 1; *o; o++) {
            if (*o == 'c') opt_c = true;
            else if (*o == 'i') icase = true;
            else if (*o == 'n') opt_n = true;
            else if (*o == 'v') opt_v = true;
            else {
                fprintf(2, "grep: unknown option -%c\n", *o);
                exit(2);
            }
        }
    if (i >= argc) {
        fprintf(2, "usage: grep [-cinv] pattern [file...]\n");
        exit(2);
    }
    const char *pat = argv[i++];
    long found = 0;
    if (i == argc) found = grep_fd(pat, 0, "", false);
    for (int k = i; k < argc; k++) {
        int fd = open(argv[k], O_RDONLY);
        if (fd < 0) {
            fprintf(2, "grep: %s: %s\n", argv[k], strerror(fd));
            continue;
        }
        found += grep_fd(pat, fd, argv[k], argc - i > 1);
        close(fd);
    }
    exit(found ? 0 : 1);
}
