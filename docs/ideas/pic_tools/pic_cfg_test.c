/*
 * pic_cfg_test — Try SM0_CFG=0xFA and NEW manual mode read for PIC
 * Runs as userspace via /dev/mem mmap (needs lcd_gpio or direct mmap)
 *
 * Build: zig cc -target mipsel-linux-musleabi -O2 -static -o pic_cfg_test pic_cfg_test.c
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#define PALMBUS_BASE 0x1E000000
#define PALMBUS_SIZE 0x1000

/* SM0 register offsets */
#define SM0_CFG      0x900
#define SM0_DATA     0x908
#define SM0_SLAVE    0x90C
#define SM0_DATAOUT  0x910
#define SM0_DATAIN   0x914
#define SM0_POLLSTA  0x918
#define SM0_STATUS   0x91C
#define SM0_START    0x920
#define SM0_CFG2     0x928
#define N_CTL0       0x940
#define N_CTL1       0x944
#define N_D0         0x950
#define N_D1         0x954

#define PIC_ADDR     0x2A

static volatile unsigned int *base;

static unsigned int gr(int off) { return base[off/4]; }
static void gw(int off, unsigned int val) { base[off/4] = val; }

static void udelay_approx(int us) { usleep(us); }

/* NEW manual mode commands (N_CTL1) */
#define CMD_START    0x11
#define CMD_WRITE    0x21
#define CMD_STOP     0x31
#define CMD_READ     0x51  /* read + ACK */
#define CMD_READ_LAST 0x41 /* read + NACK (last byte) */

static int new_wait_ready(void) {
    int i;
    for (i = 0; i < 100000; i++) {
        if (gr(SM0_POLLSTA) & 0x01) return 1; /* bit 0 = done */
    }
    return 0;
}

static int new_write_byte(unsigned char byte) {
    gw(N_D0, byte);
    gw(N_CTL1, CMD_WRITE);
    return new_wait_ready();
}

static unsigned char new_read_byte(int last) {
    gw(N_CTL1, last ? CMD_READ_LAST : CMD_READ);
    new_wait_ready();
    return gr(N_D0) & 0xFF;
}

static void new_start(void) {
    gw(N_CTL1, CMD_START);
    new_wait_ready();
}

static void new_stop(void) {
    gw(N_CTL1, CMD_STOP);
    new_wait_ready();
}

/* Test 1: Try writing SM0_CFG=0xFA */
static void test_sm0_cfg(void) {
    unsigned int before, after;

    printf("\n=== TEST 1: SM0_CFG write attempts ===\n");

    before = gr(SM0_CFG);
    printf("SM0_CFG before: 0x%08X\n", before);

    /* Direct write */
    gw(SM0_CFG, 0xFA);
    after = gr(SM0_CFG);
    printf("After write 0xFA: 0x%08X %s\n", after, after == 0xFA ? "SUCCESS!" : "(no change)");

    /* Try with CFG2=0 (manual mode) */
    gw(SM0_CFG2, 0);
    gw(SM0_CFG, 0xFA);
    after = gr(SM0_CFG);
    printf("After CFG2=0 + write 0xFA: 0x%08X %s\n", after, after == 0xFA ? "SUCCESS!" : "(no change)");

    /* Try with different CTL0 values */
    unsigned int saved_ctl0 = gr(N_CTL0);
    gw(N_CTL0, 0x90640042);
    gw(SM0_CFG, 0xFA);
    after = gr(SM0_CFG);
    printf("After CTL0=stock + write 0xFA: 0x%08X %s\n", after, after == 0xFA ? "SUCCESS!" : "(no change)");

    /* Try all possible write patterns */
    int vals[] = {0xFA, 0xFF, 0x01, 0x80, 0x7F, 0xFE, 0xAA, 0x55};
    int i;
    for (i = 0; i < 8; i++) {
        gw(SM0_CFG, vals[i]);
        after = gr(SM0_CFG);
        if (after != 0) printf("  SM0_CFG=0x%02X → readback 0x%08X ***\n", vals[i], after);
    }

    gw(N_CTL0, saved_ctl0);
    gw(SM0_CFG2, 1); /* restore auto mode */
}

