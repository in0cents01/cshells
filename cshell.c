/* cshell - a small, fast POSIX-ish shell in C.
 *
 * Features:
 *   - word splitting, single/double quotes, backslash escapes
 *   - globbing (* ? [ ])
 *   - redirection: <  >  >>  2>  2>&1
 *   - pipelines: cmd | cmd | cmd
 *   - lists: ;  &&  ||  and background &
 *   - variable assignment prefix: FOO=bar cmd
 *   - builtins: cd  exit  echo  history  jobs  wait
 *   - interactive prompts, Ctrl-C handling, in-memory history (!!, !N)
 *   - script mode: ./cshell script.sh
 */

#define _GNU_SOURCE
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

/* Defined in pipe_shim.S so the optimizer cannot fold it into a no-op. */
extern int cs_pipe(int fds[2]);

#define CS_MAX_INPUT 8192
#define MAX_ARGS  2048
#define MAX_PIPE  64
#define MAX_JOBS  256
#define MAX_HIST  512

static int   g_interactive;
static int   g_status;
static char  g_prev_cwd[PATH_MAX];
static pid_t g_jobs[MAX_JOBS];
static int   g_job_count;
static char *g_hist[MAX_HIST];
static int   g_hist_count;

static void  run_list(char **args, int n);
static int   run_segment(char **args, int n, int bg);
static int   run_pipeline(char **args, int n);
static int   tokenize(const char *line, char **args, int max);
static char *read_stream(FILE *f);
static char *read_line(FILE *f);
static void  run_source(FILE *f);
static void  run_stdin_loop(void);
static void  expand_globs(char **args, int *n);
static int   is_op(const char *s);
static void  print_prompt(void);
static void  on_sigint(int sig);
static void  reap_jobs(void);
static int   builtin(char **args, int n);
static int   bi_cd(char **a, int n);
static int   bi_echo(char **a, int n);
static int   bi_history(void);
static int   bi_jobs(void);
static int   bi_wait(void);
static void  hist_add(const char *line);
static int   hist_expand(const char *line, char **out);
static char *xstrdup(const char *s);
static void  close_fd(int fd);

int main(int argc, char **argv)
{
    struct sigaction sa;

    g_interactive = isatty(STDIN_FILENO);
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_sigint;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    sigaction(SIGINT, &sa, NULL);
    signal(SIGQUIT, SIG_IGN);
    signal(SIGTSTP, SIG_IGN);

    if (argc > 1) {
        FILE *f = fopen(argv[1], "r");

        if (!f) {
            fprintf(stderr, "cshell: %s: %s\n", argv[1], strerror(errno));
            return 1;
        }
        run_source(f);
        fclose(f);
        return g_status;
    }

    if (!g_interactive) {
        run_source(stdin);
        return g_status;
    }

    run_stdin_loop();
    return g_status;
}

static void run_stdin_loop(void)
{
    char *line;

    while ((line = read_line(stdin)) != NULL) {
        char *expanded = NULL;
        if (!line[0]) { free(line); continue; }
        if (hist_expand(line, &expanded)) {
            printf("%s\n", expanded);
            hist_add(expanded);
            if (strlen(expanded) >= CS_MAX_INPUT) {
                fprintf(stderr, "cshell: history expansion too long\n");
                free(expanded);
                free(line);
                continue;
            }
            free(line);
            line = expanded;
            hist_add(line);
        } else {
            hist_add(line);
        }
        char **args = malloc(sizeof(char *) * MAX_ARGS);
        if (!args) { free(line); break; }
        int n = tokenize(line, args, MAX_ARGS);
        if (n > 0) {
            expand_globs(args, &n);
            run_list(args, n);
        }
        for (int i = 0; i < n; i++) free(args[i]);
        free(args);
        free(line);
    }
    putchar('\n');
}

/* Read an entire stream into a malloc'd, NUL-terminated buffer. */
static char *read_stream(FILE *f)
{
    size_t cap = 8192, len = 0;
    char *buf = malloc(cap);
    if (!buf) return NULL;
    for (;;) {
        if (cap - len < 2) {
            cap *= 2;
            char *nb = realloc(buf, cap);
            if (!nb) { free(buf); return NULL; }
            buf = nb;
        }
        size_t r = fread(buf + len, 1, cap - len - 1, f);
        len += r;
        if (r == 0) break;
    }
    buf[len] = '\0';
    return buf;
}

