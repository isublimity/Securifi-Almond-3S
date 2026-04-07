/*
 * pic_stock_test.c — Userspace PIC16 I2C test via palmbus mmap
 *
 * Reproduces EXACT stock kernel 3.10 SM0 auto mode protocol.
 * Must run with i2c-mt7621 and lcd_drv UNLOADED (rmmod both first).
 *
 * Build: zig cc -target mipsel-linux-musleabi -O2 -static -o pic_stock_test pic_stock_test.c
 * Usage: /tmp/pic_stock_test [command]
 *   (no args)  = full test: init + read + monitor
 *   read       = just read 8 bytes
 *   buzzer_on  = buzzer ON
 *   buzzer_off = buzzer OFF
 *   bat_read   = send bat_read + read response
 *   dump       = dump SM0 registers
 *   bb_read    = bit-bang I2C read 8 bytes
 *   bb_combined= bit-bang Combined: Write bat_read + Restart + Read
 *   bb_buzzer  = bit-bang buzzer ON 1s OFF
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <stdint.h>

#define PALMBUS_BASE  0x1E000000
#define PALMBUS_SIZE  0x1000

/* SM0 register offsets from palmbus base */
#define SM0_CFG       0x900
#define SM0_DATA      0x908
#define SM0_DATAOUT   0x910
#define SM0_DATAIN    0x914
#define SM0_POLLSTA   0x918
#define SM0_STATUS    0x91C
#define SM0_START     0x920
#define SM0_CFG2      0x928
#define SM0_CTL0      0x940
#define SM0_CTL1      0x944
#define SM0_D0        0x950
#define SM0_D1        0x954

#define PIC_ADDR      0x2A

/* GPIO sysfs bit-bang I2C */
#define SDA_GPIO  515   /* gpiochip0(512) + pin 3 */
#define SCL_GPIO  516   /* gpiochip0(512) + pin 4 */
#define BB_DELAY  10    /* us */

static volatile uint32_t *base;

/* === GPIO sysfs helpers === */
static void gpio_export(int gpio)
{
    char buf[64];
    FILE *f = fopen("/sys/class/gpio/export", "w");
    if (f) { fprintf(f, "%d", gpio); fclose(f); }
    usleep(50000);  /* wait for sysfs to create files */
    /* Set as input initially */
    snprintf(buf, sizeof(buf), "/sys/class/gpio/gpio%d/direction", gpio);
    f = fopen(buf, "w");
    if (f) { fprintf(f, "in"); fclose(f); }
}

static void gpio_unexport(int gpio)
{
    FILE *f = fopen("/sys/class/gpio/unexport", "w");
    if (f) { fprintf(f, "%d", gpio); fclose(f); }
}

static void gpio_dir_out(int gpio, int val)
{
    char buf[64];
    snprintf(buf, sizeof(buf), "/sys/class/gpio/gpio%d/direction", gpio);
    FILE *f = fopen(buf, "w");
    if (f) { fprintf(f, val ? "high" : "low"); fclose(f); }
}

static void gpio_dir_in(int gpio)
{
    char buf[64];
    snprintf(buf, sizeof(buf), "/sys/class/gpio/gpio%d/direction", gpio);
    FILE *f = fopen(buf, "w");
    if (f) { fprintf(f, "in"); fclose(f); }
}

static int gpio_read(int gpio)
{
    char buf[64], val[4] = {0};
    snprintf(buf, sizeof(buf), "/sys/class/gpio/gpio%d/value", gpio);
    FILE *f = fopen(buf, "r");
    if (!f) return -1;
    fread(val, 1, 1, f);
    fclose(f);
    return val[0] == '1' ? 1 : 0;
}

/* === Bit-bang I2C primitives === */
static void bb_sda_low(void)  { gpio_dir_out(SDA_GPIO, 0); usleep(1); }
static void bb_sda_high(void) { gpio_dir_in(SDA_GPIO); usleep(1); }
static void bb_scl_low(void)  { gpio_dir_out(SCL_GPIO, 0); usleep(BB_DELAY); }

