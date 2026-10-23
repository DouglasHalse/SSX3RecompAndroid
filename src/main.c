/**
 * SSX 3 - Recompiled Game Entry Point
 *
 * This is the Windows executable that hosts the recompiled game code.
 * It performs the following initialization sequence:
 *
 * 1. Load the original XBE file from disk
 * 2. Initialize the Xbox memory layout (map data sections to original VAs)
 * 3. Initialize the Xbox kernel replacement layer
 * 4. Initialize the kernel bridge (thunk table in Xbox memory)
 * 5. Set up game file paths for I/O redirection
 * 6. Initialize the stack pointer
 * 7. Install VEH crash handler for diagnostics
 * 8. Call the game's original entry point (recompiled)
 *
 * Customize this file for your game:
 *   - Set YOUR_GAME_ENTRY_POINT to the XBE entry point address
 *   - Set YOUR_GAME_XBE_PATH to where the XBE file lives
 *   - Set YOUR_GAME_DIR to the game data directory
 *   - Add any CRT global pre-initialization your game needs
 *   - Customize the VEH handler for game-specific crash diagnosis
 *
 * XBE Details (fill in from xbe_parser output):
 *   Title:       SSX 3
 *   Title ID:    0x4541003C (PAL, XDK 5558, built 2003-09-19)
 *   Base addr:   0x00010000
 *   Entry point: 0x00181AD2
 *   Code size:   ~2150 KB (.text 0x2196DC)
 *   Sections:    12 (.text D3D D3DX XGRPH DSOUND XPP .rdata .data DOLBY $$XTIMAGE ABRTFONT .XTLID)
 *   Kernel imports: 159
 */

#ifndef _WIN32
#define _GNU_SOURCE
#endif
#include "log.h"
#include "journal.h"
#ifdef _WIN32
#include <windows.h>
#else
#include "platform/win32_compat.h"
#endif
#ifdef _WIN32
#include <dbghelp.h>
#endif
#include <stdio.h>
#ifdef _WIN32
#include <xinput.h>
#pragma comment(lib, "xinput.lib")
#endif
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <math.h>

/* xboxrecomp runtime headers */
#include <xbox/xboxrecomp.h>

#ifdef _WIN32
/* The session's logs folder: logs\ beside the game folder when the game
 * writes its own log (started from the launcher or a double-click), else
 * logs\ in the working directory. Crash dumps go there too. */
static char s_logs_dir[MAX_PATH] = "logs";

const char *ssx3_LogsDir(void) { return s_logs_dir; }

static int cmp_name(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

/* Keep the newest `keep` files matching pattern in s_logs_dir (the names
 * carry the date and time, so name order is age order). */
static void logs_prune(const char *pattern, int keep)
{
    WIN32_FIND_DATAA fd;
    char path[MAX_PATH], *names[512];
    int n = 0, i;
    HANDLE h;
    snprintf(path, sizeof path, "%s\\%s", s_logs_dir, pattern);
    if ((h = FindFirstFileA(path, &fd)) == INVALID_HANDLE_VALUE)
        return;
    do {
        if (n < 512 && (names[n] = _strdup(fd.cFileName)) != NULL)
            n++;
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    qsort(names, (size_t)n, sizeof names[0], cmp_name);
    for (i = 0; i < n; i++) {
        if (i < n - keep) {
            snprintf(path, sizeof path, "%s\\%s", s_logs_dir, names[i]);
            DeleteFileA(path);
        }
        free(names[i]);
    }
}
#endif
#ifdef SSX3_VULKAN
#include "vkr_backend.h"
#endif

/*
 * If xboxrecomp.h is not an umbrella header in your setup, include
 * the individual headers directly:
 *
 * #include "kernel.h"
 * #include "xbox_memory_layout.h"
 * #include "d3d8_xbox.h"
 * #include "dsound_xbox.h"
 * #include "xinput_xbox.h"
 */

/* ── Global register state (defined in xbox_memory_layout.c) ── */

/* RECOMP_TLS is not optional here. The runtime defines these thread-local, and
 * a plain `extern` referencing a __declspec(thread) variable does not resolve
 * to the calling thread's copy -- it resolves to the image's TLS template. The
 * host side then writes g_esp somewhere the generated code never reads, so the
 * guest starts with every register at zero and faults immediately, having
 * apparently ignored the setup that visibly ran. */
extern RECOMP_TLS uint32_t g_eax, g_ecx, g_edx, g_esp;
extern RECOMP_TLS uint32_t g_ebx, g_esi, g_edi;
extern RECOMP_TLS uint32_t g_seh_ebp;
/* x87 and SSE state. Global for the same reason the volatile GPRs are: one
 * guest routine can lift to several C functions, so a value written in one
 * body is read in the next. Defined in xbox_memory_layout.c like the rest of
 * the register file. */
extern RECOMP_TLS double g_fp_stack[8];
extern RECOMP_TLS int g_fp_top;
extern RECOMP_TLS uint16_t g_fp_control_word;
extern RECOMP_TLS int g_fp_cmp;
extern RECOMP_TLS RecompXmm g_xmm0, g_xmm1, g_xmm2, g_xmm3;
extern RECOMP_TLS RecompXmm g_xmm4, g_xmm5, g_xmm6, g_xmm7;
extern ptrdiff_t g_xbox_mem_offset;

/* ── XBE Constants ─────────────────────────────────────────── */

/*
 * TODO: Set these from your xbe_parser output.
 * Run: py -3 -m tools.xbe_parser game/default.xbe
 */
#define YOUR_GAME_ENTRY_POINT   0x00181AD2  /* XBE entry point VA */
#define YOUR_GAME_XBE_PATH      "game/default.xbe"
#define YOUR_GAME_DIR            "game"

/* ── Forward declarations ──────────────────────────────────── */

static BOOL load_xbe(const char *path, void **out_data, size_t *out_size);

/* Recompiled entry point (generated by recomp pipeline) */
extern void xbe_entry_point(void);

/* The end of the session log: the summary (journal.c), written out before
 * the process goes -- from the toolkit's exits (xbox_before_exit) and the
 * crash report. Once: a crash report can follow an exit's summary. */
static void session_end(void)
{
    static volatile int done;
    log_drain();                        /* the pump has seen the [EXIT] line */
    if (!done) {
        done = 1;
        journal_session_end();
        log_drain();
    }
}

/* ── VEH crash handler ─────────────────────────────────────── */

/*
 * Vectored Exception Handler for crash diagnostics.
 *
 * When the recompiled game hits an access violation, this handler prints
 * the faulting address, all Xbox register values, and a native stack trace.
 * This is your primary debugging tool during bring-up.
 *
 * Customize this for your game:
 *   - Add game-specific address checks (GPU register probes, etc.)
 *   - Add dumps of game-specific globals (heap handles, state flags)
 *   - Add SEH simulation if your game uses __try/__except
 */
/* Name the guest function a fault happened in, and recover the call chain.
 *
 * Recompiled code faults as ordinary native code, so the exception record
 * carries a host RIP and nothing else -- there is no guest program counter to
 * report, and the host address changes every build. Two things recover the
 * guest view:
 *
 *   - every generated function is a real symbol in the image (sub_005A03C0
 *     and so on), so the linker's PDB already maps host address back to guest
 *     function. dbghelp turns an anonymous RIP into that name.
 *
 *   - every lifted call pushes its guest return address onto the guest stack
 *     before jumping, so the stack still holds the chain. Scanning up from esp
 *     for values inside the code sections recovers it.
 *
 * ponytail: the stack scan is a scan, not a frame walk -- these are FPO frames
 * with no reliable ebp chain, so there is nothing to walk. It over-reports,
 * since addresses from returned-from calls linger below esp, but naming the
 * guest function is the whole question at a fault.
 *
 * Requires linking dbghelp and keeping the .pdb beside the .exe.
 */
typedef struct MCPXAPUState MCPXAPUState;
extern MCPXAPUState *mcpx_apu_init_standalone(uint8_t *ram_ptr);
extern MCPXAPUState *g_apu_state;

void print_guest_stack(void);

#ifdef _WIN32
static void print_guest_context(void *rip)
{
    /* SYMBOL_INFO is variable-length: the name is written past the struct, so
     * it must be over-allocated with MaxNameLen set to the slack. */
    char buf[sizeof(SYMBOL_INFO) + 256];
    SYMBOL_INFO *sym = (SYMBOL_INFO *)buf;
    DWORD64 disp = 0;

    memset(buf, 0, sizeof(buf));
    sym->SizeOfStruct = sizeof(SYMBOL_INFO);
    sym->MaxNameLen = 255;
    if (SymFromAddr(GetCurrentProcess(), (DWORD64)(uintptr_t)rip, &disp, sym))
        fprintf(stderr, "  in %s+0x%llX\n",
                sym->Name, (unsigned long long)disp);
    print_guest_stack();
}
#endif /* _WIN32 */
#ifndef _WIN32
const char *ssx3_LogsDir(void) { return "logs"; }
#endif

static DWORD WINAPI test_crash_thread(LPVOID arg)
{
    Sleep((DWORD)(intptr_t)arg * 1000);
    *(volatile int *)(uintptr_t)8 = 1;
    return 0;
}

/* A minidump of the crash in logs\crash_<time>.dmp: threads and stacks
 * (~1 MB; RECOMP_CRASH_DUMP=full adds the exe's data, ~45 MB; never the
 * guest's memory) -- open it with Visual Studio or WinDbg next to
 * build\ssx3_recomp.pdb. Once per run; RECOMP_CRASH_DUMP=0 skips it. */
static void write_crash_dump(EXCEPTION_POINTERS *ep)
{
#ifdef _WIN32
    static volatile LONG done;
    const char *e = getenv("RECOMP_CRASH_DUMP");
    char path[MAX_PATH];
    SYSTEMTIME st;
    HANDLE f;
    MINIDUMP_EXCEPTION_INFORMATION mei;
    if ((e && *e == '0') || InterlockedExchange(&done, 1))
        return;
    CreateDirectoryA(s_logs_dir, NULL);
    GetLocalTime(&st);
    snprintf(path, sizeof path, "%s\\crash_%04u%02u%02u-%02u%02u%02u.dmp", s_logs_dir, st.wYear,
             st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    f = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE)
        return;
    mei.ThreadId = GetCurrentThreadId();
    mei.ExceptionPointers = ep;
    mei.ClientPointers = FALSE;
    if (MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), f,
                          (MINIDUMP_TYPE)((e && (*e == 'f' || *e == 'F') ? MiniDumpWithDataSegs : 0)
                                          | MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules),
                          &mei, NULL, NULL))
        fprintf(stderr, "[CRASH] minidump written to %s\n", path);
    CloseHandle(f);
#else
    (void)ep;
#endif
}

