/*
 * pic_battery_final.c — THE definitive PIC battery test
 *
 * Combines the TWO proven working methods:
 *   WRITE: bit-bang GPIO sysfs (ACK on {0x2D},{0x2E},{0x2F})
 *   READ:  NEW SM0 manual mode (stable, doesn't corrupt PIC)
 *
 * NO SM0 auto mode anywhere!
 *
 * Usage:
 *   /tmp/pic_battery_final          — full init + 5 min monitor
 *   /tmp/pic_battery_final read     — just read (no init)
 *   /tmp/pic_battery_final stop     — stop melody {0x34,0x00,0x00}
 *   /tmp/pic_battery_final buzz     — buzzer test ON 1s OFF
 *
 * Build: zig cc -target mipsel-linux-musleabi -O2 -static -o pic_battery_final pic_battery_final.c
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <stdint.h>
#include <time.h>

#define PALMBUS  0x1E000000
#define MAP_SIZE 0x1000

#define RSTCTRL  0x034
#define GPIOMODE 0x060
#define SM0_CFG2 0x928
#define N_CTL0   0x940
#define N_CTL1   0x944
#define N_D0     0x950
#define N_D1     0x954

#define PIC 0x2A
#define SDA 515
#define SCL 516

static volatile uint32_t *B;
static uint32_t R(int o) { return B[o/4]; }
static void W(int o, uint32_t v) { B[o/4] = v; }

/* ========== GPIO sysfs bit-bang I2C ========== */

static void gex(int g) {
    char b[64]; FILE *f;
    f = fopen("/sys/class/gpio/export","w");
    if(f) { fprintf(f,"%d",g); fclose(f); }
    usleep(50000);
    snprintf(b,64,"/sys/class/gpio/gpio%d/direction",g);
    f = fopen(b,"w"); if(f) { fprintf(f,"in"); fclose(f); }
}
static void gun(int g) {
    FILE *f = fopen("/sys/class/gpio/unexport","w");
    if(f) { fprintf(f,"%d",g); fclose(f); }
}
static void gout(int g, int v) {
    char b[64]; FILE *f;
    snprintf(b,64,"/sys/class/gpio/gpio%d/direction",g);
    f = fopen(b,"w"); if(f) { fprintf(f, v?"high":"low"); fclose(f); }
}
static void gin(int g) {
    char b[64]; FILE *f;
    snprintf(b,64,"/sys/class/gpio/gpio%d/direction",g);
    f = fopen(b,"w"); if(f) { fprintf(f,"in"); fclose(f); }
}
static int gval(int g) {
    char b[64], v[4]={0}; FILE *f;
    snprintf(b,64,"/sys/class/gpio/gpio%d/value",g);
    f = fopen(b,"r"); if(!f) return -1;
    fread(v,1,1,f); fclose(f);
    return v[0]=='1' ? 1 : 0;
}

static void sda_lo(void) { gout(SDA,0); usleep(1); }
static void sda_hi(void) { gin(SDA); usleep(1); }
static void scl_lo(void) { gout(SCL,0); usleep(10); }
static void scl_hi(void) {
    gin(SCL);
    int t = 10000;
    while(!gval(SCL) && t--) usleep(1);  /* clock stretching */
    usleep(10);
}

static void i2c_start(void) { sda_hi(); scl_hi(); usleep(10); sda_lo(); usleep(10); scl_lo(); }
static void i2c_stop(void)  { sda_lo(); usleep(10); scl_hi(); usleep(10); sda_hi(); usleep(10); }

static int i2c_write_byte(uint8_t byte) {
    int i, ack;
    for(i=7; i>=0; i--) {
        if((byte>>i)&1) sda_hi(); else sda_lo();
        scl_hi(); scl_lo();
    }
    gin(SDA); usleep(5);
    scl_hi(); usleep(2);
    ack = gval(SDA);
    scl_lo(); usleep(5);
    return ack == 0;  /* ACK = SDA LOW */
}

static int bb_write(uint8_t *data, int len) {
    int i, ok = 1;
    uint32_t gm = R(GPIOMODE);

    W(GPIOMODE, gm | (1<<2));  /* I2C → GPIO */
    usleep(10000);
    gex(SDA); gex(SCL);
    usleep(50000);

    i2c_start();
    if(!i2c_write_byte((PIC<<1)|0)) { ok = 0; goto done; }
    for(i=0; i<len; i++) {
        if(!i2c_write_byte(data[i])) { ok = 0; goto done; }
    }
done:
    i2c_stop();
    gun(SDA); gun(SCL);
    W(GPIOMODE, gm);
    return ok;
}

/* ========== NEW SM0 Manual Mode Read ========== */