/* Read one line from a stream. Returns a malloc'd string without the
 * trailing newline, or NULL at EOF. */
static char *read_line(FILE *f)
{
    size_t cap = 256, len = 0;
    char *buf = malloc(cap);
    int c;

    if (!buf) return NULL;
    while ((c = fgetc(f)) != EOF) {
        if (c == '\n') break;
        if (len + 2 > cap) {
            cap *= 2;
            char *nb = realloc(buf, cap);
            if (!nb) { free(buf); return NULL; }
            buf = nb;
        }
        buf[len++] = (char)c;
    }
    if (c == EOF && len == 0) { free(buf); return NULL; }
    while (len > 0 && (buf[len - 1] == '\r' || buf[len - 1] == ' ' ||
                       buf[len - 1] == '\t'))
        buf[--len] = '\0';
    buf[len] = '\0';
    return buf;
}

/* Execute every non-empty line read from a stream. */
static void run_source(FILE *f)
{
    char *line;

    while ((line = read_line(f)) != NULL) {
        if (line[0]) {
            char **args = malloc(sizeof(char *) * MAX_ARGS);
            if (args) {
                int n = tokenize(line, args, MAX_ARGS);
                if (n > 0) {
                    expand_globs(args, &n);
                    run_list(args, n);
                }
                for (int i = 0; i < n; i++) free(args[i]);
                free(args);
            }
        }
        free(line);
    }
}

static void print_prompt(void)
{
    if (!g_interactive) return;
    char cwd[PATH_MAX];
    const char *home = getenv("HOME");
    if (!getcwd(cwd, sizeof(cwd))) strcpy(cwd, "?");
    if (home && *home && strncmp(cwd, home, strlen(home)) == 0)
        printf("~%s$ ", cwd + strlen(home));
    else
        printf("%s$ ", cwd);
    fflush(stdout);
}

static void on_sigint(int sig)
{
    (void)sig;
    if (g_interactive) {
        putchar('\n');
        print_prompt();
    }
}

static char *xstrdup(const char *s)
{
    size_t n = strlen(s) + 1;
    char *d = malloc(n);
    if (d) memcpy(d, s, n);
    return d;
}

static void close_fd(int fd)
{
    if (fd > STDERR_FILENO) close(fd);
}

static int is_op(const char *s)
{
    return !strcmp(s, "|") || !strcmp(s, "||") || !strcmp(s, "&&") ||
           !strcmp(s, "&") || !strcmp(s, ";") || !strcmp(s, "<") ||
           !strcmp(s, ">") || !strcmp(s, ">>") || !strcmp(s, "2>") ||
           !strcmp(s, "2>&1");
}

