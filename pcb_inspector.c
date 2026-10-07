#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define NPROC 3

/* Parent-side bookkeeping only: this is NOT the kernel PCB/task_struct. */
typedef struct {
    pid_t pid;
    bool reaped;
    unsigned long dispatches;
    unsigned long stops;
    int64_t first_dispatch_ns;
    int64_t slice_start_ns;
    int64_t completion_ns;
    int64_t active_ns;
} ProcessInfo;

typedef struct {
    int *items;
    size_t count;
    size_t capacity;
} GanttChart;

typedef struct {
    int quantum_ms;
    int units[NPROC];
    int unit_ms;
    bool cpu;
    bool automatic;
    bool step;
} Config;

static volatile sig_atomic_t interrupted;

static void on_signal(int sig)
{
    interrupted = sig; /* Only async-signal-safe work in the handler. */
}

static int install_handler(int sig, void (*handler)(int))
{
    struct sigaction sa = {0};
    sa.sa_handler = handler;
    sigemptyset(&sa.sa_mask);
    return sigaction(sig, &sa, NULL);
}

static int64_t now_ns(clockid_t clock_id)
{
    struct timespec ts;
    if (clock_gettime(clock_id, &ts) == -1) {
        perror("clock_gettime");
        return -1;
    }
    return (int64_t)ts.tv_sec * INT64_C(1000000000) + ts.tv_nsec;
}

static int sleep_ns(int64_t ns)
{
    struct timespec remaining = {
        .tv_sec = (time_t)(ns / INT64_C(1000000000)),
        .tv_nsec = (long)(ns % INT64_C(1000000000))
    };
    while (nanosleep(&remaining, &remaining) == -1) {
        if (errno != EINTR) {
            perror("nanosleep");
            return -1;
        }
        if (interrupted)
            return -1;
    }
    return interrupted ? -1 : 0;
}

static void usage(const char *program)
{
    printf("Usage: %s [options]\n"
           "  --auto           Skip the initial ENTER prompt (EOF also continues)\n"
           "  --step           Pause before each dispatch for presentation\n"
           "  --quantum MS     Wall-clock time slice, 10..60000 (default 1000)\n"
           "  --bursts A,B,C   Units for P1,P2,P3, each 1..10000 (default 8,4,12)\n"
           "  --units N        Override all three bursts with the same unit count\n"
           "  --unit-ms MS     Time per work unit, 1..60000 (default 250)\n"
           "  --mode sleep|cpu Sleep-based demo (default) or actual CPU work\n"
           "  --help           Show this help\n", program);
}

static int number(const char *text, int min, int max)
{
    char *end;
    errno = 0;
    long value = strtol(text, &end, 10);
    if (errno || *text == '\0' || *end != '\0' || value < min || value > max)
        return -1;
    return (int)value;
}

static int parse_args(int argc, char **argv, Config *cfg)
{
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--help") == 0) {
            usage(argv[0]);
            return 1;
        }
        if (strcmp(argv[i], "--auto") == 0) {
            cfg->automatic = true;
        } else if (strcmp(argv[i], "--step") == 0) {
            cfg->step = true;
        } else if (strcmp(argv[i], "--units") == 0) {
            if (++i >= argc)
                return -1;
            int units = number(argv[i], 1, 10000);
            if (units < 0)
                return -1;
            for (int j = 0; j < NPROC; ++j)
                cfg->units[j] = units;
        } else if (strcmp(argv[i], "--bursts") == 0) {
            if (++i >= argc)
                return -1;
            const char *cursor = argv[i];
            for (int j = 0; j < NPROC; ++j) {
                char *end;
                errno = 0;
                long units = strtol(cursor, &end, 10);
                if (errno || end == cursor || units < 1 || units > 10000 ||
                    *end != (j == NPROC - 1 ? '\0' : ','))
                    return -1;
                cfg->units[j] = (int)units;
                if (j < NPROC - 1)
                    cursor = end + 1;
            }
        } else if (strcmp(argv[i], "--mode") == 0) {
            if (++i >= argc)
                return -1;
            if (strcmp(argv[i], "cpu") == 0)
                cfg->cpu = true;
            else if (strcmp(argv[i], "sleep") == 0)
                cfg->cpu = false;
            else
                return -1;
        } else {
            int *target;
            int min = 1, max = 60000;
            if (strcmp(argv[i], "--quantum") == 0) {
                target = &cfg->quantum_ms;
                min = 10;
            } else if (strcmp(argv[i], "--unit-ms") == 0) {
                target = &cfg->unit_ms;
            } else {
                return -1;
            }
            if (++i >= argc || (*target = number(argv[i], min, max)) < 0)
                return -1;
        }
    }
    return 0;
}

