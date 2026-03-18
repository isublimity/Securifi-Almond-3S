/*
 * pic_icsp — PIC16LF1509 ICSP programmer via MT7621 GPIO bit-bang
 * Reads/writes PIC flash directly from the router, no Arduino needed.
 *
 * Based on a-p-prog by jaromir-sukuba (enhanced midrange p16c protocol)
 *
 * ICSP pins (connect to free MT7621 GPIO via jumper wires):
 *   MCLR = PIC pin 4 (RA3)  — pull LOW to enter programming, HIGH to run
 *   PGD  = PIC pin 19 (RA0) — data (bidirectional)
 *   PGC  = PIC pin 18 (RA1) — clock (output from master)
 *
 * Usage:
 *   pic_icsp -m <MCLR_GPIO> -d <PGD_GPIO> -c <PGC_GPIO> read
 *   pic_icsp -m <MCLR_GPIO> -d <PGD_GPIO> -c <PGC_GPIO> id
 *   pic_icsp -m <MCLR_GPIO> -d <PGD_GPIO> -c <PGC_GPIO> dump <file.bin>
 *
 * Build: zig cc -target mipsel-linux-musleabi -O2 -static -o pic_icsp pic_icsp.c
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>

#define PALMBUS_PHYS  0x1E000000
#define GPIO_DATA     0x600
#define GPIO_DIR      0x620
#define GPIO_DSET     0x630
#define GPIO_DCLR     0x640

static volatile unsigned int *base;

/* GPIO pin numbers (configurable) */
static int PIN_MCLR = 0;   /* default: GPIO 0 */
static int PIN_PGD  = 29;  /* default: GPIO 29 */
static int PIN_PGC  = 30;  /* default: GPIO 30 */

/* GPIO helpers */
static inline void gpio_out(int pin, int val)
{
    unsigned int mask = 1u << pin;
    /* Set direction to output */
    base[GPIO_DIR/4] |= mask;
    /* Set value */
    if (val)
        base[GPIO_DSET/4] = mask;
    else
        base[GPIO_DCLR/4] = mask;
}

static inline int gpio_in(int pin)
{
    unsigned int mask = 1u << pin;
    /* Set direction to input */
    base[GPIO_DIR/4] &= ~mask;
    usleep(1);
    return (base[GPIO_DATA/4] & mask) ? 1 : 0;
}

static inline void pgc_pulse(void)
{
    gpio_out(PIN_PGC, 1);
    usleep(1);
    gpio_out(PIN_PGC, 0);
    usleep(1);
}

/* Send n bits LSB first (classic PIC14) */
static void isp_send(unsigned int data, int n)
{
    int i;
    for (i = 0; i < n; i++) {
        gpio_out(PIN_PGD, data & 1);
        usleep(1);
        gpio_out(PIN_PGC, 1);
        usleep(1);
        data >>= 1;
        gpio_out(PIN_PGC, 0);
        gpio_out(PIN_PGD, 0);
        usleep(1);
    }
}

/* Send 8 bits MSB first (enhanced midrange PIC16F1xxx) */
static void isp_send_8_msb(unsigned char data)
{
    int i;
    for (i = 0; i < 8; i++) {
        gpio_out(PIN_PGD, (data & 0x80) ? 1 : 0);
        usleep(1);
        gpio_out(PIN_PGC, 1);
        usleep(1);
        data <<= 1;
        gpio_out(PIN_PGC, 0);
        gpio_out(PIN_PGD, 0);
        usleep(1);
    }
}

/* Send 24 bits MSB first */
static void isp_send_24_msb(unsigned long data)
{
    int i;
    for (i = 0; i < 24; i++) {
        gpio_out(PIN_PGD, (data & 0x800000) ? 1 : 0);
        usleep(1);
        gpio_out(PIN_PGC, 1);
        usleep(1);
        data <<= 1;
        gpio_out(PIN_PGC, 0);
        usleep(1);
    }
}

/* Read 8 bits MSB first */
static unsigned int isp_read_8_msb(void)
{
    int i;
    unsigned int out = 0;
    for (i = 0; i < 8; i++) {
        gpio_out(PIN_PGC, 1);
        usleep(1);
        gpio_out(PIN_PGC, 0);
        usleep(1);
        out <<= 1;
        if (gpio_in(PIN_PGD))
            out |= 1;
    }
    return out;
}

