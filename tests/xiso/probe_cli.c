/* Host harness for the disc reader: probe and optionally extract a real image.
 *
 *   cc -std=gnu11 -g -fsanitize=address,undefined -I ../../src/android \
 *      probe_cli.c ../../src/android/xiso.c -o probe_cli
 *   ./probe_cli "/path/game.iso" [dest-dir]
 *
 * Prints what the launcher would show, then (with a destination) extracts and
 * prints the byte count. Run it under ASan before assuming the phone failed
 * for a platform reason: the reader is plain C and does not care what it runs
 * on.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>

#include "xiso.h"

static unsigned long long g_last_print;

static void progress(void *user, const char *path, unsigned long long done,
                     unsigned long long total)
{
    (void)user;
    (void)path;
    if (done == 0 || total == 0 || done == total || done - g_last_print >= (64u << 20)) {
        fprintf(stderr, "  %llu / %llu MB  %s\n", done >> 20, total >> 20, path);
        g_last_print = done;
    }
}

int main(int argc, char **argv)
{
    xiso_info info;
    char msg[512] = "";
    int fd;

    if (argc < 2) {
        fprintf(stderr, "usage: %s <image.iso> [dest-dir]\n", argv[0]);
        return 2;
    }
    fd = open(argv[1], O_RDONLY);
    if (fd < 0) {
        perror("open");
        return 1;
    }
    if (xiso_probe(fd, &info, msg, sizeof msg) != 0) {
        printf("probe failed: %s\n", msg);
        return 1;
    }
    printf("probe ok\n");
    printf("  image      %llu bytes (%llu MB)\n",
           (unsigned long long)info.image_size, (unsigned long long)(info.image_size >> 20));
    printf("  descriptor at 0x%X, partition base 0x%X\n",
           (unsigned)info.vd_offset, (unsigned)info.partition_base);
    printf("  title id   0x%08X\n", (unsigned)info.title_id);
    printf("  xbe        %u bytes\n", (unsigned)info.xbe_size);
    printf("  this port? %s\n", info.matches_this_port ? "yes" : "NO");

    if (argc >= 3) {
        printf("extracting to %s\n", argv[2]);
        if (xiso_extract(fd, argv[2], progress, NULL, msg, sizeof msg) != 0) {
            printf("extract failed: %s\n", msg);
            close(fd);
            return 1;
        }
        printf("extract ok\n");
    }
    close(fd);
    return 0;
}
