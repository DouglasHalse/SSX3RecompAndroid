/* HD texture replacement and the texture dump it is built from.
 *
 * The game's textures are identified by a hash of their decoded pixels
 * (A8R8G8B8, width, height): independent of where the title put them, of
 * the format they are stored in and of a palette's address, so the same
 * texture has the same key in every run.
 *
 * RECOMP_TEX_DUMP=<dir>: every texture decoded for the first time is
 * written to <dir>/<key>_<w>x<h>_f<format>.tga (once per key) -- the set
 * tools/hdtex_match.py matches replacement images against.
 *
 * RECOMP_HDTEX=<dir> (default "hd-textures": Bl4ckH4nd's SSX 3 PS2 pack,
 * https://gbatemp.net/threads/my-ps2-packs.621422/; 0 = off): ssx3_map.txt
 * in that folder maps keys to its DDS files, one per
 * line, "<key> <file.dds> [a2]" (a2: the file's alpha is on the PS2's
 * 0..128 scale, doubled when sampled). A mapped texture is drawn from the DDS (BC7,
 * all its mip levels) instead of the game's own pixels; the texture
 * coordinates are normalised by the game's size (vkr_backend.c), so any
 * replacement size works. Only BC7 DDS files (DX10 header, DXGI 98/99).
 */
#include "../log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#ifdef _WIN32
#include <windows.h>
#include <direct.h>
#define mkdir_one(p) _mkdir(p)
#else
#include <sys/stat.h>
#include <unistd.h>
#define mkdir_one(p) mkdir(p, 0755)
#endif
#include "vkr.h"
#include "vkr_hdtex.h"

static int s_init;
static int s_off;                       /* replacements switched off */
static volatile int s_want = -1;        /* hdtex_request: 1 on, 0 off, -1 none */
static char s_dump_dir[512];
static char s_hd_dir[512];

typedef struct { uint64_t key; char *file; int failed; uint32_t flags; } MapEntry;
static MapEntry *s_map;
static size_t s_nmap;

static uint64_t *s_dumped;       /* open addressing; 0 = empty */
static size_t s_dumped_cap, s_ndumped;

static unsigned s_hd_loaded, s_hd_fail;

static int cmp_entry(const void *a, const void *b)
{
    uint64_t x = ((const MapEntry *)a)->key, y = ((const MapEntry *)b)->key;
    return x < y ? -1 : x > y;
}

static void load_map(const char *dir)
{
    char path[600], line[600];
    FILE *f;
    size_t cap = 0;
    snprintf(path, sizeof path, "%s/ssx3_map.txt", dir);
    if (!(f = fopen(path, "r"))) {
        fprintf(stderr, "[HDTEX] no HD texture pack at %s: the game's own textures\n", dir);
        return;
    }
    while (fgets(line, sizeof line, f)) {
        char *p = line, *name;
        uint64_t key;
        while (*p == ' ' || *p == '\t')
            p++;
        if (*p == '#' || *p == ';' || *p == '\n' || !*p)
            continue;
        key = strtoull(p, &name, 16);
        while (*name == ' ' || *name == '\t')
            name++;
        name[strcspn(name, "\r\n")] = 0;
        {
            char *sp = strchr(name, ' ');
            uint32_t fl = 0;
            if (sp) {
                *sp++ = 0;
                if (strstr(sp, "a2"))
                    fl |= VKR_TEX_ALPHA2;
            }
            if (!key || !*name)
                continue;
            if (s_nmap == cap) {
                cap = cap ? cap * 2 : 1024;
                s_map = (MapEntry *)realloc(s_map, cap * sizeof *s_map);
            }
            s_map[s_nmap].flags = fl;
        }
        s_map[s_nmap].key = key;
        s_map[s_nmap].file = strdup(name);
        s_map[s_nmap].failed = 0;
        s_nmap++;
    }
    fclose(f);
    qsort(s_map, s_nmap, sizeof *s_map, cmp_entry);
    log_msg(LOG_INFO, "HDTEX", "%zu replacement textures mapped (%s)", s_nmap, path);
}

/* The pack's default place: "hd-textures" in the working directory, else
 * next to the exe, else one folder up -- the launcher starts the game in
 * the exe's folder, and a local build's exe sits in build\ while the pack
 * is in the project folder (the HD textures silently stayed off there). */
