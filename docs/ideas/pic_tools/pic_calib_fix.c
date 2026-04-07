/*
 * pic_calib_fix — PIC16 calibration using CORRECT stock protocol
 *
 * Fixes vs old implementation:
 * 1. SM0_START = total_len (ONCE), not 0 for each byte
 * 2. Poll register 0x918 bit 1 between bytes (write ready)
 * 3. Proper delay between bytes
 *
 * Build: zig cc -target mipsel-linux-musleabi -O2 -static -o pic_calib_fix pic_calib_fix.c
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <linux/i2c.h>
#include <linux/i2c-dev.h>

#define PALMBUS_PHYS 0x1E000000
#define PIC_ADDR     0x2A

/* SM0 registers */
#define SM0_CFG      0x900
#define SM0_DATA     0x908
#define SM0_DATAOUT  0x910
#define SM0_DATAIN   0x914
#define SM0_POLL     0x918  /* status polling register! */
#define SM0_STATUS   0x91C
#define SM0_START    0x920
#define SM0_CTL1     0x940

typedef unsigned char u8;
#include "pic_calib.h"

static volatile unsigned int *base;

static void gw(int off, unsigned int val) { base[off/4] = val; }
static unsigned int gr(int off) { return base[off/4]; }

static void hexdump(const char *label, unsigned char *buf, int len)
{
    printf("%s:", label);
    for (int i = 0; i < len; i++) printf(" %02X", buf[i]);
    printf("\n");
}

/* Poll SM0 register 0x918 for a specific bit, with timeout */
static int sm0_poll(int bit, int timeout)
{
    int i;
    for (i = 0; i < timeout; i++) {
        if (gr(SM0_POLL) & bit)
            return 0;  /* ready */
        usleep(1);
    }
    return -1;  /* timeout */
}

/*
 * STOCK PROTOCOL PIC write (from IDA reverse 0x412F78):
 * 1. SM0_DATA = slave_addr
 * 2. SM0_START = total_len (ONCE!)
 * 3. SM0_DATAOUT = first byte (cmd)
 * 4. SM0_STATUS = 0 (write mode)
 * 5. For each remaining byte:
 *    - Poll 0x918 bit 1 (write ready)
 *    - SM0_DATAOUT = next byte
 * 6. Check 0x918 bit 0 (completion)
 */
static int pic_stock_write(u8 *data, int len)
{
    int i;
    unsigned int saved_ctl1 = gr(SM0_CTL1);

    /* Setup SM0 for raw master mode */
    gw(SM0_CTL1, 0x90644042);
    usleep(10);
    gw(SM0_CFG, 0xFA);

    /* Set slave address */
    gw(SM0_DATA, PIC_ADDR);

    /* Set TOTAL transfer length */
    gw(SM0_START, len);

    /* First byte */
    gw(SM0_DATAOUT, data[0]);
    gw(SM0_STATUS, 0);  /* write mode */

    /* Remaining bytes with polling */
    for (i = 1; i < len; i++) {
        /* Poll 0x918 bit 1 (0x02) — write ready */
        if (sm0_poll(0x02, 100000) < 0) {
            printf("  poll timeout at byte %d\n", i);
            break;
        }
        usleep(100);  /* small delay between bytes */
        gw(SM0_DATAOUT, data[i]);
    }

    /* Wait for completion: poll bit 0 */
    sm0_poll(0x01, 100000);

    /* Restore SM0_CTL1 */
    gw(SM0_CTL1, saved_ctl1);
    usleep(10);

    return i;  /* bytes sent */
}

/*
 * STOCK PROTOCOL PIC read (from IDA reverse 0x412E78):
 * 1. SM0_START = len - 1 (NOT len!)
 * 2. SM0_STATUS = 1 (read mode)
 * 3. For each byte:
 *    - Poll 0x918 bit 2 (read ready)
 *    - Read SM0_DATAIN
 * 4. Check 0x918 bit 0 (completion)
 */
static int pic_stock_read(u8 *buf, int len)
{
    int i;
    unsigned int saved_ctl1 = gr(SM0_CTL1);

    gw(SM0_CTL1, 0x90644042);
    usleep(10);
    gw(SM0_CFG, 0xFA);
    gw(SM0_DATA, PIC_ADDR);

    /* CRITICAL: START = len - 1, not len */
    gw(SM0_START, len - 1);
    gw(SM0_STATUS, 1);  /* read mode */

    for (i = 0; i < len; i++) {
        /* Poll 0x918 bit 2 (0x04) — read ready */
        if (sm0_poll(0x04, 100000) < 0) {
            printf("  read poll timeout at byte %d\n", i);
            break;
        }
        usleep(10);
        buf[i] = gr(SM0_DATAIN) & 0xFF;
    }

    sm0_poll(0x01, 100000);

    gw(SM0_CTL1, saved_ctl1);
    usleep(10);

    return i;
}

