#ifndef _WIN32
#define _GNU_SOURCE
#endif
/**
 * All-thread native stack dump, for hangs.
 *
 * The toolkit watchdog (RECOMP_WATCHDOG_SECS) samples the guest stack of the
 * thread that called xbox_WatchdogStart only, so a hang where the main thread
 * waits on a worker shows the wait and nothing about the worker. This samples
 * every thread instead. Generated functions are native functions named after
 * their guest address, so each thread's native stack, run through
 *
 *     xboxrecomp/tools/symbolize.py build/ssx3_recomp.map < stderr.txt
 *
 * reads as that thread's guest call chain (sub_XXXXXXXX + offset).
 *
 * Off unless RECOMP_THREAD_DUMP_SECS is set. Addresses inside the exe are
 * printed rebased to 0x140000000, the base the map file is written for.
 */
#ifndef _WIN32
/* POSIX: RECOMP_THREAD_DUMP_SECS=N signals every thread in turn (SIGUSR2); each
 * prints its own guest registers, guest return addresses and native backtrace,
 * which names the guest call chain (sub_XXXXXXXX). Peek/watch are Windows-only. */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <signal.h>
#include <unistd.h>
#include <dirent.h>
#include <pthread.h>
#if !defined(__EMSCRIPTEN__)
#include <sys/syscall.h>
#endif
#if !defined(__EMSCRIPTEN__) && !defined(__ANDROID__)
/* Bionic has no execinfo.h (no backtrace()): on Android the dumps keep the
 * guest registers and stack, and skip the native backtrace. */
#include <execinfo.h>
#define SSX3_HAVE_BACKTRACE 1
#endif

#include "recomp/gen/recomp_types.h"
extern void print_guest_stack(void);

#if defined(__EMSCRIPTEN__)
/* Web: no per-thread signals or native backtraces of other threads. */
void ssx3_ThreadDumpStart(void)
{
    if (getenv("RECOMP_THREAD_DUMP_SECS"))
        fprintf(stderr, "[THREADS] thread dumps are not available on the web build\n");
}
#else
static volatile sig_atomic_t s_dump_done;

static void dump_handler(int sig)
{
#ifdef SSX3_HAVE_BACKTRACE
    void *bt[48];
    int n;
#endif

    (void)sig;
    fprintf(stderr, "[THREAD %ld] eax=%08X ecx=%08X edx=%08X ebx=%08X esp=%08X esi=%08X edi=%08X\n",
            (long)syscall(SYS_gettid), g_eax, g_ecx, g_edx, g_ebx, g_esp, g_esi, g_edi);
    print_guest_stack();
#ifdef SSX3_HAVE_BACKTRACE
    n = backtrace(bt, 48);
    backtrace_symbols_fd(bt, n, 2);
#endif
    s_dump_done = 1;
}

static volatile sig_atomic_t s_dump_req;

static void usr1_handler(int sig) { (void)sig; s_dump_req = 1; }

static void dump_all(const char *why)
{
    long self = (long)syscall(SYS_gettid);
    DIR *d;
    struct dirent *e;
    extern void ssx3_StreamDump(void);

    fprintf(stderr, "[THREADS] ---- dump (%s) ----\n", why);
    d = opendir("/proc/self/task");
    while (d && (e = readdir(d))) {
        long tid = atol(e->d_name);
        int waited = 0;

        if (tid <= 0 || tid == self)
            continue;
        s_dump_done = 0;
        syscall(SYS_tgkill, getpid(), tid, SIGUSR2);
        while (!s_dump_done && waited++ < 100)
            usleep(10000);
        if (!s_dump_done)
            fprintf(stderr, "[THREAD %ld] no response (blocked signals?)\n", tid);
    }
    if (d)
        closedir(d);
    ssx3_StreamDump();
    fprintf(stderr, "[THREADS] ---- end ----\n");
    fflush(stderr);
}