static int has_map(const char *dir)
{
    char path[1100];
    FILE *f;
    snprintf(path, sizeof path, "%s/ssx3_map.txt", dir);
    if (!(f = fopen(path, "rb")))
        return 0;
    fclose(f);
    return 1;
}

static void default_dir(char *out, size_t n)
{
    char exe[1024];
    size_t len = 0;
    snprintf(out, n, "hd-textures");
    if (has_map(out))
        return;
#ifdef _WIN32
    len = GetModuleFileNameA(NULL, exe, sizeof exe);
#elif defined(__linux__)
    {
        ssize_t r = readlink("/proc/self/exe", exe, sizeof exe - 1);
        len = r > 0 ? (size_t)r : 0;
    }
#endif
    if (!len || len >= sizeof exe)
        return;
    exe[len] = 0;
    while (len && exe[len - 1] != '/' && exe[len - 1] != '\\')
        len--;
    if (!len)
        return;
    exe[len - 1] = 0;                  /* the exe's folder */
    snprintf(out, n, "%s/hd-textures", exe);
    if (has_map(out))
        return;
    while (len > 1 && exe[len - 2] != '/' && exe[len - 2] != '\\')
        len--;
    if (len > 1) {
        exe[len - 2] = 0;              /* one up */
        snprintf(out, n, "%s/hd-textures", exe);
        if (has_map(out))
            return;
    }
    snprintf(out, n, "hd-textures");
}

static void init(void)
{
    const char *e;
    if (s_init)
        return;
    s_init = 1;
    if ((e = getenv("RECOMP_TEX_DUMP")) && *e && *e != '0') {
        snprintf(s_dump_dir, sizeof s_dump_dir, "%s", e);
        mkdir_one(s_dump_dir);
        fprintf(stderr, "[HDTEX] dumping textures to %s\n", s_dump_dir);
    }
    e = getenv("RECOMP_HDTEX");
    if (e && *e == '0' && !e[1]) {
        s_off = 1;                     /* hdtex_request(1) can still load it */
        return;
    }
    if (e && *e)
        snprintf(s_hd_dir, sizeof s_hd_dir, "%s", e);
    else
        default_dir(s_hd_dir, sizeof s_hd_dir);
    load_map(s_hd_dir);
}

/* On/off while the game runs (a key, live.c): asked from any thread, done
 * here on the render thread, which is the only one using the map. */
void hdtex_request(int on)
{
    s_want = on ? 1 : 0;
}

int hdtex_enabled(void)
{
    int w = s_want;
    return w >= 0 ? w : !s_off && s_nmap > 0;
}

int hdtex_active(void)
{
    int w;
    init();
    if ((w = s_want) >= 0) {
        s_want = -1;
        s_off = !w;
        if (w && !s_nmap) {            /* off at start: find and load the pack now */
            if (!s_hd_dir[0])
                default_dir(s_hd_dir, sizeof s_hd_dir);
            load_map(s_hd_dir);
        }
    }
    return s_dump_dir[0] || (s_nmap && !s_off);
}

uint64_t hdtex_key(const uint32_t *px, uint32_t w, uint32_t h)
{
    uint64_t k = 1469598103934665603ull;
    size_t i, n = (size_t)w * h;
    for (i = 0; i < n; i++)
        k = (k ^ px[i]) * 1099511628211ull;
    k ^= ((uint64_t)w << 40) ^ ((uint64_t)h << 20);
    return k ? k : 1;
}

static int dumped_add(uint64_t key)      /* 1 if new */
{
    size_t i;
    if (s_ndumped * 2 + 2 > s_dumped_cap) {
        size_t cap = s_dumped_cap ? s_dumped_cap * 2 : 4096, j;
        uint64_t *n = (uint64_t *)calloc(cap, sizeof *n);
        for (j = 0; j < s_dumped_cap; j++)
            if (s_dumped[j]) {
                i = (size_t)(s_dumped[j] * 0x9E3779B97F4A7C15ull >> 20) & (cap - 1);
                while (n[i]) i = (i + 1) & (cap - 1);
                n[i] = s_dumped[j];
            }
        free(s_dumped);
        s_dumped = n; s_dumped_cap = cap;
    }
    i = (size_t)(key * 0x9E3779B97F4A7C15ull >> 20) & (s_dumped_cap - 1);
    while (s_dumped[i]) {
        if (s_dumped[i] == key)
            return 0;
        i = (i + 1) & (s_dumped_cap - 1);
    }
    s_dumped[i] = key;
    s_ndumped++;
    return 1;
}