#define GUEST_RAM(a) (((a) >= 0x10000 && (a) < 0x04000000) || ((a) >= 0x80000000u && (a) < 0x84000000u))

/* Guest registers (this thread's) and the stack's return addresses -- for
 * reports from files that cannot see the thread-local registers. */
void print_guest_regs_and_stack(void)
{
    fprintf(stderr, "  eax=0x%08X ecx=0x%08X edx=0x%08X ebx=0x%08X esi=0x%08X edi=0x%08X esp=0x%08X\n",
            g_eax, g_ecx, g_edx, g_ebx, g_esi, g_edi, g_esp);
#ifdef _WIN32
    {
        /* The native call chain names the guest functions exactly (each is a
         * symbol), unlike the stack scan below. */
        void *fr[20];
        USHORT n = RtlCaptureStackBackTrace(1, 20, fr, NULL), k;
        char buf[sizeof(SYMBOL_INFO) + 256];
        SYMBOL_INFO *sym = (SYMBOL_INFO *)buf;
        fprintf(stderr, "  native chain (image base %p):", (void *)GetModuleHandleA(NULL));
        for (k = 0; k < n; k++) {
            DWORD64 disp = 0;
            memset(buf, 0, sizeof(buf));
            sym->SizeOfStruct = sizeof(SYMBOL_INFO);
            sym->MaxNameLen = 255;
            if (SymFromAddr(GetCurrentProcess(), (DWORD64)(uintptr_t)fr[k], &disp, sym))
                fprintf(stderr, " %s+0x%llX", sym->Name, (unsigned long long)disp);
            else
                fprintf(stderr, " %p", fr[k]);
        }
        fputc('\n', stderr);
    }
#endif
    print_guest_stack();
    /* esi is usually the object being worked on: its first 0xB0 bytes, and
     * for each dword pointing into RAM the bytes there (strings show). */
    if (g_xbox_mem_offset && GUEST_RAM(g_esi) && GUEST_RAM(g_esi + 0xB0)) {
        const uint8_t *m = (const uint8_t *)((uintptr_t)g_xbox_mem_offset);
        uint32_t o;
        for (o = 0; o < 0xB0; o += 4) {
            uint32_t v = *(const uint32_t *)(m + g_esi + o);
            fprintf(stderr, "  [esi+%02X] %08X", o, v);
            if (GUEST_RAM(v) && GUEST_RAM(v + 16)) {
                const uint8_t *q = m + v;
                int k;
                fprintf(stderr, "  -> ");
                for (k = 0; k < 16; k++)
                    fprintf(stderr, "%02X", q[k]);
                fprintf(stderr, " '");
                for (k = 0; k < 16; k++)
                    fputc(q[k] >= 32 && q[k] < 127 ? q[k] : '.', stderr);
                fputc('\'', stderr);
            }
            fputc('\n', stderr);
        }
    }
    /* RECOMP_ICALL_NULL_MEM=va:len[,*va:len][,@off:len]: guest memory to show
     * as well; "*va" shows len bytes where the pointer at va points, "@off"
     * where the dword at guest esp+off points. */
    {
        const char *e = getenv("RECOMP_ICALL_NULL_MEM");
        while (e && *e && g_xbox_mem_offset) {
            const uint8_t *m = (const uint8_t *)((uintptr_t)g_xbox_mem_offset);
            int deref = (*e == '*'), onstack = (*e == '@');
            char *end;
            uint32_t va = (uint32_t)strtoul(e + deref + onstack, &end, 16), len = 16, k;
            if (*end == ':')
                len = (uint32_t)strtoul(end + 1, &end, 0);
            if (onstack)
                va = *(const uint32_t *)(m + g_esp + va);
            if (deref && GUEST_RAM(va))
                va = *(const uint32_t *)(m + va);
            if (GUEST_RAM(va) && GUEST_RAM(va + len)) {
                fprintf(stderr, "  [MEM %08X]", va);
                for (k = 0; k < len; k++)
                    fprintf(stderr, "%s%02X", k % 16 ? " " : "\n    ", m[va + k]);
                fputc('\n', stderr);
            }
            e = strchr(end, ',');
            if (e)
                e++;
        }
    }
}

