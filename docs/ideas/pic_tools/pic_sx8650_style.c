/*
 * pic_sx8650_style.c — Read PIC using EXACT SX8650 touch protocol
 *
 * SX8650 touch read WORKS on this hardware.
 * Key difference: SM0_CTL1 = 2 (write+read mode), not 0 or 1!
 *
 * SX8650 protocol from IDA:
 *   CTL0=addr, DATA=write_cmd, CTL1=2, START=0 → udelay(150)
 *   DATA=read_cmd, CTL1=2, START=0 → CFG=0xFA → START=0 → START=1 → CTL1=1
 *   DATAIN → byte1, DATAIN → byte2
 *   START=0 → START=1 → 0x940=0x8064800E
 *
 * For PIC: addr=0x2A, write_cmd=0x2F, read result bytes
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <stdint.h>

#define PALMBUS  0x1E000000
#define MAP_SIZE 0x1000

#define SM0_CFG   0x900
#define SM0_CTL0  0x908  /* slave address */
#define SM0_DATA  0x910  /* write data */
#define SM0_DIN   0x914  /* read data */
#define SM0_POLL  0x918  /* status */
#define SM0_CTL1  0x91C  /* mode: 0=write, 1=read, 2=write+read */
#define SM0_START 0x920
#define SM0_CFG2  0x928
#define SM0_RESET 0x940

#define PIC  0x2A
#define TOUCH 0x48

static volatile uint32_t *B;
static uint32_t R(int o){return B[o/4];}
static void W(int o,uint32_t v){B[o/4]=v;}

/* Exact SX8650 read protocol — but with PIC address */
static void pic_read_sx_style(uint8_t write_cmd, uint8_t read_cmd,
                               uint8_t *hi, uint8_t *lo) {
    /* Phase 1: Write command (like SX8650 SELECT) */
    W(SM0_CTL0, PIC);          /* slave address = 0x2A */
    W(SM0_START, 0);           /* reset */
    usleep(150);
    W(SM0_DATA, write_cmd);    /* write command byte */
    W(SM0_CTL1, 2);            /* MODE 2 = write+read! */
    usleep(150);
    W(SM0_START, 0);
    usleep(150);

    /* Phase 2: Read command */
    W(SM0_DATA, read_cmd);     /* read command byte */
    W(SM0_CTL1, 2);            /* write+read */
    usleep(150);

    /* Phase 3: Config + Start read */
    W(SM0_CFG, 0xFA);          /* clock config */
    W(SM0_START, 0);
    usleep(150);
    W(SM0_START, 1);           /* START */
    usleep(150);
    W(SM0_START, 1);           /* double START (from SX8650) */
    usleep(150);

    /* Phase 4: Read mode + get data */
    W(SM0_CTL1, 1);            /* read mode */
    usleep(150);
    *hi = R(SM0_DIN) & 0xFF;   /* byte 1 */
    usleep(150);
    *lo = R(SM0_DIN) & 0xFF;   /* byte 2 */
    usleep(150);

    /* Phase 5: Stop + reset */
    W(SM0_START, 0);
    usleep(150);
    W(SM0_START, 1);
    W(SM0_RESET, 0x8064800E);  /* reset SM0 */
}

/* Multi-byte read using PIC_I2C_READ protocol from IDA */
static void pic_read_ida_style(uint8_t *buf, int count) {
    int i;
    memset(buf, 0xFF, count);

    /* From PIC_I2C_READ: SM0_START=count-1, SM0_CTL1=1 */
    W(SM0_START, count - 1);
    W(SM0_CTL1, 1);  /* read mode */

    for(i = 0; i < count; i++) {
        int timeout = 100000;
        /* Poll SM0_POLL bit 0x04 (read done) */
        while(timeout-- > 0) {
            if(R(SM0_POLL) & 0x04) break;
        }
        usleep(10);
        buf[i] = R(SM0_DIN) & 0xFF;
    }
}

/* SX8650-style raw I2C: write cmd byte, read response */
static void raw_write_read(uint8_t addr, uint8_t cmd, uint8_t *resp, int resp_len) {
    int i;

    /* Write phase */
    W(SM0_CTL0, addr);
    W(SM0_START, 0); usleep(150);
    W(SM0_DATA, cmd);
    W(SM0_CTL1, 0); /* write */
    usleep(150);
    W(SM0_START, 0); usleep(150);

    /* Transition to read */
    W(SM0_CTL1, 2); /* write+read */
    usleep(150);
    W(SM0_CFG, 0xFA);
    W(SM0_START, 0); usleep(150);
    W(SM0_START, 1); usleep(150);
    W(SM0_CTL1, 1); /* read */
    usleep(150);

    /* Read bytes */
    for(i = 0; i < resp_len; i++) {
        resp[i] = R(SM0_DIN) & 0xFF;
        usleep(150);
    }

    /* Stop */
    W(SM0_START, 0); usleep(150);
    W(SM0_START, 1);
    W(SM0_RESET, 0x8064800E);
}

