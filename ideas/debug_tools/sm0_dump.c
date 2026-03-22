#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#define PALMBUS_BASE 0x1E000000
#define PALMBUS_SIZE 0x1000

static volatile unsigned int *base;
static unsigned int gr(int off) { return base[off/4]; }

int main(int argc, char **argv) {
    int fd, i, loops;
    fd = open("/dev/mem", O_RDWR | O_SYNC);
    if (fd < 0) { perror("/dev/mem"); return 1; }
    base = mmap(NULL, PALMBUS_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, PALMBUS_BASE);
    if (base == MAP_FAILED) { perror("mmap"); return 1; }
    loops = (argc > 1) ? atoi(argv[1]) : 30;

    printf("=== SM0 FULL DUMP ===\n");
    printf("RSTCTRL  (034) = 0x%08X\n", gr(0x034));
    printf("SM0_CFG  (900) = 0x%08X\n", gr(0x900));
    printf("SM0_904  (904) = 0x%08X\n", gr(0x904));
    printf("SM0_DATA (908) = 0x%08X (slave addr)\n", gr(0x908));
    printf("SM0_SLAV (90C) = 0x%08X\n", gr(0x90C));
    printf("SM0_DOUT (910) = 0x%08X\n", gr(0x910));
    printf("SM0_DIN  (914) = 0x%08X (read data)\n", gr(0x914));
    printf("SM0_POLL (918) = 0x%08X\n", gr(0x918));
    printf("SM0_STAT (91C) = 0x%08X (0=W 1=R 2=WR)\n", gr(0x91C));
    printf("SM0_STRT (920) = 0x%08X\n", gr(0x920));
    printf("SM0_CFG2 (928) = 0x%08X (0=man 1=auto)\n", gr(0x928));
    printf("N_CTL0   (940) = 0x%08X\n", gr(0x940));
    printf("N_CTL1   (944) = 0x%08X\n", gr(0x944));
    printf("N_D0     (950) = 0x%08X\n", gr(0x950));
    printf("N_D1     (954) = 0x%08X\n", gr(0x954));

    printf("\n=== POLLING (catching SM0 mid-transaction) ===\n");
    printf("# CFG DATA DOUT DIN POLL STAT STRT CFG2 CTL0 D0 D1\n");
    for (i = 0; i < loops * 100; i++) {
        unsigned int din = gr(0x914);
        unsigned int poll = gr(0x918);
        unsigned int stat = gr(0x91C);
        unsigned int data = gr(0x908);
        unsigned int strt = gr(0x920);
        /* Only print when something interesting: poll != idle or data == PIC addr */
        if ((data & 0xFF) == 0x2A || (poll & 0x07) != 0x02 || (stat & 0xFF) != 0) {
            printf("%d %08X %02X %08X %08X %02X %02X %04X %08X %08X %08X %08X\n",
                   i, gr(0x900), data & 0xFF, gr(0x910), din,
                   poll & 0xFF, stat & 0xFF, strt & 0xFFFF,
                   gr(0x928), gr(0x940), gr(0x950), gr(0x954));
        }
        usleep(10000); /* 10ms */
    }

    munmap((void *)base, PALMBUS_SIZE);
    close(fd);
    return 0;
}