static void bb_scl_high(void)
{
    int timeout = 10000;
    gpio_dir_in(SCL_GPIO);
    /* Clock stretching: wait for slave to release SCL */
    while (gpio_read(SCL_GPIO) == 0 && timeout--)
        usleep(1);
    if (timeout <= 0) printf("  [SCL stretch timeout!]\n");
    usleep(BB_DELAY);
}

static int bb_sda_read(void)
{
    gpio_dir_in(SDA_GPIO);
    usleep(2);
    return gpio_read(SDA_GPIO);
}

static void bb_start(void)
{
    bb_sda_high(); bb_scl_high(); usleep(BB_DELAY);
    bb_sda_low(); usleep(BB_DELAY);
    bb_scl_low();
}

static void bb_stop(void)
{
    bb_sda_low(); usleep(BB_DELAY);
    bb_scl_high(); usleep(BB_DELAY);
    bb_sda_high(); usleep(BB_DELAY);
}

static void bb_restart(void)
{
    bb_sda_high(); usleep(BB_DELAY);
    bb_scl_high(); usleep(BB_DELAY);
    bb_sda_low(); usleep(BB_DELAY);
    bb_scl_low();
}

static int bb_write_byte(uint8_t byte)
{
    int i, ack;
    for (i = 7; i >= 0; i--) {
        if ((byte >> i) & 1) bb_sda_high(); else bb_sda_low();
        bb_scl_high();
        bb_scl_low();
    }
    /* ACK: release SDA, clock, read */
    bb_sda_high();
    usleep(5);
    bb_scl_high();
    usleep(2);
    ack = bb_sda_read();
    bb_scl_low();
    usleep(5);
    return (ack == 0);  /* 0 = ACK */
}

static uint8_t bb_read_byte(int send_ack)
{
    uint8_t byte = 0;
    int i;
    gpio_dir_in(SDA_GPIO);
    usleep(5);
    for (i = 7; i >= 0; i--) {
        bb_scl_high();
        usleep(2);
        if (gpio_read(SDA_GPIO)) byte |= (1 << i);
        bb_scl_low();
        usleep(5);
    }
    /* ACK/NACK */
    if (send_ack) {
        gpio_dir_out(SDA_GPIO, 0);  /* SDA LOW = ACK */
    } else {
        gpio_dir_in(SDA_GPIO);      /* SDA HIGH = NACK */
    }
    usleep(5);
    bb_scl_high();
    usleep(5);
    bb_scl_low();
    usleep(5);
    gpio_dir_in(SDA_GPIO);  /* release */
    usleep(10);
    return byte;
}

/* === High-level bit-bang I2C === */
static int bb_pic_write(uint8_t *data, int len)
{
    int i, ok = 1;
    bb_start();
    if (!bb_write_byte((PIC_ADDR << 1) | 0)) {
        printf("  BB write: addr NACK\n");
        ok = 0; goto done;
    }
    for (i = 0; i < len; i++) {
        if (!bb_write_byte(data[i])) {
            printf("  BB write: byte %d NACK\n", i);
            ok = 0; goto done;
        }
    }
done:
    bb_stop();
    return ok;
}

static int bb_pic_read(uint8_t *buf, int len)
{
    int i, ok = 1;
    bb_start();
    if (!bb_write_byte((PIC_ADDR << 1) | 1)) {
        printf("  BB read: addr NACK\n");
        ok = 0; goto done;
    }
    for (i = 0; i < len; i++)
        buf[i] = bb_read_byte(i < len - 1);
done:
    bb_stop();
    return ok;
}