void hdtex_dump(uint64_t key, const uint32_t *px, uint32_t w, uint32_t h, uint32_t format)
{
    char path[640];
    uint8_t hdr[18];
    FILE *f;
    uint32_t y;
    init();
    if (!s_dump_dir[0] || !dumped_add(key))
        return;
    snprintf(path, sizeof path, "%s/%016llx_%ux%u_f%02X.tga", s_dump_dir,
             (unsigned long long)key, w, h, format);
    if (!(f = fopen(path, "wb")))
        return;
    memset(hdr, 0, sizeof hdr);
    hdr[2] = 2;                                   /* uncompressed true colour */
    hdr[12] = (uint8_t)w; hdr[13] = (uint8_t)(w >> 8);
    hdr[14] = (uint8_t)h; hdr[15] = (uint8_t)(h >> 8);
    hdr[16] = 32; hdr[17] = 0x28;                 /* 8 alpha bits, top-down */
    fwrite(hdr, 1, sizeof hdr, f);
    for (y = 0; y < h; y++)                       /* A8R8G8B8 = BGRA bytes */
        fwrite(px + (size_t)y * w, 4, w, f);
    fclose(f);
}

static MapEntry *find(uint64_t key)
{
    size_t lo = 0, hi = s_nmap;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (s_map[mid].key < key) lo = mid + 1;
        else hi = mid;
    }
    return lo < s_nmap && s_map[lo].key == key ? &s_map[lo] : NULL;
}

/* 1 created (*out), 0 not mapped (or a bad file), -1 mapped but not now. */
int hdtex_get(VkrContext *vk, uint64_t key, uint64_t cache_key, VkrTex **out)
{
    MapEntry *e;
    char path[1100];
    FILE *f;
    uint8_t hdr[148];
    uint8_t *data;
    long size;
    uint32_t w, h, mips, dxgi;
    init();
    if (!s_nmap || !(e = find(key)) || e->failed || !vkr_has_bc7(vk))
        return 0;
    snprintf(path, sizeof path, "%s/%s", s_hd_dir, e->file);
    if (!(f = fopen(path, "rb"))) {
        fprintf(stderr, "[HDTEX] cannot open %s\n", path);
        e->failed = 1; s_hd_fail++;
        return 0;
    }
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size < 148 || fread(hdr, 1, 148, f) != 148 || memcmp(hdr, "DDS ", 4)
        || memcmp(hdr + 84, "DX10", 4)) {
        fclose(f);
        log_msg(LOG_WARN, "HDTEX", "%s: not a DX10 DDS", e->file);
        e->failed = 1; s_hd_fail++;
        return 0;
    }
    memcpy(&h, hdr + 12, 4); memcpy(&w, hdr + 16, 4);
    memcpy(&mips, hdr + 28, 4); memcpy(&dxgi, hdr + 128, 4);
    if (!mips)
        mips = 1;
    if (dxgi != 98 && dxgi != 99) {
        fclose(f);
        log_msg(LOG_WARN, "HDTEX", "%s: DXGI format %u, only BC7 (98/99) is supported", e->file, dxgi);
        e->failed = 1; s_hd_fail++;
        return 0;
    }
    data = (uint8_t *)malloc((size_t)(size - 148));
    if (!data || fread(data, 1, (size_t)(size - 148), f) != (size_t)(size - 148)) {
        fclose(f); free(data);
        e->failed = 1; s_hd_fail++;
        return 0;
    }
    fclose(f);
    *out = vkr_tex_create_bc7(vk, cache_key, w, h, mips, data, (size_t)(size - 148), e->flags);
    free(data);
    if (!*out)
        return -1;                     /* the frame's staging buffer is full */
    if (++s_hd_loaded <= 8 || (s_hd_loaded & 63) == 0)
        log_msg(LOG_DEBUG, "HDTEX", "%u loaded (%s, %ux%u, %u mips)", s_hd_loaded, e->file, w, h, mips);
    return 1;
}
