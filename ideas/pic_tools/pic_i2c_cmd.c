/* pic_i2c_cmd — send bytes to PIC via Linux I2C (repeated start write+read)
 * Usage: pic_i2c_cmd 0x34 0x01 0x00
 * Build: zig cc -target mipsel-linux-musleabi -O2 -static -o pic_i2c_cmd pic_i2c_cmd.c
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/i2c.h>
#include <linux/i2c-dev.h>

#define PIC 0x2A

int main(int argc, char **argv)
{
    unsigned char wbuf[64], rbuf[8] = {0};
    int i, n = 0, ret, fd;

    if (argc < 2) { printf("Usage: %s <b0> [b1] ...\n", argv[0]); return 1; }
    fd = open("/dev/i2c-0", O_RDWR);
    if (fd < 0) { perror("/dev/i2c-0"); return 1; }

    for (i = 1; i < argc && n < 64; i++)
        wbuf[n++] = (unsigned char)strtol(argv[i], NULL, 0);

    printf("I2C write %d bytes:", n);
    for (i = 0; i < n; i++) printf(" %02X", wbuf[i]);

    /* Try 1: write+read (repeated start) — PIC sometimes ACKs this */
    {
        struct i2c_msg msgs[2] = {
            { .addr = PIC, .flags = 0, .len = n, .buf = wbuf },
            { .addr = PIC, .flags = I2C_M_RD, .len = 4, .buf = rbuf },
        };
        struct i2c_rdwr_ioctl_data data = { .msgs = msgs, .nmsgs = 2 };
        int attempt;
        for (attempt = 0; attempt < 3; attempt++) {
            ret = ioctl(fd, I2C_RDWR, &data);
            if (ret >= 0) break;
            usleep(20000);
        }
        printf(" → ret=%d", ret);
        if (ret >= 0)
            printf(" resp: %02X %02X %02X %02X", rbuf[0], rbuf[1], rbuf[2], rbuf[3]);
        printf("\n");
    }

    close(fd);
    return 0;
}
