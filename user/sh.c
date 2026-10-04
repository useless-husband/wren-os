/* sh: a small shell.
 *   cmd args...        run /bin/cmd (or a path containing '/')
 *   a | b | c          pipelines
 *   < file  > file  >> file
 *   cmd &              background job; finished jobs are reported at the next prompt
 *   a ; b              sequence
 *   "quoted words" and 'quoted words'
 * Built-ins: cd, exit, jobs, wait.  `sh file` runs commands from a file. */
#include "lib/ulib.h"

#define MAXARGS  24
#define MAXCMDS  8
#define MAXJOBS  16
#define LINE     512

struct cmd {
    char *argv[MAXARGS + 1];
    int   argc;
    char *in, *out;
    bool  append;
};

struct job {
    int  id;
    int  pids[MAXCMDS];
    int  npids, running;
    char text[64];
};

static struct job jobs[MAXJOBS];
static int next_job = 1;
static int last_status;

static void report_done(bool print)
{
    for (;;) {
        int status, pid = waitpid(-1, &status, WNOHANG);
        if (pid <= 0) return;
        for (int j = 0; j < MAXJOBS; j++) {
            struct job *jb = &jobs[j];
            for (int k = 0; k < jb->npids; k++)
                if (jb->pids[k] == pid) {
                    jb->pids[k] = -1;
                    if (--jb->running == 0) {
                        if (print) printf("[%d] Done  %s\n", jb->id, jb->text);
                        jb->npids = 0;
                    }
                }
        }
    }
}

/* Split src into words and operator tokens.  Words are copied (quotes
 * removed) into dst, which must be at least as large as src; operators are
 * returned as the static strings "|", "<", ">", ">>", "&", ";". */
static int tokenize(const char *src, char *dst, char **tok, bool *is_op, int max)
{
    static char *ops[] = {"|", "<", ">", ">>", "&", ";"};
    int n = 0;
    const char *p = src;
    char *w = dst;
    while (n < max) {
        while (isspace_c(*p)) p++;
        if (!*p || *p == '#') break;
        if (strchr("|<>&;", *p)) {
            int k = *p == '|' ? 0 : *p == '<' ? 1 : *p == '&' ? 4 : *p == ';' ? 5 : (p[1] == '>' ? 3 : 2);
            p += k == 3 ? 2 : 1;
            tok[n] = ops[k];
            is_op[n++] = true;
            continue;
        }
        tok[n] = w;
        is_op[n++] = false;
        while (*p && !isspace_c(*p) && !strchr("|<>&;", *p)) {
            if (*p == '"' || *p == '\'') {
                char q = *p++;
                while (*p && *p != q) *w++ = *p++;
                if (*p) p++;
            } else {
                *w++ = *p++;
            }
        }
        *w++ = 0;
    }
    return n;
}

static void run_child(struct cmd *c)
{
    if (c->in) {
        int fd = open(c->in, O_RDONLY);
        if (fd < 0) {
            fprintf(2, "sh: %s: %s\n", c->in, strerror(fd));
            exit(1);
        }
        dup2(fd, 0);
        close(fd);
    }
    if (c->out) {
        int fd = open(c->out, O_WRONLY | O_CREATE | (c->append ? O_APPEND : O_TRUNC));
        if (fd < 0) {
            fprintf(2, "sh: %s: %s\n", c->out, strerror(fd));
            exit(1);
        }
        dup2(fd, 1);
        close(fd);
    }
    char path[128];
    const char *prog = c->argv[0];
    if (!strchr(prog, '/')) {
        snprintf(path, sizeof path, "/bin/%s", prog);
        prog = path;
    }
    int r = exec(prog, c->argv);
    fprintf(2, "sh: %s: %s\n", c->argv[0], strerror(r));
    exit(127);
}

static bool builtin(struct cmd *c)
{
    char *name = c->argv[0];
    if (strcmp(name, "cd") == 0) {
        int r = chdir(c->argc > 1 ? c->argv[1] : "/");
        if (r < 0) fprintf(2, "cd: %s: %s\n", c->argc > 1 ? c->argv[1] : "/", strerror(r));
        last_status = r < 0;
        return true;
    }
    if (strcmp(name, "exit") == 0) exit(c->argc > 1 ? atoi(c->argv[1]) : 0);
    if (strcmp(name, "jobs") == 0) {
        for (int j = 0; j < MAXJOBS; j++)
            if (jobs[j].npids) printf("[%d] Running  %s\n", jobs[j].id, jobs[j].text);
        return true;
    }
    if (strcmp(name, "wait") == 0) {
        for (int j = 0; j < MAXJOBS; j++)
            for (int k = 0; k < jobs[j].npids; k++)
                if (jobs[j].pids[k] > 0) waitpid(jobs[j].pids[k], 0, 0);
        report_done(false);
        for (int j = 0; j < MAXJOBS; j++) jobs[j].npids = 0;
        return true;
    }
    return false;
}

