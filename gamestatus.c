#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* Fixed storage, no persistent child, no shell evaluation of input. */
typedef struct { char active[8], game[2048]; long long updated; } State;
static volatile sig_atomic_t running = 1;
static void stop(int sig) { (void)sig; running = 0; }
static State unknown(void) { State s = {"unknown", "", 0}; return s; }
static double monotime(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}
static int same(const State *a, const State *b) {
    return !strcmp(a->active, b->active) && !strcmp(a->game, b->game);
}
static int title(char *out, const char *in) {
    size_t n = 0;
    if (*in++ != '"') return 0;
    while (*in && *in != '"') {
        unsigned char c = (unsigned char)*in++;
        if (c == '\\') {
            if (!*in || !strchr("\\\"$`", *in)) return 0;
            c = (unsigned char)*in++;
        }
        if (c < 32 || c == 127 || n >= 2047) return 0;
        out[n++] = (char)c;
    }
    out[n] = 0;
    return *in == '"' && in[1] == 0;
}
static State read_state(const char *path) {
    State s = unknown();
    FILE *f = fopen(path, "r");
    if (!f) return s;
    char line[4096];
    unsigned fields = 0;
    int valid = 1;
    while (fgets(line, sizeof line, f)) {
        char *end = strchr(line, '\n');
        if (!end) { valid = 0; break; }
        *end = 0;
        if (end > line && end[-1] == '\r') end[-1] = 0;
        if (!strncmp(line, "active=", 7) && !(fields & 1)) {
            const char *v = line + 7;
            if (strcmp(v, "true") && strcmp(v, "false") && strcmp(v, "unknown")) { valid = 0; break; }
            strcpy(s.active, v); fields |= 1;
        } else if (!strncmp(line, "game=", 5) && !(fields & 2)) {
            if (!title(s.game, line + 5)) { valid = 0; break; }
            fields |= 2;
        } else if (!strncmp(line, "updated_at=", 11) && !(fields & 4)) {
            char *tail;
            errno = 0;
            s.updated = strtoll(line + 11, &tail, 10);
            if (errno || tail == line + 11 || *tail || s.updated <= 0) { valid = 0; break; }
            fields |= 4;
        } else { valid = 0; break; }
    }
    if (ferror(f)) valid = 0;
    fclose(f);
    long long now = (long long)time(NULL);
    if (!valid || fields != 7 || s.updated > now || now - s.updated > 90) return unknown();
    if (strcmp(s.active, "true")) s.game[0] = 0;
    return s;
}
static int publish(const char *path, const State *s) {
    char tmp[4096];
    if (snprintf(tmp, sizeof tmp, "%s.tmp.XXXXXX", path) >= (int)sizeof tmp) return -1;
    int fd = mkstemp(tmp);
    if (fd < 0) return -1;
    FILE *out = fdopen(fd, "w");
    if (!out) { close(fd); unlink(tmp); return -1; }
    fprintf(out, "active=%s\ngame=\"", s->active);
    for (const char *p = s->game; *p; p++) {
        if (strchr("\\\"$`", *p)) fputc('\\', out);
        fputc((unsigned char)*p, out);
    }
    fprintf(out, "\"\nupdated_at=%lld\ncached_at=%lld\n", s->updated, (long long)time(NULL));
    int bad = ferror(out);
    if (fclose(out)) bad = 1;
    if (!bad && rename(tmp, path) == 0) return 0;
    unlink(tmp); return -1;
}
static pid_t event(const State *s, const State *prev, const char *hook) {
    pid_t pid = fork();
    if (pid != 0) { if (pid > 0) (void)setpgid(pid, pid); return pid; }
    (void)setpgid(0, 0);
    signal(SIGTERM, SIG_DFL); signal(SIGINT, SIG_DFL);
    if (hook) {
        execl("/bin/sh", "sh", hook, s->active, s->game, prev->active, (char *)NULL);
        perror("event hook"); _exit(127);
    }
    _exit(0);
}
int main(int argc, char **argv) {
    const char *hook = NULL;
    if (argc < 3) goto usage;
    for (int i = 3; i < argc; i++) {
        if (!strcmp(argv[i], "--hook") && i + 1 < argc) hook = argv[++i];
        else goto usage;
    }
    struct stat src, dst;
    /* A symlink output is replaced by rename, never written through. Reject
       identical names and hard links; allow migration from the old symlink. */
    if (!strcmp(argv[1], argv[2]) ||
        (!stat(argv[1], &src) && !lstat(argv[2], &dst) &&
         !S_ISLNK(dst.st_mode) && src.st_dev == dst.st_dev && src.st_ino == dst.st_ino)) {
        fprintf(stderr, "Source and cache must be different files\n"); return 2;
    }
    char lockpath[4096];
    if (snprintf(lockpath, sizeof lockpath, "%s.lock", argv[2]) >= (int)sizeof lockpath) return 2;
    int lock = open(lockpath, O_CREAT | O_RDWR | O_CLOEXEC, 0600);
    if (lock < 0 || flock(lock, LOCK_EX | LOCK_NB)) { perror("cache lock"); return 1; }
    struct sigaction sa = {0};
    sa.sa_handler = stop; sigemptyset(&sa.sa_mask);
    sigaction(SIGTERM, &sa, NULL); sigaction(SIGINT, &sa, NULL);
    State delivered = unknown();
    delivered.active[0] = 0;
    pid_t worker = 0;
    double started = 0;
    int result = 0;
    while (running) {
        State current = read_state(argv[1]);
        /* Refresh local liveness every poll, including while source is unknown. */
        if (publish(argv[2], &current)) { perror("publish cache"); result = 1; break; }
        if (worker > 0) {
            int status;
            pid_t done = waitpid(worker, &status, WNOHANG);
            if (done == worker) {
                /* Remove any background descendants of a completed hook. */
                kill(-worker, SIGKILL);
                if (!WIFEXITED(status) || WEXITSTATUS(status)) fprintf(stderr, "Event hook failed\n");
                worker = 0;
            } else if (monotime() - started >= 30) {
                kill(-worker, SIGKILL);
                while (waitpid(worker, NULL, 0) < 0 && errno == EINTR) {}
                fprintf(stderr, "Event hook timed out\n"); worker = 0;
            }
        }
        if (!worker && !same(&current, &delivered)) {
            if (hook) {
                worker = event(&current, &delivered, hook);
                if (worker < 0) { perror("fork event"); worker = 0; }
                else { delivered = current; started = monotime(); }
            } else delivered = current;
        }
        struct timespec delay = {2, 0};
        while (running && nanosleep(&delay, &delay) < 0 && errno == EINTR) {}
    }
    if (worker > 0) {
        kill(-worker, SIGKILL);
        while (waitpid(worker, NULL, 0) < 0 && errno == EINTR) {}
    }
    State final = unknown();
    if (publish(argv[2], &final)) result = 1;
    close(lock);
    return result;
usage:
    fprintf(stderr, "Usage: %s SOURCE CACHE [--hook SCRIPT]\n", argv[0]);
    return 2;
}