/* RECOMP_THREAD_DUMP_SECS=N: one dump after N s. Any time: kill -USR1 <pid>. */
static void *dump_thread(void *arg)
{
    long secs = (long)(intptr_t)arg, t = 0;

    for (;;) {
        usleep(100000);
        t += 100;
        if (s_dump_req) {
            s_dump_req = 0;
            dump_all("SIGUSR1");
        }
        if (secs > 0 && t == secs * 1000)
            dump_all("timer");
    }
    return NULL;
}

void ssx3_ThreadDumpStart(void)
{
    const char *v = getenv("RECOMP_THREAD_DUMP_SECS");
    pthread_t t;

    signal(SIGUSR1, usr1_handler);
    signal(SIGUSR2, dump_handler);
    pthread_create(&t, NULL, dump_thread, (void *)(intptr_t)(v ? atoi(v) : 0));
    pthread_detach(t);
}
#endif /* __EMSCRIPTEN__ */
extern void xbox_PeekSample(const char *label);

static void *peek_thread(void *arg)
{
    unsigned ms = (unsigned)(uintptr_t)arg, n;
    char label[32];

    for (n = 1; ; n++) {
        usleep(ms * 1000u);
        snprintf(label, sizeof label, "peek@%us",
                 (unsigned)((n * (unsigned long long)ms) / 1000u));
        xbox_PeekSample(label);
        fflush(stderr);
    }
    return NULL;
}

void ssx3_PeekEveryStart(void)
{
    const char *e = getenv("RECOMP_PEEK_EVERY_MS");
    unsigned ms = e ? (unsigned)atoi(e) : 0;
    pthread_t t;

    if (ms && pthread_create(&t, NULL, peek_thread, (void *)(uintptr_t)ms) == 0)
        pthread_detach(t);
}

uint32_t g_ssx3_main_tid;   /* DWORD */

/* Another thread's native stack needs the Windows debug APIs. */
void ssx3_StackOfThread(unsigned long tid, const char *why)
{
    (void)tid; (void)why;
}
#else
#include <windows.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include "recomp/gen/recomp_types.h"   /* the thread-local guest registers */

extern ULONG _tls_index;

#define TD_MAX_THREADS 64
#define TD_MAX_FRAMES  40
#define TD_MAP_BASE    0x140000000ULL

typedef LONG (NTAPI *NtQueryInformationThread_t)(HANDLE, ULONG, PVOID, ULONG, PULONG);

typedef struct {
    DWORD   tid;
    DWORD64 start;
    int     n;
    DWORD64 rip[TD_MAX_FRAMES];
    int     has_regs;
    uint32_t regs[8];   /* eax ecx edx ebx esp ebp esi edi */
} ThreadStack;

/* The guest registers are __declspec(thread), so another thread's copies live
 * in its own TLS block: TEB+0x58 is ThreadLocalStoragePointer, indexed by the
 * module's _tls_index, and each variable sits at the same offset into every
 * thread's block. Offsets are taken from this thread's own block. */
static uint32_t *td_reg_addr[8];
static ptrdiff_t td_reg_off[8];

static BYTE *td_tls_block(BYTE *teb)
{
    BYTE **slots = *(BYTE ***)(teb + 0x58);
    return slots ? slots[_tls_index] : NULL;
}

static void td_reg_offsets(void)
{
    BYTE *mine = td_tls_block((BYTE *)NtCurrentTeb());
    int i;

    td_reg_addr[0] = &g_eax; td_reg_addr[1] = &g_ecx; td_reg_addr[2] = &g_edx;
    td_reg_addr[3] = &g_ebx; td_reg_addr[4] = &g_esp; td_reg_addr[5] = &g_ebp;
    td_reg_addr[6] = &g_esi; td_reg_addr[7] = &g_edi;
    for (i = 0; i < 8; i++)
        td_reg_off[i] = mine ? (BYTE *)td_reg_addr[i] - mine : -1;
}

