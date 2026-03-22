/* sm0_fix — restore SM0_CTL1 after palmbus test corruption */
#include <stdio.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <stdlib.h>

int main(int argc, char **argv)
{
    int fd = open("/dev/mem", O_RDWR | O_SYNC);
    if (fd < 0) { perror("open /dev/mem"); return 1; }

    volatile unsigned int *base = mmap(NULL, 0x1000, PROT_READ | PROT_WRITE,
                                       MAP_SHARED, fd, 0x1E000000);
    if (base == (void *)-1) { perror("mmap"); close(fd); return 1; }

    unsigned int sm0_ctl1 = base[0x940/4];
    printf("SM0_CTL1 = 0x%08X\n", sm0_ctl1);

    if (argc > 1) {
        unsigned int val = strtoul(argv[1], NULL, 0);
        base[0x940/4] = val;
        printf("SM0_CTL1 set to 0x%08X\n", val);
        printf("SM0_CTL1 readback = 0x%08X\n", base[0x940/4]);
    }

    /* Also show SM0_CFG */
    printf("SM0_CFG  = 0x%08X\n", base[0x900/4]);
    printf("SM0_DATA = 0x%08X\n", base[0x908/4]);

    munmap((void *)base, 0x1000);
    close(fd);
    return 0;
}
