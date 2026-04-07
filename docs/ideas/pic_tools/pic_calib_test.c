/*
 * pic_calib_test — Send calibration to PIC16 via I2C repeated-start
 * PIC requires write+read combined (no STOP between)
 *
 * Build: zig cc -target mipsel-linux-musleabi -O2 -static -o pic_calib_test pic_calib_test.c
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

/* Calibration tables from stock firmware RAM dump */
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

int main(int argc, char **argv)
{
    unsigned char buf[20] = {0};
    int ret;

    fd = open("/dev/i2c-0", O_RDWR);
    if (fd < 0) {
        perror("open /dev/i2c-0");
        return 1;
    }

    printf("=== PIC16 Calibration Test ===\n\n");

    /* Read initial state */
    pic_read(buf, 7);
    hexdump("Initial", buf, 7);
    printf("\n");

    /* Test 1: Send wake command {0x33, 0x00, 0x01} with repeated start + read */
    printf("--- Test 1: Wake {0x33,0x00,0x01} + read ---\n");
    {
        unsigned char cmd[] = { 0x33, 0x00, 0x01 };
        unsigned char resp[7] = {0};
        ret = pic_write_read(cmd, 3, resp, 7);
        printf("ret=%d\n", ret);
        hexdump("Response", resp, 7);
    }
    printf("\n");

    /* Test 2: Send battery command {0x2F, 0x00, 0x02} + read */
    printf("--- Test 2: Battery {0x2F,0x00,0x02} + read ---\n");
    {
        unsigned char cmd[] = { 0x2F, 0x00, 0x02 };
        unsigned char resp[17] = {0};
        ret = pic_write_read(cmd, 3, resp, 17);
        printf("ret=%d\n", ret);
        hexdump("Battery response", resp, 17);

        if (resp[0] != 0xAA) {
            printf("  -> NOT test pattern! Different response after 0x2F!\n");
            int raw = (resp[0] << 8) | resp[1];
            printf("  -> raw_adc=%d (0x%03X)\n", raw, raw);
        }
    }
    printf("\n");

    /* Test 3: Send calibration table 1 (401 bytes) in chunks via repeated start */
    if (argc > 1 && strcmp(argv[1], "calib") == 0) {
        printf("--- Test 3: Sending calibration table 1 (%d bytes) ---\n", 401);

        /* PIC may have a buffer limit. Try sending in small chunks.
         * Stock firmware sends all 401 bytes in one I2C transaction.
         * Try with i2c_transfer: write 401 + read 1 */
        {
            unsigned char resp[4] = {0};
            ret = pic_write_read((unsigned char *)pic_calib1, 401, resp, 4);
            printf("Calib1 (401 bytes): ret=%d\n", ret);
            if (ret >= 0)
                hexdump("  resp", resp, 4);
            else
                perror("  calib1");
        }
        usleep(100000);

        /* Read PIC state after calib1 */
        memset(buf, 0, 17);
        pic_read(buf, 7);
        hexdump("After calib1", buf, 7);
        printf("\n");

        printf("--- Test 4: Sending calibration table 2 (%d bytes) ---\n", 401);
        {
            unsigned char resp[4] = {0};
            ret = pic_write_read((unsigned char *)pic_calib2, 401, resp, 4);
            printf("Calib2 (401 bytes): ret=%d\n", ret);
            if (ret >= 0)
                hexdump("  resp", resp, 4);
            else
                perror("  calib2");
        }
        usleep(100000);

        /* Read PIC state after calib2 */
        memset(buf, 0, 17);
        pic_read(buf, 7);
        hexdump("After calib2", buf, 7);
        printf("\n");

        /* Now try battery read */
        printf("--- Battery read after calibration ---\n");
        {
            unsigned char cmd[] = { 0x2F, 0x00, 0x02 };
            unsigned char resp[17] = {0};
            ret = pic_write_read(cmd, 3, resp, 17);
            printf("ret=%d\n", ret);
            hexdump("Battery", resp, 17);
            if (resp[0] != 0xAA && resp[0] != 0xFF) {
                printf("  -> REAL BATTERY DATA!\n");
                int raw = (resp[0] << 8) | resp[1];
                printf("  -> raw_adc=%d (0x%03X)\n", raw, raw);
            }
        }
    } else {
        printf("Run with 'calib' argument to send calibration tables\n");
    }

    close(fd);
    return 0;
}
