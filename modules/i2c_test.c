/*
 * i2c_test — Standalone PIC16/SX8650 I2C test via /dev/i2c-0
 * Works WITHOUT lcd_drv.ko — uses Linux I2C directly
 *
 * Usage:
 *   i2c_test scan         — scan I2C bus for all devices
 *   i2c_test pic           — read PIC16 battery (addr 0x2A)
 *   i2c_test pic calib     — send calibration + read battery
 *   i2c_test touch         — read SX8650 touch (addr 0x48)
 *   i2c_test               — scan + pic + touch
 *
 * Build: zig cc -target mipsel-linux-musleabi -O2 -static -o i2c_test i2c_test.c
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/i2c.h>
#include <linux/i2c-dev.h>

#define PIC_ADDR   0x2A
#define TOUCH_ADDR 0x48

static int i2c_fd = -1;

static void hexdump(const char *label, unsigned char *buf, int len)
{
    printf("%s:", label);
    for (int i = 0; i < len; i++)
        printf(" %02X", buf[i]);
    printf("\n");
}

static int i2c_read(int addr, unsigned char *buf, int len)
{
    struct i2c_msg msg = { .addr = addr, .flags = I2C_M_RD, .len = len, .buf = buf };
    struct i2c_rdwr_ioctl_data data = { .msgs = &msg, .nmsgs = 1 };
    return ioctl(i2c_fd, I2C_RDWR, &data);
}

static int i2c_write(int addr, unsigned char *buf, int len)
{
    struct i2c_msg msg = { .addr = addr, .flags = 0, .len = len, .buf = buf };
    struct i2c_rdwr_ioctl_data data = { .msgs = &msg, .nmsgs = 1 };
    return ioctl(i2c_fd, I2C_RDWR, &data);
}

static int i2c_write_read(int addr, unsigned char *wbuf, int wlen,
                          unsigned char *rbuf, int rlen)
{
    struct i2c_msg msgs[2] = {
        { .addr = addr, .flags = 0,        .len = wlen, .buf = wbuf },
        { .addr = addr, .flags = I2C_M_RD, .len = rlen, .buf = rbuf },
    };
    struct i2c_rdwr_ioctl_data data = { .msgs = msgs, .nmsgs = 2 };
    return ioctl(i2c_fd, I2C_RDWR, &data);
}

/* Scan I2C bus */
static void do_scan(void)
{
    printf("=== I2C Bus 0 Scan ===\n");
    printf("     0  1  2  3  4  5  6  7  8  9  a  b  c  d  e  f\n");
    for (int row = 0; row < 0x80; row += 16) {
        printf("%02x:", row);
        for (int col = 0; col < 16; col++) {
            int addr = row + col;
            if (addr < 0x03 || addr > 0x77) {
                printf("   ");
                continue;
            }
            unsigned char buf;
            int ret = i2c_read(addr, &buf, 1);
            if (ret >= 0)
                printf(" %02x", addr);
            else
                printf(" --");
        }
        printf("\n");
    }
    printf("\n");
}