static int pause_for_enter(void)
{
    printf("Press ENTER to continue (Ctrl+C to quit).\n");
    while (!interrupted) {
        struct pollfd fd = {.fd = STDIN_FILENO, .events = POLLIN};
        int result = poll(&fd, 1, 100);
        if (result < 0) {
            if (errno == EINTR)
                continue;
            perror("poll stdin");
            return -1;
        }
        if (result > 0) {
            char ch;
            ssize_t n = read(STDIN_FILENO, &ch, 1);
            if (n == 0 || (n == 1 && ch == '\n'))
                return 0;
            if (n < 0 && errno != EINTR) {
                perror("read stdin");
                return -1;
            }
        }
    }
    return -1;
}

/* Called only after waitpid confirms a stopped child, never after reaping. */
static int inspect_process(pid_t pid)
{
    static const char *keys[] = {
        "Name:", "State:", "Pid:", "PPid:", "Threads:", "VmRSS:",
        "voluntary_ctxt_switches:", "nonvoluntary_ctxt_switches:"
    };
    char path[64], line[512];
    snprintf(path, sizeof(path), "/proc/%ld/status", (long)pid);
    FILE *fp = fopen(path, "r");
    if (!fp) {
        perror(path);
        return -1;
    }
    printf("--- %s (kernel snapshot) ---\n", path);
    while (fgets(line, sizeof(line), fp)) {
        for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); ++i) {
            if (strncmp(line, keys[i], strlen(keys[i])) == 0) {
                fputs(line, stdout);
                break;
            }
        }
    }
    int failed = ferror(fp);
    if (fclose(fp) != 0)
        failed = 1;
    puts("----------------------------------------");
    return failed ? -1 : 0;
}

static void child_work(int index, pid_t parent, const Config *cfg)
{
    /* Linux-only fallback: do not leave stopped children if parent is killed.
       Check PPID after prctl to cover parent death before registration. */
    if (prctl(PR_SET_PDEATHSIG, SIGKILL) == -1 || getppid() != parent)
        _exit(EXIT_FAILURE);
    if (install_handler(SIGINT, SIG_DFL) == -1 ||
        install_handler(SIGTERM, SIG_DFL) == -1 ||
        install_handler(SIGHUP, SIG_DFL) == -1 ||
        install_handler(SIGPIPE, SIG_DFL) == -1)
        _exit(EXIT_FAILURE);
    interrupted = 0;
    printf("[P%d] created: PID=%ld PPID=%ld\n", index, (long)getpid(), (long)getppid());
    if (raise(SIGSTOP) != 0)
        _exit(EXIT_FAILURE);

    volatile uint64_t checksum = (uint64_t)index;
    const int units = cfg->units[index - 1];
    for (int unit = 1; unit <= units; ++unit) {
        if (cfg->cpu) {
            /* CPU time excludes time stopped or waiting for the real scheduler. */
            int64_t start = now_ns(CLOCK_PROCESS_CPUTIME_ID);
            if (start < 0)
                _exit(EXIT_FAILURE);
            int64_t current;
            do {
                for (int j = 0; j < 10000; ++j)
                    checksum = checksum * UINT64_C(1664525) + UINT64_C(1013904223);
                current = now_ns(CLOCK_PROCESS_CPUTIME_ID);
                if (current < 0)
                    _exit(EXIT_FAILURE);
            } while (current - start < (int64_t)cfg->unit_ms * 1000000);
        } else if (sleep_ns((int64_t)cfg->unit_ms * 1000000) < 0) {
            _exit(EXIT_FAILURE);
        }
        printf("[P%d | PID=%ld] work unit %02d/%02d completed\n",
               index, (long)getpid(), unit, units);
    }
    printf("[P%d] FINISHED\n", index);
    _exit(EXIT_SUCCESS);
}