static int new_read(uint8_t *buf, int len) {
    int p;
    memset(buf, 0xFF, len);

    /* RSTCTRL reset — clean SM0 state */
    uint32_t r = R(RSTCTRL);
    W(RSTCTRL, r | 0x10000); usleep(10);
    W(RSTCTRL, r & ~0x10000); usleep(500);

    /* Setup NEW manual mode */
    W(N_CTL0, 0x01F3800F); usleep(10);
    W(SM0_CFG2, 0); usleep(10);  /* manual mode */

    /* Wait idle */
    for(p=0; p<5000; p++) if(!(R(N_CTL1)&1)) break;

    /* START */
    W(N_CTL1, 0x11);
    for(p=0; p<5000; p++) if(!(R(N_CTL1)&1)) break;

    /* Write addr+R */
    W(N_D0, (PIC<<1)|1);
    W(N_CTL1, 0x21);  /* WRITE|TRI|PGLEN(1) */
    for(p=0; p<5000; p++) if(!(R(N_CTL1)&1)) break;

    /* Check ACK */
    if(!(R(N_CTL1) & (1<<16))) {
        W(N_CTL1, 0x31); /* STOP */
        for(p=0; p<5000; p++) if(!(R(N_CTL1)&1)) break;
        return -1;  /* NACK */
    }

    /* Read chunks */
    int off=0, rem=len;
    while(rem > 0) {
        int ch = (rem > 8) ? 8 : rem;
        uint32_t cmd = (rem > 8) ? 0x50 : 0x40;  /* READ or READ_LAST */
        uint32_t d0, d1;

        W(N_CTL1, cmd | 1 | ((ch-1)<<8));
        for(p=0; p<5000; p++) if(!(R(N_CTL1)&1)) break;

        d0 = R(N_D0); d1 = R(N_D1);
        memcpy(&buf[off], &d0, ch>4 ? 4 : ch);
        if(ch > 4) memcpy(&buf[off+4], &d1, ch-4);
        off += ch; rem -= ch;
    }

    /* STOP */
    W(N_CTL1, 0x31);
    for(p=0; p<5000; p++) if(!(R(N_CTL1)&1)) break;

    return 0;
}

/* ========== NEW SM0 Manual Mode Write ========== */

static int new_write(uint8_t *data, int len) {
    int p;

    uint32_t r = R(RSTCTRL);
    W(RSTCTRL, r | 0x10000); usleep(10);
    W(RSTCTRL, r & ~0x10000); usleep(500);

    W(N_CTL0, 0x01F3800F); usleep(10);
    W(SM0_CFG2, 0); usleep(10);
    for(p=0; p<5000; p++) if(!(R(N_CTL1)&1)) break;

    /* START */
    W(N_CTL1, 0x11);
    for(p=0; p<5000; p++) if(!(R(N_CTL1)&1)) break;

    /* Write addr+W */
    W(N_D0, (PIC<<1)|0);
    W(N_CTL1, 0x21);
    for(p=0; p<5000; p++) if(!(R(N_CTL1)&1)) break;
    if(!(R(N_CTL1) & (1<<16))) {
        W(N_CTL1, 0x31); for(p=0; p<5000; p++) if(!(R(N_CTL1)&1)) break;
        return -1;
    }

    /* Write data in chunks */
    int off = 0;
    while(off < len) {
        int ch = (len-off > 8) ? 8 : (len-off);
        uint32_t d0=0, d1=0;
        memcpy(&d0, &data[off], ch>4 ? 4 : ch);
        if(ch > 4) memcpy(&d1, &data[off+4], ch-4);
        W(N_D0, d0);
        if(ch > 4) W(N_D1, d1);
        W(N_CTL1, 0x20 | 1 | ((ch-1)<<8));
        for(p=0; p<5000; p++) if(!(R(N_CTL1)&1)) break;
        off += ch;
    }

    W(N_CTL1, 0x31);
    for(p=0; p<5000; p++) if(!(R(N_CTL1)&1)) break;
    return 0;
}

/* ========== Helpers ========== */

static void hex8(const char *label, uint8_t *d) {
    printf("%-10s %02x %02x %02x %02x %02x %02x %02x %02x\n",
           label, d[0],d[1],d[2],d[3],d[4],d[5],d[6],d[7]);
}

static void unbind_i2c(void) {
    FILE *f = fopen("/sys/bus/platform/drivers/i2c-mt7621/unbind","w");
    if(f) { fprintf(f,"1e000900.i2c"); fclose(f); }
    usleep(100000);
}

static void bind_i2c(void) {
    FILE *f = fopen("/sys/bus/platform/drivers/i2c-mt7621/bind","w");
    if(f) { fprintf(f,"1e000900.i2c"); fclose(f); }
}

/* ========== Commands ========== */

