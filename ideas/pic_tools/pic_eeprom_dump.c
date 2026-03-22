/*
 * pic_eeprom_dump — Read PIC16 internal registers/EEPROM via I2C
 * Try reading at different register addresses 0x00-0xFF
 * Build: zig cc -target mipsel-linux-musleabi -O2 -static -o pic_eeprom_dump pic_eeprom_dump.c
 */
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/i2c.h>
#include <linux/i2c-dev.h>

#define PIC_ADDR 0x2A

static int fd;

/* Write register address, then read N bytes (repeated start) */
static int pic_reg_read(unsigned char reg, unsigned char *buf, int len)
{
    struct i2c_msg msgs[2] = {
        { .addr = PIC_ADDR, .flags = 0,        .len = 1,   .buf = &reg },
        { .addr = PIC_ADDR, .flags = I2C_M_RD, .len = len, .buf = buf },
    };
    struct i2c_rdwr_ioctl_data data = { .msgs = msgs, .nmsgs = 2 };
    return ioctl(fd, I2C_RDWR, &data);
}

/* Simple read (no register address) */
static int pic_read(unsigned char *buf, int len)
{
    struct i2c_msg msg = { .addr = PIC_ADDR, .flags = I2C_M_RD, .len = len, .buf = buf };
    struct i2c_rdwr_ioctl_data data = { .msgs = &msg, .nmsgs = 1 };
    return ioctl(fd, I2C_RDWR, &data);
}

int main(void)
{
    unsigned char buf[32] = {0};
    unsigned char baseline[8] = {0};
    int ret, reg;

    fd = open("/dev/i2c-0", O_RDWR);
    if (fd < 0) { perror("/dev/i2c-0"); return 1; }

    /* Baseline: simple read */
    pic_read(baseline, 7);
    printf("Baseline (no reg): %02X %02X %02X %02X %02X %02X %02X\n\n",
           baseline[0], baseline[1], baseline[2], baseline[3],
           baseline[4], baseline[5], baseline[6]);

    /* Read at each register address 0x00-0xFF */
    printf("Register dump (write addr, read 4 bytes):\n");
    printf("Addr | Data            | Status\n");
    printf("-----|-----------------|-------\n");

    for (reg = 0; reg < 256; reg++) {
        memset(buf, 0xEE, 8);  /* fill with marker */

        /* Try up to 2 times (alternating ACK/NACK) */
        ret = pic_reg_read((unsigned char)reg, buf, 4);
        if (ret < 0) {
            usleep(10000);
            ret = pic_reg_read((unsigned char)reg, buf, 4);
        }

        if (ret >= 0) {
            int diff = (buf[0] != baseline[0] || buf[1] != baseline[1] ||
                       buf[2] != baseline[2] || buf[3] != baseline[3]);
            printf("0x%02X | %02X %02X %02X %02X    | %s%s\n",
                   reg, buf[0], buf[1], buf[2], buf[3],
                   ret >= 0 ? "OK" : "FAIL",
                   diff ? " ***DIFFERENT***" : "");
        }
        usleep(20000);
    }

    /* Also try reading longer responses at key addresses */
    printf("\n--- Extended reads at key addresses ---\n");
    int addrs[] = { 0x00, 0x03, 0x0F, 0x2E, 0x2F, 0x33, 0x34 };
    for (int i = 0; i < 7; i++) {
        memset(buf, 0, 17);
        ret = pic_reg_read(addrs[i], buf, 17);
        if (ret < 0) { usleep(10000); ret = pic_reg_read(addrs[i], buf, 17); }
        if (ret >= 0) {
            printf("0x%02X (17b):", addrs[i]);
            for (int j = 0; j < 17; j++) printf(" %02X", buf[j]);
            printf("\n");
        } else {
            printf("0x%02X: NACK\n", addrs[i]);
        }
        usleep(20000);
    }

    close(fd);
    return 0;
}