int main(void) {
    int fd = open("/dev/mem", O_RDWR|O_SYNC);
    if(fd < 0) { perror("/dev/mem"); return 1; }
    B = mmap(NULL, MAP_SIZE, PROT_READ|PROT_WRITE, MAP_SHARED, fd, PALMBUS);
    if(B == MAP_FAILED) { perror("mmap"); return 1; }

    FILE *f = fopen("/sys/bus/platform/drivers/i2c-mt7621/unbind","w");
    if(f) { fprintf(f,"1e000900.i2c"); fclose(f); }
    usleep(100000);

    uint8_t hi, lo;
    uint8_t buf[8];
    int i;

    printf("=== PIC read using SX8650 protocol ===\n\n");

    /* Test 1: SX8650 touch read (MUST work — baseline) */
    printf("--- Test 1: SX8650 touch read (baseline) ---\n");
    W(SM0_RESET, 0x90640042); usleep(10);
    W(SM0_CFG2, 1); usleep(10);
    pic_read_sx_style(0x80, 0x90, &hi, &lo);  /* SX8650 SELECT(X)=0x80, READ=0x90 */
    printf("  SX8650 X: hi=0x%02X lo=0x%02X val=%d\n", hi, lo, ((hi&0xF)<<8)|lo);

    W(SM0_RESET, 0x90640042); usleep(10);
    W(SM0_CFG2, 1); usleep(10);
    pic_read_sx_style(0x83, 0x93, &hi, &lo);  /* SX8650 SELECT(Y)=0x83, READ=0x93 */
    printf("  SX8650 Y: hi=0x%02X lo=0x%02X val=%d\n\n", hi, lo, ((hi&0xF)<<8)|lo);

    /* Test 2: PIC read with SX8650 protocol */
    printf("--- Test 2: PIC read with SX8650 protocol ---\n");
    for(i = 0; i < 5; i++) {
        W(SM0_RESET, 0x90640042); usleep(10);
        W(SM0_CFG2, 1); usleep(10);
        pic_read_sx_style(0x2F, 0x00, &hi, &lo);
        printf("  [%d] cmd=0x2F: hi=0x%02X lo=0x%02X val=%d\n", i, hi, lo, ((hi&0xF)<<8)|lo);
    }

    /* Test 3: PIC raw write+read */
    printf("\n--- Test 3: PIC raw write(0x2F)+read ---\n");
    for(i = 0; i < 5; i++) {
        W(SM0_RESET, 0x90640042); usleep(10);
        W(SM0_CFG2, 1); usleep(10);
        raw_write_read(PIC, 0x2F, buf, 4);
        printf("  [%d] %02x %02x %02x %02x\n", i, buf[0], buf[1], buf[2], buf[3]);
    }

    /* Test 4: PIC_I2C_READ style (IDA protocol) after write */
    printf("\n--- Test 4: PIC_I2C_READ style (count-1, poll 0x04) ---\n");
    for(i = 0; i < 5; i++) {
        W(SM0_RESET, 0x90640042); usleep(10);
        W(SM0_CFG2, 1); usleep(10);
        W(SM0_CTL0, PIC);
        pic_read_ida_style(buf, 8);
        printf("  [%d] %02x %02x %02x %02x %02x %02x %02x %02x\n",
               i, buf[0],buf[1],buf[2],buf[3],buf[4],buf[5],buf[6],buf[7]);
    }

    /* Test 5: CTL1=2 variations */
    printf("\n--- Test 5: CTL1=2 (write+read mode) variations ---\n");
    uint8_t cmds[] = {0x2F, 0x33, 0x2D, 0x2E, 0x34};
    const char *names[] = {"bat_read", "wake", "table1", "table2", "buzzer"};
    for(i = 0; i < 5; i++) {
        W(SM0_RESET, 0x90640042); usleep(10);
        W(SM0_CFG2, 1); usleep(10);
        W(SM0_CTL0, PIC);
        W(SM0_START, 0); usleep(150);
        W(SM0_DATA, cmds[i]);
        W(SM0_CTL1, 2); usleep(150);  /* write+read! */
        W(SM0_START, 0); usleep(150);
        W(SM0_CFG, 0xFA);
        W(SM0_START, 0); usleep(150);
        W(SM0_START, 1); usleep(150);
        W(SM0_CTL1, 1); usleep(150);
        hi = R(SM0_DIN) & 0xFF; usleep(150);
        lo = R(SM0_DIN) & 0xFF; usleep(150);
        W(SM0_START, 0); usleep(150);
        W(SM0_START, 1);
        W(SM0_RESET, 0x8064800E);
        printf("  %-10s cmd=0x%02X: hi=0x%02X lo=0x%02X\n", names[i], cmds[i], hi, lo);
    }

    f = fopen("/sys/bus/platform/drivers/i2c-mt7621/bind","w");
    if(f) { fprintf(f,"1e000900.i2c"); fclose(f); }
    munmap((void*)B, MAP_SIZE); close(fd);
    return 0;
}