void print_guest_stack(void)
{
    if (g_xbox_mem_offset && g_esp) {
        const uint32_t *sp =
            (const uint32_t *)((uintptr_t)g_xbox_mem_offset + g_esp);
        int shown = 0, i;
        fprintf(stderr, "  guest stack (return addresses, innermost first):\n");
        for (i = 0; i < 256 && shown < 24; i++) {
            uint32_t v = sp[i];
            if (v > g_xbox_code_lo && v < g_xbox_code_hi) {
                fprintf(stderr, "    [esp+%-4d] 0x%08X\n", i * 4, v);
                shown++;
            }
        }
        /* When the frame is gone, say so, and fall back on what is left.
         *
         * A fault inside a block copy leaves no return address in range: the
         * copy is a host memmove and its caller's frame is further up than
         * this reaches. Worse, a copy that ran past its buffer overwrites the
         * stack itself, so the list comes out empty exactly when it is most
         * needed -- and an empty list under a promising heading reads like
         * "nothing to see" rather than "the evidence is destroyed". */
        if (!shown)
            fprintf(stderr, "    (no return addresses in range)\n");

        /* Everything below only pays for itself when the scan came back
         * empty. A crash where the chain was recovered already says what
         * happened, and burying that under another twenty lines makes the
         * common report worse to read. */
        if (!shown) {
            uint32_t k;

            /* The indirect-call history, which the stack cannot overwrite.
             * recomp_manual.c prints this too, but only from
             * recomp_icall_fail_log -- a page fault inside a block copy
             * never reaches that path, and this is the same information by
             * the route the fault actually took.
             *
             * Declared here rather than included: main.c does not pull in
             * recomp_types.h, and recomp_manual.c spells it the same way. */
            extern volatile uint32_t g_icall_trace[16];
            extern volatile uint32_t g_icall_trace_idx;

            fprintf(stderr, "  recent ICALL targets:");
            for (k = 0; k < 16; k++)
                fprintf(stderr, " %08X",
                        g_icall_trace[(g_icall_trace_idx + k) & 15]);
            fprintf(stderr, "\n");

            /* The frame unfiltered. The pointers and lengths handed to
             * whatever faulted live here and look nothing like code, so the
             * filter above drops exactly what is wanted. On the crash this
             * was written for, one of these words was the end of the mapped
             * RAM mirror -- which is what identified the fault as a copy
             * running off its buffer rather than a stray pointer. */
            for (i = 0; i < 20; i++)
                fprintf(stderr, "    raw[esp+%-4d] 0x%08X\n", i * 4, sp[i]);
        }
    }
}

#ifdef _WIN32
/* The emulated APU.
 *
 * Two entry points that had no callers anywhere in the tree. apu_mmio_hook.c
 * documents apu_hook_handle_mmio as "called from VEH in main.c" and no main.c
 * called it; apu_decode_and_handle's first line is `if (!g_apu_state) return
 * false` and nothing assigned g_apu_state. So with RECOMP_AC97_READY set the
 * APU's registers were unmapped to be trapped, and every trap was then
 * declined and surfaced as an access violation on the first register
 * DirectSound touched.
 *
 * Declared rather than included so this does not depend on src/apu being on
 * the include path. */
extern bool apu_hook_handle_mmio(PCONTEXT ctx, uintptr_t fault_addr,
                                 uint32_t fault_xbox_va, int is_write);

/* The trapped span, matching what MemoryLayoutInit unmaps: the APU's own
 * 512 KB, not the whole MCPX aperture. AC'97 above it stays plain memory,
 * which is what the codec-ready bit needs. */
#define APU_TRAP_BASE 0xFE800000u
#define APU_TRAP_END  0xFE880000u

extern int ssx3_WatchHit(PEXCEPTION_POINTERS ep);

static LONG CALLBACK veh_handler(PEXCEPTION_POINTERS ep)
{
    if (ssx3_WatchHit(ep))
        return EXCEPTION_CONTINUE_EXECUTION;
    if (ep->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION) {
        uintptr_t fault_addr = ep->ExceptionRecord->ExceptionInformation[1];

        /*
         * GPU register probe at 0xFD000000 range.
         * Some games probe NV2A registers directly. On real hardware this
         * returns GPU state; here we just skip the instruction.
         * TODO: Implement mini x86-64 decoder for instruction skipping,
         * or connect to the xbox_nv2a library for proper handling.
         */
        if (fault_addr >= 0xFD000000 && fault_addr < 0xFE000000) {
            return EXCEPTION_CONTINUE_SEARCH;
        }

        /* USB host controllers -> the emulated OHCI (RECOMP_USB).
         *
         * xbox_OhciInit makes the two 4 KB register blocks at 0xFED00000 and
         * 0xFED08000 inaccessible so every access faults here; the model
         * decodes the instruction, answers it and steps over it. Without this
         * the template never routes them, so XAPI reads HcRevision out of
         * zeroed RAM and finds no controller -- no pad, no Start to skip the
         * intro with. */
        {
            extern int xbox_OhciOwnsAddress(uint32_t xbox_va);
            extern int xbox_OhciHandleMmio(void *ctx, uint32_t xbox_va);
            uint32_t xbox_va =
                (uint32_t)(fault_addr - (uintptr_t)g_xbox_mem_offset);

            if (xbox_OhciOwnsAddress(xbox_va)
                    && xbox_OhciHandleMmio(ep->ContextRecord, xbox_va))
                return EXCEPTION_CONTINUE_EXECUTION;
        }

        /* APU registers -> the emulated APU.
         *
         * The other half of RECOMP_AC97_READY: reporting the codec ready is
         * what lets DirectSoundCreate past its first gate, and from there it
         * drives the APU directly, so the registers have to fault to be seen.
         * "Handled" means the access was decoded and the instruction stepped
         * over, so execution resumes instead of unwinding. */
        {
            uint32_t xbox_va =
                (uint32_t)(fault_addr - (uintptr_t)g_xbox_mem_offset);

            if (xbox_va >= APU_TRAP_BASE && xbox_va < APU_TRAP_END
                    && apu_hook_handle_mmio(
                           ep->ContextRecord, fault_addr, xbox_va,
                           ep->ExceptionRecord->ExceptionInformation[0]
                               ? 1 : 0)) {
                return EXCEPTION_CONTINUE_EXECUTION;
            }
        }

        write_crash_dump(ep);
        fprintf(stderr, "[CRASH] Access violation at RIP=0x%llX, fault addr=0x%llX (%s)\n",
            (unsigned long long)ep->ContextRecord->Rip,
            (unsigned long long)fault_addr,
            ep->ExceptionRecord->ExceptionInformation[0] ? "write" : "read");
        fprintf(stderr, "  Xbox regs: eax=0x%08X ecx=0x%08X edx=0x%08X esp=0x%08X\n",
            g_eax, g_ecx, g_edx, g_esp);
        fprintf(stderr, "  Xbox regs: ebx=0x%08X esi=0x%08X edi=0x%08X\n",
            g_ebx, g_esi, g_edi);
        fprintf(stderr, "  Xbox VA of fault: 0x%08X\n",
            (uint32_t)(fault_addr - (uintptr_t)g_xbox_mem_offset));
        print_guest_context((void *)ep->ContextRecord->Rip);
        journal_crash_context();        /* where the game was, what the player did */

        /* Print native stack return addresses for debugging (the exe's own,
         * wherever ASLR put it -- a fixed 0x140000000 range found none). */
        {
            uintptr_t *sp = (uintptr_t *)ep->ContextRecord->Rsp;
            uintptr_t lo = (uintptr_t)GetModuleHandleA(NULL);
            uintptr_t hi = lo + ((IMAGE_NT_HEADERS *)(lo + ((IMAGE_DOS_HEADER *)lo)->e_lfanew))
                                    ->OptionalHeader.SizeOfImage;
            fprintf(stderr, "  Native stack (exe return addrs, image base 0x%llX):\n",
                    (unsigned long long)lo);
            for (int i = 0; i < 64 && sp[i]; i++) {
                if (sp[i] >= lo && sp[i] < hi) {
                    fprintf(stderr, "    [%d] 0x%llX\n", i, (unsigned long long)sp[i]);
                }
            }
        }
        fflush(stderr);
        session_end();
    }

    return EXCEPTION_CONTINUE_SEARCH;
}
#elif defined(__EMSCRIPTEN__)