static void run_pipeline(struct cmd *cmds, int n, bool background, const char *text)
{
    if (n == 1 && !background && builtin(&cmds[0])) return;
    int pids[MAXCMDS], npids = 0, prev = -1;
    for (int i = 0; i < n; i++) {
        int pfd[2] = {-1, -1};
        if (i < n - 1 && pipe(pfd) < 0) {
            fprintf(2, "sh: pipe failed\n");
            break;
        }
        int pid = fork();
        if (pid < 0) {
            fprintf(2, "sh: fork: %s\n", strerror(pid));
            if (pfd[0] >= 0) { close(pfd[0]); close(pfd[1]); }
            break;
        }
        if (pid == 0) {
            if (prev >= 0) {
                dup2(prev, 0);
                close(prev);
            }
            if (pfd[1] >= 0) {
                close(pfd[0]);
                dup2(pfd[1], 1);
                close(pfd[1]);
            }
            run_child(&cmds[i]);
        }
        pids[npids++] = pid;
        if (prev >= 0) close(prev);
        if (pfd[1] >= 0) close(pfd[1]);
        prev = pfd[0];
    }
    if (prev >= 0) close(prev);
    if (background) {
        for (int j = 0; j < MAXJOBS; j++)
            if (!jobs[j].npids) {
                jobs[j].id = next_job++;
                memcpy(jobs[j].pids, pids, sizeof pids);
                jobs[j].npids = jobs[j].running = npids;
                strlcpy(jobs[j].text, text, sizeof jobs[j].text);
                printf("[%d] %d\n", jobs[j].id, pids[npids - 1]);
                return;
            }
        fprintf(2, "sh: too many jobs\n");
        return;
    }
    for (int i = 0; i < npids; i++) {
        int status = 0;
        waitpid(pids[i], &status, 0);
        if (i == npids - 1) last_status = status;
    }
}

static void run_line(char *line)
{
    char copy[LINE];
    strlcpy(copy, line, sizeof copy);
    char *nl = strchr(copy, '\n');
    if (nl) *nl = 0;
    char words[LINE];
    char *tok[128];
    bool op[128];
    int nt = tokenize(line, words, tok, op, 128);
    struct cmd cmds[MAXCMDS];
    int ncmd = 0;
    memset(cmds, 0, sizeof cmds);
    for (int i = 0; i <= nt; i++) {
        bool end = i == nt || (op[i] && (tok[i][0] == '&' || tok[i][0] == ';'));
        if (end) {
            if (cmds[ncmd].argc > 0) ncmd++;
            if (ncmd > 0) run_pipeline(cmds, ncmd, i < nt && tok[i][0] == '&', copy);
            ncmd = 0;
            memset(cmds, 0, sizeof cmds);
            continue;
        }
        struct cmd *c = &cmds[ncmd];
        if (op[i] && tok[i][0] == '|') {
            if (c->argc == 0 || ncmd == MAXCMDS - 1) {
                fprintf(2, "sh: syntax error near |\n");
                return;
            }
            ncmd++;
        } else if (op[i]) {
            if (i + 1 >= nt || op[i + 1]) {
                fprintf(2, "sh: missing file name after %s\n", tok[i]);
                return;
            }
            if (tok[i][0] == '<') c->in = tok[++i];
            else {
                c->append = tok[i][1] == '>';
                c->out = tok[++i];
            }
        } else if (c->argc < MAXARGS) {
            c->argv[c->argc++] = tok[i];
        }
    }
}

int main(int argc, char **argv)
{
    int fd = 0;
    bool interactive = true;
    if (argc > 1) {
        fd = open(argv[1], O_RDONLY);
        if (fd < 0) {
            fprintf(2, "sh: %s: %s\n", argv[1], strerror(fd));
            exit(1);
        }
        interactive = false;
    }
    char line[LINE];
    for (;;) {
        report_done(interactive);
        if (interactive) write(1, "$ ", 2);
        int n = readline(fd, line, sizeof line);
        if (n == 0) break;
        run_line(line);
    }
    exit(last_status);
}