static int td_read_regs(HANDLE h, NtQueryInformationThread_t query, uint32_t *out)
{
    struct { LONG status; BYTE *teb; ULONG_PTR rest[4]; } tbi;  /* THREAD_BASIC_INFORMATION */
    int i;

    if (!query || query(h, 0, &tbi, sizeof(tbi), NULL) < 0 || !tbi.teb)
        return 0;
    __try {
        BYTE *block = td_tls_block(tbi.teb);
        if (!block)
            return 0;
        for (i = 0; i < 8; i++) {
            if (td_reg_off[i] < 0)
                return 0;
            out[i] = *(uint32_t *)(block + td_reg_off[i]);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
    return 1;
}

static DWORD64 s_exe_base, s_exe_end;

static DWORD64 td_rebase(DWORD64 a)
{
    return (a >= s_exe_base && a < s_exe_end) ? a - s_exe_base + TD_MAP_BASE : a;
}

/* Unwind with the exe's own .pdata, so no PDB or dbghelp is needed. Runs while
 * the thread is suspended; a fault on a torn stack ends the walk early. */
static int td_walk(CONTEXT *ctx, DWORD64 *out)
{
    int n = 0;

    __try {
        while (n < TD_MAX_FRAMES && ctx->Rip) {
            DWORD64 base;
            PRUNTIME_FUNCTION f;

            out[n++] = ctx->Rip;
            f = RtlLookupFunctionEntry(ctx->Rip, &base, NULL);
            if (f) {
                PVOID handler_data;
                DWORD64 frame;
                RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, ctx->Rip, f, ctx,
                                 &handler_data, &frame, NULL);
            } else {                    /* leaf: return address on top */
                ctx->Rip = *(DWORD64 *)ctx->Rsp;
                ctx->Rsp += 8;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return n;
}

/* One thread's native stack, now, on one line ([HOLDSTACK] <why> <tid>:
 * rebased return addresses; tools/symbolize.py or the map names them) --
 * for a caller that has spotted a thread misbehaving (the guest-CPU hold
 * sampler in ssx3_overrides.c). */
void ssx3_StackOfThread(unsigned long tid, const char *why)
{
    DWORD64 frames[TD_MAX_FRAMES];
    CONTEXT ctx;
    int n = 0, k;
    HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, (DWORD)tid);
    if (!h)
        return;
    if (!s_exe_base) {
        HMODULE m = GetModuleHandleA(NULL);
        IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)((BYTE *)m + ((IMAGE_DOS_HEADER *)m)->e_lfanew);
        s_exe_base = (DWORD64)(uintptr_t)m;
        s_exe_end = s_exe_base + nt->OptionalHeader.SizeOfImage;
    }
    if (SuspendThread(h) != (DWORD)-1) {
        ZeroMemory(&ctx, sizeof ctx);
        ctx.ContextFlags = CONTEXT_FULL;
        if (GetThreadContext(h, &ctx))
            n = td_walk(&ctx, frames);
        ResumeThread(h);
    }
    CloseHandle(h);
    fprintf(stderr, "[HOLDSTACK] %s %lu:", why, tid);
    for (k = 0; k < n; k++)
        fprintf(stderr, " 0x%llX", (unsigned long long)td_rebase(frames[k]));
    fprintf(stderr, "\n");
    fflush(stderr);
}

static char s_mem_spec[512];  /* from the trigger file, if it had content */

static unsigned long g_cpu_tid, g_cpu_ms;
static long g_cpu_waiting;
static char g_draw_info[200];

static void td_dump(const char *why)
{
    static ThreadStack stacks[TD_MAX_THREADS];
    NtQueryInformationThread_t query = (NtQueryInformationThread_t)
        GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtQueryInformationThread");
    DWORD self = GetCurrentThreadId(), pid = GetCurrentProcessId();
    THREADENTRY32 te = { sizeof(te) };
    HANDLE snap;
    int count = 0, i, j;

    td_reg_offsets();
    /* Who holds the guest CPU, read first; the traces below are printed only
     * after the thread snapshot -- printing ~2000 lines first let the other
     * threads run on meanwhile (and block on the stderr lock the printing
     * holds), so the stacks no longer showed the hang. */
    {
        extern void xbox_GuestCpuInfo(unsigned long *, unsigned long *, long *);
        extern void xbox_Nv2aDrawInfo(char *, size_t);
        xbox_GuestCpuInfo(&g_cpu_tid, &g_cpu_ms, &g_cpu_waiting);
        xbox_Nv2aDrawInfo(g_draw_info, sizeof g_draw_info);
    }

    snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE)
        return;
    /* Collect everything first and print afterwards: printing while another
     * thread is suspended can deadlock on a CRT lock it holds. */
    for (BOOL ok = Thread32First(snap, &te); ok && count < TD_MAX_THREADS;
         ok = Thread32Next(snap, &te)) {
        HANDLE h;
        CONTEXT ctx;

        if (te.th32OwnerProcessID != pid || te.th32ThreadID == self)
            continue;
        h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT |
                       THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID);
        if (!h)
            continue;
        stacks[count].tid = te.th32ThreadID;
        stacks[count].start = 0;
        if (query)              /* 9 = ThreadQuerySetWin32StartAddress */
            query(h, 9, &stacks[count].start, sizeof(DWORD64), NULL);
        stacks[count].n = 0;
        stacks[count].has_regs = 0;
        if (SuspendThread(h) != (DWORD)-1) {
            stacks[count].has_regs = td_read_regs(h, query, stacks[count].regs);
            ZeroMemory(&ctx, sizeof(ctx));
            ctx.ContextFlags = CONTEXT_FULL;
            if (GetThreadContext(h, &ctx))
                stacks[count].n = td_walk(&ctx, stacks[count].rip);
            ResumeThread(h);
        }
        CloseHandle(h);
        count++;
    }
    CloseHandle(snap);

    fprintf(stderr, "[THREADS] %d threads (%s)\n", count, why);
    {   /* The indirect-call ring (all threads share it): what a spinning
         * interpreter or vtable loop keeps calling. Oldest first. */
        extern volatile uint32_t g_icall_trace[16];
        extern volatile uint32_t g_icall_trace_idx;
        extern volatile uint64_t g_icall_count;
        uint32_t k;
        fprintf(stderr, "  [ICALLS] count=%llu recent:",
                (unsigned long long)g_icall_count);
        for (k = 0; k < 16; k++)
            fprintf(stderr, " %08X", g_icall_trace[(g_icall_trace_idx + k) & 15]);
        fprintf(stderr, "\n");
    }
    {   /* RECOMP_DUMP_MEM=<va>:<len>[,<va>:<len>...] -- guest memory too, as
         * dwords, so a table the stall is polling can be read at the moment
         * of the dump. */
        const char *spec = s_mem_spec[0] ? s_mem_spec : getenv("RECOMP_DUMP_MEM");
        while (spec && *spec) {
            char *end;
            uint32_t va = (uint32_t)strtoul(spec, &end, 0), len = 0, k;
            if (*end == ':')
                len = (uint32_t)strtoul(end + 1, &end, 0);
            for (k = 0; k < len; k += 4) {
                if (k % 32 == 0)
                    fprintf(stderr, "%s  [MEM] %08X:", k ? "\n" : "", va + k);
                __try {
                    fprintf(stderr, " %08X", MEM32(va + k));
                } __except (EXCEPTION_EXECUTE_HANDLER) {
                    fprintf(stderr, " ????????");
                }
            }
            if (len)
                fprintf(stderr, "\n");
            spec = (*end == ',') ? end + 1 : NULL;
        }
    }
    for (i = 0; i < count; i++) {
        fprintf(stderr, "[THREAD] tid=%lu start=0x%llX\n", stacks[i].tid,
                (unsigned long long)td_rebase(stacks[i].start));
        if (stacks[i].has_regs)
            fprintf(stderr, "    guest eax=%08X ecx=%08X edx=%08X ebx=%08X"
                    " esp=%08X ebp=%08X esi=%08X edi=%08X\n",
                    stacks[i].regs[0], stacks[i].regs[1], stacks[i].regs[2],
                    stacks[i].regs[3], stacks[i].regs[4], stacks[i].regs[5],
                    stacks[i].regs[6], stacks[i].regs[7]);
        for (j = 0; j < stacks[i].n; j++)
            fprintf(stderr, "    #%-2d 0x%llX\n", j,
                    (unsigned long long)td_rebase(stacks[i].rip[j]));
    }
    fprintf(stderr, "[GUESTCPU] held by thread %lu for %lu ms, %ld waiting (at the start of the dump)\n",
            g_cpu_tid, g_cpu_ms, g_cpu_waiting);
    fprintf(stderr, "[GPUDRAW] %s\n", g_draw_info);
    {
        /* EA job-queue ring (RECOMP_JOB_TRACE), streamer and STRM traces
         * (ssx3_overrides.c). After the snapshot: they take the stderr lock. */
        extern void ssx3_JobTraceDump(void);
        extern void ssx3_StreamDump(void);
        extern void ssx3_StrmTraceDump(void);
        ssx3_JobTraceDump();
        ssx3_StreamDump();
        ssx3_StrmTraceDump();
    }
    fprintf(stderr, "[THREADS-END]\n");   /* tools/race_test.ps1 waits for this */
    fflush(stderr);
}