/* Web: no faults. Every guest 8/16/32-bit access to the MCPX span
 * (0xFE800000-0xFEFFFFFF) arrives here from the checked accessors in
 * recomp_types.h; what no device owns is plain memory, as natively. */
#include <unistd.h>

extern uint64_t mcpx_apu_mmio_read(MCPXAPUState *d, uint64_t addr, unsigned int size);
extern void mcpx_apu_mmio_write(MCPXAPUState *d, uint64_t addr, uint64_t val, unsigned int size);
extern int xbox_OhciMmioAccess(uint32_t xbox_va, int is_write, uint64_t *val, int size);
extern int xbox_OhciOwnsAddress(uint32_t xbox_va);
extern void *xbox_GuestToHost(uint32_t va);

#define APU_MMIO_BASE 0xFE800000u
/* Natively only the first 0x30000 bytes are unmapped (the toolkit's
 * APU_TRAP_BYTES); the GP/EP DSP memory above stays plain RAM. */
#define APU_MMIO_END  0xFE830000u
#define AC97_BASE     0xFEC00000u

uint32_t xbox_mmio_read(uint32_t va, int size)
{
    uint8_t *p;
    uint64_t v = 0;

    if (g_apu_state && va >= APU_MMIO_BASE && va < APU_MMIO_END)
        return (uint32_t)mcpx_apu_mmio_read(g_apu_state, va - APU_MMIO_BASE, (unsigned)size);
    if (xbox_OhciOwnsAddress(va)) {
        xbox_OhciMmioAccess(va, 0, &v, size);
        return (uint32_t)v;
    }
    p = (uint8_t *)xbox_GuestToHost(va);
    return size == 1 ? *(volatile uint8_t *)p
         : size == 2 ? *(volatile uint16_t *)p : *(volatile uint32_t *)p;
}

void xbox_mmio_write(uint32_t va, uint32_t val, int size)
{
    uint8_t *p;
    uint64_t v = val;

    if (g_apu_state && va >= APU_MMIO_BASE && va < APU_MMIO_END) {
        mcpx_apu_mmio_write(g_apu_state, va - APU_MMIO_BASE, val, (unsigned)size);
        return;
    }
    if (xbox_OhciOwnsAddress(va)) {
        xbox_OhciMmioAccess(va, 1, &v, size);
        return;
    }
    p = (uint8_t *)xbox_GuestToHost(va);
    if (size == 1)      *(volatile uint8_t *)p = (uint8_t)val;
    else if (size == 2) *(volatile uint16_t *)p = (uint16_t)val;
    else                *(volatile uint32_t *)p = val;
    /* AC'97 bus-master reset (RR, bit 1 of each channel's control byte):
     * the XDK sets it and waits for the controller to clear it. */
    if (va >= AC97_BASE && va < AC97_BASE + 0x1000u) {
        uint32_t off;
        for (off = 0x10B; off < 0x180; off += 0x10) {
            volatile uint8_t *r = (volatile uint8_t *)xbox_GuestToHost(AC97_BASE + off);
            if (*r & 0x02)
                *r = (uint8_t)(*r & ~0x02);
        }
    }
}

static void ac97_arm(void) {}
static void install_crash_handler(void) {}

#else /* !_WIN32 */

#include <signal.h>
#include <ucontext.h>
#ifndef __ANDROID__
#include <execinfo.h>          /* no backtrace() in Bionic: Android skips it */
#define SSX3_HAVE_BACKTRACE 1
#endif
#include <unistd.h>
#include <sys/prctl.h>
#include <sys/mman.h>

#include "mmio_posix.h"

extern uint64_t mcpx_apu_mmio_read(MCPXAPUState *d, uint64_t addr, unsigned int size);
extern void mcpx_apu_mmio_write(MCPXAPUState *d, uint64_t addr, uint64_t val, unsigned int size);
extern int xbox_OhciMmioAccess(uint32_t xbox_va, int is_write, uint64_t *val, int size);

#define APU_MMIO_BASE 0xFE800000u
#define APU_MMIO_END  0xFE880000u

static uint32_t mmio_va(uintptr_t host)
{
    return (uint32_t)(host - (uintptr_t)g_xbox_mem_offset);
}

static uint64_t mmio_read(uintptr_t host, int size)
{
    uint32_t va = mmio_va(host);
    uint64_t v = 0;

    if (va >= APU_MMIO_BASE && va < APU_MMIO_END)
        return mcpx_apu_mmio_read(g_apu_state, va - APU_MMIO_BASE, size);
    xbox_OhciMmioAccess(va, 0, &v, size);
    return v;
}

static void mmio_write(uintptr_t host, uint64_t val, int size)
{
    uint32_t va = mmio_va(host);

    if (va >= APU_MMIO_BASE && va < APU_MMIO_END)
        mcpx_apu_mmio_write(g_apu_state, va - APU_MMIO_BASE, val, size);
    else
        xbox_OhciMmioAccess(va, 1, &val, size);
}

static int mmio_owned(uint32_t va)
{
    extern int xbox_OhciOwnsAddress(uint32_t xbox_va);

    return (g_apu_state && va >= APU_MMIO_BASE && va < APU_MMIO_END)
        || xbox_OhciOwnsAddress(va);
}

/* AC'97 bus-master reset bit (RR): the XDK sets it and waits for the controller
 * to clear it, reading the register once (see ac97_arm_write_trap in the
 * toolkit's xbox_memory_layout.c). The page is read-only; a write faults, is
 * emulated here, and every control byte's RR is cleared before the page is
 * closed again. */
#define AC97_BASE 0xFEC00000u
static size_t ac97_len;

static void ac97_arm(void)
{
    long pg = sysconf(_SC_PAGESIZE);
    void *p = (void *)((uintptr_t)g_xbox_mem_offset + AC97_BASE);

    ac97_len = pg > 0x1000 ? (size_t)pg : 0x1000;
    if (mprotect(p, ac97_len, PROT_READ) != 0) {
        fprintf(stderr, "  AC97: mprotect failed; a channel reset will spin\n");
        ac97_len = 0;
    }
}

static void ac97_wr(uintptr_t addr, uint64_t val, int size)
{
    memcpy((void *)addr, &val, (size_t)size);
}

static int ac97_write(void *uc, uintptr_t fault, uint32_t *insn)
{
    void *page = (void *)((uintptr_t)g_xbox_mem_offset + AC97_BASE);
    uint32_t off;
    int ok;

    mprotect(page, ac97_len, PROT_READ | PROT_WRITE);
    ok = mmio_posix_step(uc, fault, NULL, ac97_wr, insn);
    for (off = 0x10B; off < 0x180; off += 0x10) {
        volatile uint8_t *r = (volatile uint8_t *)((char *)page + off);
        if (*r & 0x02)
            *r = (uint8_t)(*r & ~0x02);
    }
    mprotect(page, ac97_len, PROT_READ);
    return ok;
}

#if defined(__ANDROID__)
/* /proc/self/maps lookup: a crash inside recompiled code reports a host pc and
 * a host fault address, and neither means anything without the module and the
 * offset into it. Cheap enough to do in a signal handler (reads a text file). */
static int maps_lookup(uintptr_t a, char *out, size_t out_size)
{
    FILE *f = fopen("/proc/self/maps", "r");
    char line[512];

    if (!f)
        return 0;
    while (fgets(line, sizeof line, f)) {
        unsigned long lo, hi;
        if (sscanf(line, "%lx-%lx", &lo, &hi) == 2 && a >= lo && a < hi) {
            char *nl = strchr(line, '\n');
            if (nl)
                *nl = 0;
            snprintf(out, out_size, "%s", line);
            fclose(f);
            return 1;
        }
    }
    fclose(f);
    return 0;
}

