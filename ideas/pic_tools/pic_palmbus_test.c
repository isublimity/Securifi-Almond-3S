/*
 * pic_palmbus_test — Test PIC16 battery via direct palmbus I2C (SM0)
 * Uses /dev/mem mmap to access SM0 registers directly
 * Same protocol as stock kernel 3.10.14
 *
 * Build: zig cc -target mipsel-linux-musleabi -O2 -static -o pic_palmbus_test pic_palmbus_test.c
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>

#define PALMBUS_PHYS 0x1E000000
#define MAP_SIZE     0x1000

/* SM0 I2C controller registers */
#define SM0_CFG     0x900
#define SM0_CLKDIV  0x904
#define SM0_DATA    0x908
#define SM0_DATAOUT 0x910
#define SM0_DATAIN  0x914
#define SM0_STATUS  0x91C
#define SM0_START   0x920
#define SM0_CTL1    0x940

#define PIC_ADDR    0x2A

static volatile unsigned int *base;

static void gw(int off, unsigned int val) { base[off/4] = val; }
static unsigned int gr(int off) { return base[off/4]; }

static void hexdump(const char *label, unsigned char *buf, int len)
{
    printf("%s:", label);
    for (int i = 0; i < len; i++)
        printf(" %02X", buf[i]);
    printf("\n");
}

/*
 * PIC16 I2C write via palmbus SM0 (stock protocol)
 * SM0_CTL1=0x90644042 enables special SM0 master mode
 */
static int pic_palmbus_write(unsigned char *data, int len)
{
    int i;
    gw(SM0_CTL1, 0x90644042);
    usleep(10);
    gw(SM0_DATA, PIC_ADDR);

    for (i = 0; i < len; i++) {
        gw(SM0_DATAOUT, data[i]);
        gw(SM0_STATUS, 0);
        usleep(150);
        gw(SM0_START, 0);
        usleep(150);
    }

    /* STOP */
    gw(SM0_STATUS, 2);
    usleep(150);
    gw(SM0_START, 0);
    usleep(150);

    /* Restore I2C for Linux driver */
    gw(SM0_CTL1, 0x8064800E);
    usleep(10);

    return 0;
}

/*
 * PIC16 I2C read via palmbus SM0 (stock protocol)
 */
static int pic_palmbus_read(unsigned char *buf, int len)
{
    int i;
    gw(SM0_CTL1, 0x90644042);
    usleep(10);
    gw(SM0_DATA, PIC_ADDR);
    gw(SM0_START, 0);
    usleep(10);
    gw(SM0_DATAOUT, (PIC_ADDR << 1) | 1);  /* read address */
    gw(SM0_STATUS, 2);
    usleep(150);
    gw(SM0_CFG, 0xFA);
    gw(SM0_START, 0);
    usleep(10);
    gw(SM0_START, 1);
    gw(SM0_START, 1);
    usleep(10);
    gw(SM0_STATUS, 1);
    usleep(150);

    for (i = 0; i < len; i++) {
        buf[i] = gr(SM0_DATAIN) & 0xFF;
        usleep(150);
    }

    /* STOP */
    gw(SM0_START, 0);
    usleep(10);
    gw(SM0_START, 1);

    /* Restore I2C for Linux driver */
    gw(SM0_CTL1, 0x8064800E);
    usleep(10);

    return 0;
}

int main(int argc, char **argv)
{
    int fd;
    unsigned char buf[17] = {0};

    fd = open("/dev/mem", O_RDWR | O_SYNC);
    if (fd < 0) {
        perror("open /dev/mem");
        printf("Hint: need root and CONFIG_DEVMEM=y\n");
        return 1;
    }

    base = mmap(NULL, MAP_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED,
                fd, PALMBUS_PHYS);
    if (base == MAP_FAILED) {
        perror("mmap");
        close(fd);
        return 1;
    }

    printf("=== PIC16 Palmbus I2C Test ===\n");
    printf("SM0_CTL1 current: 0x%08X\n", gr(SM0_CTL1));
    printf("SM0_CFG current: 0x%08X\n", gr(SM0_CFG));
    printf("\n");

    /* 1. Raw read (no command) */
    pic_palmbus_read(buf, 17);
    hexdump("Raw read", buf, 17);
    printf("\n");

    /* 2. Wake command {0x33, 0x00, 0x01} */
    {
        unsigned char wake[] = { 0x33, 0x00, 0x01 };
        printf("Sending wake cmd {0x33, 0x00, 0x01}...\n");
        pic_palmbus_write(wake, 3);
        usleep(50000);

        memset(buf, 0, 17);
        pic_palmbus_read(buf, 17);
        hexdump("After wake", buf, 17);
        printf("\n");
    }

    /* 3. Battery read command {0x2F, 0x00, 0x02} */
    {
        unsigned char cmd[] = { 0x2F, 0x00, 0x02 };
        printf("Sending bat cmd {0x2F, 0x00, 0x02}...\n");
        pic_palmbus_write(cmd, 3);
        usleep(200000);

        memset(buf, 0, 17);
        pic_palmbus_read(buf, 17);
        hexdump("Battery data", buf, 17);

        /* Check for test pattern */
        if (buf[0] == 0xAA && buf[1] == 0x54) {
            printf("  -> Still test pattern (no calibration)\n");
        } else if (buf[0] == 0xFF) {
            printf("  -> 0xFF = no response or error\n");
        } else {
            printf("  -> NEW DATA! Might be real battery values\n");
        }
        printf("\n");
    }

    /* 4. Try calibration table 1 header (just first 5 bytes) */
    {
        unsigned char calib_test[] = { 0x03, 0x00, 0x04, 0x00, 0x08 };
        printf("Sending calib1 test {0x03, 0x00, 0x04, 0x00, 0x08}...\n");
        pic_palmbus_write(calib_test, 5);
        usleep(100000);

        memset(buf, 0, 17);
        pic_palmbus_read(buf, 17);
        hexdump("After calib test", buf, 17);

        if (buf[0] != 0xAA) {
            printf("  -> PIC responded differently after partial calib!\n");
        }
        printf("\n");
    }

    /* 5. Simple read to check final state */
    memset(buf, 0, 17);
    pic_palmbus_read(buf, 7);
    hexdump("Final state", buf, 7);

    munmap((void *)base, MAP_SIZE);
    close(fd);
    return 0;
}
