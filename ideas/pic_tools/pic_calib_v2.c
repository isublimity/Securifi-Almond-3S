/*
 * pic_calib_v2 — Send calibration with phase alignment
 * PIC alternates ACK/NACK - need to hit ACK phase for writes
 *
 * Build: zig cc -target mipsel-linux-musleabi -O2 -static -o pic_calib_v2 pic_calib_v2.c
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

static void hexdump(const char *label, unsigned char *buf, int len)
{
    printf("%s:", label);
    for (int i = 0; i < len; i++)
        printf(" %02X", buf[i]);
    printf("\n");
}

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

/* Try I2C write-only (no read after) */
static int pic_write_only(unsigned char *buf, int len)
{
    struct i2c_msg msg = { .addr = PIC_ADDR, .flags = 0, .len = len, .buf = buf };
    struct i2c_rdwr_ioctl_data data = { .msgs = &msg, .nmsgs = 1 };
    return ioctl(fd, I2C_RDWR, &data);
}

int main(int argc, char **argv)
{
    unsigned char buf[20] = {0};
    unsigned char resp[8] = {0};
    int ret;
    int attempt;

    fd = open("/dev/i2c-0", O_RDWR);
    if (fd < 0) {
        perror("open /dev/i2c-0");
        return 1;
    }

    printf("=== PIC16 Calibration v2 (phase-aware) ===\n\n");

    /* Strategy: try each operation up to 3 times until ACK */

    /* Step 1: Wake command */
    printf("--- Step 1: Wake ---\n");
    for (attempt = 0; attempt < 3; attempt++) {
        unsigned char cmd[] = { 0x33, 0x00, 0x01 };
        ret = pic_write_read(cmd, 3, resp, 4);
        printf("  attempt %d: ret=%d", attempt, ret);
        if (ret >= 0) {
            printf(" OK resp: %02X %02X %02X %02X\n", resp[0], resp[1], resp[2], resp[3]);
            break;
        }
        printf(" NACK, retrying...\n");
        /* Shift phase with a dummy read */
        pic_read(buf, 1);
        usleep(10000);
    }
    usleep(50000);

    /* Step 2: Calibration table 1 — retry without dummy read (just immediate retry) */
    printf("\n--- Step 2: Calibration Table 1 (401 bytes, cmd=0x03) ---\n");
    for (attempt = 0; attempt < 5; attempt++) {
        ret = pic_write_read((unsigned char *)pic_calib1, 401, resp, 4);
        printf("  attempt %d: ret=%d", attempt, ret);
        if (ret >= 0) {
            printf(" OK resp: %02X %02X %02X %02X\n", resp[0], resp[1], resp[2], resp[3]);
            break;
        }
        printf(" NACK\n");
        /* NO dummy read — just retry immediately to flip phase */
        usleep(10000);
    }
    usleep(100000);

    /* Check state after calib1 */
    pic_read(buf, 7);
    hexdump("After calib1", buf, 7);

    /* Step 3: Calibration table 2 — same retry strategy */
    printf("\n--- Step 3: Calibration Table 2 (401 bytes, cmd=0x2E) ---\n");
    for (attempt = 0; attempt < 5; attempt++) {
        ret = pic_write_read((unsigned char *)pic_calib2, 401, resp, 4);
        printf("  attempt %d: ret=%d", attempt, ret);
        if (ret >= 0) {
            printf(" OK resp: %02X %02X %02X %02X\n", resp[0], resp[1], resp[2], resp[3]);
            break;
        }
        printf(" NACK\n");
        usleep(10000);
    }
    usleep(100000);

    /* Check state after calib2 */
    pic_read(buf, 7);
    hexdump("After calib2", buf, 7);

    /* Step 4: Battery read */
    printf("\n--- Step 4: Battery Read ---\n");
    usleep(1000000); /* Wait 1 sec for PIC to process */
    for (attempt = 0; attempt < 5; attempt++) {
        unsigned char cmd[] = { 0x2F, 0x00, 0x02 };
        unsigned char bat[17] = {0};
        ret = pic_write_read(cmd, 3, bat, 17);
        printf("  attempt %d: ret=%d", attempt, ret);
        if (ret >= 0) {
            printf("\n");
            hexdump("  Battery", bat, 17);
            if (bat[0] != 0xAA && bat[0] != 0x00) {
                int raw = (bat[0] << 8) | bat[1];
                printf("  -> raw_adc=%d (0x%03X) — REAL DATA!\n", raw, raw);
            } else if (bat[0] == 0xAA) {
                printf("  -> Still test pattern\n");
            }
            break;
        }
        printf(" NACK\n");
        pic_read(buf, 1);
        usleep(10000);
    }

    /* Step 5: Simple read to check final state */
    printf("\n--- Final state ---\n");
    pic_read(buf, 7);
    hexdump("Final", buf, 7);

    /* Also try write-only (without repeated-start read) */
    printf("\n--- Bonus: Write-only test ---\n");
    for (attempt = 0; attempt < 3; attempt++) {
        unsigned char cmd[] = { 0x33, 0x00, 0x01 };
        ret = pic_write_only(cmd, 3);
        printf("  write-only attempt %d: ret=%d\n", attempt, ret);
        if (ret >= 0) break;
        pic_read(buf, 1);
        usleep(10000);
    }

    close(fd);
    return 0;
}