/* Walk the host stack words above SP and print the ones that land in executable
 * code: recompiled guest functions run natively, so the guest call chain is the
 * host call chain. */
static void print_native_stack(const ucontext_t *uc)
{
#if defined(__aarch64__)
    uintptr_t sp = (uintptr_t)uc->uc_mcontext.sp;
#elif defined(__x86_64__)
    uintptr_t sp = (uintptr_t)uc->uc_mcontext.gregs[REG_RSP];
#else
    uintptr_t sp = 0;
#endif
    int i, printed = 0;

    if (!sp)
        return;
    fprintf(stderr, "  host stack (SP=%p):\n", (void *)sp);
    for (i = 0; i < 48 && printed < 24; i++) {
        char map[512];
        uintptr_t w;

        if (!maps_lookup(sp + (uintptr_t)i * sizeof(uintptr_t), map, sizeof map))
            break;                     /* SP left the stack mapping */
        w = ((const uintptr_t *)sp)[i];
        if (!maps_lookup(w, map, sizeof map))
            continue;
        if (!strstr(map, " r-xp ") && !strstr(map, " r-xs "))
            continue;                  /* keep only executable targets */
        fprintf(stderr, "    [sp+%3d] %p  %s\n", i * (int)sizeof(uintptr_t),
                (void *)w, map);
        printed++;
    }
}
#endif

/* Trapped device registers are serviced here (src/mmio_posix.c); anything else
 * is a crash and gets reported. */
static void segv_handler(int sig, siginfo_t *si, void *uc_)
{
    ucontext_t *uc = (ucontext_t *)uc_;
    uintptr_t fault = (uintptr_t)si->si_addr;
#ifdef SSX3_HAVE_BACKTRACE
    void *bt[32];
    int n;
#endif
#if defined(__aarch64__)
    uintptr_t pc = (uintptr_t)uc->uc_mcontext.pc;
#else
    uintptr_t pc = (uintptr_t)uc->uc_mcontext.gregs[REG_RIP];
#endif

    uint32_t insn = 0;

    if (sig == SIGSEGV && ac97_len && g_xbox_mem_offset
            && mmio_va(fault) >= AC97_BASE && mmio_va(fault) < AC97_BASE + ac97_len
            && ac97_write(uc, fault, &insn))
        return;
    if (sig == SIGSEGV && g_xbox_mem_offset && mmio_owned(mmio_va(fault))
            && mmio_posix_step(uc, fault, mmio_read, mmio_write, &insn))
        return;

    fprintf(stderr, "[CRASH] signal %d at pc=%p, fault addr=%p, insn %08X\n",
            sig, (void *)pc, (void *)fault, insn);
    fprintf(stderr, "  Xbox regs: eax=0x%08X ecx=0x%08X edx=0x%08X esp=0x%08X\n",
            g_eax, g_ecx, g_edx, g_esp);
    fprintf(stderr, "  Xbox regs: ebx=0x%08X esi=0x%08X edi=0x%08X\n",
            g_ebx, g_esi, g_edi);
    fprintf(stderr, "  Xbox VA of fault: 0x%08X\n",
            (uint32_t)(fault - (uintptr_t)g_xbox_mem_offset));
#if defined(__ANDROID__)
    {
        char map[512];
        if (maps_lookup(pc, map, sizeof map))
            fprintf(stderr, "  pc in: %s\n", map);
        if (maps_lookup(fault, map, sizeof map))
            fprintf(stderr, "  fault in: %s\n", map);
        print_native_stack(uc);
    }
#endif
    print_guest_stack();
    journal_crash_context();
#ifdef SSX3_HAVE_BACKTRACE
    n = backtrace(bt, 32);
    backtrace_symbols_fd(bt, n, 2);
#endif
    session_end();
    _exit(139);
}

static void install_crash_handler(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = segv_handler;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    sigaction(SIGILL, &sa, NULL);
}
#endif /* _WIN32 */

/* The game folder (the rip: default.xbe + data\): RECOMP_GAME_DIR, else
 * "game" in the working directory, else "game" next to the exe or one level
 * up (build\ssx3_recomp.exe started by double-click). Fills `dir` with the
 * first that holds default.xbe, else lists where it looked in `tried`. */
static char s_game_dir[512], s_game_xbe[560];

static int find_game_dir(char *tried, size_t tried_size)
{
    char cand[4][512];
    int n = 0, i;
    const char *env = getenv("RECOMP_GAME_DIR");
    if (env && *env)
        snprintf(cand[n++], sizeof cand[0], "%s", env);
    snprintf(cand[n++], sizeof cand[0], "%s", YOUR_GAME_DIR);
#ifdef _WIN32
    {
        char exe[512], *slash;
        DWORD len = GetModuleFileNameA(NULL, exe, sizeof exe);
        if (len && len < sizeof exe && (slash = strrchr(exe, '\\')) != NULL) {
            *slash = 0;
            snprintf(cand[n++], sizeof cand[0], "%s\\%s", exe, YOUR_GAME_DIR);
            if ((slash = strrchr(exe, '\\')) != NULL) {
                *slash = 0;
                snprintf(cand[n++], sizeof cand[0], "%s\\%s", exe, YOUR_GAME_DIR);
            }
        }
    }
#endif
    tried[0] = 0;
    for (i = 0; i < n; i++) {
        FILE *f;
        snprintf(s_game_xbe, sizeof s_game_xbe, "%s/default.xbe", cand[i]);
        if ((f = fopen(s_game_xbe, "rb")) != NULL) {
            fclose(f);
            snprintf(s_game_dir, sizeof s_game_dir, "%s", cand[i]);
            return 1;
        }
        snprintf(tried + strlen(tried), tried_size - strlen(tried), "  %s\n", cand[i]);
    }
    return 0;
}


/* ── WinMain ───────────────────────────────────────────────── */

