/* A sampler for builds where the kernel will not give us perf events.
 *
 * The phone's perf_event_open offers nothing usable to the shell user, so the
 * profile comes from inside the process instead: ITIMER_PROF posts SIGPROF on
 * the thread that is burning CPU, and the handler records the interrupted
 * instruction pointer and its host thread. Offline, the names behind those
 * addresses come from the unstripped libmain.so with llvm-symbolizer, and the
 * thread ids are named in the file's header from /proc/self/task/<tid>/comm.
 *
 * RECOMP_PROFILE=<path>     start the sampler, write samples here
 * RECOMP_PROFILE_DELAY=<s>  arm it this many seconds in (skip loading screens)
 */
#include <dlfcn.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>

#define PROF_MAX (1u << 18)

static uintptr_t s_pcs[PROF_MAX];
static unsigned s_tids[PROF_MAX];
static uintptr_t s_self_base;
static atomic_uint s_count;
static const char *s_out;

static uintptr_t pc_of(void *uc) {
#if defined(__aarch64__)
    return (uintptr_t)((ucontext_t *)uc)->uc_mcontext.pc;
#elif defined(__x86_64__)
    return (uintptr_t)((ucontext_t *)uc)->uc_mcontext.gregs[REG_RIP];
#else
    (void)uc;
    return 0;
#endif
}

static void prof_signal(int sig, siginfo_t *info, void *uc) {
    (void)sig;
    (void)info;
    unsigned i = atomic_fetch_add(&s_count, 1);
    if (i < PROF_MAX) {
        s_pcs[i] = pc_of(uc);
        s_tids[i] = (unsigned)syscall(SYS_gettid);
    }
}

static void arm_timer(void) {
    struct itimerval it;
    it.it_interval.tv_sec = 0;
    it.it_interval.tv_usec = 1000;
    it.it_value = it.it_interval;
    setitimer(ITIMER_PROF, &it, NULL);
}

static void prof_dump(void) {
    if (!s_out)
        return;
    unsigned n = atomic_load(&s_count);
    if (n > PROF_MAX)
        n = PROF_MAX;
    char tmp[512];
    snprintf(tmp, sizeof tmp, "%s.tmp", s_out);
    FILE *f = fopen(tmp, "w");
    if (!f)
        return;

    /* The load address and the thread names: the app cannot read its own
     * /proc/<pid>/maps from the shell user, and dladdr turns an address into
     * module plus offset, which is exactly what symbolising needs. */
    fprintf(f, "#base %llx\n", (unsigned long long)s_self_base);
    {
        unsigned seen[64];
        unsigned ns = 0;
        for (unsigned i = 0; i < n && ns < 64; i++) {
            unsigned t = s_tids[i], k;
            for (k = 0; k < ns; k++)
                if (seen[k] == t)
                    break;
            if (k < ns)
                continue;
            seen[ns++] = t;
            char path[64], comm[32] = "?";
            snprintf(path, sizeof path, "/proc/self/task/%u/comm", t);
            FILE *c = fopen(path, "r");
            if (c) {
                if (fgets(comm, sizeof comm, c)) {
                    char *nl = strchr(comm, '\n');
                    if (nl)
                        *nl = 0;
                }
                fclose(c);
            }
            fprintf(f, "#tid %u %s\n", t, comm);
        }
    }

    for (unsigned i = 0; i < n; i++) {
        uintptr_t pc = s_pcs[i];
        Dl_info di;
        if (pc && dladdr((void *)pc, &di) && di.dli_fbase) {
            const char *name = di.dli_fname ? strrchr(di.dli_fname, '/') : NULL;
            fprintf(f, "%s+%llx %u\n",
                    name ? name + 1 : (di.dli_fname ? di.dli_fname : "?"),
                    (unsigned long long)(pc - (uintptr_t)di.dli_fbase), s_tids[i]);
        } else {
            fprintf(f, "?+%llx %u\n", (unsigned long long)pc, s_tids[i]);
        }
    }
    fclose(f);
    rename(tmp, s_out);
}

/* A phone that is killed with SIGKILL never reaches atexit, so the samples are
 * also written from an ordinary thread every few seconds. */
static void *prof_flusher(void *arg) {
    (void)arg;
    const char *delay = getenv("RECOMP_PROFILE_DELAY");
    if (delay && *delay) {
        struct timespec d = { atoi(delay), 0 };
        nanosleep(&d, NULL);
    }
    arm_timer();
    for (;;) {
        struct timespec ts = { 5, 0 };
        nanosleep(&ts, NULL);
        prof_dump();
    }
    return NULL;
}

void ssx3_prof_start(const char *path) {
    if (!path || !*path)
        return;
    s_out = path;
    Dl_info info;
    if (dladdr((void *)ssx3_prof_start, &info) && info.dli_fbase)
        s_self_base = (uintptr_t)info.dli_fbase;
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = prof_signal;
    sa.sa_flags = SA_SIGINFO | SA_RESTART;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGPROF, &sa, NULL) != 0)
        return;
    atexit(prof_dump);
    pthread_t th;
    if (pthread_create(&th, NULL, prof_flusher, NULL) == 0)
        pthread_detach(th);
}