/* Test 2: NEW manual mode PIC read */
static void test_new_manual_read(unsigned int ctl0_val) {
    unsigned char resp[8] = {0};
    int i;
    unsigned int saved_ctl0 = gr(N_CTL0);

    printf("\n--- NEW manual read, CTL0=0x%08X ---\n", ctl0_val);

    gw(SM0_CFG2, 0);  /* manual mode */
    gw(N_CTL0, ctl0_val);
    udelay_approx(100);

    /* Write bat_read command first */
    new_start();
    new_write_byte((PIC_ADDR << 1) | 0);  /* write address */
    new_write_byte(0x2F);
    new_write_byte(0x00);
    new_write_byte(0x01);
    new_stop();

    printf("  Write bat_read: done\n");
    usleep(500000);  /* 500ms */

    /* Read */
    new_start();
    int ack = new_write_byte((PIC_ADDR << 1) | 1);  /* read address */
    printf("  Read addr ACK: %d\n", ack);

    for (i = 0; i < 8; i++) {
        resp[i] = new_read_byte(i == 7);
    }
    new_stop();

    printf("  Data: [%02x %02x %02x %02x %02x %02x %02x %02x]\n",
           resp[0], resp[1], resp[2], resp[3],
           resp[4], resp[5], resp[6], resp[7]);

    /* Check D0/D1 */
    printf("  D0=0x%08X D1=0x%08X\n", gr(N_D0), gr(N_D1));

    gw(N_CTL0, saved_ctl0);
    gw(SM0_CFG2, 1);
}

/* Test 3: OLD auto mode with SM0_CFG write attempt before read */
static void test_old_auto_with_cfg(void) {
    unsigned char resp[8] = {0};
    int i;

    printf("\n=== TEST 3: OLD auto mode + SM0_CFG=0xFA ===\n");

    gw(SM0_CFG2, 1); /* auto mode */
    gw(N_CTL0, 0x90640042); /* stock CTL0 */
    udelay_approx(10);
    gw(SM0_CFG, 0xFA); /* try to set (probably fails) */

    /* Write bat_read */
    gw(SM0_DATA, PIC_ADDR);
    gw(SM0_DATAOUT, 0x2F);
    gw(SM0_STATUS, 0);
    gw(SM0_START, 3);
    { int p; for (p = 0; p < 100000; p++) if (gr(SM0_POLLSTA) & 0x02) break; }
    udelay_approx(5000);
    gw(SM0_DATAOUT, 0x00);
    { int p; for (p = 0; p < 100000; p++) if (gr(SM0_POLLSTA) & 0x02) break; }
    udelay_approx(5000);
    gw(SM0_DATAOUT, 0x01);
    { int p; for (p = 0; p < 100000; p++) if (gr(SM0_POLLSTA) & 0x02) break; }

    printf("  Write bat_read done, waiting 500ms...\n");
    usleep(500000);

    /* Read */
    gw(SM0_START, 7); /* count-1 */
    gw(SM0_STATUS, 1); /* read mode */

    for (i = 0; i < 8; i++) {
        int p;
        for (p = 0; p < 100000; p++) if (gr(SM0_POLLSTA) & 0x04) break;
        udelay_approx(10);
        resp[i] = gr(SM0_DATAIN) & 0xFF;
    }

    printf("  SM0_CFG=0x%08X (after attempt)\n", gr(SM0_CFG));
    printf("  Data: [%02x %02x %02x %02x %02x %02x %02x %02x]\n",
           resp[0], resp[1], resp[2], resp[3],
           resp[4], resp[5], resp[6], resp[7]);
}

int main(void) {
    int fd = open("/dev/mem", O_RDWR | O_SYNC);
    if (fd < 0) {
        /* Try lcd_gpio */
        fd = open("/dev/lcd_gpio", O_RDWR | O_SYNC);
        if (fd < 0) {
            perror("open /dev/mem and /dev/lcd_gpio");
            return 1;
        }
    }

    base = mmap(NULL, PALMBUS_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, PALMBUS_BASE);
    if (base == MAP_FAILED) {
        perror("mmap");
        return 1;
    }

    printf("PIC CFG Test — SM0_CFG and NEW manual mode\n");
    printf("Current: SM0_CFG=0x%08X N_CTL0=0x%08X SM0_CFG2=0x%08X\n",
           gr(SM0_CFG), gr(N_CTL0), gr(SM0_CFG2));

    /* Test 1: SM0_CFG write */
    test_sm0_cfg();

    /* Test 2: NEW manual mode with different CTL0 values */
    printf("\n=== TEST 2: NEW manual mode PIC read ===\n");
    test_new_manual_read(0x01F3800F);  /* kernel default */
    test_new_manual_read(0x90640042);  /* stock kernel value */
    test_new_manual_read(0x01F380FA);  /* try embedding 0xFA in CTL0 */
    test_new_manual_read(0x01FA800F);  /* try another position */

    /* Test 3: OLD auto + CFG attempt */
    test_old_auto_with_cfg();

    printf("\nDone.\n");

    munmap((void*)base, PALMBUS_SIZE);
    close(fd);
    return 0;
}