/* Linux I2C read for comparison */
static int pic_linux_read(int i2c_fd, u8 *buf, int len)
{
    struct i2c_msg msg = { .addr = PIC_ADDR, .flags = 0x01, .len = len, .buf = buf };
    struct i2c_rdwr_ioctl_data data = { .msgs = &msg, .nmsgs = 1 };
    return ioctl(i2c_fd, 0x0707, &data);
}

int main(int argc, char **argv)
{
    u8 buf[20] = {0};
    int ret, fd, i2c_fd;

    fd = open("/dev/mem", O_RDWR | O_SYNC);
    if (fd < 0) { perror("/dev/mem"); return 1; }
    base = mmap(NULL, 0x1000, PROT_READ | PROT_WRITE, MAP_SHARED, fd, PALMBUS_PHYS);
    if (base == (void *)-1) { perror("mmap"); return 1; }

    i2c_fd = open("/dev/i2c-0", O_RDWR);

    printf("=== PIC16 Calibration — STOCK PROTOCOL ===\n");
    printf("SM0_CTL1: 0x%08X\n", gr(SM0_CTL1));
    printf("SM0_POLL: 0x%08X\n\n", gr(SM0_POLL));

    /* Step 1: Read initial state via stock protocol */
    printf("--- Stock protocol read ---\n");
    memset(buf, 0, 20);
    ret = pic_stock_read(buf, 7);
    printf("Read %d bytes: ", ret);
    hexdump("data", buf, 7);

    /* Also Linux I2C read for comparison */
    if (i2c_fd >= 0) {
        memset(buf, 0, 20);
        pic_linux_read(i2c_fd, buf, 7);
        hexdump("Linux I2C", buf, 7);
    }
    printf("\n");

    /* Step 2: Send calibration table 1 using stock protocol */
    printf("--- Sending calibration table 1 (cmd=0x03, 401 bytes) ---\n");
    ret = pic_stock_write((u8 *)pic_calib1, 401);
    printf("Sent %d bytes\n", ret);
    usleep(100000);

    /* Read after calib1 */
    memset(buf, 0, 20);
    pic_stock_read(buf, 7);
    hexdump("After calib1", buf, 7);
    printf("\n");

    /* Step 3: Send calibration table 2 using stock protocol */
    printf("--- Sending calibration table 2 (cmd=0x2E, 401 bytes) ---\n");
    ret = pic_stock_write((u8 *)pic_calib2, 401);
    printf("Sent %d bytes\n", ret);
    usleep(100000);

    /* Read after calib2 */
    memset(buf, 0, 20);
    pic_stock_read(buf, 7);
    hexdump("After calib2", buf, 7);
    printf("\n");

    /* Step 4: Wait and read battery */
    printf("--- Waiting 2 sec then reading battery ---\n");
    usleep(2000000);

    /* Send battery read command {0x2F, 0x00, 0x02} via stock write */
    {
        u8 cmd[] = { 0x2F, 0x00, 0x02 };
        ret = pic_stock_write(cmd, 3);
        printf("Battery cmd sent: %d bytes\n", ret);
        usleep(200000);
    }

    /* Read battery response */
    memset(buf, 0, 20);
    ret = pic_stock_read(buf, 17);
    printf("Battery read %d bytes: ", ret);
    hexdump("data", buf, 17);

    if (buf[0] != 0xAA) {
        printf("  >>> NOT TEST PATTERN! Could be real data! <<<\n");
        int raw = (buf[0] << 8) | buf[1];
        printf("  raw_adc = %d (0x%03X)\n", raw, raw);
    } else if (buf[1] != 0x54) {
        printf("  >>> AA but different pattern (battery present?) <<<\n");
    } else {
        printf("  Still test pattern (no calibration effect)\n");
    }

    /* Final Linux I2C read */
    if (i2c_fd >= 0) {
        printf("\n--- Final Linux I2C read ---\n");
        memset(buf, 0, 20);
        pic_linux_read(i2c_fd, buf, 7);
        hexdump("Linux", buf, 7);
        close(i2c_fd);
    }

    munmap((void *)base, 0x1000);
    close(fd);
    return 0;
}