/* Read PIC16 battery */
static void do_pic(int with_calib)
{
    unsigned char buf[17] = {0};
    int ret;

    printf("=== PIC16 Battery (addr 0x%02X) ===\n", PIC_ADDR);

    /* 1. Simple read (no command) */
    ret = i2c_read(PIC_ADDR, buf, 17);
    if (ret < 0) {
        printf("PIC read failed (not responding)\n");
        return;
    }
    hexdump("PIC raw read", buf, 17);

    /* Check for test pattern */
    if (buf[0] == 0xAA && buf[1] == 0x54) {
        printf("  -> Test pattern (AA 54 ...) = NO CALIBRATION\n");
    }

    /* 2. Send wake command {0x33, 0x00, 0x01} then read */
    {
        unsigned char wake[] = { 0x33, 0x00, 0x01 };
        ret = i2c_write(PIC_ADDR, wake, 3);
        printf("PIC wake cmd 0x33: %s\n", ret >= 0 ? "OK" : "FAIL");
        usleep(50000);

        memset(buf, 0, 17);
        ret = i2c_read(PIC_ADDR, buf, 17);
        if (ret >= 0)
            hexdump("PIC after wake", buf, 17);
    }

    /* 3. Send battery read command {0x2F, 0x00, 0x02} then read */
    {
        unsigned char cmd[] = { 0x2F, 0x00, 0x02 };
        ret = i2c_write(PIC_ADDR, cmd, 3);
        printf("PIC bat cmd 0x2F: %s\n", ret >= 0 ? "OK" : "FAIL");
        usleep(200000);

        memset(buf, 0, 17);
        ret = i2c_read(PIC_ADDR, buf, 17);
        if (ret >= 0) {
            hexdump("PIC battery", buf, 17);

            /* Try to interpret as stock firmware format */
            int raw_adc = (buf[0] << 8) | buf[1];
            int vref = buf[2];
            printf("  Possible: raw_adc=%d (0x%03X), vref=%d\n", raw_adc, raw_adc, vref);

            /* Check if data looks real (not test pattern) */
            if (buf[0] != 0xAA && buf[0] != 0xFF && buf[0] != 0x00) {
                printf("  -> DATA LOOKS REAL!\n");
            }
        }
    }

    /* 4. Write-read combined (command + read in one transaction) */
    {
        unsigned char cmd = 0x2F;
        memset(buf, 0, 17);
        ret = i2c_write_read(PIC_ADDR, &cmd, 1, buf, 7);
        if (ret >= 0)
            hexdump("PIC w/r 0x2F", buf, 7);
        else
            printf("PIC write-read failed\n");
    }

    printf("\n");
}

/* Read SX8650 touchscreen */
static void do_touch(void)
{
    unsigned char buf[4] = {0};
    int ret;

    printf("=== SX8650 Touch (addr 0x%02X) ===\n", TOUCH_ADDR);

    /* Simple read */
    ret = i2c_read(TOUCH_ADDR, buf, 4);
    if (ret < 0) {
        printf("SX8650 not responding\n");
        return;
    }
    hexdump("Touch raw", buf, 4);

    /* SELECT(X) = 0x80, then read */
    {
        unsigned char cmd = 0x80;
        unsigned char data[2] = {0};
        ret = i2c_write_read(TOUCH_ADDR, &cmd, 1, data, 2);
        if (ret >= 0) {
            int ch = (data[0] >> 4) & 7;
            int val = ((data[0] & 0x0F) << 8) | data[1];
            printf("SELECT(X): ch=%d val=%d (0x%03X)\n", ch, val, val);
        } else {
            printf("SELECT(X) failed\n");
        }
    }

    /* SELECT(Y) = 0x81, then read */
    {
        unsigned char cmd = 0x81;
        unsigned char data[2] = {0};
        ret = i2c_write_read(TOUCH_ADDR, &cmd, 1, data, 2);
        if (ret >= 0) {
            int ch = (data[0] >> 4) & 7;
            int val = ((data[0] & 0x0F) << 8) | data[1];
            printf("SELECT(Y): ch=%d val=%d (0x%03X)\n", ch, val, val);
        } else {
            printf("SELECT(Y) failed\n");
        }
    }

    printf("\n");
}

int main(int argc, char **argv)
{
    i2c_fd = open("/dev/i2c-0", O_RDWR);
    if (i2c_fd < 0) {
        perror("open /dev/i2c-0");
        printf("Hint: is kmod-i2c-core loaded? Is i2c-mt7621 enabled?\n");
        return 1;
    }

    int do_all = (argc <= 1);

    if (do_all || (argc > 1 && strcmp(argv[1], "scan") == 0))
        do_scan();

    if (do_all || (argc > 1 && strcmp(argv[1], "pic") == 0))
        do_pic(argc > 2 && strcmp(argv[2], "calib") == 0);

    if (do_all || (argc > 1 && strcmp(argv[1], "touch") == 0))
        do_touch();

    close(i2c_fd);
    return 0;
}
