/* sm0_dump — dump all SM0 I2C controller registers */
#include <stdio.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
int main(void) {
    int fd = open("/dev/mem", O_RDWR | O_SYNC);
    if (fd < 0) { perror("/dev/mem"); return 1; }
    volatile unsigned int *b = mmap(0, 4096, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0x1E000000);
    if (b == (void*)-1) { perror("mmap"); return 1; }
    int off;
    printf("SM0 I2C Controller registers:\n");
    for (off = 0x900; off <= 0x948; off += 4)
        printf("  0x%03X = 0x%08X\n", off, b[off/4]);

    /* Try setting CTL1 to stock mode and read poll register */
    printf("\nSet SM0_CTL1=0x90644042, read poll:\n");
    unsigned int saved = b[0x940/4];
    b[0x940/4] = 0x90644042;
    usleep(100);
    printf("  0x918 = 0x%08X (after CTL1 switch)\n", b[0x918/4]);
    printf("  0x940 = 0x%08X (readback CTL1)\n", b[0x940/4]);

    /* Restore */
    b[0x940/4] = saved;
    usleep(10);

    munmap((void*)b, 4096);
    close(fd);
    return 0;
}
