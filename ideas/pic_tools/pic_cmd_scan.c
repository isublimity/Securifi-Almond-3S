/*
 * pic_cmd_scan — Try all safe single-byte commands on PIC16
 * Skip 0x41 (kills PIC!)
 * Build: zig cc -target mipsel-linux-musleabi -O2 -static -o pic_cmd_scan pic_cmd_scan.c
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

static int pic_wr(unsigned char cmd, unsigned char *rbuf, int rlen)
{
    struct i2c_msg msgs[2] = {
        { .addr = PIC_ADDR, .flags = 0,        .len = 1,    .buf = &cmd },
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

int main(void)
{
    unsigned char resp[8] = {0};
    unsigned char baseline[8] = {0};
    int ret, cmd;

    fd = open("/dev/i2c-0", O_RDWR);
    if (fd < 0) { perror("/dev/i2c-0"); return 1; }

    /* Get baseline */
    pic_read(baseline, 7);
    printf("Baseline: %02X %02X %02X %02X %02X %02X %02X\n\n",
           baseline[0], baseline[1], baseline[2], baseline[3],
           baseline[4], baseline[5], baseline[6]);

    printf("Scanning commands 0x00-0xFF (skip 0x41)...\n");
    printf("Showing only commands that get ACK or change response:\n\n");

    for (cmd = 0; cmd < 256; cmd++) {
        if (cmd == 0x41) continue;  /* DANGEROUS! */

        /* Try up to 3 times (alternating ACK/NACK) */
        int attempt, got = 0;
        for (attempt = 0; attempt < 3 && !got; attempt++) {
            memset(resp, 0, 8);
            ret = pic_wr((unsigned char)cmd, resp, 7);
            if (ret >= 0) got = 1;
            usleep(20000);
        }

        if (got) {
            int diff = memcmp(resp, baseline, 7);
            printf("cmd 0x%02X: ACK  resp=%02X %02X %02X %02X %02X %02X %02X%s\n",
                   cmd, resp[0], resp[1], resp[2], resp[3], resp[4], resp[5], resp[6],
                   diff ? " *** DIFFERENT ***" : "");
        }
        usleep(10000);
    }

    /* Final state */
    printf("\nFinal: ");
    pic_read(resp, 7);
    printf("%02X %02X %02X %02X %02X %02X %02X\n",
           resp[0], resp[1], resp[2], resp[3], resp[4], resp[5], resp[6]);

    close(fd);
    return 0;
}
