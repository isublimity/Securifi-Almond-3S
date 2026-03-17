/* gpio_check — read GPIOMODE and GPIO DIR/DATA registers */
#include <stdio.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

int main(void)
{
    int fd = open("/dev/mem", O_RDWR | O_SYNC);
    if (fd < 0) { perror("open /dev/mem"); return 1; }
    volatile unsigned int *base = mmap(NULL, 0x1000, PROT_READ | PROT_WRITE,
                                       MAP_SHARED, fd, 0x1E000000);
    if (base == (void *)-1) { perror("mmap"); return 1; }

    unsigned int gpiomode = base[0x060/4];
    unsigned int gpio_data = base[0x600/4];
    unsigned int gpio_dir = base[0x620/4];

    printf("GPIOMODE = 0x%08X\n", gpiomode);
    printf("GPIO_DATA= 0x%08X\n", gpio_data);
    printf("GPIO_DIR = 0x%08X\n", gpio_dir);

    /* Decode GPIOMODE bits for LCD-relevant groups */
    printf("\nGPIOMODE decode:\n");
    printf("  UART1  [1]    = %d (%s)\n", (gpiomode>>1)&1, ((gpiomode>>1)&1) ? "GPIO" : "UART1");
    printf("  I2C    [2]    = %d (%s)\n", (gpiomode>>2)&1, ((gpiomode>>2)&1) ? "GPIO" : "I2C");
    printf("  UART3  [4:3]  = %d (%s)\n", (gpiomode>>3)&3, ((gpiomode>>3)&3) ? "GPIO" : "UART3");
    printf("  UART2  [6:5]  = %d (%s)\n", (gpiomode>>5)&3, ((gpiomode>>5)&3) ? "GPIO" : "UART2");
    printf("  JTAG   [7]    = %d (%s) <- LCD D0,WRX,RST,CSX,DCX\n", (gpiomode>>7)&1, ((gpiomode>>7)&1) ? "GPIO" : "JTAG");
    printf("  WDT    [9:8]  = %d (%s) <- LCD D1\n", (gpiomode>>8)&3, ((gpiomode>>8)&3) ? "GPIO" : "WDT");
    printf("  PCIE   [11:10]= %d (%s)\n", (gpiomode>>10)&3, ((gpiomode>>10)&3) ? "GPIO" : "PCIE");
    printf("  MDIO   [13:12]= %d (%s)\n", (gpiomode>>12)&3, ((gpiomode>>12)&3) ? "GPIO" : "MDIO");
    printf("  RGMII1 [14]   = %d (%s)\n", (gpiomode>>14)&1, ((gpiomode>>14)&1) ? "GPIO" : "RGMII1");
    printf("  RGMII2 [15]   = %d (%s) <- LCD D2-D7,BL\n", (gpiomode>>15)&1, ((gpiomode>>15)&1) ? "GPIO" : "RGMII2");

    /* Check LCD-critical pins */
    printf("\nLCD pins DIR (1=output, 0=input):\n");
    int lcd_pins[] = {13,14,15,16,17,18,22,23,24,25,26,27,31};
    char *lcd_names[] = {"D0","WRX","RST","CSX","DCX","D1","D2","D3","D4","D5","D6","D7","BL"};
    for (int i = 0; i < 13; i++) {
        int pin = lcd_pins[i];
        int dir = (gpio_dir >> pin) & 1;
        int dat = (gpio_data >> pin) & 1;
        printf("  GPIO%-2d %-3s: DIR=%d DATA=%d\n", pin, lcd_names[i], dir, dat);
    }

    munmap((void *)base, 0x1000);
    close(fd);
    return 0;
}
