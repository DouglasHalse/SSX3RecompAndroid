#include "ssx3_music_capture.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <time.h>
#endif

static FILE    *g_f;
static int      g_state;        /* 0 = unchecked, 1 = on, 2 = off */
static uint64_t g_t0;
static int      g_lines;
static const char *(*g_screen)(void);

static uint64_t now_ms(void)
{
#ifdef _WIN32
    return (uint64_t)GetTickCount64();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000);
#endif
}

void mx_set_screen_provider(const char *(*fn)(void)) { g_screen = fn; }

int mx_on(void)
{
    const char *p;

    if (g_state)
        return g_state == 1;
    p = getenv("RECOMP_MUSIC_CAPTURE");
    if (!p || !*p) {
        g_state = 2;
        return 0;
    }
    g_f = fopen(p, "w");
    if (!g_f) {
        fprintf(stderr, "[MX] cannot open %s for writing\n", p);
        g_state = 2;
        return 0;
    }
    g_t0 = now_ms();
    g_state = 1;
    fprintf(stderr, "[MX] music capture -> %s\n", p);
    return 1;
}

void mx_sig(const char *kind, uint32_t va, const uint32_t *args, int n)
{
    int i;

    if (!mx_on() || !g_f)
        return;
    fprintf(g_f, "{\"t\":%llu,\"k\":\"%s\",\"va\":\"0x%08X\",\"s\":\"%s\",\"a\":[",
            (unsigned long long)(now_ms() - g_t0), kind, va,
            g_screen ? (g_screen() ? g_screen() : "") : "");
    for (i = 0; i < n; i++)
        fprintf(g_f, "%s%u", i ? "," : "", args[i]);
    fprintf(g_f, "]}\n");
    if ((++g_lines & 63) == 0)
        fflush(g_f);
}

void mx_note(const char *kind, const char *text)
{
    if (!mx_on() || !g_f)
        return;
    fprintf(g_f, "{\"t\":%llu,\"k\":\"%s\",\"text\":\"%s\"}\n",
            (unsigned long long)(now_ms() - g_t0), kind, text ? text : "");
    fflush(g_f);
}

void mx_close(void)
{
    if (!g_f)
        return;
    fflush(g_f);
    fclose(g_f);
    g_f = NULL;
    g_state = 2;
}
