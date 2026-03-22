/*
 * pic_test.c — PIC16LF1509 battery test tool for Almond 3S
 *
 * Tests all ideas for getting live ADC data from PIC.
 * Uses /dev/lcd ioctl(2) for reading and direct palmbus for writing.
 *
 * Build: zig cc -target mipsel-linux-musleabi -Os -static -o pic_test pic_test.c
 * Usage: pic_test [test_number]
 *   1 — Read current PIC data (ioctl 2)
 *   2 — Send bat_read {0x2F, 0x00, 0x01} via ioctl(3) + read
 *   3 — Send bat_read {0x2F, 0x00, 0x02} + read
 *   4 — Send WAKE {0x33, 0x00, 0x00} + bat_read + read
 *   5 — Send WAKE {0x33, 0x00, 0x01} + bat_read + read
 *   6 — Continuous read (10 iterations, check if data changes)
 *   7 — Buzzer test ON/OFF
 *   8 — Raw PIC read (ioctl 3)
 *   9 — Send {0x2F, 0x00, 0x00} (bat_read mode 0) + read
 *  10 — Full re-init: {0x41} + buzzer off + WAKE + calib + bat_read + read
 *  all — Run tests 1-6,8,9 sequentially
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <stdint.h>

#define PIC_ADDR 0x2A
#define PALMBUS_BASE 0x1E000000
#define SM0_CFG     0x900
#define SM0_DATA    0x908
#define SM0_DATAOUT 0x910
#define SM0_DATAIN  0x914
#define SM0_STATUS  0x91C
#define SM0_START   0x920
#define SM0_CTL1    0x940

static int lcd_fd;
static volatile uint32_t *palmbus;

static void palmbus_init(void)
{
    int mem_fd = open("/dev/mem", O_RDWR | O_SYNC);
    if (mem_fd < 0) { perror("/dev/mem"); return; }
    palmbus = mmap(NULL, 0x1000, PROT_READ | PROT_WRITE, MAP_SHARED, mem_fd, PALMBUS_BASE);
    close(mem_fd);
    if (palmbus == MAP_FAILED) { perror("mmap"); palmbus = NULL; }
}

static inline void gw(uint32_t off, uint32_t v) { if (palmbus) palmbus[off/4] = v; }
static inline uint32_t gr(uint32_t off) { return palmbus ? palmbus[off/4] : 0; }

/* PIC write via lcd_drv ioctl(8) — safe, no /dev/mem needed */
static int pic_write(const uint8_t *data, int len)
{
    uint8_t p[9] = {0};
    if (len > 8) len = 8;
    p[0] = len;
    memcpy(p + 1, data, len);
    int ret = ioctl(lcd_fd, 8, p);
    if (ret < 0) perror("ioctl(8) PIC write");
    return ret;
}

static void hex_dump(const char *label, const uint8_t *data, int len)
{
    int i;
    printf("%s: ", label);
    for (i = 0; i < len; i++) printf("%02x ", data[i]);
    printf("\n");
}

/* Read PIC via ioctl(2) — returns lcd_drv cached battery data */
static int pic_read_cached(uint8_t *buf)
{
    return ioctl(lcd_fd, 2, buf);
}

/* Read PIC via ioctl(3) — raw PIC read */
static int pic_read_raw(uint8_t *buf)
{
    return ioctl(lcd_fd, 3, buf);
}

/* Test 1: Read current cached data */
static void test_read(void)
{
    uint8_t buf[17] = {0};
    printf("\n=== Test 1: Read cached PIC data (ioctl 2) ===\n");
    int ret = pic_read_cached(buf);
    hex_dump("Cached", buf, 8);
    printf("ret=%d\n", ret);
}