static DWORD WINAPI td_thread(LPVOID arg)
{
    char why[32];

    Sleep((DWORD)(uintptr_t)arg * 1000u);
    _snprintf_s(why, sizeof why, _TRUNCATE, "after %us", (unsigned)(uintptr_t)arg);
    td_dump(why);
    return 0;
}

/* Sampling profiler: `profile` in the trigger file. Every period_ms, every
 * thread's native stack is unwound (top 8 frames) and printed as one
 * "[PROF] tid rip rip ..." line, rebased like the dump, so
 *   symbolize.py build/ssx3_recomp.map
 * plus a tally shows where a slow frame goes -- game code, the pushbuffer
 * executor, or the rasteriser. */
/* The thread WinMain runs on, which runs the title's entry point and with it
 * the game's main loop (main.c sets it). */
DWORD g_ssx3_main_tid;

/* `profile main`: only the main thread, 1000 samples 3 ms apart, kept in
 * memory and printed at the end -- far lighter than the all-thread profile
 * (which suspends and unwinds every thread and prints as it goes, enough to
 * stall the game for 100+ ms), so a frame-rate problem stays as it was while
 * it is measured. Same [PROF] format, so profile_tally.py reads it. */
static void td_profile_main(void)
{
    enum { N = 1000 };
    static DWORD64 rips[N][16];
    static int nrip[N];
    HANDLE h;
    int s, j, got = 0;
    if (!g_ssx3_main_tid || !(h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, g_ssx3_main_tid))) {
        fprintf(stderr, "[PROFILE] main thread unknown\n");
        return;
    }
    fprintf(stderr, "[PROFILE] main thread %lu: %d samples every 3 ms\n", g_ssx3_main_tid, N);
    for (s = 0; s < N; s++) {
        CONTEXT ctx;
        DWORD64 all[TD_MAX_FRAMES];
        int n = 0;
        if (SuspendThread(h) != (DWORD)-1) {
            ZeroMemory(&ctx, sizeof(ctx));
            ctx.ContextFlags = CONTEXT_FULL;
            if (GetThreadContext(h, &ctx))
                n = td_walk(&ctx, all);
            ResumeThread(h);
        }
        if (n > 0) {
            nrip[got] = n < 16 ? n : 16;
            for (j = 0; j < nrip[got]; j++)
                rips[got][j] = all[j];
            got++;
        }
        Sleep(3);
    }
    CloseHandle(h);
    for (s = 0; s < got; s++) {
        fprintf(stderr, "[PROF] %lu", g_ssx3_main_tid);
        for (j = 0; j < nrip[s]; j++)
            fprintf(stderr, " 0x%llX", (unsigned long long)td_rebase(rips[s][j]));
        fprintf(stderr, "\n");
    }
    fprintf(stderr, "[PROFILE] done\n");
    fflush(stderr);
}