static int bb_pic_combined(uint8_t *cmd, int cmd_len, uint8_t *resp, int resp_len)
{
    int i, ok = 1;
    bb_start();
    if (!bb_write_byte((PIC_ADDR << 1) | 0)) {
        printf("  BB combined: addr+W NACK\n");
        ok = 0; goto done;
    }
    for (i = 0; i < cmd_len; i++) {
        if (!bb_write_byte(cmd[i])) {
            printf("  BB combined: cmd[%d] NACK\n", i);
            ok = 0; goto done;
        }
    }
    /* RESTART — not STOP! */
    bb_restart();
    if (!bb_write_byte((PIC_ADDR << 1) | 1)) {
        printf("  BB combined: addr+R NACK\n");
        ok = 0; goto done;
    }
    for (i = 0; i < resp_len; i++)
        resp[i] = bb_read_byte(i < resp_len - 1);
done:
    bb_stop();
    return ok;
}

static uint32_t reg_read(int off) { return base[off / 4]; }
static void reg_write(int off, uint32_t val) { base[off / 4] = val; }

static void dump_regs(const char *label)
{
    printf("[%s] CTL0=0x%08X CTL1=0x%08X CFG=0x%08X CFG2=0x%08X\n",
           label, reg_read(SM0_CTL0), reg_read(SM0_CTL1),
           reg_read(SM0_CFG), reg_read(SM0_CFG2));
    printf("  DATA=0x%08X DATAOUT=0x%08X DATAIN=0x%08X\n",
           reg_read(SM0_DATA), reg_read(SM0_DATAOUT), reg_read(SM0_DATAIN));
    printf("  STATUS=0x%08X START=0x%08X POLLSTA=0x%08X\n",
           reg_read(SM0_STATUS), reg_read(SM0_START), reg_read(SM0_POLLSTA));
    printf("  D0=0x%08X D1=0x%08X\n",
           reg_read(SM0_D0), reg_read(SM0_D1));
}

/* Initialize SM0 exactly as stock kernel 3.10.14 */
static void sm0_init_stock(void)
{
    /* CTL0: hardware modifies bits [6:0], 0x42→0x0E on this silicon.
     * Use 0x80644042 and accept readback 0x8064800E.
     * Clock divider 0x0064 = 100 → ~500kHz with 50MHz base.
     * Bit 31 = ODRAIN (inverted on some revisions). */
    printf("SM0 init: CTL0=0x80644002 CFG=0xFA CFG2=0x01 (auto mode)\n");
    reg_write(SM0_CTL0, 0x80644002);  /* EN=1, SCL_STRETCH=1 */
    usleep(10);
    reg_write(SM0_CFG, 0xFA);
    reg_write(SM0_CFG2, 0x01);  /* auto mode ON */
    usleep(100);
}

/* Stock auto mode WRITE: SM0_DATA=addr, SM0_START=len, SM0_DATAOUT per byte */
static int sm0_write(uint8_t *data, int len)
{
    int i, timeout;

    reg_write(SM0_DATA, PIC_ADDR);
    reg_write(SM0_START, len);
    reg_write(SM0_DATAOUT, data[0]);
    reg_write(SM0_STATUS, 0);  /* write mode */

    for (i = 1; i < len; i++) {
        /* Poll POLLSTA bit 1 (ready for next byte) */
        timeout = 10000;
        while (!(reg_read(SM0_POLLSTA) & 0x02) && timeout--)
            usleep(1);
        if (timeout <= 0) {
            printf("  WRITE timeout at byte %d, POLLSTA=0x%02X\n", i, reg_read(SM0_POLLSTA));
            return -1;
        }
        usleep(5000);  /* stock: udelay(5000) between bytes */
        reg_write(SM0_DATAOUT, data[i]);
    }

    /* Wait for completion (bit 0) */
    timeout = 10000;
    while (!(reg_read(SM0_POLLSTA) & 0x01) && timeout--)
        usleep(1);
    if (timeout <= 0) {
        printf("  WRITE completion timeout, POLLSTA=0x%02X\n", reg_read(SM0_POLLSTA));
        return -1;
    }

    printf("  WRITE %d bytes OK, POLLSTA=0x%02X\n", len, reg_read(SM0_POLLSTA));
    return 0;
}