/* Split line into tokens. Operators are their own tokens even without spaces. */
static int tokenize(const char *line, char **args, int max)
{
    int n = 0;
    const char *p = line;

#define TOK_PUSH()                                                          \
    do {                                                                    \
        tok[tl] = '\0';                                                     \
        if (n < max - 1) args[n++] = tok;                                   \
        else { free(tok); return n; }                                       \
        tok = NULL;                                                         \
    } while (0)

    while (*p) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;

        size_t tl = 0, tc = 16;
        char *tok = malloc(tc);
        int has_quote = 0;
        if (!tok) break;

#define TOK_PUT(ch)                                                         \
    do {                                                                    \
        if (tl + 2 > tc) {                                                  \
            tc *= 2;                                                        \
            char *nb = realloc(tok, tc);                                    \
            if (!nb) { free(tok); return n; }                               \
            tok = nb;                                                       \
        }                                                                   \
        tok[tl++] = (ch);                                                   \
    } while (0)

        while (*p && *p != ' ' && *p != '\t') {
            char c = *p;

            if (c == '\'' || c == '"') {
                char q = c;
                has_quote = 1;
                p++;
                while (*p && *p != q) {
                    /* Backslash is literal in single quotes. In double
                     * quotes it only escapes " \ $ ` and newline. */
                    if (q == '"' && *p == '\\' &&
                        (p[1] == '"' || p[1] == '\\' || p[1] == '$' ||
                         p[1] == '`' || p[1] == '\n')) {
                        p++;
                        if (*p == '\n') { p++; continue; }
                        TOK_PUT(*p++);
                    } else {
                        TOK_PUT(*p++);
                    }
                }
                if (*p == q) p++;
                continue;
            }
            if (c == '\\' && p[1]) {
                p++;
                if (*p == '\n') { p++; continue; }   /* line continuation */
                TOK_PUT(*p++);
                continue;
            }
            if (c == '|' || c == ';' || c == '&' || c == '<' || c == '>') {
                if (c == '>' && tl == 1 && tok[0] == '2') {
                    /* 2> or 2>&1 */
                    if (p[1] == '&' && p[2] == '1') {
                        p += 3;
                        TOK_PUT('>');
                        TOK_PUT('&');
                        TOK_PUT('1');
                        TOK_PUSH();
                        goto next_token;
                    }
                    p++;
                    TOK_PUT('>');
                    TOK_PUSH();
                    goto next_token;
                }
                if (tl > 0) TOK_PUSH();
                if (c == '|' && p[1] == '|') {
                    if (n < max - 1) args[n++] = xstrdup("||");
                    p += 2;
                } else if (c == '&' && p[1] == '&') {
                    if (n < max - 1) args[n++] = xstrdup("&&");
                    p += 2;
                } else if (c == '>' && p[1] == '>') {
                    if (n < max - 1) args[n++] = xstrdup(">>");
                    p += 2;
                } else {
                    char op[2] = { c, 0 };
                    if (n < max - 1) args[n++] = xstrdup(op);
                    p++;
                }
                goto next_token;
            }
            TOK_PUT(*p++);
        }

        if (tl > 0 || has_quote) TOK_PUSH();
        else free(tok);
next_token:
        continue;
#undef TOK_PUT
#undef TOK_PUSH
    }
    args[n] = NULL;
    return n;
}

static void expand_globs(char **args, int *n)
{
    char *out[MAX_ARGS];
    int m = 0;

    for (int i = 0; i < *n; i++) {
        char *s = args[i];
        if (!strpbrk(s, "*?[")) {
            out[m++] = s;
            continue;
        }
        glob_t g;
        memset(&g, 0, sizeof(g));
        if (glob(s, 0, NULL, &g) == 0 && g.gl_pathc > 0) {
            for (size_t k = 0; k < g.gl_pathc && m < MAX_ARGS - 1; k++)
                out[m++] = xstrdup(g.gl_pathv[k]);
            globfree(&g);
            free(s);
        } else {
            globfree(&g);
            out[m++] = s;
        }
    }
    out[m] = NULL;
    *n = m;
    memcpy(args, out, sizeof(char *) * (size_t)(m + 1));
}

static void reap_jobs(void)
{
    int st;
    pid_t pid;
    while ((pid = waitpid(-1, &st, WNOHANG)) > 0) {
        for (int i = 0; i < g_job_count; i++) {
            if (g_jobs[i] == pid) {
                g_jobs[i] = g_jobs[--g_job_count];
                break;
            }
        }
    }
}

/* status of a whole list: handles ; && || & */
static void run_list(char **args, int n)
{
    int i = 0;
    int status = 0;
    int pending = 0; /* 1 = &&, 2 = || */

    while (i < n) {
        int j = i;
        while (j < n && !is_op(args[j])) j++;

        int bg = (j < n && !strcmp(args[j], "&"));
        if (j > i) {
            int run = 1;
            if (pending == 1 && status != 0) run = 0;
            if (pending == 2 && status == 0) run = 0;
            if (run) status = run_segment(args + i, j - i, bg);
        }
        if (j >= n) break;

        if (!strcmp(args[j], "&&")) pending = 1;
        else if (!strcmp(args[j], "||")) pending = 2;
        else pending = 0;
        i = j + 1;
    }
    g_status = status;
}

static int run_segment(char **args, int n, int bg)
{
    /* leading NAME=value assignments */
    int first = 0;
    while (first < n) {
        char *eq = strchr(args[first], '=');
        if (!eq || eq == args[first]) break;
        int ok = 1;
        for (char *q = args[first]; q < eq; q++)
            if (!isalnum((unsigned char)*q) && *q != '_') { ok = 0; break; }
        if (!ok) break;
        setenv(args[first], eq + 1, 1);
        first++;
    }
    if (first > 0) {
        args += first;
        n -= first;
    }
    if (n <= 0) return 0;

    reap_jobs();

    if (bg) {
        pid_t pid = fork();
        if (pid < 0) {
            perror("fork");
            return 1;
        }
        if (pid == 0) {
            signal(SIGINT, SIG_DFL);
            signal(SIGQUIT, SIG_DFL);
            int st = run_segment(args, n, 0);
            _exit(st & 0xff);
        }
        if (g_job_count < MAX_JOBS) g_jobs[g_job_count++] = pid;
        printf("[%d] %d\n", g_job_count, (int)pid);
        return 0;
    }

    if (builtin(args, n)) return g_status;
    return run_pipeline(args, n);
}