static void td_profile(int samples, int period_ms)
{
    static DWORD64 rips[TD_MAX_THREADS][8];
    static int nrip[TD_MAX_THREADS];
    static DWORD tids[TD_MAX_THREADS];
    DWORD self = GetCurrentThreadId(), pid = GetCurrentProcessId();
    int s, i, j;

    fprintf(stderr, "[PROFILE] %d samples every %d ms\n", samples, period_ms);
    for (s = 0; s < samples; s++) {
        THREADENTRY32 te = { sizeof(te) };
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        int count = 0;

        if (snap == INVALID_HANDLE_VALUE)
            break;
        for (BOOL ok = Thread32First(snap, &te); ok && count < TD_MAX_THREADS;
             ok = Thread32Next(snap, &te)) {
            HANDLE h;
            CONTEXT ctx;
            DWORD64 all[TD_MAX_FRAMES];
            int n = 0;

            if (te.th32OwnerProcessID != pid || te.th32ThreadID == self)
                continue;
            h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE,
                           te.th32ThreadID);
            if (!h)
                continue;
            if (SuspendThread(h) != (DWORD)-1) {
                ZeroMemory(&ctx, sizeof(ctx));
                ctx.ContextFlags = CONTEXT_FULL;
                if (GetThreadContext(h, &ctx))
                    n = td_walk(&ctx, all);
                ResumeThread(h);
            }
            CloseHandle(h);
            /* Keep blocked threads too (innermost frame in ntdll): where a
             * thread waits is as much the answer as where it computes. The
             * tally attributes each sample to its first frame in the exe. */
            if (n == 0)
                continue;
            tids[count] = te.th32ThreadID;
            nrip[count] = n < 8 ? n : 8;
            for (j = 0; j < nrip[count]; j++)
                rips[count][j] = all[j];
            count++;
        }
        CloseHandle(snap);
        for (i = 0; i < count; i++) {
            fprintf(stderr, "[PROF] %lu", tids[i]);
            for (j = 0; j < nrip[i]; j++)
                fprintf(stderr, " 0x%llX", (unsigned long long)td_rebase(rips[i][j]));
            fprintf(stderr, "\n");
        }
        Sleep(period_ms);
    }
    fprintf(stderr, "[PROFILE] done\n");
    fflush(stderr);
}

