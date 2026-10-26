/* Host test harness for src/android/xiso.c: probe and extract a disc image. */
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "xiso.h"

static void on_progress(void *user, const char *path, unsigned long long done,
                        unsigned long long total)
{
    (void)user;
    if (total)
        fprintf(stderr, "  %5.1f%%  %s\n", 100.0 * (double)done / (double)total, path);
    else
        fprintf(stderr, "  %s\n", path);
}

int main(int argc, char **argv)
{
    char msg[256] = "";
    int fd;

    if (argc < 3) {
        fprintf(stderr, "usage: %s probe|extract <image> [dest]\n", argv[0]);
        return 2;
    }
    fd = open(argv[2], O_RDONLY);
    if (fd < 0) {
        perror("open");
        return 2;
    }
    if (strcmp(argv[1], "probe") == 0) {
        xiso_info info;
        if (xiso_probe(fd, &info, msg, sizeof msg) != 0) {
            printf("FAIL %s\n", msg);
            return 1;
        }
        printf("size=%llu vd=0x%X base=0x%X title=0x%08X xbe_size=%u pal=%d\n",
               (unsigned long long)info.image_size, info.vd_offset,
               info.partition_base, info.title_id, info.xbe_size,
               info.matches_this_port);
        return 0;
    }
    if (strcmp(argv[1], "extract") == 0) {
        if (argc < 4) {
            fprintf(stderr, "extract needs a destination\n");
            return 2;
        }
        if (xiso_extract(fd, argv[3], on_progress, NULL, msg, sizeof msg) != 0) {
            printf("FAIL %s\n", msg);
            return 1;
        }
        printf("OK\n");
        return 0;
    }
    fprintf(stderr, "unknown command %s\n", argv[1]);
    return 2;
}
