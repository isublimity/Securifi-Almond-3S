/*
 * pic_i2c_test2 — PIC16 I2C test with multiple protocols
 * Try SMBus, repeated start, different timings
 *
 * Build: zig cc -target mipsel-linux-musleabi -O2 -static -o pic_i2c_test2 pic_i2c_test2.c
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

static int fd;

static void hexdump(const char *label, unsigned char *buf, int len)
{
    printf("%s:", label);
    for (int i = 0; i < len; i++)
        printf(" %02X", buf[i]);
    printf("\n");
}

/* Simple slave-address ioctl + read/write */
static int set_slave(int addr)
{
    return ioctl(fd, I2C_SLAVE_FORCE, addr);
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

    printf("=== PIC16 I2C Protocol Test ===\n\n");

    /* Method 1: I2C_SLAVE + read() */
    printf("--- Method 1: I2C_SLAVE + read() ---\n");
    set_slave(PIC_ADDR);
    memset(buf, 0, 20);
    ret = read(fd, buf, 7);
    printf("read(%d bytes): ret=%d\n", 7, ret);
    if (ret > 0) hexdump("  data", buf, ret);
    printf("\n");

    /* Method 2: I2C_SLAVE + write() single byte */
    printf("--- Method 2: I2C_SLAVE + write(0x33) ---\n");
    set_slave(PIC_ADDR);
    buf[0] = 0x33;
    ret = write(fd, buf, 1);
    printf("write(0x33): ret=%d\n", ret);
    if (ret < 0) perror("  write");
    usleep(50000);
    memset(buf, 0, 20);
    ret = read(fd, buf, 7);
    printf("read after write: ret=%d\n", ret);
    if (ret > 0) hexdump("  data", buf, ret);
    printf("\n");

    /* Method 3: Write 3 bytes {0x33, 0x00, 0x01} */
    printf("--- Method 3: write({0x33,0x00,0x01}) ---\n");
    set_slave(PIC_ADDR);
    buf[0] = 0x33; buf[1] = 0x00; buf[2] = 0x01;
    ret = write(fd, buf, 3);
    printf("write(3 bytes): ret=%d\n", ret);
    if (ret < 0) perror("  write");
    usleep(50000);
    memset(buf, 0, 20);
    ret = read(fd, buf, 7);
    printf("read: ret=%d\n", ret);
    if (ret > 0) hexdump("  data", buf, ret);
    printf("\n");

    /* Method 4: SMBus byte read (command byte) */
    printf("--- Method 4: SMBus read_byte ---\n");
    set_slave(PIC_ADDR);
    {
        union {
            unsigned char byte;
            unsigned short word;
            unsigned char block[34];
        } sdata;

        /* i2c_smbus_read_byte */
        struct {
            unsigned char read_write;
            unsigned char command;
            unsigned int size;
            void *data;
        } smbus;

        smbus.read_write = 1; /* read */
        smbus.command = 0;
        smbus.size = 1; /* I2C_SMBUS_BYTE */
        smbus.data = &sdata;
        ret = ioctl(fd, 0x0720, &smbus); /* I2C_SMBUS */
        printf("SMBus read_byte: ret=%d, data=0x%02X\n", ret, sdata.byte);
    }
    printf("\n");

    /* Method 5: I2C_RDWR with write followed by read (repeated start) */
    printf("--- Method 5: I2C_RDWR write+read (repeated start) ---\n");
    {
        unsigned char wbuf[] = { 0x33, 0x00, 0x01 };
        unsigned char rbuf[7] = {0};
        struct i2c_msg msgs[2] = {
            { .addr = PIC_ADDR, .flags = 0, .len = 3, .buf = wbuf },
            { .addr = PIC_ADDR, .flags = I2C_M_RD, .len = 7, .buf = rbuf },
        };
        struct i2c_rdwr_ioctl_data data = { .msgs = msgs, .nmsgs = 2 };
        ret = ioctl(fd, I2C_RDWR, &data);
        printf("I2C_RDWR 2 msgs: ret=%d\n", ret);
        if (ret >= 0) hexdump("  read data", rbuf, 7);
        if (ret < 0) perror("  ioctl");
    }
    printf("\n");

    /* Method 6: I2C_RDWR write only */
    printf("--- Method 6: I2C_RDWR write only ---\n");
    {
        unsigned char wbuf[] = { 0x33, 0x00, 0x01 };
        struct i2c_msg msgs[1] = {
            { .addr = PIC_ADDR, .flags = 0, .len = 3, .buf = wbuf },
        };
        struct i2c_rdwr_ioctl_data data = { .msgs = msgs, .nmsgs = 1 };
        ret = ioctl(fd, I2C_RDWR, &data);
        printf("I2C_RDWR write: ret=%d\n", ret);
        if (ret < 0) perror("  ioctl");

        /* Wait and read separately */
        usleep(50000);
        unsigned char rbuf[7] = {0};
        struct i2c_msg rmsg = { .addr = PIC_ADDR, .flags = I2C_M_RD, .len = 7, .buf = rbuf };
        struct i2c_rdwr_ioctl_data rdata = { .msgs = &rmsg, .nmsgs = 1 };
        ret = ioctl(fd, I2C_RDWR, &rdata);
        printf("I2C_RDWR read: ret=%d\n", ret);
        if (ret >= 0) hexdump("  data", rbuf, 7);
    }
    printf("\n");

    /* Method 7: I2C_RDWR with NO_START flag */
    printf("--- Method 7: I2C_RDWR with NOSTART ---\n");
    {
        unsigned char wbuf[] = { 0x33 };
        unsigned char wbuf2[] = { 0x00, 0x01 };
        struct i2c_msg msgs[2] = {
            { .addr = PIC_ADDR, .flags = 0, .len = 1, .buf = wbuf },
            { .addr = PIC_ADDR, .flags = I2C_M_NOSTART, .len = 2, .buf = wbuf2 },
        };
        struct i2c_rdwr_ioctl_data data = { .msgs = msgs, .nmsgs = 2 };
        ret = ioctl(fd, I2C_RDWR, &data);
        printf("I2C_RDWR NOSTART: ret=%d\n", ret);
        if (ret < 0) perror("  ioctl");
    }
    printf("\n");

    /* Final read to see current state */
    printf("--- Final state ---\n");
    {
        unsigned char rbuf[17] = {0};
        struct i2c_msg msg = { .addr = PIC_ADDR, .flags = I2C_M_RD, .len = 17, .buf = rbuf };
        struct i2c_rdwr_ioctl_data data = { .msgs = &msg, .nmsgs = 1 };
        ret = ioctl(fd, I2C_RDWR, &data);
        if (ret >= 0)
            hexdump("Final read", rbuf, 17);
    }

    close(fd);
    return 0;
}
