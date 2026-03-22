#include <stdio.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <stdlib.h>

int main(int argc, char *argv[]) {
    int fd = open("/dev/mem", O_RDONLY | O_SYNC);
    if (fd < 0) { perror("/dev/mem"); return 1; }

    volatile unsigned int *base = mmap(NULL, 0x1000,
        PROT_READ, MAP_SHARED, fd, 0x1E000000);
    if (base == MAP_FAILED) { perror("mmap"); return 1; }

    unsigned int off = argc > 1 ? strtoul(argv[1], NULL, 0) : 0x950;

    printf("palmbus[0x%03X] = 0x%08X\n", off, base[off/4]);

    if (argc <= 1) {
        /* Dump SM0 regs */
        printf("SM0_CFG   (0x900) = 0x%08X\n", base[0x900/4]);
        printf("SM0_DATA  (0x908) = 0x%08X\n", base[0x908/4]);
        printf("SM0_DOUT  (0x910) = 0x%08X\n", base[0x910/4]);
        printf("SM0_DIN   (0x914) = 0x%08X\n", base[0x914/4]);
        printf("SM0_POLL  (0x918) = 0x%08X\n", base[0x918/4]);
        printf("SM0_STAT  (0x91C) = 0x%08X\n", base[0x91C/4]);
        printf("SM0_START (0x920) = 0x%08X\n", base[0x920/4]);
        printf("SM0_CFG2  (0x928) = 0x%08X\n", base[0x928/4]);
        printf("N_CTL0    (0x940) = 0x%08X\n", base[0x940/4]);
        printf("N_CTL1    (0x944) = 0x%08X\n", base[0x944/4]);
        printf("N_D0      (0x950) = 0x%08X\n", base[0x950/4]);
        printf("N_D1      (0x954) = 0x%08X\n", base[0x954/4]);
    }

    munmap((void*)base, 0x1000);
    close(fd);
    return 0;
}
