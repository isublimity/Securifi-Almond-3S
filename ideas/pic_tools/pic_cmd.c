/* pic_cmd — send arbitrary bytes to PIC via palmbus
 * Usage: pic_cmd 0x34 0x0B 0xB8
 * Build: zig cc -target mipsel-linux-musleabi -O2 -static -o pic_cmd pic_cmd.c
 */
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
static volatile unsigned int *base;
static void gw(int off, unsigned int v) { base[off/4] = v; }
static unsigned int gr(int off) { return base[off/4]; }

static void pic_write(unsigned char *d, int n)
{
    unsigned int saved = gr(0x940);
    int i;
    gw(0x940, 0x90644042); usleep(10);
    gw(0x900, 0xFA);
    gw(0x908, 0x2A);
    gw(0x920, n);
    gw(0x910, d[0]);
    gw(0x91C, 0);
    for (i = 1; i < n; i++) {
        int p; for (p = 0; p < 500; p++) { if (gr(0x918) & 0x02) break; usleep(10); }
        usleep(1000);
        gw(0x910, d[i]);
    }
    { int p; for (p = 0; p < 500; p++) { if (gr(0x918) & 0x01) break; usleep(10); } }
    gw(0x940, saved); usleep(10);
}

int main(int argc, char **argv)
{
    unsigned char buf[64];
    int i, n = 0;

    if (argc < 2) {
        printf("Usage: %s <byte0> [byte1] [byte2] ...\n", argv[0]);
        printf("Example: %s 0x34 0x01 0x00  # buzzer ON?\n", argv[0]);
        printf("         %s 0x34 0x0B 0xB8  # buzzer 3000Hz?\n", argv[0]);
        printf("         %s 0x33 0x00 0x01  # wake\n", argv[0]);
        return 1;
    }

    int fd = open("/dev/mem", O_RDWR | O_SYNC);
    if (fd < 0) { perror("/dev/mem"); return 1; }
    base = mmap(NULL, 0x1000, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0x1E000000);
    if (base == (void *)-1) { perror("mmap"); return 1; }

    for (i = 1; i < argc && n < 64; i++)
        buf[n++] = (unsigned char)strtol(argv[i], NULL, 0);

    printf("PIC write %d bytes:", n);
    for (i = 0; i < n; i++) printf(" %02X", buf[i]);
    printf("\n");

    pic_write(buf, n);
    printf("Done!\n");

    munmap((void *)base, 0x1000);
    close(fd);
    return 0;
}