/* Write watchpoint ("watch <va>" in the trigger file): DR0 on every thread,
 * 4 bytes, writes only. ssx3_WatchHit (called first in the VEH) logs who
 * wrote: native RIP (symbolize against the map) and the guest stack, where
 * the return addresses into generated code name the guest caller chain. */
extern ptrdiff_t xbox_GetMemoryOffset(void);
static uintptr_t s_watch_host;
static uint32_t s_watch_va;
static volatile LONG s_watch_hits;

int ssx3_WatchHit(PEXCEPTION_POINTERS ep)
{
    PCONTEXT c = ep->ContextRecord;
    const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
    LONG n;
    int i;

    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP
        || !s_watch_host || !(c->Dr6 & 1))
        return 0;
    c->Dr6 = 0;
    n = InterlockedIncrement(&s_watch_hits);
    if (n <= 40) {
        fprintf(stderr, "[WATCH] #%ld tid %lu va %08X = %08X rip 0x%llX | eax %08X ecx %08X edx %08X"
                        " ebx %08X esi %08X edi %08X esp %08X | stack",
                n, GetCurrentThreadId(), s_watch_va, *(const uint32_t *)s_watch_host,
                (unsigned long long)td_rebase(c->Rip), g_eax, g_ecx, g_edx,
                g_ebx, g_esi, g_edi, g_esp);
        for (i = 0; i < 12; i++)
            fprintf(stderr, " %08X", *(const uint32_t *)(mem + g_esp + 4 * i));
        fprintf(stderr, "\n");
        fflush(stderr);
    }
    if (n >= 40)                        /* enough: disarm this thread */
        c->Dr7 &= ~3ull;
    return 1;
}

