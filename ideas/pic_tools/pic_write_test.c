/*
 * pic_write_test — Test PIC16 write with various sizes
 * Find max write size PIC accepts
 *
 * Build: zig cc -target mipsel-linux-musleabi -O2 -static -o pic_write_test pic_write_test.c
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/i2c.h>
#include <linux/i2c-dev.h>

#define PIC_ADDR 0x2A

typedef unsigned char u8;
#include "pic_calib.h"

static int fd;

static int pic_write_read(unsigned char *wbuf, int wlen,
                          unsigned char *rbuf, int rlen)
{
    struct i2c_msg msgs[2] = {
        { .addr = PIC_ADDR, .flags = 0,        .len = wlen, .buf = wbuf },
        { .addr = PIC_ADDR, .flags = I2C_M_RD, .len = rlen, .buf = rbuf },
    };
    struct i2c_rdwr_ioctl_data data = { .msgs = msgs, .nmsgs = 2 };
    return ioctl(fd, I2C_RDWR, &data);
}

static int pic_read(unsigned char *buf, int len)
{
    struct i2c_msg msg = { .addr = PIC_ADDR, .flags = I2C_M_RD, .len = len, .buf = buf };
    struct i2c_rdwr_ioctl_data data = { .msgs = &msg, .nmsgs = 1 };
    return ioctl(fd, I2C_RDWR, &data);
}

int main(int argc, char **argv)
{
    unsigned char buf[20] = {0};
    unsigned char resp[8] = {0};
    int ret;

    fd = open("/dev/i2c-0", O_RDWR);
    if (fd < 0) {
        perror("open /dev/i2c-0");
        return 1;
    }

    printf("=== PIC16 Write Size Test ===\n\n");

    /* Test write sizes: 1, 2, 3, 4, 8, 16, 32, 64, 128, 256, 401 */
    int sizes[] = { 1, 2, 3, 4, 5, 8, 9, 16, 17, 32, 33, 64, 128, 256, 401 };
    int nsizes = sizeof(sizes) / sizeof(sizes[0]);

    for (int i = 0; i < nsizes; i++) {
        int sz = sizes[i];
        memset(resp, 0, 8);

        /* Use calib1 data for write (starts with 0x03) */
        ret = pic_write_read((unsigned char *)pic_calib1, sz, resp, 4);
        printf("write(%3d bytes) + read: ret=%d", sz, ret);
        if (ret >= 0)
            printf("  resp: %02X %02X %02X %02X", resp[0], resp[1], resp[2], resp[3]);
        else
            printf("  NACK");
        printf("\n");

        usleep(50000);
    }
    printf("\n");

    /* Also test single commands */
    printf("--- Single command tests ---\n");
    unsigned char cmds[][4] = {
        { 0x03 },                    /* calib1 cmd byte only */
        { 0x2E },                    /* calib2 cmd byte only */
        { 0x33 },                    /* wake */
        { 0x2F },                    /* battery read */
        { 0x41 },                    /* DANGEROUS: known to kill PIC! SKIP */
        { 0x00 },
    };
    int cmd_lens[] = { 1, 1, 1, 1, /* skip 0x41 */ 1 };
    char *cmd_names[] = { "0x03 (calib1)", "0x2E (calib2)", "0x33 (wake)",
                          "0x2F (bat)", /* "0x41 (KILL!)" */ "0x00" };
    int ncmds = 5;

    for (int i = 0; i < ncmds; i++) {
        if (cmds[i][0] == 0x41) {
            printf("SKIP 0x41 (kills PIC!)\n");
            continue;
        }
        memset(resp, 0, 8);
        ret = pic_write_read(cmds[i], 1, resp, 7);
        printf("cmd %s: ret=%d", cmd_names[i], ret);
        if (ret >= 0)
            printf("  resp: %02X %02X %02X %02X %02X %02X %02X",
                   resp[0], resp[1], resp[2], resp[3], resp[4], resp[5], resp[6]);
        else
            printf("  NACK");
        printf("\n");
        usleep(50000);
    }

    /* Final state */
    printf("\n--- Final state ---\n");
    pic_read(buf, 7);
    printf("Final: %02X %02X %02X %02X %02X %02X %02X\n",
           buf[0], buf[1], buf[2], buf[3], buf[4], buf[5], buf[6]);

    close(fd);
    return 0;
}