/* Test 2: Send bat_read {0x2F, 0x00, 0x01} then read */
static void test_bat_read_01(void)
{
    uint8_t cmd[] = {0x2F, 0x00, 0x01};
    uint8_t buf[17] = {0};
    printf("\n=== Test 2: bat_read {2F,00,01} + read ===\n");
    
    pic_write(cmd, 3);
    printf("Sent bat_read mode 0x01\n");
    usleep(500000);
    pic_read_raw(buf);
    hex_dump("After", buf, 8);
}

/* Test 3: Send bat_read {0x2F, 0x00, 0x02} then read */
static void test_bat_read_02(void)
{
    uint8_t cmd[] = {0x2F, 0x00, 0x02};
    uint8_t buf[17] = {0};
    printf("\n=== Test 3: bat_read {2F,00,02} + read ===\n");
    
    pic_write(cmd, 3);
    printf("Sent bat_read mode 0x02\n");
    usleep(500000);
    pic_read_raw(buf);
    hex_dump("After", buf, 8);
}

/* Test 4: WAKE {0x33,0,0} + bat_read + read */
static void test_wake_bat_read(void)
{
    uint8_t wake[] = {0x33, 0x00, 0x00};
    uint8_t bat[] = {0x2F, 0x00, 0x01};
    uint8_t buf[17] = {0};
    printf("\n=== Test 4: WAKE {33,00,00} + bat_read {2F,00,01} + read ===\n");
    
    pic_write(wake, 3);
    printf("Sent WAKE\n");
    usleep(5000);
    pic_write(bat, 3);
    printf("Sent bat_read\n");
    usleep(500000);
    pic_read_raw(buf);
    hex_dump("After", buf, 8);
}

/* Test 5: WAKE {0x33,0,1} + bat_read + read */
static void test_wake_count1(void)
{
    uint8_t wake[] = {0x33, 0x00, 0x01};
    uint8_t bat[] = {0x2F, 0x00, 0x01};
    uint8_t buf[17] = {0};
    printf("\n=== Test 5: WAKE {33,00,01} + bat_read + read ===\n");
    
    pic_write(wake, 3);
    printf("Sent WAKE count=1\n");
    usleep(5000);
    pic_write(bat, 3);
    printf("Sent bat_read\n");
    usleep(500000);
    pic_read_raw(buf);
    hex_dump("After", buf, 8);
}

/* Test 6: Continuous read — check if data changes */
static void test_continuous(void)
{
    uint8_t buf[17] = {0};
    uint8_t prev[8] = {0};
    int i, changes = 0;
    printf("\n=== Test 6: Continuous read (10 iterations, 2s interval) ===\n");
    for (i = 0; i < 10; i++) {
        pic_read_raw(buf);
        int changed = memcmp(buf, prev, 8) != 0;
        if (changed) changes++;
        printf("[%2d] %02x %02x %02x %02x %02x %02x %02x %02x %s\n",
               i, buf[0], buf[1], buf[2], buf[3],
               buf[4], buf[5], buf[6], buf[7],
               changed ? "CHANGED!" : "same");
        memcpy(prev, buf, 8);
        sleep(2);
    }
    printf("Changes: %d/10\n", changes);
}

/* Test 7: Buzzer — DISABLED! No known OFF command. Power cycle only. */
static void test_buzzer(void)
{
    printf("\n=== Test 7: Buzzer DISABLED ===\n");
    printf("WARNING: {0x34,0,3} turns buzzer ON but {0x34,0,0} does NOT turn it OFF!\n");
    printf("Only power cycle stops it. DO NOT RUN.\n");
}

/* Test 8: Raw PIC read */
static void test_raw_read(void)
{
    uint8_t buf[17] = {0};
    printf("\n=== Test 8: Raw PIC read (ioctl 3) ===\n");
    int ret = pic_read_raw(buf);
    hex_dump("Raw[17]", buf, 17);
    printf("ret=%d\n", ret);
}