/* Stock auto mode READ: SM0_START=len-1, SM0_STATUS=1, poll bit 2, SM0_DATAIN */
static int sm0_read(uint8_t *buf, int len)
{
    int i, timeout;

    reg_write(SM0_DATA, PIC_ADDR);
    reg_write(SM0_START, len - 1);
    reg_write(SM0_STATUS, 1);  /* read mode */

    for (i = 0; i < len; i++) {
        /* Poll POLLSTA bit 2 (data ready) */
        timeout = 10000;
        while (!(reg_read(SM0_POLLSTA) & 0x04) && timeout--)
            usleep(1);
        if (timeout <= 0) {
            printf("  READ timeout at byte %d, POLLSTA=0x%02X\n", i, reg_read(SM0_POLLSTA));
            return i;
        }
        usleep(10);
        buf[i] = reg_read(SM0_DATAIN) & 0xFF;
    }

    /* Wait for completion */
    timeout = 10000;
    while (!(reg_read(SM0_POLLSTA) & 0x01) && timeout--)
        usleep(1);

    return len;
}

static void print_hex(const char *label, uint8_t *buf, int len)
{
    printf("%s:", label);
    for (int i = 0; i < len; i++)
        printf(" %02x", buf[i]);
    printf("\n");
}

/* ===== Commands ===== */

static void cmd_dump(void)
{
    dump_regs("current");
}

static void cmd_read(void)
{
    uint8_t buf[17] = {0};
    int n;

    sm0_init_stock();
    n = sm0_read(buf, 8);
    printf("Read %d bytes: ", n);
    print_hex("DATA", buf, 8);
    dump_regs("after-read");
}

static void cmd_bat_read(void)
{
    uint8_t cmd[3] = { 0x2F, 0x00, 0x02 };
    uint8_t buf[8] = {0};
    int rc, n;

    sm0_init_stock();

    printf("=== bat_read {0x2F, 0x00, 0x02} ===\n");
    rc = sm0_write(cmd, 3);
    if (rc < 0) return;

    usleep(200000);  /* 200ms for PIC to process */

    printf("=== Reading response ===\n");
    n = sm0_read(buf, 8);
    printf("Read %d bytes: ", n);
    print_hex("BAT", buf, 8);
}

static void cmd_buzzer(int on)
{
    uint8_t cmd[3] = { 0x34, 0x00, on ? 0x03 : 0x00 };

    sm0_init_stock();
    printf("=== Buzzer %s {0x34, 0x00, 0x%02X} ===\n", on ? "ON" : "OFF", cmd[2]);
    sm0_write(cmd, 3);
}

static void cmd_wake(void)
{
    uint8_t cmd[3] = { 0x33, 0x00, 0x01 };

    sm0_init_stock();
    printf("=== WAKE {0x33, 0x00, 0x01} ===\n");
    sm0_write(cmd, 3);
}

