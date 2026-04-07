/*
 * pic_calib_v3 — PIC16 calibration experiments
 * Try different I2C transaction formats
 * Build: zig cc -target mipsel-linux-musleabi -O2 -static -o pic_calib_v3 pic_calib_v3.c
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
    for (int i = 0; i < len; i++) printf(" %02X", buf[i]);
    printf("\n");
}

static int pic_wr(unsigned char *wbuf, int wlen, unsigned char *rbuf, int rlen)
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

static int pic_write(unsigned char *buf, int len)
{
    struct i2c_msg msg = { .addr = PIC_ADDR, .flags = 0, .len = len, .buf = buf };
    struct i2c_rdwr_ioctl_data data = { .msgs = &msg, .nmsgs = 1 };
    return ioctl(fd, I2C_RDWR, &data);
}

int main(void)
{
    unsigned char buf[20] = {0};
    unsigned char resp[8] = {0};
    int ret;

    fd = open("/dev/i2c-0", O_RDWR);
    if (fd < 0) { perror("/dev/i2c-0"); return 1; }

    printf("=== PIC16 Calibration v3 ===\n\n");

    /* Initial state */
    ret = pic_read(buf, 7);
    printf("Initial read: ret=%d ", ret);
    hexdump("data", buf, 7);

    /* Test 1: Write-only command 0x03 (no read after) */
    printf("\n--- Test 1: write-only cmd 0x03 ---\n");
    {
        unsigned char cmd = 0x03;
        ret = pic_write(&cmd, 1);
        printf("write(0x03): ret=%d\n", ret);
        usleep(50000);
        pic_read(buf, 7);
        hexdump("after", buf, 7);
    }

    /* Test 2: Write cmd+data in chunks (8 bytes at a time) */
    printf("\n--- Test 2: calib1 in 8-byte chunks ---\n");
    {
        int i;
        int ok = 0, fail = 0;
        for (i = 0; i < 401; i += 8) {
            int len = (401 - i) < 8 ? (401 - i) : 8;
            ret = pic_write((unsigned char *)pic_calib1 + i, len);
            if (ret >= 0) ok++; else fail++;
            usleep(1000);  /* 1ms between chunks */
        }
        printf("Sent %d chunks: %d OK, %d FAIL\n", ok+fail, ok, fail);
        usleep(200000);
        pic_read(buf, 7);
        hexdump("after calib1 chunks", buf, 7);
    }

    /* Test 3: Calib2 in 8-byte chunks */
    printf("\n--- Test 3: calib2 in 8-byte chunks ---\n");
    {
        int i;
        int ok = 0, fail = 0;
        for (i = 0; i < 401; i += 8) {
            int len = (401 - i) < 8 ? (401 - i) : 8;
            ret = pic_write((unsigned char *)pic_calib2 + i, len);
            if (ret >= 0) ok++; else fail++;
            usleep(1000);
        }
        printf("Sent %d chunks: %d OK, %d FAIL\n", ok+fail, ok, fail);
        usleep(200000);
        pic_read(buf, 7);
        hexdump("after calib2 chunks", buf, 7);
    }

    /* Test 4: Full calib1 as write-only (no read) + retry */
    printf("\n--- Test 4: calib1 full write-only ---\n");
    {
        int attempt;
        for (attempt = 0; attempt < 3; attempt++) {
            ret = pic_write((unsigned char *)pic_calib1, 401);
            printf("  attempt %d: ret=%d\n", attempt, ret);
            if (ret >= 0) break;
            usleep(10000);
        }
        usleep(200000);
        pic_read(buf, 7);
        hexdump("after", buf, 7);
    }

    /* Test 5: Full calib2 write-only */
    printf("\n--- Test 5: calib2 full write-only ---\n");
    {
        int attempt;
        for (attempt = 0; attempt < 3; attempt++) {
            ret = pic_write((unsigned char *)pic_calib2, 401);
            printf("  attempt %d: ret=%d\n", attempt, ret);
            if (ret >= 0) break;
            usleep(10000);
        }
        usleep(200000);
        pic_read(buf, 7);
        hexdump("after", buf, 7);
    }

    /* Test 6: Battery command after calibration */
    printf("\n--- Test 6: battery read ---\n");
    usleep(2000000);  /* Wait 2 sec for PIC to process */
    {
        unsigned char cmd[] = { 0x2F, 0x00, 0x02 };
        unsigned char bat[17] = {0};
        ret = pic_wr(cmd, 3, bat, 17);
        printf("bat read: ret=%d\n", ret);
        hexdump("battery", bat, 17);
        if (bat[0] != 0xAA && bat[0] != 0x00 && bat[0] != 0xFF)
            printf("  -> DIFFERENT FROM TEST PATTERN!\n");
    }

    /* Test 7: Just read many times — see if PIC changes */
    printf("\n--- Test 7: repeated reads ---\n");
    {
        int i;
        for (i = 0; i < 5; i++) {
            memset(buf, 0, 17);
            ret = pic_read(buf, 7);
            hexdump("  read", buf, 7);
            usleep(500000);
        }
    }

    close(fd);
    return 0;
}