#ifndef _WIN32
/* No SEH/VEH on the POSIX shim: the crash handler below is Windows-only. */
#define WINAPI
typedef void *HINSTANCE;
#define _putenv_s(n, v) setenv((n), (v), 1)
int recomp_dispatch_init(void);
#endif

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance,
                   LPSTR lpCmdLine, int nCmdShow)
{
    {   /* the thread that will run the title's main loop (thread_dump.c) */
        extern DWORD g_ssx3_main_tid;
        g_ssx3_main_tid = GetCurrentThreadId();
    }
    void *xbe_data = NULL;
    size_t xbe_size = 0;

    (void)hInstance;
    (void)hPrevInstance;
    (void)lpCmdLine;
    (void)nCmdShow;

    /* Unbuffered output for immediate visibility during debugging */
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    /* Runtime options SSX 3 needs; set before anything reads them, and only
     * when not already set, so a run can still override them.
     *   RECOMP_TITLE_KEVENTS: XAPI's timer thread (guest 0x0021C1B5) and D3D
     *     build KEVENTs by hand; without host events behind them every wait
     *     fails at once and the timer thread spins.
     *   RECOMP_VBLANK: D3D waits on its vblank event (0x0024CE8C) between
     *     AvSetDisplayMode steps; the ISR at 0x0023B290 only sets it when a
     *     vblank interrupt is delivered.
     *   RECOMP_AC97_READY: the XDK DirectSound probes the AC'97 codec; when it
     *     is not ready DirectSoundCreate fails with E_OUTOFMEMORY and a game
     *     thread AddRefs the failed device (0xFFFFFFFF) in sub_0025FC35. */
    if (!getenv("RECOMP_TITLE_KEVENTS")) _putenv_s("RECOMP_TITLE_KEVENTS", "1");
    if (!getenv("RECOMP_VBLANK"))        _putenv_s("RECOMP_VBLANK", "1");
    if (!getenv("RECOMP_AC97_READY"))    _putenv_s("RECOMP_AC97_READY", "1");
    /*   (The DirectSound DSP command word is registered at run time by the
     *     sub_0025BD81 wrapper in ssx3_overrides.c; no RECOMP_DSP_ACK.)
     *   RECOMP_FB_WINDOW / RECOMP_PB_EXEC: show the guest framebuffer in a
     *     window and execute the pushbuffer into it, which is the only path
     *     from SSX 3's own D3D to the screen. */
    if (!getenv("RECOMP_FB_WINDOW"))     _putenv_s("RECOMP_FB_WINDOW", "1");
    if (!getenv("RECOMP_PB_EXEC"))       _putenv_s("RECOMP_PB_EXEC", "1");
    /*   RECOMP_D3D_PRESENT: show frames through a D3D11 swap chain with
     *     vsync (toolkit gpu_present.c; falls back to GDI if it fails). The
     *     GDI path blits the latest frame every Sleep(16), ~59 Hz against the
     *     game's 60: a doubled or skipped frame about once a second, and
     *     tearing. =0 for GDI. */
    if (!getenv("RECOMP_D3D_PRESENT"))   _putenv_s("RECOMP_D3D_PRESENT", "1");
    /*   RECOMP_USB: emulated OHCI host + gamepad (toolkit src/usb). XAPI
     *     enumerates it, reads the XID descriptor and polls reports; input
     *     comes from a host pad, RECOMP_KEYBOARD or the RECOMP_PAD_* probes. */
    if (!getenv("RECOMP_USB"))           _putenv_s("RECOMP_USB", "1");
    /*   RECOMP_RES: the Vulkan renderer's internal resolution, 1080 =
     *   1440x1080 (4:3 at full-HD height), shown directly in the window;
     *   the guest still gets 640x480. Scripted runs set 1 (live.ps1). */
    if (!getenv("RECOMP_RES")) {
        /* The primary monitor's height (1080, 1440, 2160...), so fullscreen
         * is sharp; 1080 if unknown. */
        int cy = 0;
        char buf[16];
#ifdef _WIN32
        cy = GetSystemMetrics(SM_CYSCREEN);
#endif
        snprintf(buf, sizeof buf, "%d", cy >= 480 ? cy : 1080);
        _putenv_s("RECOMP_RES", buf);
    }
    /*   RECOMP_WIDESCREEN: 16:9 in game (wider camera, 1920x1080 at
     *   RECOMP_RES=1080), menus pillarboxed 4:3. =0 for the original 4:3. */
    if (!getenv("RECOMP_WIDESCREEN"))    _putenv_s("RECOMP_WIDESCREEN", "auto");
    {
        /* RECOMP_WIDESCREEN -> RECOMP_ASPECT (width / height), which the
         * camera wrapper, the renderer and the window read: "auto" = the
         * primary monitor's shape to start with (32:9 on a 5120x1440
         * screen), then the window's whenever it is resized (vkr_backend.c
         * follow_window); "1" = 16:9, "W:H" = that, "0" = none (4:3).
         * Narrower than 4:3: none. */
        const char *ws = getenv("RECOMP_WIDESCREEN");
        double a = 0;
        if (ws && !strcmp(ws, "auto")) {
#ifdef _WIN32
            int cx = GetSystemMetrics(SM_CXSCREEN), cy = GetSystemMetrics(SM_CYSCREEN);
            a = cy > 0 ? (double)cx / cy : 0;
#endif
        } else if (ws && strchr(ws, ':')) {
            double w = atof(ws), h = atof(strchr(ws, ':') + 1);
            a = h > 0 ? w / h : 0;
        } else if (ws && *ws && *ws != '0') {
            a = 16.0 / 9.0;
        }
        if (a > 4.0 / 3.0 + 0.01 && a < 8.0) {
            char buf[32];
            snprintf(buf, sizeof buf, "%.4f", a);
            _putenv_s("RECOMP_ASPECT", buf);
            fprintf(stderr, "[BOOT] widescreen: aspect %.4f (%s)\n", a, ws);
        } else {
            _putenv_s("RECOMP_ASPECT", "");
        }
    }
#ifdef _WIN32
    /* RECOMP_USB_PADS: as many emulated pads as XInput controllers are
     * plugged in at start (1..4), so a second controller makes Multi Play
     * (2-player split-screen) available. Scripted runs (RECOMP_NO_HOST_PAD)
     * keep one; set it to force a count. */
    if (!getenv("RECOMP_USB_PADS")) {
        const char *nh = getenv("RECOMP_NO_HOST_PAD");
        int pads = 0;
        if (!(nh && *nh && *nh != '0')) {
            DWORD i;
            for (i = 0; i < 4; i++) {
                XINPUT_STATE st;
                if (XInputGetState(i, &st) == ERROR_SUCCESS)
                    pads++;
            }
        }
        if (pads > 1) {
            char n[4];
            snprintf(n, sizeof n, "%d", pads);
            _putenv_s("RECOMP_USB_PADS", n);
            fprintf(stderr, "[BOOT] %d controllers: %d emulated pads\n", pads, pads);
        }
    }
#else
    /* SDL's controllers (toolkit xinput_device.c), opened here on the main
     * thread before the guest polls them; then the same pad count as above. */
    {
        extern void xbox_InputInit(void);
        extern BOOL xbox_InputIsConnected(DWORD port);
        DWORD i;
        int pads = 0;
        xbox_InputInit();
        for (i = 0; i < 4; i++)
            pads += xbox_InputIsConnected(i) ? 1 : 0;
        fprintf(stderr, "[BOOT] %d controller(s) found\n", pads);
        if (pads > 1 && !getenv("RECOMP_USB_PADS")) {
            char n[4];
            snprintf(n, sizeof n, "%d", pads);
            _putenv_s("RECOMP_USB_PADS", n);
            fprintf(stderr, "[BOOT] %d controllers: %d emulated pads\n", pads, pads);
        }
    }
#endif
    /*   RECOMP_GUEST_LOCK: one guest thread runs guest code at a time, as on
     *     the Xbox's single CPU (threads switch at kernel calls). Without it
     *     the race load intermittently hung at 43% (resources stuck in state
     *     8, waiting on the streamer, sub_0020Bxxx) or crashed on a garbage
     *     pointer in the world streamer (0x0020D26A). */
    if (!getenv("RECOMP_GUEST_LOCK"))    _putenv_s("RECOMP_GUEST_LOCK", "1");
    /*   RECOMP_NV2A_ACK_THREAD: the GPU executor runs asynchronously, as the
     *   NV2A does, instead of the title waiting for every batch to be drawn
     *   -- ~2x the frame rate in a race (9-12 vs 4-7 FPS). =0 for the old
     *   lock-step mode. */
    if (!getenv("RECOMP_NV2A_ACK_THREAD")) _putenv_s("RECOMP_NV2A_ACK_THREAD", "1");
    /* Skip the intro movies (EA logo, THX, intro): the intro state plays the
     * movies whose bits are set in the mask at 0x003083B8 (bit 0 EA, 1 THX,
     * 2 intro; starts at 7) -- held at 0, none plays, and boot reaches the
     * title in seconds instead of ~35 s (found by Corne Dorrestijn,
     * scripts/race_loop.sh). RECOMP_SKIP_INTRO=0 plays them. Added to any
     * RECOMP_POKE a run already sets. */
    {
        const char *skip = getenv("RECOMP_SKIP_INTRO");
        if (!(skip && skip[0] == '0')) {
            const char *poke = getenv("RECOMP_POKE");
            char buf[256];
            snprintf(buf, sizeof buf, "%s%s0x3083B8:0", poke && *poke ? poke : "",
                     poke && *poke ? "," : "");
            _putenv_s("RECOMP_POKE", buf);
        }
    }

#if defined(__linux__) && defined(__aarch64__)
    /* The guest is x86: its lock-free code, and the pushbuffer / PUT hand-off
     * to the GPU threads, assume total store order. Apple cores (Asahi) can
     * run a thread in hardware TSO; inherited by every thread created later. */
    {
        extern int prctl(int, ...);
#ifndef PR_SET_MEM_MODEL
#define PR_SET_MEM_MODEL 0x4d4d444c
#define PR_SET_MEM_MODEL_TSO 1
#endif
        if (prctl(PR_SET_MEM_MODEL, PR_SET_MEM_MODEL_TSO, 0, 0, 0) != 0)
            fprintf(stderr, "[BOOT] hardware TSO unavailable; expect ordering bugs\n");
        else
            fprintf(stderr, "[BOOT] hardware TSO on\n");
    }
#endif

    printf("=== SSX 3 - Static Recompilation ===\n");
    printf("Loading XBE...\n");

    /* Install VEH handler (first handler in chain) */
    /* Load symbols up front rather than from inside the handler: at fault
     * time the process is already in a bad way, and SymInitialize
     * allocates. Failure is not fatal -- the handler prints no name. */
#ifdef _WIN32
    SymSetOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_UNDNAME);
    SymInitialize(GetCurrentProcess(), NULL, TRUE);
    AddVectoredExceptionHandler(1, veh_handler);
    {
        /* RECOMP_TEST_CRASH=<seconds>: fault on purpose after that long, to
         * test the crash report and minidump. */
        const char *tc = getenv("RECOMP_TEST_CRASH");
        if (tc && atoi(tc) > 0)
            CloseHandle(CreateThread(NULL, 0, test_crash_thread, (LPVOID)(intptr_t)atoi(tc), 0, NULL));
    }