/* Read 16 bits MSB first */
static unsigned int isp_read_16_msb(void)
{
    int i;
    unsigned int out = 0;
    for (i = 0; i < 16; i++) {
        gpio_out(PIN_PGC, 1);
        usleep(1);
        gpio_out(PIN_PGC, 0);
        usleep(1);
        out <<= 1;
        if (gpio_in(PIN_PGD))
            out |= 1;
    }
    return out;
}

/* === Enhanced midrange (PIC16F1xxx) ICSP protocol === */

/* Enter LVP programming mode: MCLR low + key "MCHP" MSB first */
static void p16c_enter_progmode(void)
{
    gpio_out(PIN_MCLR, 0);
    usleep(300);
    isp_send_8_msb(0x4D);  /* 'M' */
    isp_send_8_msb(0x43);  /* 'C' */
    isp_send_8_msb(0x48);  /* 'H' */
    isp_send_8_msb(0x50);  /* 'P' */
    usleep(300);
    printf("Entered programming mode\n");
}

/* Exit programming mode */
static void p16c_exit_progmode(void)
{
    gpio_out(PIN_MCLR, 1);
    usleep(30000);
    gpio_out(PIN_MCLR, 0);
    usleep(30000);
    gpio_out(PIN_MCLR, 1);
    printf("Exited programming mode\n");
}

/* Set program counter */
static void p16c_set_pc(unsigned long pc)
{
    isp_send_8_msb(0x80);
    usleep(2);
    isp_send_24_msb(pc);
}

/* Read data from NVM (with or without increment) */
static unsigned int p16c_read_data_nvm(int inc)
{
    unsigned int retval;
    unsigned char tmp;

    if (inc == 0)
        isp_send_8_msb(0xFC);
    else
        isp_send_8_msb(0xFE);
    usleep(2);
    tmp = isp_read_8_msb();
    retval = isp_read_16_msb();
    retval >>= 1;
    if (tmp & 0x01)
        retval |= 0x8000;
    return retval;
}

/* Read device ID */
static unsigned int p16c_get_id(void)
{
    p16c_set_pc(0x8006);
    return p16c_read_data_nvm(1);
}

/* Read program memory */
static void p16c_read_pgm(unsigned int *data, unsigned long addr, int n)
{
    int i;
    p16c_set_pc(addr);
    for (i = 0; i < n; i++)
        data[i] = p16c_read_data_nvm(1);
}

/* Read config words */
static void p16c_read_config(unsigned int *data, int n)
{
    int i;
    p16c_set_pc(0x8000);
    for (i = 0; i < n; i++)
        data[i] = p16c_read_data_nvm(1);
}

/* === Main === */

static void usage(const char *prog)
{
    printf("Usage: %s [-m MCLR_GPIO] [-d PGD_GPIO] [-c PGC_GPIO] <command>\n", prog);
    printf("Commands:\n");
    printf("  id              Read device ID\n");
    printf("  read            Read first 64 words\n");
    printf("  config          Read config words\n");
    printf("  dump <file>     Dump full 8192 words to file\n");
    printf("  test            Test GPIO connectivity\n");
    printf("\nDefault GPIOs: MCLR=%d PGD=%d PGC=%d\n", PIN_MCLR, PIN_PGD, PIN_PGC);
    printf("Connect jumper wires from MT7621 GPIO to PIC ICSP pins:\n");
    printf("  MCLR = PIC pin 4 (RA3/VPP)\n");
    printf("  PGD  = PIC pin 19 (RA0/ICSPDAT)\n");
    printf("  PGC  = PIC pin 18 (RA1/ICSPCLK)\n");
}

