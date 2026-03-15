/*
 * pic_test — Тест чтения батареи PIC16 через lcd_drv ioctl
 *
 * Usage:
 *   pic_test raw    — raw PIC I2C read (ioctl 3)
 *   pic_test bat    — battery command 0x2F read (ioctl 2)
 *   pic_test        — both
 *
 * Build: zig cc -target mipsel-linux-musleabi -O2 -static -o pic_test pic_test.c
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>

#define PIC_LEN 17

static void hexdump(const char *label, unsigned char *buf, int len)
{
    printf("%s:", label);
    for (int i = 0; i < len; i++)
        printf(" %02X", buf[i]);
    printf("\n");
}

int main(int argc, char **argv)
{
    int fd = open("/dev/lcd", O_RDWR);
    if (fd < 0) {
        perror("open /dev/lcd");
        return 1;
    }

    int do_raw = 1, do_bat = 1;
    if (argc > 1) {
        if (strcmp(argv[1], "raw") == 0)
            do_bat = 0;
        else if (strcmp(argv[1], "bat") == 0)
            do_raw = 0;
    }

    if (do_raw) {
        unsigned char buf[PIC_LEN] = {0};
        int ret = ioctl(fd, 3, buf);
        if (ret < 0) {
            perror("ioctl(3) raw PIC read");
        } else {
            hexdump("PIC raw", buf, PIC_LEN);
        }
    }

    if (do_bat) {
        unsigned char buf[PIC_LEN] = {0};
        int ret = ioctl(fd, 2, buf);
        if (ret < 0) {
            perror("ioctl(2) battery read");
        } else {
            hexdump("PIC bat", buf, PIC_LEN);

            /* Try to interpret the data */
            printf("\nInterpretation attempt:\n");
            printf("  Byte 0: 0x%02X", buf[0]);
            if (buf[0] == 0xAA) printf(" (READY)");
            else if (buf[0] == 0xEE) printf(" (ERROR/ACTIVE)");
            printf("\n");

            /* Try 16-bit values (big-endian) */
            for (int i = 0; i + 1 < PIC_LEN; i += 2) {
                int val = (buf[i] << 8) | buf[i+1];
                if (val > 0 && val < 0xFFFF)
                    printf("  [%d-%d] BE: %d (0x%04X)\n", i, i+1, val, val);
            }
        }
    }

    close(fd);
    return 0;
}