static pid_t wait_child(pid_t pid, int *status, int options)
{
    pid_t result;
    do {
        if (interrupted) {
            errno = EINTR;
            return -1;
        }
        result = waitpid(pid, status, options);
    } while (result < 0 && errno == EINTR && !interrupted);
    return result;
}

static int record_exit(ProcessInfo *p, int index, int status, int *alive)
{
    p->reaped = true;
    --*alive;
    p->completion_ns = now_ns(CLOCK_MONOTONIC);
    if (p->completion_ns < 0)
        return -1;
    if (p->dispatches > 0)
        p->active_ns += p->completion_ns - p->slice_start_ns;
    if (WIFEXITED(status)) {
        printf("[Parent] P%d reaped: exit=%d, remaining=%d\n",
               index, WEXITSTATUS(status), *alive);
        return WEXITSTATUS(status) == 0 ? 0 : -1;
    }
    printf("[Parent] P%d reaped: signal=%d, remaining=%d\n",
           index, WTERMSIG(status), *alive);
    return -1;
}

static int reserve_gantt(GanttChart *chart)
{
    if (chart->count < chart->capacity)
        return 0;
    if (chart->capacity > SIZE_MAX / 2 / sizeof(*chart->items)) {
        fprintf(stderr, "Gantt chart is too large.\n");
        return -1;
    }
    size_t capacity = chart->capacity ? chart->capacity * 2 : 64;
    int *items = realloc(chart->items, capacity * sizeof(*items));
    if (!items) {
        perror("Gantt allocation");
        return -1;
    }
    chart->items = items;
    chart->capacity = capacity;
    return 0;
}

static void print_summary(const ProcessInfo *p, const Config *cfg,
                          const GanttChart *chart, int64_t arrival_ns)
{
    puts("\nTiming summary (ms, CLOCK_MONOTONIC; controller-observed):");
    printf("%-4s %7s %7s %12s %12s %12s %14s %12s\n",
           "Proc", "PID", "Units", "Target(ms)", "Response", "Waiting",
           "Turnaround", "Active");
    double response_sum = 0, waiting_sum = 0, turnaround_sum = 0;
    for (int i = 0; i < NPROC; ++i) {
        double response = (double)(p[i].first_dispatch_ns - arrival_ns) / 1e6;
        double turnaround = (double)(p[i].completion_ns - arrival_ns) / 1e6;
        double active = (double)p[i].active_ns / 1e6;
        double waiting = (double)(p[i].completion_ns - arrival_ns - p[i].active_ns) / 1e6;
        printf("P%-3d %7ld %7d %12.3f %12.3f %12.3f %14.3f %12.3f\n",
               i + 1, (long)p[i].pid, cfg->units[i],
               (double)cfg->units[i] * cfg->unit_ms, response, waiting, turnaround, active);
        response_sum += response;
        waiting_sum += waiting;
        turnaround_sum += turnaround;
    }
    printf("Average: response=%.3f ms waiting=%.3f ms turnaround=%.3f ms\n",
           response_sum / NPROC, waiting_sum / NPROC, turnaround_sum / NPROC);
    puts("Arrival=0 for all children, after the initial ENTER prompt.\n"
         "Response=first SIGCONT request - arrival; turnaround=exit observation - arrival.\n"
         "Active=sum of CONT-to-confirmed-stop/exit intervals; waiting=turnaround-active.\n"
         "Active is wall time, not CPU time; waiting is RR-controller queue time.\n"
         "Target=units*unit-ms (CPU target in cpu mode; sleep request total in sleep mode).\n"
         "Stop/exit observation includes scheduler and polling delays (nominal poll <=10 ms).\n"
         "Initial prompt excluded; --step prompts and inspection overhead ARE included.");
    puts("\nGantt chart (dispatch order; each cell is one dispatch, not to time scale):");
    putchar('|');
    for (size_t i = 0; i < chart->count; ++i)
        printf("P%d|", chart->items[i]);
    putchar('\n');
}