/* Test 9: bat_read mode 0 */
static void test_bat_read_00(void)
{
    uint8_t cmd[] = {0x2F, 0x00, 0x00};
    uint8_t buf[17] = {0};
    printf("\n=== Test 9: bat_read {2F,00,00} (mode 0) + read ===\n");
    
    pic_write(cmd, 3);
    printf("Sent bat_read mode 0x00\n");
    usleep(500000);
    pic_read_raw(buf);
    hex_dump("After", buf, 8);
}

/* Test 10: Full re-init */
static void test_full_reinit(void)
{
    /* NO {0x41} — triggers buzzer with no OFF command! */
    uint8_t wake[] = {0x33, 0x00, 0x01};
    uint8_t bat[] = {0x2F, 0x00, 0x02};
    uint8_t buf[17] = {0};

    printf("\n=== Test 10: Re-init (NO {0x41} — buzzer unsafe!) ===\n");


    printf("Before: ");
    pic_read_raw(buf);
    hex_dump("", buf, 8);

    printf("Sending WAKE {33,00,01}...\n");
    pic_write(wake, 3);
    usleep(5000);

    printf("Sending bat_read {2F,00,02}...\n");
    pic_write(bat, 3);
    usleep(500000);

    printf("Reading...\n");
    pic_read_raw(buf);
    hex_dump("After reinit", buf, 8);

    printf("Wait 2s and read again...\n");
    sleep(2);
    pic_read_raw(buf);
    hex_dump("After 2s", buf, 8);

    printf("Wait 5s and read again...\n");
    sleep(5);
    pic_read_raw(buf);
    hex_dump("After 5s", buf, 8);
}

int main(int argc, char **argv)
{
    int test = 0;

    lcd_fd = open("/dev/lcd", O_RDWR);
    if (lcd_fd < 0) { perror("/dev/lcd"); return 1; }

    palmbus_init();

    printf("PIC16 Battery Test Tool\n");
    printf("palmbus: %s\n", palmbus ? "OK" : "FAILED (no /dev/mem?)");
    printf("lcd_drv: ");
    {
        char ver[64] = {0};
        if (ioctl(lcd_fd, 7, ver) == 0) printf("%s\n", ver);
        else printf("unknown\n");
    }

    if (argc < 2) {
        printf("\nUsage: %s <test_number|all>\n", argv[0]);
        printf("  1  — Read cached PIC data\n");
        printf("  2  — bat_read {2F,00,01} + read\n");
        printf("  3  — bat_read {2F,00,02} + read\n");
        printf("  4  — WAKE {33,00,00} + bat_read + read\n");
        printf("  5  — WAKE {33,00,01} + bat_read + read\n");
        printf("  6  — Continuous read (10x, check changes)\n");
        printf("  7  — Buzzer ON/OFF\n");
        printf("  8  — Raw PIC read (17 bytes)\n");
        printf("  9  — bat_read {2F,00,00} (mode 0)\n");
        printf("  10 — Full re-init ({41}+wake+calib+bat_read)\n");
        printf("  all — Run tests 1-6,8,9\n");
        close(lcd_fd);
        return 0;
    }

    if (strcmp(argv[1], "all") == 0) {
        test_read();
        test_bat_read_01();
        test_bat_read_02();
        test_wake_bat_read();
        test_wake_count1();
        test_continuous();
        test_raw_read();
        test_bat_read_00();
    } else {
        test = atoi(argv[1]);
        switch (test) {
        case 1:  test_read(); break;
        case 2:  test_bat_read_01(); break;
        case 3:  test_bat_read_02(); break;
        case 4:  test_wake_bat_read(); break;
        case 5:  test_wake_count1(); break;
        case 6:  test_continuous(); break;
        case 7:  test_buzzer(); break;
        case 8:  test_raw_read(); break;
        case 9:  test_bat_read_00(); break;
        case 10: test_full_reinit(); break;
        default: printf("Unknown test: %d\n", test); break;
        }
    }

    close(lcd_fd);
    return 0;
}
