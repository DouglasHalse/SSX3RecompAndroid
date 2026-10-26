/* XDVDFS (original Xbox disc image) reader and extractor.
 *
 * The port needs the disc's files (default.xbe and data/) on the device, and
 * the only copy a player has is their own disc image. This reads the image in
 * place -- no temporary copy, which matters because the image and the extracted
 * tree together would need twice the space -- through a plain file descriptor,
 * so the same code serves a normal file on the host and an Android
 * Storage-Access-Framework descriptor handed over by the file picker.
 *
 * Format, all little endian:
 *   volume descriptor at image offset 0x10000 (a trimmed .xiso) or
 *   0x18300000 + 0x10000 (a redump-style image, whose first 405 MiB are the
 *   video partition). Its bytes 20/24 are the root directory's start sector and
 *   table size, sectors relative to the descriptor minus 0x10000.
 *   A directory table is a B-tree of dirents:
 *     u16 left, u16 right, u32 start_sector, u32 size, u8 attributes,
 *     u8 name_len, char name[name_len]
 *   left/right are offsets in 4-byte units from the start of the same table;
 *   attributes bit 0x10 marks a directory.
 */
#ifndef SSX3_XISO_H
#define SSX3_XISO_H

#include <stddef.h>
#include <stdint.h>

/* Fingerprint of the XBE this port was recompiled from. A different region's
 * disc runs different code at the addresses the generated source calls, so it
 * has to be refused before a 2 GB extraction, not after a dead boot. */
#define XISO_PAL_XBE_SIZE   3428352u
#define XISO_TITLE_ID       0x4541003Cu

typedef struct {
    uint64_t image_size;
    uint32_t vd_offset;       /* volume descriptor offset in the image */
    uint32_t partition_base;  /* sector 0 of the filesystem */
    uint32_t title_id;        /* from the XBE certificate, 0 when unreadable */
    uint32_t xbe_size;
    int      has_xbe;         /* a default.xbe was found in the root */
    int      matches_this_port;
} xiso_info;

/* progress: called before each file is written. total is the sum of all file
 * sizes, so a caller can show a real bar. */
typedef void (*xiso_progress_fn)(void *user, const char *path,
                                 unsigned long long done,
                                 unsigned long long total);

/* Read the descriptor and fingerprint the root XBE. 0 on success; negative on
 * failure, with a player-readable reason in msg. */
int xiso_probe(int fd, xiso_info *info, char *msg, size_t msg_cap);

/* Extract every file into dest_dir (created if needed), preserving the
 * hierarchy. Skips files whose name would escape dest_dir. 0 on success. */
int xiso_extract(int fd, const char *dest_dir, xiso_progress_fn progress,
                 void *user, char *msg, size_t msg_cap);

#endif /* SSX3_XISO_H */