#else
    install_crash_handler();
#endif

    /* Step 1: Load XBE */
    {
        char tried[2200], msg[2600];
        if (!find_game_dir(tried, sizeof tried)) {
            snprintf(msg, sizeof msg,
                     "SSX 3's default.xbe was not found. Looked in:\n%s\n"
                     "Put your own rip of the disc (default.xbe and the data folder) in a\n"
                     "folder named 'game' next to the program (or its parent folder),\n"
                     "or set RECOMP_GAME_DIR to the folder that holds default.xbe.", tried);
            fprintf(stderr, "%s\n", msg);
            MessageBoxA(NULL, msg, "SSX 3", MB_ICONERROR);
            return 1;
        }
#ifdef _WIN32
        {
            /* Started without a console or redirection (a double-click: the
             * exe is a GUI program): the log goes to logs\ beside the game
             * folder instead of nowhere. play.bat and the scripts redirect. */
            HANDLE h = GetStdHandle(STD_ERROR_HANDLE);
            if (!h || h == INVALID_HANDLE_VALUE || GetFileType(h) == FILE_TYPE_UNKNOWN) {
                char base[512], logp[600], *slash;
                SYSTEMTIME st;
                GetFullPathNameA(s_game_dir, sizeof base, base, NULL);
                if ((slash = strrchr(base, '\\')) != NULL)
                    *slash = 0;
                snprintf(logp, sizeof logp, "%s\\logs", base);
                CreateDirectoryA(logp, NULL);
                snprintf(s_logs_dir, sizeof s_logs_dir, "%s", logp);
                /* the last 20 sessions and 10 crash dumps (with this one) */
                logs_prune("ssx3_*_stderr.txt", 19);
                logs_prune("crash_*.dmp", 9);
                logs_prune("mark_*.bmp", 30);
                GetLocalTime(&st);
                snprintf(logp + strlen(logp), sizeof logp - strlen(logp),
                         "\\ssx3_%04u%02u%02u-%02u%02u%02u_stderr.txt", st.wYear, st.wMonth,
                         st.wDay, st.wHour, st.wMinute, st.wSecond);
                if (freopen(logp, "w", stderr))
                    setvbuf(stderr, NULL, _IONBF, 0);
                {
                    /* latest.txt: which file is this session's ("send me your log") */
                    char lp[700];
                    FILE *lf;
                    snprintf(lp, sizeof lp, "%s\\latest.txt", s_logs_dir);
                    if ((lf = fopen(lp, "w")) != NULL) {
                        fprintf(lf, "%s\n", strrchr(logp, '\\') + 1);
                        fclose(lf);
                    }
                }
                freopen("NUL", "w", stdout);
            }
        }
#endif
        if (log_pump_start()) {
            extern void (*volatile xbox_before_exit)(void);
            xbox_before_exit = session_end;
        }

    }
    if (!load_xbe(s_game_xbe, &xbe_data, &xbe_size)) {
        MessageBoxA(NULL, "default.xbe could not be read.", "SSX 3", MB_ICONERROR);
        return 1;
    }
    printf("XBE loaded: %zu bytes\n", xbe_size);
    journal_session_header(xbe_data, xbe_size, s_game_dir);

    /* Step 2: Initialize Xbox memory layout */
    printf("Initializing Xbox memory layout...\n");
    if (!xbox_MemoryLayoutInit(xbe_data, xbe_size)) {
        MessageBoxA(NULL, "Failed to initialize Xbox memory layout.\n"
                    "The required virtual address range may be unavailable.",
                    "Recomp", MB_ICONERROR);
        free(xbe_data);
        return 1;
    }

    g_xbox_mem_offset = xbox_GetMemoryOffset();
    printf("Xbox memory mapped. Offset: 0x%llX\n", (unsigned long long)g_xbox_mem_offset);

    /* Bring up the emulated APU.
     *
     * Gated on the same variable that unmaps its registers, because the two
     * halves are useless apart: a trap with no model declines every access,
     * and a model nothing traps into never sees a register. The APU walks
     * Xbox physical RAM to find voice buffers, so it gets the guest RAM
     * base. */
    if (getenv("RECOMP_AC97_READY")) {
        g_apu_state = mcpx_apu_init_standalone((uint8_t *)xbox_GetMemoryBase());
        fprintf(stderr, "[BOOT] emulated APU %s\n",
                g_apu_state ? "up" : "FAILED to initialise");
#ifndef _WIN32
        ac97_arm();
#endif
    }

    /* Step 3: Initialize Xbox kernel */
    printf("Initializing Xbox kernel replacement...\n");
    xbox_kernel_init();

    /* Step 4: Set game directory for file I/O path translation */
    {
        extern void xbox_path_init(const char *game_dir, const char *save_dir);
        /* RECOMP_SYSTEM_DIR: where the emulated hard disk's partition
         * images and cache live (default %LOCALAPPDATA%\xboxrecomp). Scripted
         * runs get their own (live.ps1): eight instances booting at once
         * shared one Partition5.img, and the one that lost the open quit to
         * the dashboard at boot. */
        {
            const char *sys = getenv("RECOMP_SYSTEM_DIR");
            xbox_path_init(s_game_dir, sys && *sys ? sys : NULL);
        }
    }

    /* Step 5: Initialize kernel bridge (thunk table in Xbox memory) */
    printf("Initializing kernel bridge...\n");
    xbox_kernel_bridge_init();

    /* RECOMP_GUARD_NULL=1: make guest page zero inaccessible, so a guest NULL
     * dereference faults at once (as on the console) instead of silently
     * reading zeros from the mapped window. A diagnostic: it turned a race
     * script "executing" a NULL bytecode pointer into a crash at the first
     * use of the missing object. */
    if (getenv("RECOMP_GUARD_NULL")) {
        DWORD old_protect;
        void *page0 = (void *)(uintptr_t)g_xbox_mem_offset;
        if (VirtualProtect(page0, 0x1000, PAGE_NOACCESS, &old_protect))
            fprintf(stderr, "  NULL guard: guest page 0 (native %p) inaccessible\n", page0);
        else
            fprintf(stderr, "  NULL guard: VirtualProtect failed (%lu)\n", GetLastError());
    }

    /* USB host controllers (src/usb/ohci.c): inert unless RECOMP_USB is set.
     * Needs guest memory mapped, which xbox_MemoryLayoutInit has done. */
    {
        extern void xbox_OhciInit(void);
        xbox_OhciInit();
    }

    /* Step 6: Initialize stack */
    g_esp = XBOX_STACK_TOP;

    /* D3D's block-until-GPU-time loop (guest 0x00234D10, in sub_00234BE0)
     * spins until the GPU's completed sequence, *[device+0x30], catches up
     * with the submitted one at device+0x2C. The device pointer lives at
     * 0x0024B0D8. Nothing executes the pushbuffer on the GPU here, so all
     * submitted work is complete; mirror that so the wait terminates. */
    xbox_Nv2aMirrorFence(0x0024B0D8, 0x2C, 0x30);