static void cmd_full_test(void)
{
    uint8_t buf[8] = {0};
    uint8_t bat_cmd[3] = { 0x2F, 0x00, 0x02 };
    int n, i;

    dump_regs("before-init");

    /* Test ALL SM0 register writes */
    printf("\n=== SM0 register write test ===\n");
    struct { int off; const char *name; uint32_t val; } tests[] = {
        {SM0_CTL0, "CTL0", 0x90644042},
        {SM0_CTL0, "CTL0", 0x80644002},
        {SM0_CTL0, "CTL0", 0x01F3800F},
        {SM0_CFG,  "CFG",  0xFA},
        {SM0_CFG,  "CFG",  0x01},
        {SM0_CFG,  "CFG",  0xFF},
        {SM0_CFG2, "CFG2", 0x00},
        {SM0_CFG2, "CFG2", 0x01},
        {SM0_DATA, "DATA", 0x2A},
        {SM0_DATA, "DATA", 0x48},
    };
    for (i = 0; i < 10; i++) {
        reg_write(tests[i].off, tests[i].val);
        usleep(100);
        printf("  %s: write 0x%08X → read 0x%08X\n",
               tests[i].name, tests[i].val, reg_read(tests[i].off));
    }

    sm0_init_stock();
    dump_regs("after-init");

    /* Step 1: Read current PIC state */
    printf("\n=== Step 1: Initial read ===\n");
    n = sm0_read(buf, 8);
    print_hex("INITIAL", buf, n);

    /* Step 2: WAKE */
    printf("\n=== Step 2: WAKE ===\n");
    cmd_wake();
    usleep(5000);

    /* Step 3: bat_read */
    printf("\n=== Step 3: bat_read ===\n");
    sm0_write(bat_cmd, 3);
    usleep(200000);

    /* Step 4: Read after bat_read */
    printf("\n=== Step 4: Read after bat_read ===\n");
    n = sm0_read(buf, 8);
    print_hex("AFTER_BAT", buf, n);

    /* Step 5: Monitor 60 seconds */
    printf("\n=== Step 5: Monitor (60 sec, every 5 sec) ===\n");
    for (i = 0; i < 12; i++) {
        sleep(5);
        memset(buf, 0, 8);
        n = sm0_read(buf, 8);
        printf("[%3ds] ", (i + 1) * 5);
        print_hex("BAT", buf, n);
    }

    /* Step 6: Buzzer test */
    printf("\n=== Step 6: Buzzer test ===\n");
    cmd_buzzer(1);
    printf("Buzzer ON — hear anything? (waiting 2 sec)\n");
    sleep(2);
    cmd_buzzer(0);
    printf("Buzzer OFF\n");

    printf("\n=== DONE ===\n");
}

