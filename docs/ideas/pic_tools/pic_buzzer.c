/* pic_buzzer — PIC16 buzzer via BOTH old and new SM0 registers
 * Usage: pic_buzzer on | pic_buzzer off
 * Build: zig cc -target mipsel-linux-musleabi -O2 -static -o pic_buzzer pic_buzzer.c
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>

static volatile unsigned int *base;
static void gw(int off, unsigned int v) { base[off/4] = v; }
static unsigned int gr(int off) { return base[off/4]; }

/* New register interface (kernel 6.12 i2c-mt7621) */
#define N_CTL0  0x940
#define N_CTL1  0x944
#define N_D0    0x950
#define N_D1    0x954
#define N_TRI   0x01
#define N_START 0x10
#define N_WRITE 0x20
#define N_STOP  0x30

#define N_PGLEN(x) ((((x)-1)<<8) & 0x700)

static void n_wait(void) {
    int i; for (i = 0; i < 5000; i++) { if (!(gr(N_CTL1) & N_TRI)) return; usleep(10); }
}

/* Write 3 bytes to PIC using NEW register interface */
static void pic_write_new(unsigned char b0, unsigned char b1, unsigned char b2)
{
    unsigned int addr_byte = (0x2A << 1); /* write address */

    n_wait();

    /* START */
    gw(N_CTL1, N_START | N_TRI);
    n_wait();

    /* Write address (1 byte) */
    gw(N_D0, addr_byte);
    gw(N_CTL1, N_WRITE | N_TRI | N_PGLEN(1));
    n_wait();

    /* Write 3 data bytes */
    gw(N_D0, b0 | (b1 << 8) | (b2 << 16));
    gw(N_CTL1, N_WRITE | N_TRI | N_PGLEN(3));
    n_wait();

    /* STOP */
    gw(N_CTL1, N_STOP | N_TRI);
    n_wait();
}

/* Write 3 bytes using OLD palmbus registers */
static void pic_write_old(unsigned char b0, unsigned char b1, unsigned char b2)
{
    unsigned int saved = gr(0x940);
    int i;

    gw(0x940, 0x90644042); usleep(10);
    gw(0x900, 0xFA);
    gw(0x908, 0x2A);
    gw(0x920, 3);
    gw(0x910, b0);
    gw(0x91C, 0);

    for (i = 0; i < 500; i++) { if (gr(0x918) & 0x02) break; usleep(10); }
    usleep(1000);
    gw(0x910, b1);

    for (i = 0; i < 500; i++) { if (gr(0x918) & 0x02) break; usleep(10); }
    usleep(1000);
    gw(0x910, b2);

    for (i = 0; i < 500; i++) { if (gr(0x918) & 0x01) break; usleep(10); }

    gw(0x940, saved); usleep(10);
}

int main(int argc, char **argv)
{
    int fd = open("/dev/mem", O_RDWR | O_SYNC);
    if (fd < 0) { perror("/dev/mem"); return 1; }
    base = mmap(NULL, 0x1000, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0x1E000000);
    if (base == (void *)-1) { perror("mmap"); return 1; }

    if (argc < 2) {
        printf("Usage: %s on|off [old|new]\n", argv[0]);
        return 1;
    }

    int state;
    if (strcmp(argv[1], "on") == 0) state = 1;
    else if (strcmp(argv[1], "off") == 0) state = 2;
    else state = atoi(argv[1]);

    int use_new = (argc > 2 && strcmp(argv[2], "new") == 0);

    if (use_new) {
        printf("NEW regs: PIC {0x34, 0x%02X, 0x00}\n", state);
        pic_write_new(0x34, state, 0x00);
    } else {
        printf("OLD regs: PIC {0x34, 0x%02X, 0x00}\n", state);
        pic_write_old(0x34, state, 0x00);
    }

    /* Also try cmd 0x33 (wake) + then buzzer */
    if (argc > 2 && strcmp(argv[2], "wake") == 0) {
        printf("Wake first: {0x33, 0x00, 0x01}\n");
        pic_write_old(0x33, 0x00, 0x01);
        usleep(50000);
        printf("Then buzzer: {0x34, 0x%02X, 0x00}\n", state);
        pic_write_old(0x34, state, 0x00);
    }

    printf("Done!\n");
    munmap((void *)base, 0x1000);
    close(fd);
    return 0;
}