#ifdef SSX3_VULKAN
    {
        /* The Vulkan renderer is the default (race 53-58 FPS vs ~12 in
         * software); RECOMP_GPU=sw (or 0) keeps the software rasteriser,
         * which is also the fallback when no Vulkan device can be opened. */
        const char *gpu = getenv("RECOMP_GPU");
        int want = !(gpu && (!strcmp(gpu, "sw") || !strcmp(gpu, "0")));
        extern int ssx3_lensfx_nowait;
        if (want) {
            if (vkr_backend_install())
                ssx3_lensfx_nowait = 1;
            else
                fprintf(stderr, "[VKR] unavailable, software rasteriser stays\n");
        }
    }
#endif

    /*
     * TODO: Pre-initialize CRT globals if needed.
     *
     * Many Xbox games use the MSVC CRT. The CRT's __heap_init sets up a
     * heap descriptor at a game-specific address. You may need to
     * pre-initialize __active_heap to avoid small-block heap issues:
     *
     *   uint32_t *active_heap = (uint32_t *)((uint8_t *)g_xbox_mem_offset + ACTIVE_HEAP_VA);
     *   *active_heap = 1;  // 1 = system heap (HeapAlloc), avoids SBH init
     *
     * Find ACTIVE_HEAP_VA by searching the disassembly for __heap_init
     * or by looking for the CRT's __active_heap global in the data section.
     */

    printf("\n=== Initialization complete ===\n");
    printf("Entry point: 0x%08X\n", YOUR_GAME_ENTRY_POINT);
    printf("ESP: 0x%08X\n", g_esp);

    /* Build the flat dispatch table before the guest runs.
     *
     * Without this recomp_lookup falls back to a binary search over the
     * whole function table -- roughly log2(n) branches on *every*
     * indirect call, which for a 45,000-function C++ title is about 16
     * every time the game goes through a vtable. Half-Life 2 spent most
     * of its static initialisation inside recomp_lookup for exactly this
     * reason, and it read as a hang.
     *
     * Optional by design: if the allocation fails the search still works,
     * so a failure is worth one line and not a fatal error. */
    if (!recomp_dispatch_init())
        fprintf(stderr, "[BOOT] flat dispatch unavailable; "
                        "indirect calls will use the binary search\n");

    /* Arm the hang watchdog. Does nothing unless RECOMP_WATCHDOG_SECS is set,
     * and must be called from this thread -- the guest registers it samples are
     * thread-local, so it has to be handed the copies belonging to the thread
     * that runs guest code.
     *
     * Not optional boilerplate: without this call RECOMP_WATCHDOG_SECS is
     * silently inert, and the one diagnostic that tells a hang from slowness
     * does nothing while appearing to be set. */
    xbox_WatchdogStart();

    /* All-thread native stacks after RECOMP_THREAD_DUMP_SECS (src/thread_dump.c). */
    {
        extern void ssx3_ThreadDumpStart(void);
        extern void ssx3_PeekEveryStart(void);
        ssx3_ThreadDumpStart();
        ssx3_PeekEveryStart();      /* RECOMP_PEEK_EVERY_MS */
        {
            extern void ssx3_GameStateStart(void);
            ssx3_GameStateStart();  /* [STATE] lines + game_state.txt */
        }
        {
            extern void ssx3_MarkStart(void);
            ssx3_MarkStart();       /* F12 / both sticks: a player mark in the log */
        }
        {
            extern void ssx3_AutonavStart(void);
            ssx3_AutonavStart();    /* RECOMP_AUTONAV=race,...: menus -> race */
        }
    }

    /* Step 7: Call the recompiled entry point */
    printf("\nStarting game...\n");
    fflush(stdout);

    xbe_entry_point();

    printf("\nGame returned. Cleaning up...\n");

    /* Cleanup */
    xbox_kernel_shutdown();
    xbox_MemoryLayoutShutdown();
    free(xbe_data);

    return 0;
}

/* ── XBE Loading ───────────────────────────────────────────── */

static BOOL load_xbe(const char *path, void **out_data, size_t *out_size)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "Cannot open XBE: %s\n", path);
        return FALSE;
    }

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (size <= 0) {
        fclose(f);
        return FALSE;
    }

    void *data = malloc((size_t)size);
    if (!data) {
        fclose(f);
        return FALSE;
    }

    if (fread(data, 1, (size_t)size, f) != (size_t)size) {
        free(data);
        fclose(f);
        return FALSE;
    }

    fclose(f);
    *out_data = data;
    *out_size = (size_t)size;
    return TRUE;
}

#if defined(__EMSCRIPTEN__)
#include <emscripten/wasmfs.h>
#include <sys/stat.h>

/* The page (web/) copies the user's rip into the origin-private file system
 * once, as ssx3/game/; this mounts OPFS at /opfs, where saves and the
 * emulated hard disk persist too. main() runs on a worker (PROXY_TO_PTHREAD),
 * which the OPFS backend needs for its synchronous access handles. */
static void web_setup(void)
{
    backend_t opfs = wasmfs_create_opfs_backend();

    if (!opfs || wasmfs_create_directory("/opfs", 0777, opfs) != 0)
        fprintf(stderr, "[WEB] could not mount OPFS at /opfs\n");
    mkdir("/opfs/ssx3", 0777);
    mkdir("/opfs/ssx3/saves", 0777);
    mkdir("/opfs/ssx3/system", 0777);
    /* The page can set any of these through ?env=NAME=value. */
    setenv("RECOMP_GAME_DIR", "/opfs/ssx3/game", 0);
    setenv("RECOMP_SAVE_DIR", "/opfs/ssx3/saves", 0);
    setenv("RECOMP_SYSTEM_DIR", "/opfs/ssx3/system", 0);
    /* The software rasteriser draws 640x480 4:3; nothing un-squeezes a
     * widened camera. */
    setenv("RECOMP_WIDESCREEN", "0", 0);
    setenv("RECOMP_KEYBOARD", "1", 0);
}
#endif

/* Console entry point (for debugging -- lets you see printf output) */
int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
#if defined(__EMSCRIPTEN__)
    web_setup();
#endif
#ifdef _WIN32
    return WinMain(GetModuleHandle(NULL), NULL, GetCommandLineA(), SW_SHOW);
#else
    return WinMain(NULL, NULL, NULL, 0);
#endif
}