static void cmd_init_and_monitor(void) {
    uint8_t buf[8];
    uint8_t tab1[] = {0x2D};
    uint8_t tab2[] = {0x2E};
    uint8_t bat[]  = {0x2F, 0x00, 0x01};
    uint8_t boff[] = {0x34, 0x00, 0x00};

    printf("=== PIC BATTERY FINAL ===\n\n");

    /* Phase 1: Read BEFORE init */
    printf("Phase 1: Read before init\n");
    new_read(buf, 8);
    hex8("before:", buf);

    /* Phase 2: Init PIC via bit-bang */
    printf("\nPhase 2: Init via bit-bang\n");
    printf("  {0x2D} Table1: %s\n", bb_write(tab1,1) ? "ACK" : "NACK");
    usleep(5000);
    printf("  {0x2E} Table2: %s\n", bb_write(tab2,1) ? "ACK" : "NACK");
    usleep(5000);
    printf("  {0x2F} bat_read: %s\n", bb_write(bat,3) ? "ACK" : "NACK");
    sleep(1);

    /* Phase 3: Stop melody */
    printf("\nPhase 3: Stop melody\n");
    printf("  {0x34,00,00}: %s\n", bb_write(boff,3) ? "ACK" : "NACK");
    sleep(1);

    /* Phase 4: Read after init */
    printf("\nPhase 4: Read after init\n");
    new_read(buf, 8);
    hex8("after:", buf);

    /* Phase 5: Also try NEW mode write bat_read */
    printf("\nPhase 5: NEW mode write bat_read\n");
    int rc = new_write(bat, 3);
    printf("  result: %d (%s)\n", rc, rc==0 ? "OK" : "FAIL");
    usleep(500000);
    new_read(buf, 8);
    hex8("new_wr:", buf);

    /* Phase 6: Monitor 5 min */
    printf("\nPhase 6: Monitor 60 reads × 5s = 5 min\n");
    printf("  (bat_read every 30s via NEW mode write)\n\n");

    uint8_t prev[8] = {0};
    for(int i=0; i<60; i++) {
        new_read(buf, 8);

        int allff=1, changed=0;
        for(int j=0;j<8;j++) if(buf[j]!=0xFF) allff=0;
        if(memcmp(buf,prev,8)!=0) changed=1;

        if(!allff) {
            int adc = (buf[2]<<8) | buf[3];
            int adc2 = (buf[0]<<8) | buf[1];
            int adc3 = (buf[4]<<8) | buf[5];
            const char *lvl = (adc<401)?"CRIT":(adc<542)?"LOW":"OK";
            printf("[%3d] %02x %02x %02x %02x %02x %02x %02x %02x  "
                   "adc23=%d(%s) adc01=%d adc45=%d%s\n",
                   i, buf[0],buf[1],buf[2],buf[3],buf[4],buf[5],buf[6],buf[7],
                   adc, lvl, adc2, adc3,
                   changed ? "  *** CHANGED" : "");
            memcpy(prev, buf, 8);
        } else {
            printf("[%3d] ff ff ff ff ff ff ff ff\n", i);
        }

        /* Send bat_read every 30s (6 reads × 5s) */
        if(i > 0 && i % 6 == 0) {
            printf("[%3d] --- bat_read (NEW mode) ---\n", i);
            new_write(bat, 3);
            usleep(500000);
        }

        sleep(5);
    }
}

static void cmd_read(void) {
    uint8_t buf[8];
    new_read(buf, 8);
    hex8("read:", buf);
    printf("D0=0x%08X D1=0x%08X\n", R(N_D0), R(N_D1));
}

static void cmd_stop(void) {
    uint8_t boff[] = {0x34, 0x00, 0x00};
    printf("Buzzer OFF: %s\n", bb_write(boff,3) ? "ACK" : "NACK");
}

static void cmd_buzz(void) {
    uint8_t bon[]  = {0x34, 0x00, 0x03};
    uint8_t boff[] = {0x34, 0x00, 0x00};
    printf("Buzzer ON: %s\n", bb_write(bon,3) ? "ACK" : "NACK");
    sleep(1);
    printf("Buzzer OFF: %s\n", bb_write(boff,3) ? "ACK" : "NACK");
}

/* ========== Main ========== */

int main(int argc, char **argv) {
    int fd = open("/dev/mem", O_RDWR|O_SYNC);
    if(fd < 0) { perror("/dev/mem"); return 1; }
    B = mmap(NULL, MAP_SIZE, PROT_READ|PROT_WRITE, MAP_SHARED, fd, PALMBUS);
    if(B == MAP_FAILED) { perror("mmap"); return 1; }

    unbind_i2c();

    if(argc < 2)          cmd_init_and_monitor();
    else if(!strcmp(argv[1],"read"))  cmd_read();
    else if(!strcmp(argv[1],"stop"))  cmd_stop();
    else if(!strcmp(argv[1],"buzz"))  cmd_buzz();
    else printf("Usage: %s [read|stop|buzz]\n", argv[0]);

    bind_i2c();
    munmap((void*)B, MAP_SIZE);
    close(fd);
    return 0;
}
