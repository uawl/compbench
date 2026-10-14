#define _GNU_SOURCE
#include "ast.h"

#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <math.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#if !defined(__linux__) || !defined(__x86_64__)
#error "compbench requires Linux x86-64"
#endif

#define MAX_CASES 256
#define MAX_TASKS 128
#define SAMPLES 7
#define REF_BUDGET UINT64_C(20000000)

typedef uint64_t (*Entry)(const uint64_t *, uint64_t);
typedef struct {
    uint64_t *xs, n;
    uint64_t expected;
    unsigned pattern;
} Case;
typedef struct {
    int status, case_index;
    unsigned checks;
    uint64_t expected, actual, calls;
    double ns;
    char error[256];
} Packet;
typedef struct {
    char name[256], path[PATH_MAX];
    int level;
    uint64_t length;
} Task;
typedef struct {
    int passed;
    Packet candidate, baseline;
    char error[256];
} Result;

static double now_ns(void) {
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t)) { perror("clock_gettime"); exit(2); }
    return (double)t.tv_sec * 1e9 + (double)t.tv_nsec;
}

static uint64_t random64(uint64_t *state) {
    uint64_t z = (*state += UINT64_C(0x9e3779b97f4a7c15));
    z = (z ^ (z >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
    z = (z ^ (z >> 27)) * UINT64_C(0x94d049bb133111eb);
    return z ^ (z >> 31);
}

static void fill(uint64_t *xs, uint64_t n, unsigned pattern, uint64_t *state) {
    static const uint64_t edges[] = {
        0, 1, 2, 63, 64, 65, UINT64_MAX, UINT64_MAX - 1,
        UINT64_C(0x8000000000000000), UINT64_C(0x7fffffffffffffff)
    };
    for (uint64_t i = 0; i < n; ++i) {
        switch (pattern) {
        case 0: xs[i] = 0; break;
        case 1: xs[i] = 1; break;
        case 2: xs[i] = UINT64_MAX; break;
        case 3: xs[i] = UINT64_C(0x8000000000000000); break;
        case 4: xs[i] = edges[i % (sizeof(edges) / sizeof(edges[0]))]; break;
        case 5: xs[i] = i; break;
        case 6: xs[i] = random64(state); break;
        default: xs[i] = i % 3 ? random64(state) : 0; break;
        }
    }
}

static void limits(unsigned seconds) {
    struct rlimit cpu = {seconds, seconds + 1};
    struct rlimit memory = {UINT64_C(1024) * 1024 * 1024, UINT64_C(1024) * 1024 * 1024};
    struct rlimit core = {0, 0};
    (void)setrlimit(RLIMIT_CPU, &cpu);
    (void)setrlimit(RLIMIT_AS, &memory);
    (void)setrlimit(RLIMIT_CORE, &core);
}

/* Every child owns a process group, so timeouts also stop its subprocesses. */
static int wait_child(pid_t pid, double timeout, char *error, size_t size) {
    double deadline = now_ns() + timeout * 1e9;
    int status;
    for (;;) {
        pid_t r = waitpid(pid, &status, WNOHANG);
        if (r == pid) break;
        if (r < 0 && errno != EINTR) {
            snprintf(error, size, "waitpid: %s", strerror(errno));
            (void)kill(-pid, SIGKILL); (void)kill(pid, SIGKILL);
            return 0;
        }
        if (now_ns() >= deadline) {
            (void)kill(-pid, SIGKILL); (void)kill(pid, SIGKILL);
            while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
            snprintf(error, size, "timeout after %.1f seconds", timeout);
            return 0;
        }
        struct timespec delay = {.tv_nsec = 1000000};
        nanosleep(&delay, NULL);
    }
    (void)kill(-pid, SIGKILL);
    if (WIFSIGNALED(status)) {
        if (WTERMSIG(status) == SIGXCPU)
            snprintf(error, size, "CPU timeout (signal %d)", WTERMSIG(status));
        else snprintf(error, size, "child terminated by signal %d", WTERMSIG(status));
        return 0;
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status)) {
        snprintf(error, size, "child exited with status %d", WIFEXITED(status) ? WEXITSTATUS(status) : -1);
        return 0;
    }
    return 1;
}

static int command(char *const argv[], const char *log, double timeout, char *error, size_t size) {
    fflush(NULL);
    pid_t pid = fork();
    if (pid < 0) { snprintf(error, size, "fork: %s", strerror(errno)); return 0; }
    if (!pid) {
        (void)setpgid(0, 0);
        limits((unsigned)timeout + 1);
        int fd = open(log, O_CREAT | O_TRUNC | O_WRONLY, 0600);
        if (fd < 0 || dup2(fd, STDOUT_FILENO) < 0 || dup2(fd, STDERR_FILENO) < 0) _exit(126);
        close(fd);
        int nullfd = open("/dev/null", O_RDONLY);
        if (nullfd >= 0) { (void)dup2(nullfd, STDIN_FILENO); close(nullfd); }
        execvp(argv[0], argv);
        perror("execvp");
        _exit(127);
    }
    (void)setpgid(pid, pid);
    int ok = wait_child(pid, timeout, error, size);
    if (!ok) {
        FILE *f = fopen(log, "r");
        if (f) {
            char detail[128] = {0};
            if (fgets(detail, sizeof(detail), f)) {
                detail[strcspn(detail, "\r\n")] = 0;
                size_t used = strlen(error);
                if (used < size) snprintf(error + used, size - used, ": %s", detail);
            }
            fclose(f);
        }
    }
    return ok;
}

static int read_manifest(const char *path, Task *tasks, char *error, size_t size) {
    FILE *f = fopen(path, "r");
    if (!f) { snprintf(error, size, "%s: %s", path, strerror(errno)); return -1; }
    char directory[PATH_MAX];
    snprintf(directory, sizeof(directory), "%s", path);
    char *slash = strrchr(directory, '/');
    if (slash) *slash = 0; else strcpy(directory, ".");
    if (!*directory) strcpy(directory, "/");
    char line[4096], relative[256], extra;
    int count = 0, line_number = 0;
    while (fgets(line, sizeof(line), f)) {
        ++line_number;
        char *start = line;
        while (*start == ' ' || *start == '\t') ++start;
        if (*start == '#' || *start == '\n' || !*start) continue;
        int level;
        uint64_t length;
        if (count == MAX_TASKS || sscanf(start, "%d %" SCNu64 " %255s %c", &level, &length, relative, &extra) != 3 ||
            level < 1 || level > 4 || length > 1048576) {
            snprintf(error, size, "invalid suite entry at line %d", line_number);
            fclose(f); return -1;
        }
        char full[PATH_MAX];
        int used = relative[0] == '/' ? snprintf(full, sizeof(full), "%s", relative) :
            snprintf(full, sizeof(full), "%s/%s", directory, relative);
        if (used < 0 || (size_t)used >= sizeof(full) || !realpath(full, tasks[count].path)) {
            snprintf(error, size, "cannot resolve AST at suite line %d", line_number);
            fclose(f); return -1;
        }
        snprintf(tasks[count].name, sizeof(tasks[count].name), "%s", relative);
        tasks[count].level = level; tasks[count].length = length;
        ++count;
    }
    int failed = ferror(f);
    fclose(f);
    if (failed || !count) { snprintf(error, size, "suite is empty or unreadable"); return -1; }
    return count;
}

static int add_case(Case *cases, int *count, const CbProgram *p, uint64_t n,
                    unsigned pattern, uint64_t *seed, char *error, size_t size) {
    if (*count == MAX_CASES) { snprintf(error, size, "too many test cases"); return 0; }
    Case *c = &cases[(*count)++];
    c->n = n; c->pattern = pattern;
    c->xs = calloc(n ? (size_t)n : 1, sizeof(uint64_t));
    if (!c->xs) { snprintf(error, size, "out of memory"); return 0; }
    fill(c->xs, n, pattern, seed);
    return cb_eval(p, c->xs, n, REF_BUDGET, &c->expected, error, size);
}

/* Arrays end immediately before a guard page and are read-only during execution. */
static void *guarded(const Case *c, void **mapping, size_t *bytes) {
    size_t page = (size_t)sysconf(_SC_PAGESIZE);
    size_t data = (size_t)c->n * sizeof(uint64_t);
    size_t pages = (data + page - 1) / page;
    if (!pages) pages = 1;
    *bytes = (pages + 2) * page;
    *mapping = mmap(NULL, *bytes, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (*mapping == MAP_FAILED) return NULL;
    char *start = (char *)*mapping + page;
    if (mprotect(start, pages * page, PROT_READ | PROT_WRITE)) {
        munmap(*mapping, *bytes); return NULL;
    }
    void *xs = start + pages * page - data;
    if (data) memcpy(xs, c->xs, data);
    if (mprotect(start, pages * page, PROT_READ)) { munmap(*mapping, *bytes); return NULL; }
    return xs;
}

static void pin_cpu(void) {
    cpu_set_t allowed;
    if (sched_getaffinity(0, sizeof(allowed), &allowed)) return;
    for (int i = 0; i < CPU_SETSIZE; ++i) if (CPU_ISSET(i, &allowed)) {
        cpu_set_t chosen;
        CPU_ZERO(&chosen); CPU_SET(i, &chosen);
        (void)sched_setaffinity(0, sizeof(chosen), &chosen);
        break;
    }
}

static volatile uint64_t sink;

static int check_value(uint64_t value, const Case *c, Packet *packet) {
    if (value == c->expected) return 1;
    packet->expected = c->expected; packet->actual = value;
    snprintf(packet->error, sizeof(packet->error), "wrong result (n=%" PRIu64 ", pattern=%u)", c->n, c->pattern);
    return 0;
}

static double measure(Entry entry, uint64_t **xs,
                      const Case *cases, int first, unsigned variants, uint64_t calls) {
    uint64_t checksum = 0;
    double begin = now_ns();
    for (uint64_t i = 0; i < calls; ++i) {
        unsigned j = (unsigned)(i % variants);
        checksum += entry(xs[j], cases[first + (int)j].n);
    }
    double elapsed = now_ns() - begin;
    sink = checksum;
    return elapsed;
}

static void worker(const char *library, const char *baseline_library, const Case *cases,
                   int count, int perf_first, Packet *packet, Packet *baseline) {
    limits(9);
    pin_cpu();
    void *handle = dlopen(library, RTLD_NOW | RTLD_LOCAL);
    if (!handle) { snprintf(packet->error, sizeof(packet->error), "dlopen: %s", dlerror()); return; }
    void *symbol = dlsym(handle, "bench_entry");
    if (!symbol) { snprintf(packet->error, sizeof(packet->error), "missing bench_entry"); dlclose(handle); return; }
    Entry entry;
    _Static_assert(sizeof(entry) == sizeof(symbol), "function pointer ABI");
    memcpy(&entry, &symbol, sizeof(entry));
    void *baseline_handle = dlopen(baseline_library, RTLD_NOW | RTLD_LOCAL);
    if (!baseline_handle) {
        snprintf(packet->error, sizeof(packet->error), "baseline dlopen: %s", dlerror());
        dlclose(handle); return;
    }
    symbol = dlsym(baseline_handle, "bench_entry");
    if (!symbol) {
        snprintf(packet->error, sizeof(packet->error), "missing baseline bench_entry");
        dlclose(baseline_handle); dlclose(handle); return;
    }
    Entry baseline_entry;
    memcpy(&baseline_entry, &symbol, sizeof(baseline_entry));
    uint64_t *inputs[MAX_CASES];
    void *maps[MAX_CASES];
    size_t bytes[MAX_CASES];
    int allocated = 0;
    for (int i = 0; i < count; ++i) {
        inputs[i] = guarded(&cases[i], &maps[i], &bytes[i]);
        if (!inputs[i]) { snprintf(packet->error, sizeof(packet->error), "guarded allocation failed"); goto done; }
        ++allocated;
    }
    for (int pass = 0; pass < 2; ++pass) for (int j = 0; j < count; ++j) {
        int i = pass ? count - 1 - j : j;
        uint64_t result = entry(inputs[i], cases[i].n);
        ++packet->checks;
        if (!check_value(result, &cases[i], packet)) {
            packet->case_index = i;
            goto done;
        }
        result = baseline_entry(inputs[i], cases[i].n);
        ++baseline->checks;
        if (!check_value(result, &cases[i], baseline)) {
            snprintf(packet->error, sizeof(packet->error), "baseline returned an incorrect result");
            goto done;
        }
    }
    Entry entries[] = {entry, baseline_entry};
    uint64_t calls[] = {4, 4};
    for (int j = 0; j < 2; ++j) for (;;) {
        double elapsed = measure(entries[j], inputs + perf_first, cases, perf_first, 4, calls[j]);
        if (elapsed >= 10000000.0 || calls[j] >= 1048576) break;
        calls[j] *= 2;
    }
    double samples[2][SAMPLES];
    /* Alternate measurement order on one CPU to reduce drift between runs. */
    for (int i = 0; i < SAMPLES; ++i) for (int turn = 0; turn < 2; ++turn) {
        int j = (i + turn) % 2;
        double elapsed = measure(entries[j], inputs + perf_first, cases, perf_first, 4, calls[j]);
        samples[j][i] = elapsed / (double)calls[j];
    }
    for (int k = 0; k < 2; ++k) for (int i = 1; i < SAMPLES; ++i) {
        double v = samples[k][i];
        int j = i;
        while (j && samples[k][j - 1] > v) { samples[k][j] = samples[k][j - 1]; --j; }
        samples[k][j] = v;
    }
    packet->ns = samples[0][SAMPLES / 2]; packet->calls = calls[0]; packet->status = 1;
    baseline->ns = samples[1][SAMPLES / 2]; baseline->calls = calls[1]; baseline->status = 1;
done:
    for (int i = 0; i < allocated; ++i) munmap(maps[i], bytes[i]);
    dlclose(handle);
    dlclose(baseline_handle);
}

static int run_library(const char *library, const char *baseline_library, const Case *cases,
                       int count, int perf_first, Packet *packet, Packet *baseline) {
    int fds[2];
    if (pipe(fds)) { snprintf(packet->error, sizeof(packet->error), "pipe: %s", strerror(errno)); return 0; }
    fflush(NULL);
    pid_t pid = fork();
    if (pid < 0) {
        close(fds[0]); close(fds[1]);
        snprintf(packet->error, sizeof(packet->error), "fork: %s", strerror(errno)); return 0;
    }
    if (!pid) {
        close(fds[0]); (void)setpgid(0, 0);
        int nullfd = open("/dev/null", O_RDWR);
        if (nullfd >= 0) {
            (void)dup2(nullfd, STDIN_FILENO); (void)dup2(nullfd, STDOUT_FILENO);
            (void)dup2(nullfd, STDERR_FILENO); close(nullfd);
        }
        Packet result[2] = {{0}};
        worker(library, baseline_library, cases, count, perf_first, &result[0], &result[1]);
        ssize_t sent = write(fds[1], result, sizeof(result));
        close(fds[1]);
        _exit(sent == (ssize_t)sizeof(result) ? 0 : 1);
    }
    close(fds[1]); (void)setpgid(pid, pid);
    if (!wait_child(pid, 10, packet->error, sizeof(packet->error))) { close(fds[0]); return 0; }
    Packet result[2];
    size_t received = 0;
    while (received < sizeof(result)) {
        ssize_t n = read(fds[0], (char *)result + received, sizeof(result) - received);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        received += (size_t)n;
    }
    close(fds[0]);
    if (received != sizeof(result)) {
        snprintf(packet->error, sizeof(packet->error), "worker did not return a complete result"); return 0;
    }
    *packet = result[0]; *baseline = result[1];
    return packet->status;
}

static int compile_assembly(const char *cc, const char *assembly, const char *library,
                            const char *log, char *error, size_t size) {
    char *argv[] = {(char *)cc, "-shared", "-fPIC", "-mavx2", "-Wl,-z,noexecstack",
                    "-Wl,-z,defs", (char *)assembly, "-o", (char *)library, NULL};
    return command(argv, log, 15, error, size);
}

static void task_run(const Task *task, const char *compiler, const char *cc,
                     const char *directory, int index, uint64_t seed, Result *result) {
    CbProgram *p = malloc(sizeof(*p));
    Case cases[MAX_CASES] = {{0}};
    int count = 0, perf_first = 0;
    if (!p) { snprintf(result->error, sizeof(result->error), "out of memory"); return; }
    if (!cb_parse_file(task->path, p, result->error, sizeof(result->error))) goto done;
    static const uint64_t lengths[] = {0, 1, 2, 3, 4, 5, 7, 8, 9, 15, 16, 17, 31, 32, 33, 63, 64, 65, 127, 128, 129};
    /* Non-tail recursive tasks use a smaller domain to fit the reference depth budget. */
    uint64_t maximum = task->level == 4 ? 32 : 129;
    for (size_t i = 0; i < sizeof(lengths) / sizeof(lengths[0]); ++i) {
        if (lengths[i] > maximum) continue;
        for (unsigned pattern = 0; pattern < 8; ++pattern)
            if (!add_case(cases, &count, p, lengths[i], pattern, &seed, result->error, sizeof(result->error))) goto done;
    }
    for (int i = 0; i < 24; ++i) {
        uint64_t n = random64(&seed) % (maximum + 1);
        if (!add_case(cases, &count, p, n, 6 + (unsigned)i % 2, &seed, result->error, sizeof(result->error))) goto done;
    }
    perf_first = count;
    for (int i = 0; i < 4; ++i)
        if (!add_case(cases, &count, p, task->length, 4 + (unsigned)i, &seed, result->error, sizeof(result->error))) goto done;
    char assembly[PATH_MAX], library[PATH_MAX], log[PATH_MAX];
    snprintf(assembly, sizeof(assembly), "%s/%03d-candidate.s", directory, index);
    snprintf(library, sizeof(library), "%s/%03d-candidate.so", directory, index);
    snprintf(log, sizeof(log), "%s/%03d-compiler.log", directory, index);
    char *argv[] = {(char *)compiler, (char *)task->path, assembly, NULL};
    if (!command(argv, log, 10, result->error, sizeof(result->error))) goto done;
    snprintf(log, sizeof(log), "%s/%03d-assembler.log", directory, index);
    if (!compile_assembly(cc, assembly, library, log, result->error, sizeof(result->error))) goto done;
    char candidate_library[PATH_MAX];
    snprintf(candidate_library, sizeof(candidate_library), "%s", library);
    snprintf(assembly, sizeof(assembly), "%s/%03d-baseline.s", directory, index);
    snprintf(library, sizeof(library), "%s/%03d-baseline.so", directory, index);
    snprintf(log, sizeof(log), "%s/%03d-baseline.log", directory, index);
    if (!cb_emit_assembly(p, assembly, result->error, sizeof(result->error)) ||
        !compile_assembly(cc, assembly, library, log, result->error, sizeof(result->error))) goto done;
    if (!run_library(candidate_library, library, cases, count, perf_first, &result->candidate, &result->baseline)) {
        snprintf(result->error, sizeof(result->error), "%s", result->candidate.error); goto done;
    }
    result->passed = 1;
done:
    for (int i = 0; i < count; ++i) free(cases[i].xs);
    free(p);
}

static void json_string(FILE *f, const char *s) {
    fputc('"', f);
    for (; *s; ++s) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') fprintf(f, "\\%c", c);
        else if (c < 32) fprintf(f, "\\u%04x", c);
        else fputc(c, f);
    }
    fputc('"', f);
}

static int report(const char *path, const Task *tasks, const Result *results,
                   int count, uint64_t seed, int passed, double score, const char *suite) {
    FILE *f = fopen(path, "w");
    if (!f) { perror(path); return 0; }
    fprintf(f, "{\n  \"benchmark_version\": \"v1\",\n  \"entry_abi\": \"u64(xs,n)\",\n  \"baseline\": \"v1/src/codegen.c\",\n  \"harness\": \"v1 scalar harness (two-argument ABI)\",\n  \"suite\": ");
    json_string(f, suite);
    fprintf(f, ",\n  ");
    fprintf(f, "\"schema_version\": 2,\n  \"seed\": %" PRIu64 ",\n  \"passed\": %d,\n  \"total\": %d,\n  \"score\": ", seed, passed, count);
    fprintf(f, "%.6f", score);
    fprintf(f, ",\n  \"score_policy\": \"geomean_failures_as_1\",\n  \"failure_speedup\": 1.0,\n  \"tasks\": [\n");
    for (int i = 0; i < count; ++i) {
        const Result *r = &results[i];
        fprintf(f, "    {\"name\": "); json_string(f, tasks[i].name);
        fprintf(f, ", \"level\": %d, \"timing_length\": %" PRIu64 ", \"status\": \"%s\", \"checks\": %u",
                tasks[i].level, tasks[i].length, r->passed ? "pass" : "fail", r->candidate.checks);
        if (r->passed) fprintf(f, ", \"candidate_ns\": %.3f, \"baseline_ns\": %.3f, \"speedup\": %.6f, \"calls_per_sample\": %" PRIu64,
                r->candidate.ns, r->baseline.ns, r->baseline.ns / r->candidate.ns, r->candidate.calls);
        else {
            fprintf(f, ", \"error\": "); json_string(f, r->error);
            if (r->candidate.checks) fprintf(f, ", \"expected\": \"%" PRIu64 "\", \"actual\": \"%" PRIu64 "\"", r->candidate.expected, r->candidate.actual);
        }
        fprintf(f, "}%s\n", i + 1 == count ? "" : ",");
    }
    fprintf(f, "  ]\n}\n");
    int ok = !ferror(f);
    if (fclose(f)) ok = 0;
    return ok;
}

static void cleanup(const char *directory) {
    DIR *d = opendir(directory);
    if (!d) return;
    struct dirent *entry;
    while ((entry = readdir(d))) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
        (void)unlinkat(dirfd(d), entry->d_name, 0);
    }
    closedir(d); (void)rmdir(directory);
}

