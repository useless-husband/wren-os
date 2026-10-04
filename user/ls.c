/* ls [path...]: name, type, inode number, size. */
#include "lib/ulib.h"
#include <wren/fsformat.h>

static const char *type_name(int t)
{
    return t == T_DIR ? "dir" : t == T_FILE ? "file" : t == T_DEVICE ? "dev" : "?";
}

static void show(const char *path, const char *name)
{
    struct stat st;
    int fd = open(path, O_RDONLY);
    if (fd < 0 || fstat(fd, &st) < 0) {
        fprintf(2, "ls: %s: %s\n", path, strerror(fd < 0 ? fd : -EIO));
        if (fd >= 0) close(fd);
        return;
    }
    close(fd);
    printf("%-20s %-4s %4u %8u\n", name, type_name(st.type), st.ino, st.size);
}

static int list(const char *path)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        fprintf(2, "ls: %s: %s\n", path, strerror(fd));
        return 1;
    }
    struct stat st;
    fstat(fd, &st);
    if (st.type != T_DIR) {
        close(fd);
        show(path, path);
        return 0;
    }
    struct dirent de;
    char full[160];
    while (read(fd, &de, sizeof de) == sizeof de) {
        if (de.inum == 0) continue;
        char name[DIRSIZ + 1];
        memcpy(name, de.name, DIRSIZ);
        name[DIRSIZ] = 0;
        snprintf(full, sizeof full, "%s/%s", path, name);
        show(full, name);
    }
    close(fd);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 2) exit(list("."));
    int status = 0;
    for (int i = 1; i < argc; i++) status |= list(argv[i]);
    exit(status);
}