static void td_watch(uint32_t va)
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    THREADENTRY32 te;
    DWORD self = GetCurrentThreadId(), pid = GetCurrentProcessId();
    int armed = 0;

    s_watch_va = va;
    s_watch_hits = 0;
    s_watch_host = (uintptr_t)xbox_GetMemoryOffset() + va;
    te.dwSize = sizeof te;
    if (snap == INVALID_HANDLE_VALUE)
        return;
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
        HANDLE h;
        CONTEXT ctx;
        if (te.th32OwnerProcessID != pid || te.th32ThreadID == self)
            continue;
        h = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME,
                       FALSE, te.th32ThreadID);
        if (!h)
            continue;
        SuspendThread(h);
        memset(&ctx, 0, sizeof ctx);
        ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
        if (GetThreadContext(h, &ctx)) {
            ctx.Dr0 = s_watch_host;
            ctx.Dr7 = (ctx.Dr7 & ~0xF0003ull) | 1ull | (1ull << 16) | (3ull << 18);
            if (SetThreadContext(h, &ctx))
                armed++;
        }
        ResumeThread(h);
        CloseHandle(h);
    }
    CloseHandle(snap);
    fprintf(stderr, "[WATCH] va %08X (host %p) armed on %d threads\n",
            va, (void *)s_watch_host, armed);
    fflush(stderr);
}

/* RECOMP_THREAD_DUMP_TRIGGER=<file>: dump every time the file appears (it is
 * deleted after each dump), for a stall reached at an unpredictable time --
 * e.g. by driving menus with RECOMP_PAD_LIVE. */
static DWORD WINAPI td_trigger_thread(LPVOID arg)
{
    const char *path = (const char *)arg;

    for (;;) {
        Sleep(500);
        if (GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES) {
            /* The file's content, if any, is a RECOMP_DUMP_MEM-style list of
             * ranges for this dump: addresses are often only known once the
             * stall is in front of you. */
            FILE *f = fopen(path, "rb");
            s_mem_spec[0] = 0;
            if (f) {
                size_t n = fread(s_mem_spec, 1, sizeof s_mem_spec - 1, f);
                fclose(f);
                while (n && (s_mem_spec[n - 1] == '\n' || s_mem_spec[n - 1] == '\r'
                             || s_mem_spec[n - 1] == ' '))
                    n--;
                s_mem_spec[n] = 0;
            }
            DeleteFileA(path);
            if (strncmp(s_mem_spec, "watch ", 6) == 0) {
                td_watch((uint32_t)strtoul(s_mem_spec + 6, NULL, 16));
                s_mem_spec[0] = 0;
            } else if (strncmp(s_mem_spec, "profile main", 12) == 0) {
                td_profile_main();
                s_mem_spec[0] = 0;
            } else if (strncmp(s_mem_spec, "profile", 7) == 0) {
                td_profile(300, 10);
                s_mem_spec[0] = 0;
            } else {
                td_dump("triggered");
            }
        }
    }
}

/* RECOMP_CPU_STATS=1: once a second, the CPU time each thread used in that
 * second (GetThreadTimes: user + kernel), top 6, the main thread marked --
 * no suspension, so it measures a frame-rate problem without changing it.
 * A thread near 100% is computing; one well below it is waiting. */