static int run_pipeline(char **args, int n)
{
    int pipes[MAX_PIPE][2];
    pid_t pids[MAX_PIPE];
    int start = 0, seg = 0, status = 0;

    for (int k = 0; k < MAX_PIPE; k++) {
        pipes[k][0] = -1;
        pipes[k][1] = -1;
    }

    for (int i = 0; i <= n; i++) {
        if (i != n && strcmp(args[i], "|") != 0) continue;

        char *cmd[MAX_ARGS];
        int ci = 0;
        char *in_file = NULL, *out_file = NULL, *err_file = NULL;
        int append = 0, err_to_out = 0;

        for (int j = start; j < i; j++) {
            if (!strcmp(args[j], "<") && j + 1 < i) {
                in_file = args[++j];
            } else if (!strcmp(args[j], ">") && j + 1 < i) {
                out_file = args[++j];
                append = 0;
            } else if (!strcmp(args[j], ">>") && j + 1 < i) {
                out_file = args[++j];
                append = 1;
            } else if (!strcmp(args[j], "2>") && j + 1 < i) {
                err_file = args[++j];
            } else if (!strcmp(args[j], "2>&1")) {
                err_to_out = 1;
            } else {
                cmd[ci++] = args[j];
            }
        }
        if (ci == 0) { start = i + 1; continue; }
        cmd[ci] = NULL;

        int has_next = (i < n);
        if (has_next && cs_pipe(pipes[seg]) < 0) {
            perror("pipe");
            return 1;
        }

        pid_t pid = fork();
        if (pid < 0) {
            perror("fork");
            return 1;
        }
        if (pid == 0) {
            signal(SIGINT, SIG_DFL);
            signal(SIGQUIT, SIG_DFL);

            if (seg > 0) dup2(pipes[seg - 1][0], STDIN_FILENO);
            if (has_next) dup2(pipes[seg][1], STDOUT_FILENO);

            if (in_file) {
                int fd = open(in_file, O_RDONLY);
                if (fd < 0) {
                    fprintf(stderr, "cshell: %s: %s\n", in_file, strerror(errno));
                    _exit(127);
                }
                dup2(fd, STDIN_FILENO);
                close(fd);
            }
            if (out_file) {
                int flags = O_WRONLY | O_CREAT | (append ? O_APPEND : O_TRUNC);
                int fd = open(out_file, flags, 0644);
                if (fd < 0) {
                    fprintf(stderr, "cshell: %s: %s\n", out_file, strerror(errno));
                    _exit(127);
                }
                dup2(fd, STDOUT_FILENO);
                close(fd);
            }
            if (err_file) {
                int fd = open(err_file, O_WRONLY | O_CREAT | O_TRUNC, 0644);
                if (fd < 0) {
                    fprintf(stderr, "cshell: %s: %s\n", err_file, strerror(errno));
                    _exit(127);
                }
                dup2(fd, STDERR_FILENO);
                close(fd);
            }
            if (err_to_out) dup2(STDOUT_FILENO, STDERR_FILENO);

            /* Close every pipe descriptor this child inherited. */
            for (int k = 0; k < MAX_PIPE; k++) {
                close_fd(pipes[k][0]);
                close_fd(pipes[k][1]);
            }

            int bi = builtin(cmd, ci);
            if (bi == 1) {
                /* cd inside a pipeline cannot affect the parent */
                _exit(bi_cd(cmd, ci));
            }
            if (bi == 2) _exit(0);
            execvp(cmd[0], cmd);
            fprintf(stderr, "cshell: %s: %s\n", cmd[0], strerror(errno));
            _exit(errno == ENOENT ? 127 : 126);
        }

        pids[seg] = pid;
        if (seg > 0) close(pipes[seg - 1][0]);   /* my input end */
        if (has_next) close(pipes[seg][1]);      /* my output end */
        seg++;
        start = i + 1;
    }
    if (seg > 0) close(pipes[seg - 1][0]);

    for (int k = 0; k < seg; k++) {
        int st = 0;
        waitpid(pids[k], &st, 0);
        if (k == seg - 1) {
            if (WIFEXITED(st)) status = WEXITSTATUS(st);
            else if (WIFSIGNALED(st)) status = 128 + WTERMSIG(st);
            else status = 1;
        }
    }
    g_status = status;
    return status;
}