int main(int argc, char **argv)
{
    int fd, opt;
    char *cmd;

    while ((opt = getopt(argc, argv, "m:d:c:h")) != -1) {
        switch (opt) {
        case 'm': PIN_MCLR = atoi(optarg); break;
        case 'd': PIN_PGD = atoi(optarg); break;
        case 'c': PIN_PGC = atoi(optarg); break;
        default: usage(argv[0]); return 1;
        }
    }

    if (optind >= argc) { usage(argv[0]); return 1; }
    cmd = argv[optind];

    fd = open("/dev/mem", O_RDWR | O_SYNC);
    if (fd < 0) { perror("/dev/mem"); return 1; }
    base = mmap(NULL, 0x1000, PROT_READ | PROT_WRITE, MAP_SHARED, fd, PALMBUS_PHYS);
    if (base == (void *)-1) { perror("mmap"); return 1; }

    printf("PIC ICSP: MCLR=GPIO%d PGD=GPIO%d PGC=GPIO%d\n", PIN_MCLR, PIN_PGD, PIN_PGC);

    /* Init pins */
    gpio_out(PIN_PGC, 0);
    gpio_out(PIN_PGD, 0);
    gpio_out(PIN_MCLR, 1);  /* MCLR high = run mode */

    if (strcmp(cmd, "test") == 0) {
        printf("Testing GPIO pins...\n");
        printf("MCLR toggle (watch PIC reset):\n");
        gpio_out(PIN_MCLR, 0); printf("  MCLR=0\n"); usleep(100000);
        gpio_out(PIN_MCLR, 1); printf("  MCLR=1\n"); usleep(100000);
        printf("PGD read: %d\n", gpio_in(PIN_PGD));
        printf("PGC toggle:\n");
        gpio_out(PIN_PGC, 1); usleep(1000); gpio_out(PIN_PGC, 0);
        printf("  PGC toggled\n");
        printf("GPIO test done\n");

    } else if (strcmp(cmd, "id") == 0) {
        p16c_enter_progmode();
        unsigned int id = p16c_get_id();
        printf("Device ID: 0x%04X\n", id);
        printf("Expected PIC16LF1509: 0x2DA0-0x2DAF\n");
        if ((id & 0xFFF0) == 0x2DA0)
            printf("MATCH! PIC16(L)F1509 detected\n");
        else
            printf("NO MATCH (check wiring)\n");
        p16c_exit_progmode();

    } else if (strcmp(cmd, "read") == 0) {
        unsigned int data[64];
        p16c_enter_progmode();
        p16c_read_pgm(data, 0, 64);
        printf("First 64 words:\n");
        for (int i = 0; i < 64; i++) {
            if (i % 8 == 0) printf("%04X: ", i);
            printf("%04X ", data[i]);
            if (i % 8 == 7) printf("\n");
        }
        p16c_exit_progmode();

    } else if (strcmp(cmd, "config") == 0) {
        unsigned int data[16];
        p16c_enter_progmode();
        p16c_read_config(data, 16);
        printf("Config area (0x8000-0x800F):\n");
        for (int i = 0; i < 16; i++)
            printf("  0x%04X: 0x%04X\n", 0x8000 + i, data[i]);
        p16c_exit_progmode();

    } else if (strcmp(cmd, "dump") == 0) {
        if (optind + 1 >= argc) { printf("Need filename\n"); return 1; }
        char *fname = argv[optind + 1];
        unsigned int data[256];
        FILE *fp = fopen(fname, "wb");
        if (!fp) { perror(fname); return 1; }

        p16c_enter_progmode();
        printf("Dumping 8192 words (16KB)...\n");
        for (int addr = 0; addr < 8192; addr += 256) {
            p16c_read_pgm(data, addr, 256);
            for (int i = 0; i < 256; i++) {
                unsigned char lo = data[i] & 0xFF;
                unsigned char hi = (data[i] >> 8) & 0xFF;
                fwrite(&lo, 1, 1, fp);
                fwrite(&hi, 1, 1, fp);
            }
            printf("  %d/%d words\r", addr + 256, 8192);
            fflush(stdout);
        }
        printf("\nDump saved to %s\n", fname);

        /* Also dump config */
        printf("Config words:\n");
        unsigned int cfg[16];
        p16c_read_config(cfg, 16);
        for (int i = 0; i < 16; i++)
            printf("  0x%04X: 0x%04X\n", 0x8000 + i, cfg[i]);

        p16c_exit_progmode();
        fclose(fp);

    } else {
        usage(argv[0]);
        return 1;
    }

    munmap((void *)base, 0x1000);
    close(fd);
    return 0;
}