static DWORD WINAPI cpu_stats_thread(LPVOID unused)
{
    enum { M = 256 };
    static DWORD tids[M];
    static ULONGLONG prev[M];
    int n = 0;
    DWORD pid = GetCurrentProcessId();
    LARGE_INTEGER f, t0, t1;
    (void)unused;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t0);
    for (;;) {
        THREADENTRY32 te = { sizeof(te) };
        HANDLE snap;
        DWORD ctid[M];
        ULONGLONG cur[M], delta[M];
        int cn = 0, i, k;
        double wall;
        Sleep(1000);
        QueryPerformanceCounter(&t1);
        wall = (double)(t1.QuadPart - t0.QuadPart) / (double)f.QuadPart;
        t0 = t1;
        snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (snap == INVALID_HANDLE_VALUE)
            continue;
        for (BOOL ok = Thread32First(snap, &te); ok && cn < M; ok = Thread32Next(snap, &te)) {
            HANDLE h;
            FILETIME c, x, kt, ut;
            if (te.th32OwnerProcessID != pid)
                continue;
            if (!(h = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, te.th32ThreadID)))
                continue;
            if (GetThreadTimes(h, &c, &x, &kt, &ut)) {
                ctid[cn] = te.th32ThreadID;
                cur[cn] = ((ULONGLONG)kt.dwHighDateTime << 32 | kt.dwLowDateTime)
                        + ((ULONGLONG)ut.dwHighDateTime << 32 | ut.dwLowDateTime);
                delta[cn] = 0;
                for (k = 0; k < n; k++)
                    if (tids[k] == ctid[cn]) { delta[cn] = cur[cn] - prev[k]; break; }
                cn++;
            }
            CloseHandle(h);
        }
        CloseHandle(snap);
        fprintf(stderr, "[CPU]");
        for (k = 0; k < 6; k++) {
            int best = -1;
            for (i = 0; i < cn; i++)
                if (delta[i] != (ULONGLONG)-1 && (best < 0 || delta[i] > delta[best]))
                    best = i;
            if (best < 0 || delta[best] == 0)
                break;
            fprintf(stderr, " %s%lu %.0f%%", ctid[best] == g_ssx3_main_tid ? "main:" : "",
                    ctid[best], (double)delta[best] / 1e5 / wall);
            delta[best] = (ULONGLONG)-1;
        }
        fprintf(stderr, "\n");
        for (i = 0; i < cn; i++) { tids[i] = ctid[i]; prev[i] = cur[i]; }
        n = cn;
    }
}

void ssx3_ThreadDumpStart(void)
{
    const char *e = getenv("RECOMP_THREAD_DUMP_SECS");
    unsigned secs = e ? (unsigned)atoi(e) : 0;
    HMODULE exe = GetModuleHandleA(NULL);
    PIMAGE_NT_HEADERS nt;

    const char *trig = getenv("RECOMP_THREAD_DUMP_TRIGGER");

    if (getenv("RECOMP_CPU_STATS"))
        CloseHandle(CreateThread(NULL, 0, cpu_stats_thread, NULL, 0, NULL));
    if (!secs && !(trig && *trig))
        return;
    nt = (PIMAGE_NT_HEADERS)((BYTE *)exe + ((PIMAGE_DOS_HEADER)exe)->e_lfanew);
    s_exe_base = (DWORD64)exe;
    s_exe_end  = s_exe_base + nt->OptionalHeader.SizeOfImage;
    if (secs)
        CloseHandle(CreateThread(NULL, 0, td_thread, (LPVOID)(uintptr_t)secs, 0, NULL));
    if (trig && *trig)
        CloseHandle(CreateThread(NULL, 0, td_trigger_thread, (LPVOID)trig, 0, NULL));
}

/* RECOMP_PEEK_EVERY_MS=<ms>: sample RECOMP_PEEK on a clock, so one run shows
 * whether a guest field is moving. The toolkit samples it only at the
 * watchdog and in the pushbuffer report, which is one sample per run when the
 * title is not drawing much. */
extern void xbox_PeekSample(const char *label);

static DWORD WINAPI td_peek_thread(LPVOID arg)
{
    DWORD ms = (DWORD)(uintptr_t)arg;
    char label[32];
    unsigned n;

    for (n = 1; ; n++) {
        Sleep(ms);
        _snprintf_s(label, sizeof label, _TRUNCATE, "peek@%us",
                    (unsigned)((n * (unsigned long long)ms) / 1000u));
        xbox_PeekSample(label);
        fflush(stderr);
    }
}

void ssx3_PeekEveryStart(void)
{
    const char *e = getenv("RECOMP_PEEK_EVERY_MS");
    DWORD ms = e ? (DWORD)atoi(e) : 0;

    if (ms)
        CloseHandle(CreateThread(NULL, 0, td_peek_thread, (LPVOID)(uintptr_t)ms, 0, NULL));
}
#endif /* _WIN32 */