static void cleanup(ProcessInfo *p)
{
    /* SIGKILL also terminates a stopped child without needing SIGCONT.
       Only signal our own, not-yet-reaped children: no process-group kills. */
    for (int i = 0; i < NPROC; ++i)
        if (p[i].pid > 0 && !p[i].reaped)
            if (kill(p[i].pid, SIGKILL) < 0 && errno != ESRCH)
                perror("cleanup kill");
    for (int i = 0; i < NPROC; ++i) {
        if (p[i].pid > 0 && !p[i].reaped) {
            pid_t result;
            do {
                result = waitpid(p[i].pid, NULL, 0);
            } while (result < 0 && errno == EINTR);
            if (result < 0 && errno != ECHILD)
                perror("cleanup waitpid");
            p[i].reaped = true;
        }
    }
}

int main(int argc, char **argv)
{
    Config cfg = {.quantum_ms = 1000, .units = {8, 4, 12}, .unit_ms = 250};
    ProcessInfo processes[NPROC] = {{0}};
    GanttChart chart = {0};
    int parsed = parse_args(argc, argv, &cfg);
    if (parsed != 0) {
        if (parsed < 0) {
            fprintf(stderr, "Invalid arguments. Use --help.\n");
            return 2;
        }
        return 0;
    }
    setvbuf(stdout, NULL, _IONBF, 0);
    if (install_handler(SIGINT, on_signal) == -1 ||
        install_handler(SIGTERM, on_signal) == -1 ||
        install_handler(SIGHUP, on_signal) == -1 ||
        install_handler(SIGPIPE, on_signal) == -1) {
        perror("sigaction");
        return EXIT_FAILURE;
    }
    const pid_t parent = getpid();
    printf("PCB & Context Switching Inspector\nParent PID: %ld\n"
           "Children: %d | quantum: %d ms | units: %d,%d,%d | unit: %d ms | mode: %s\n"
           "User-space Round Robin controller; Linux performs real scheduling.\n"
           "Confirmed stop events are NOT kernel context-switch counts.\n\n",
           (long)parent, NPROC, cfg.quantum_ms, cfg.units[0], cfg.units[1], cfg.units[2], cfg.unit_ms,
           cfg.cpu ? "cpu" : "sleep");

    int exit_code = EXIT_FAILURE;
    int alive = NPROC;
    for (int i = 0; i < NPROC; ++i) {
        if (interrupted)
            goto done;
        pid_t pid = fork();
        if (pid < 0) {
            perror("fork");
            goto done;
        }
        if (pid == 0)
            child_work(i + 1, parent, &cfg);
        processes[i].pid = pid;
    }
    for (int i = 0; i < NPROC; ++i) {
        int status;
        if (wait_child(processes[i].pid, &status, WUNTRACED) < 0) {
            if (!interrupted)
                perror("initial waitpid");
            goto done;
        }
        if (!WIFSTOPPED(status)) {
            (void)record_exit(&processes[i], i + 1, status, &alive);
            goto done;
        }
        printf("[Parent] P%d PID=%ld initially STOPPED\n", i + 1, (long)processes[i].pid);
        if (inspect_process(processes[i].pid) < 0)
            goto done;
    }
    printf("\nInspect from another terminal:\nps -o pid,ppid,stat,comm -p %ld,%ld,%ld\n",
           (long)processes[0].pid, (long)processes[1].pid, (long)processes[2].pid);
    puts("All processes are ready. Starting Round Robin...");
    if (!cfg.automatic && pause_for_enter() < 0)
        goto done;
    /* All children enter the experiment's ready queue together. Setup and the
       initial presentation pause are excluded; step pauses remain wall time. */
    int64_t arrival_ns = now_ns(CLOCK_MONOTONIC);
    if (arrival_ns < 0)
        goto done;

    for (int current = 0; alive > 0; current = (current + 1) % NPROC) {
        ProcessInfo *p = &processes[current];
        if (interrupted)
            goto done;
        if (p->reaped)
            continue;
        if (cfg.step && pause_for_enter() < 0)
            goto done;
        if (reserve_gantt(&chart) < 0)
            goto done;
        printf("\n[Scheduler] Select P%d (PID=%ld)\n[Action] SIGCONT -> P%d\n",
               current + 1, (long)p->pid, current + 1);
        int64_t start = now_ns(CLOCK_MONOTONIC);
        if (start < 0)
            goto done;
        if (kill(p->pid, SIGCONT) < 0) {
            perror("SIGCONT");
            goto done;
        }
        if (p->dispatches == 0)
            p->first_dispatch_ns = start;
        p->slice_start_ns = start;
        ++p->dispatches;
        chart.items[chart.count++] = current + 1;
        int status;
        /* Poll for early termination; the quantum uses a monotonic deadline. */
        for (;;) {
            pid_t result = wait_child(p->pid, &status, WNOHANG);
            if (result < 0) {
                if (!interrupted)
                    perror("waitpid during quantum");
                goto done;
            }
            if (result > 0) {
                if (record_exit(p, current + 1, status, &alive) < 0)
                    goto done;
                break;
            }
            int64_t now = now_ns(CLOCK_MONOTONIC);
            if (now < 0)
                goto done;
            int64_t remaining = start + (int64_t)cfg.quantum_ms * 1000000 - now;
            if (remaining <= 0)
                break;
            if (sleep_ns(remaining < 10000000 ? remaining : 10000000) < 0)
                goto done;
        }
        if (p->reaped)
            continue;
        printf("[Quantum] expired for P%d\n[Action] SIGSTOP -> P%d\n", current + 1, current + 1);
        if (kill(p->pid, SIGSTOP) < 0 && errno != ESRCH) {
            perror("SIGSTOP");
            goto done;
        }
        /* Child may exit between the poll and SIGSTOP. Always decode status. */
        if (wait_child(p->pid, &status, WUNTRACED) < 0) {
            if (!interrupted)
                perror("waitpid after SIGSTOP");
            goto done;
        }
        if (WIFSTOPPED(status)) {
            int64_t stopped_ns = now_ns(CLOCK_MONOTONIC);
            if (stopped_ns < 0)
                goto done;
            p->active_ns += stopped_ns - p->slice_start_ns;
            ++p->stops;
            printf("[Stop confirmed] P%d, stop event #%lu for this child\n", current + 1, p->stops);
            if (inspect_process(p->pid) < 0)
                goto done;
        } else if (record_exit(p, current + 1, status, &alive) < 0) {
            goto done;
        }
    }
    puts("\nAll child processes finished and reaped.\nSummary:");
    for (int i = 0; i < NPROC; ++i)
        printf("P%d PID=%ld dispatches=%lu confirmed_stops=%lu\n", i + 1,
               (long)processes[i].pid, processes[i].dispatches, processes[i].stops);
    puts("Initial stops excluded. Kernel counters in /proc are separate measurements.");
    print_summary(processes, &cfg, &chart, arrival_ns);
    exit_code = EXIT_SUCCESS;
done:
    cleanup(processes);
    free(chart.items);
    if (interrupted) {
        fprintf(stderr, "Interrupted by signal %d; all children cleaned up.\n", interrupted);
        return 128 + interrupted;
    }
    return exit_code;
}