int main(int argc, char **argv)
{
    int fd;

    fd = open("/dev/mem", O_RDWR | O_SYNC);
    if (fd < 0) {
        perror("open /dev/mem");
        printf("Run as root!\n");
        return 1;
    }

    base = mmap(NULL, PALMBUS_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, PALMBUS_BASE);
    if (base == MAP_FAILED) {
        perror("mmap");
        close(fd);
        return 1;
    }

    printf("pic_stock_test: palmbus mapped at %p\n", base);

    /* Unbind i2c-mt7621 driver to free SM0 controller */
    {
        FILE *f = fopen("/sys/bus/platform/drivers/i2c-mt7621/unbind", "w");
        if (f) {
            fprintf(f, "1e000900.i2c");
            fclose(f);
            printf("i2c-mt7621 unbound\n");
            usleep(100000);
        } else {
            printf("WARNING: cannot unbind i2c (may already be unbound)\n");
        }
    }

    if (argc < 2 || strcmp(argv[1], "full") == 0) {
        cmd_full_test();
    } else if (strcmp(argv[1], "dump") == 0) {
        cmd_dump();
    } else if (strcmp(argv[1], "read") == 0) {
        cmd_read();
    } else if (strcmp(argv[1], "bat_read") == 0) {
        cmd_bat_read();
    } else if (strcmp(argv[1], "buzzer_on") == 0) {
        cmd_buzzer(1);
    } else if (strcmp(argv[1], "buzzer_off") == 0) {
        cmd_buzzer(0);
    } else if (strcmp(argv[1], "wake") == 0) {
        cmd_wake();
    } else if (strcmp(argv[1], "bb_read") == 0) {
        printf("=== BB GPIO sysfs I2C read ===\n");
        /* Switch GPIOMODE to GPIO for I2C pins */
        uint32_t gm = reg_read(0x060);
        reg_write(0x060, gm | (1 << 2));
        printf("GPIOMODE: 0x%08X → 0x%08X\n", gm, reg_read(0x060));
        usleep(10000);
        gpio_export(SDA_GPIO); gpio_export(SCL_GPIO);
        usleep(50000);
        printf("SDA=%d SCL=%d (idle)\n", gpio_read(SDA_GPIO), gpio_read(SCL_GPIO));
        uint8_t buf[8] = {0};
        if (bb_pic_read(buf, 8)) {
            print_hex("BB_READ", buf, 8);
        }
        gpio_unexport(SDA_GPIO); gpio_unexport(SCL_GPIO);
    } else if (strcmp(argv[1], "bb_combined") == 0) {
        printf("=== BB Combined: Write bat_read + Restart + Read ===\n");
        uint32_t gm = reg_read(0x060);
        reg_write(0x060, gm | (1 << 2));
        usleep(10000);
        gpio_export(SDA_GPIO); gpio_export(SCL_GPIO);
        usleep(50000);
        printf("SDA=%d SCL=%d (idle)\n", gpio_read(SDA_GPIO), gpio_read(SCL_GPIO));
        uint8_t cmd[3] = { 0x2F, 0x00, 0x02 };
        uint8_t resp[8] = {0};
        if (bb_pic_combined(cmd, 3, resp, 8)) {
            print_hex("COMBINED", resp, 8);
        }
        /* Monitor 30 sec */
        printf("Monitoring 30 sec...\n");
        for (int i = 0; i < 6; i++) {
            sleep(5);
            memset(resp, 0, 8);
            bb_pic_combined(cmd, 3, resp, 8);
            printf("[%2ds] ", (i+1)*5);
            print_hex("BAT", resp, 8);
        }
        gpio_unexport(SDA_GPIO); gpio_unexport(SCL_GPIO);
    } else if (strcmp(argv[1], "bb_buzzer") == 0) {
        printf("=== BB Buzzer test ===\n");
        uint32_t gm = reg_read(0x060);
        reg_write(0x060, gm | (1 << 2));
        usleep(10000);
        gpio_export(SDA_GPIO); gpio_export(SCL_GPIO);
        usleep(50000);
        uint8_t on[3]  = { 0x34, 0x00, 0x03 };
        uint8_t off[3] = { 0x34, 0x00, 0x00 };
        printf("Buzzer ON: %s\n", bb_pic_write(on, 3) ? "ACK" : "NACK");
        sleep(1);
        printf("Buzzer OFF: %s\n", bb_pic_write(off, 3) ? "ACK" : "NACK");
        gpio_unexport(SDA_GPIO); gpio_unexport(SCL_GPIO);
    } else if (strcmp(argv[1], "bb_write") == 0 && argc > 2) {
        printf("=== BB Write test ===\n");
        uint32_t gm = reg_read(0x060);
        reg_write(0x060, gm | (1 << 2));
        usleep(10000);
        gpio_export(SDA_GPIO); gpio_export(SCL_GPIO);
        usleep(50000);
        /* Parse hex bytes from args: bb_write 2F 00 02 */
        uint8_t data[16];
        int n = 0;
        for (int i = 2; i < argc && n < 16; i++)
            data[n++] = strtol(argv[i], NULL, 16);
        printf("Sending %d bytes: ", n);
        print_hex("CMD", data, n);
        printf("Result: %s\n", bb_pic_write(data, n) ? "ACK" : "NACK");
        gpio_unexport(SDA_GPIO); gpio_unexport(SCL_GPIO);
    } else {
        printf("Usage: %s [full|dump|read|bat_read|buzzer_on|buzzer_off|wake|bb_read|bb_combined|bb_buzzer|bb_write XX XX]\n", argv[0]);
    }

    /* Rebind i2c-mt7621 driver */
    {
        FILE *f = fopen("/sys/bus/platform/drivers/i2c-mt7621/bind", "w");
        if (f) {
            fprintf(f, "1e000900.i2c");
            fclose(f);
            printf("i2c-mt7621 rebound\n");
        }
    }

    munmap((void *)base, PALMBUS_SIZE);
    close(fd);
    return 0;
}