static void usage(const char *name) {
    fprintf(stderr, "usage: %s --compiler PATH [--suite PATH] [--output PATH]\n"
                    "       [--seed U64] [--cc PATH] [--keep-artifacts]\n", name);
}

int main(int argc, char **argv) {
    const char *compiler_arg = NULL, *suite = "v1/benchmarks/suite.tsv", *output = NULL, *cc = "cc";
    uint64_t seed = UINT64_C(0xc0de2026);
    int keep = 0;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--keep-artifacts")) keep = 1;
        else if (!strcmp(argv[i], "--help")) { usage(argv[0]); return 0; }
        else if (i + 1 < argc && !strcmp(argv[i], "--compiler")) compiler_arg = argv[++i];
        else if (i + 1 < argc && !strcmp(argv[i], "--suite")) suite = argv[++i];
        else if (i + 1 < argc && !strcmp(argv[i], "--output")) output = argv[++i];
        else if (i + 1 < argc && !strcmp(argv[i], "--cc")) cc = argv[++i];
        else if (i + 1 < argc && !strcmp(argv[i], "--seed")) {
            char *end;
            const char *value = argv[++i];
            errno = 0; seed = strtoull(value, &end, 0);
            if (errno || !*value || *end || *value == '-') { usage(argv[0]); return 2; }
        } else { usage(argv[0]); return 2; }
    }
    if (!compiler_arg) { usage(argv[0]); return 2; }
    __builtin_cpu_init();
    if (!__builtin_cpu_supports("avx2")) {
        fprintf(stderr, "AVX2 CPU/OS support is required\n"); return 2;
    }
    char compiler[PATH_MAX];
    if (!realpath(compiler_arg, compiler) || access(compiler, X_OK)) { perror(compiler_arg); return 2; }
    Task *tasks = calloc(MAX_TASKS, sizeof(*tasks));
    Result *results = calloc(MAX_TASKS, sizeof(*results));
    if (!tasks || !results) { free(tasks); free(results); return 2; }
    char error[256];
    int count = read_manifest(suite, tasks, error, sizeof(error));
    if (count < 0) { fprintf(stderr, "%s\n", error); free(tasks); free(results); return 2; }
    char directory[] = "/tmp/compbench-XXXXXX";
    if (!mkdtemp(directory)) { perror("mkdtemp"); free(tasks); free(results); return 2; }
    int passed = 0;
    double sum_log = 0;
    printf("compbench v1: Linux x86-64, seed=%" PRIu64 ", %d tasks\n", seed, count);
    for (int i = 0; i < count; ++i) {
        task_run(&tasks[i], compiler, cc, directory, i, seed ^ (uint64_t)i, &results[i]);
        if (results[i].passed) {
            double speedup = results[i].baseline.ns / results[i].candidate.ns;
            sum_log += log(speedup); ++passed;
            printf("PASS L%d %-24s %3u checks  %10.1f ns/call  %6.3fx\n",
                   tasks[i].level, tasks[i].name, results[i].candidate.checks, results[i].candidate.ns, speedup);
        } else printf("FAIL L%d %-24s %s\n", tasks[i].level, tasks[i].name, results[i].error);
        fflush(stdout);
    }
    /* Failed tasks contribute log(1.0) = 0 and remain in count. */
    double score = exp(sum_log / count);
    printf("%d/%d passed; geometric mean speedup (failures=1x): %.3fx\n", passed, count, score);
    int ok = !output || report(output, tasks, results, count, seed, passed, score, suite);
    if (keep) printf("Artifacts: %s\n", directory); else cleanup(directory);
    free(tasks); free(results);
    return !ok ? 2 : passed == count ? 0 : 1;
}