/* returns: 0 = not a builtin, 1 = ran, 2 = exit */
static int builtin(char **args, int n)
{
    if (!args[0]) return 0;
    if (!strcmp(args[0], "cd"))     { g_status = bi_cd(args, n); return 1; }
    if (!strcmp(args[0], "echo"))   { g_status = bi_echo(args, n); return 1; }
    if (!strcmp(args[0], "history")){ g_status = bi_history(); return 1; }
    if (!strcmp(args[0], "jobs"))   { g_status = bi_jobs(); return 1; }
    if (!strcmp(args[0], "wait"))   { g_status = bi_wait(); return 1; }
    if (!strcmp(args[0], "exit")) {
        int code = (n > 1) ? atoi(args[1]) : g_status;
        exit(code & 0xff);
    }
    return 0;
}

static int bi_cd(char **a, int n)
{
    const char *target = (n > 1) ? a[1] : getenv("HOME");
    if (!target || !*target) {
        fprintf(stderr, "cshell: cd: missing operand\n");
        return 1;
    }
    if (!strcmp(target, "-")) {
        if (!g_prev_cwd[0]) {
            fprintf(stderr, "cshell: cd: no previous directory\n");
            return 1;
        }
        target = g_prev_cwd;
        printf("%s\n", target);
    }
    if (!getcwd(g_prev_cwd, sizeof(g_prev_cwd))) g_prev_cwd[0] = '\0';
    if (chdir(target) != 0) {
        fprintf(stderr, "cshell: cd: %s: %s\n", target, strerror(errno));
        return 1;
    }
    return 0;
}

static int bi_echo(char **a, int n)
{
    int i = 1, nl = 1;
    if (i < n && !strcmp(a[i], "-n")) { nl = 0; i++; }
    for (; i < n; i++) {
        fputs(a[i], stdout);
        if (i + 1 < n) putchar(' ');
    }
    if (nl) putchar('\n');
    return 0;
}

static int bi_history(void)
{
    for (int i = 0; i < g_hist_count; i++)
        printf("%4d  %s\n", i + 1, g_hist[i]);
    return 0;
}

static int bi_jobs(void)
{
    reap_jobs();
    for (int i = 0; i < g_job_count; i++)
        printf("[%d]  running  %d\n", i + 1, (int)g_jobs[i]);
    return 0;
}

static int bi_wait(void)
{
    int st;
    while (g_job_count > 0) {
        pid_t pid = waitpid(g_jobs[0], &st, 0);
        if (pid < 0) break;
        for (int i = 0; i < g_job_count; i++)
            if (g_jobs[i] == pid) { g_jobs[i] = g_jobs[--g_job_count]; break; }
    }
    return 0;
}

static void hist_add(const char *line)
{
    if (g_hist_count >= MAX_HIST) {
        free(g_hist[0]);
        memmove(g_hist, g_hist + 1, sizeof(char *) * (size_t)(MAX_HIST - 1));
        g_hist_count--;
    }
    g_hist[g_hist_count++] = xstrdup(line);
}

static int hist_expand(const char *line, char **out)
{
    if (line[0] != '!') return 0;
    if (!line[1]) return 0;
    if (line[1] == '!') {
        if (g_hist_count == 0) return 0;
        *out = xstrdup(g_hist[g_hist_count - 1]);
        return 1;
    }
    if (isdigit((unsigned char)line[1])) {
        char *end = NULL;
        long idx = strtol(line + 1, &end, 10);
        if (*end) return 0;
        if (idx < 1 || idx > g_hist_count) {
            fprintf(stderr, "cshell: history: !%ld: not found\n", idx);
            *out = xstrdup("");
            return 1;
        }
        *out = xstrdup(g_hist[idx - 1]);
        return 1;
    }
    return 0;
}
