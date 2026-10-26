/* XDVDFS reader/extractor -- see xiso.h for the format and the why. */
#include "xiso.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>   /* strcasecmp */
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define SECTOR        2048u
#define XISO_MAGIC    "MICROSOFT*XBOX*MEDIA"
#define VD_CANDIDATE  0x10000ULL            /* trimmed .xiso */
#define VD_REDUMP     (0x18300000ULL + 0x10000ULL)  /* behind the video partition */
#define MAX_TABLE     (4u << 20)            /* a directory table this big is broken */
#define MAX_DEPTH     32
#define COPY_CHUNK    (1u << 20)

typedef struct {
    int         fd;
    uint32_t    base;          /* partition origin in the image */
    unsigned long long total;  /* sum of file sizes, for progress */
    unsigned long long done;
    xiso_progress_fn progress;
    void       *user;
    char       *msg;
    size_t      msg_cap;
} xiso_ctx;

static int fail(xiso_ctx *c, const char *what, const char *detail)
{
    if (c->msg && c->msg_cap)
        snprintf(c->msg, c->msg_cap, "%s%s%s", what, detail && detail[0] ? ": " : "",
                 detail ? detail : "");
    return -1;
}

static int read_at(int fd, unsigned long long off, void *buf, size_t n)
{
    unsigned char *p = (unsigned char *)buf;
    size_t got = 0;

    while (got < n) {
        ssize_t r = pread(fd, p + got, n - got, (off_t)(off + got));
        if (r < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (r == 0)
            return -1;          /* short image */
        got += (size_t)r;
    }
    return 0;
}

static int write_all(int fd, const void *buf, size_t n)
{
    const unsigned char *p = (const unsigned char *)buf;
    size_t put = 0;

    while (put < n) {
        ssize_t w = write(fd, p + put, n - put);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        put += (size_t)w;
    }
    return 0;
}

/* Find the volume descriptor: the two layouts we know first (a trimmed image
 * costs two reads, a redump image three), then a sector-by-sector scan for
 * anything else. */
static int find_descriptor(xiso_ctx *c, unsigned long long image_size,
                           uint32_t *vd_out, char *msg, size_t msg_cap)
{
    char magic[20];
    unsigned long long off;

    if (image_size > VD_CANDIDATE &&
        read_at(c->fd, VD_CANDIDATE, magic, sizeof magic) == 0 &&
        memcmp(magic, XISO_MAGIC, sizeof magic) == 0) {
        *vd_out = (uint32_t)VD_CANDIDATE;
        return 0;
    }
    if (image_size > VD_REDUMP &&
        read_at(c->fd, VD_REDUMP, magic, sizeof magic) == 0 &&
        memcmp(magic, XISO_MAGIC, sizeof magic) == 0) {
        *vd_out = (uint32_t)VD_REDUMP;
        return 0;
    }
    for (off = VD_CANDIDATE; off + sizeof magic < image_size && off < (1ULL << 30);
         off += SECTOR) {
        if (read_at(c->fd, off, magic, sizeof magic) == 0 &&
            memcmp(magic, XISO_MAGIC, sizeof magic) == 0) {
            *vd_out = (uint32_t)off;
            return 0;
        }
    }
    if (msg && msg_cap)
        snprintf(msg, msg_cap, "no Xbox filesystem found in this file");
    return -1;
}

static uint16_t rd16(const unsigned char *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const unsigned char *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

/* A name we are willing to turn into a path: no separators, no traversal, not
 * empty, and short enough for our buffers. The image is untrusted input. */
static int name_ok(const char *name, size_t len)
{
    size_t i;

    if (len == 0 || len >= 255)
        return 0;
    if ((len == 1 && name[0] == '.') || (len == 2 && name[0] == '.' && name[1] == '.'))
        return 0;
    for (i = 0; i < len; i++) {
        unsigned char ch = (unsigned char)name[i];
        if (ch == '/' || ch == '\\' || ch == ':' || ch < 0x20 || ch == 0x7f)
            return 0;
    }
    return 1;
}

/* Walk one directory table's B-tree, calling back for each entry. */
typedef int (*entry_fn)(xiso_ctx *c, const char *path, const char *name,
                        uint32_t start, uint32_t size, int is_dir, void *user);

static int walk_table(xiso_ctx *c, const unsigned char *table, uint32_t table_size,
                      uint32_t off, unsigned char *seen, const char *prefix,
                      entry_fn fn, void *user)
{
    uint16_t left, right;
    uint32_t start, size;
    unsigned char attrs, namelen;
    char name[256];
    char path[1024];
    int is_dir, rc;

    if (off + 14 > table_size || seen[off / 4])
        return 0;
    seen[off / 4] = 1;

    left    = rd16(table + off);
    right   = rd16(table + off + 2);
    start   = rd32(table + off + 4);
    size    = rd32(table + off + 8);
    attrs   = table[off + 12];
    namelen = table[off + 13];

    if (namelen && off + 14u + namelen <= table_size &&
        name_ok((const char *)table + off + 14, namelen)) {
        memcpy(name, table + off + 14, namelen);
        name[namelen] = '\0';
        is_dir = (attrs & 0x10) != 0;
        if (*prefix)
            snprintf(path, sizeof path, "%s/%s", prefix, name);
        else
            snprintf(path, sizeof path, "%s", name);
        rc = fn(c, path, name, start, size, is_dir, user);
        if (rc != 0)
            return rc;
    }

    if (left && walk_table(c, table, table_size, left * 4u, seen, prefix, fn, user) != 0)
        return -1;
    if (right && walk_table(c, table, table_size, right * 4u, seen, prefix, fn, user) != 0)
        return -1;
    return 0;
}

typedef struct {
    xiso_ctx *c;
    int       depth;
} walk_state;

/* Read a directory table into memory and walk it. Depth-limited: a malformed
 * image can otherwise describe a directory that contains itself. */
static int walk_dir(xiso_ctx *c, uint32_t dir_start, uint32_t dir_size,
                    const char *prefix, entry_fn fn, void *user, int depth)
{
    unsigned char *table;
    unsigned char *seen;
    uint32_t table_size;
    int rc;

    if (depth > MAX_DEPTH)
        return fail(c, "directory tree too deep", prefix);
    if (dir_size == 0)
        return 0;
    if (dir_size > MAX_TABLE)
        return fail(c, "directory table too large", prefix);

    table_size = dir_size;
    table = (unsigned char *)malloc(table_size ? table_size : 1);
    seen = (unsigned char *)calloc(table_size / 4 + 1, 1);
    if (!table || !seen) {
        free(table);
        free(seen);
        return fail(c, "out of memory", prefix);
    }
    if (read_at(c->fd, (unsigned long long)c->base + (unsigned long long)dir_start * SECTOR,
                table, table_size) != 0) {
        free(table);
        free(seen);
        return fail(c, "truncated image", prefix);
    }
    rc = walk_table(c, table, table_size, 0, seen, prefix, fn, user);
    free(table);
    free(seen);
    return rc;
}

/* ---- probe ---------------------------------------------------------- */

typedef struct {
    int      want_total;
    uint32_t xbe_start;
    uint32_t xbe_size;
    int      found_xbe;
} probe_state;

static int probe_entry(xiso_ctx *c, const char *path, const char *name,
                       uint32_t start, uint32_t size, int is_dir, void *user)
{
    probe_state *st = (probe_state *)user;

    (void)path;
    if (is_dir) {
        if (st->want_total)
            return walk_dir(c, start, size, name, probe_entry, user, 1);
        return 0;
    }
    if (st->want_total) {
        c->total += size;
        return 0;
    }
    if (!st->found_xbe && strcasecmp(name, "default.xbe") == 0) {
        st->found_xbe = 1;
        st->xbe_start = start;
        st->xbe_size = size;
    }
    return 0;
}

/* SSX 3's XBE from the PAL disc: base 0x10000, image 0x3AF780, and this dword
 * at the entry code. A USA/JP disc shares the header shape but not the code. */
static int xbe_matches_this_port(const unsigned char *hdr, uint32_t size)
{
    if (size != XISO_PAL_XBE_SIZE)
        return 0;
    if (memcmp(hdr, "XBEH", 4) != 0)
        return 0;
    if (rd32(hdr + 0x104) != 0x00010000u || rd32(hdr + 0x10C) != 0x003AF780u)
        return 0;
    return 1;
}

int xiso_probe(int fd, xiso_info *info, char *msg, size_t msg_cap)
{
    xiso_ctx c;
    unsigned char vd[36];
    unsigned char hdr[0x200];
    uint32_t vd_off = 0, root_sector, root_size;
    uint64_t size64;
    probe_state st;
    uint32_t cert_va, base_va, cert_off;
    off_t end;

    if (!info)
        return -1;
    memset(info, 0, sizeof *info);
    memset(&c, 0, sizeof c);
    c.fd = fd;
    c.msg = msg;
    c.msg_cap = msg_cap;

    end = lseek(fd, 0, SEEK_END);
    if (end <= 0)
        return fail(&c, "cannot read the image", NULL);
    size64 = (uint64_t)end;
    info->image_size = size64;

    if (find_descriptor(&c, size64, &vd_off, msg, msg_cap) != 0)
        return -1;
    info->vd_offset = vd_off;
    info->partition_base = vd_off - 0x10000u;
    c.base = info->partition_base;

    if (read_at(fd, vd_off, vd, sizeof vd) != 0)
        return fail(&c, "truncated image", "volume descriptor");
    root_sector = rd32(vd + 20);
    root_size   = rd32(vd + 24);

    /* Root listing: find default.xbe and fingerprint it. */
    memset(&st, 0, sizeof st);
    if (walk_dir(&c, root_sector, root_size, "", probe_entry, &st, 0) != 0)
        return -1;
    if (!st.found_xbe)
        return fail(&c, "this image has no default.xbe", NULL);

    info->has_xbe = 1;
    if (read_at(fd, (unsigned long long)c.base + (unsigned long long)st.xbe_start * SECTOR,
                hdr, sizeof hdr) != 0)
        return fail(&c, "truncated image", "default.xbe");

    base_va = rd32(hdr + 0x104);
    cert_va = rd32(hdr + 0x118);
    cert_off = cert_va - base_va;
    if (memcmp(hdr, "XBEH", 4) == 0 && cert_off > 0 && cert_off < 0x10000) {
        if (read_at(fd, (unsigned long long)c.base +
                            (unsigned long long)st.xbe_start * SECTOR + cert_off,
                    hdr, 12) == 0)
            info->title_id = rd32(hdr + 8);
    }
    info->xbe_size = st.xbe_size;
    {
        unsigned char head[0x200];
        if (read_at(fd, (unsigned long long)c.base + (unsigned long long)st.xbe_start * SECTOR,
                    head, sizeof head) == 0)
            info->matches_this_port = xbe_matches_this_port(head, st.xbe_size);
    }
    return 0;
}

/* ---- extract -------------------------------------------------------- */

static int mkdir_p(const char *path)
{
    char tmp[1024];
    size_t i, len;

    snprintf(tmp, sizeof tmp, "%s", path);
    len = strlen(tmp);
    for (i = 1; i < len; i++) {
        if (tmp[i] == '/') {
            tmp[i] = '\0';
            if (mkdir(tmp, 0775) != 0 && errno != EEXIST)
                return -1;
            tmp[i] = '/';
        }
    }
    if (mkdir(tmp, 0775) != 0 && errno != EEXIST)
        return -1;
    return 0;
}

typedef struct {
    const char *dest;
    int         depth;
} extract_state;

static int extract_entry(xiso_ctx *c, const char *path, const char *name,
                         uint32_t start, uint32_t size, int is_dir, void *user)
{
    extract_state *st = (extract_state *)user;
    const char *dest_root = st->dest;
    char dest[2048];
    unsigned char *buf;
    uint32_t left = size;
    unsigned long long off;
    int out;

    (void)name;
    snprintf(dest, sizeof dest, "%s/%s", dest_root, path);
    if (is_dir) {
        extract_state sub;
        if (mkdir_p(dest) != 0)
            return fail(c, "cannot create folder", dest);
        sub.dest = st->dest;
        sub.depth = st->depth + 1;
        return walk_dir(c, start, size, path, extract_entry, &sub, sub.depth);
    }

    if (mkdir_p(dest_root) != 0)
        return fail(c, "cannot create the game folder", dest_root);
    {
        /* create the file's folder */
        char dir[2048];
        char *slash;
        snprintf(dir, sizeof dir, "%s", dest);
        slash = strrchr(dir, '/');
        if (slash) {
            *slash = '\0';
            if (mkdir_p(dir) != 0)
                return fail(c, "cannot create folder", dir);
        }
    }

    if (c->progress)
        c->progress(c->user, path, c->done, c->total);

    out = open(dest, O_WRONLY | O_CREAT | O_TRUNC, 0664);
    if (out < 0)
        return fail(c, "cannot write", dest);
    buf = (unsigned char *)malloc(COPY_CHUNK);
    if (!buf) {
        close(out);
        return fail(c, "out of memory", NULL);
    }
    off = (unsigned long long)c->base + (unsigned long long)start * SECTOR;
    while (left) {
        uint32_t n = left < COPY_CHUNK ? left : COPY_CHUNK;
        if (read_at(c->fd, off, buf, n) != 0) {
            free(buf);
            close(out);
            return fail(c, "truncated image", path);
        }
        if (write_all(out, buf, n) != 0) {
            free(buf);
            close(out);
            return fail(c, "write failed (storage full?)", dest);
        }
        off += n;
        left -= n;
        c->done += n;
    }
    free(buf);
    close(out);
    return 0;
}

int xiso_extract(int fd, const char *dest_dir, xiso_progress_fn progress,
                 void *user, char *msg, size_t msg_cap)
{
    xiso_ctx c;
    unsigned char vd[36];
    uint32_t vd_off = 0, root_sector, root_size;
    probe_state st;
    extract_state xs;
    off_t end;

    if (!dest_dir)
        return -1;
    memset(&c, 0, sizeof c);
    c.fd = fd;
    c.progress = progress;
    c.user = user;
    /* c.user would collide with the extract callback's own user pointer, so
     * pass the destination through the ctx instead. */
    c.msg = msg;
    c.msg_cap = msg_cap;

    end = lseek(fd, 0, SEEK_END);
    if (end <= 0)
        return fail(&c, "cannot read the image", NULL);
    if (find_descriptor(&c, (unsigned long long)end, &vd_off, msg, msg_cap) != 0)
        return -1;
    c.base = vd_off - 0x10000u;
    if (read_at(fd, vd_off, vd, sizeof vd) != 0)
        return fail(&c, "truncated image", "volume descriptor");
    root_sector = rd32(vd + 20);
    root_size   = rd32(vd + 24);

    /* Total size first, so progress is a real fraction and not a file count. */
    c.total = 0;
    memset(&st, 0, sizeof st);
    st.want_total = 1;
    if (walk_dir(&c, root_sector, root_size, "", probe_entry, &st, 0) != 0)
        return -1;

    if (progress)
        progress(user, "", 0, c.total);
    if (mkdir_p(dest_dir) != 0)
        return fail(&c, "cannot create the game folder", dest_dir);
    xs.dest = dest_dir;
    xs.depth = 0;
    return walk_dir(&c, root_sector, root_size, "", extract_entry, &xs, 0);
}
