/*
 * pic_bruteforce.c — Try EVERY possible I2C read method for PIC
 * No power cycle needed. Run on live system.
 * Build: zig cc -target mipsel-linux-musleabi -O2 -static -o pic_bruteforce pic_bruteforce.c
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

/* Register offsets */
#define RSTCTRL   0x034
#define GPIOMODE  0x060

/* OLD SM0 (auto mode) */
#define SM0_CFG    0x900
#define SM0_DATA   0x908
#define SM0_DOUT   0x910
#define SM0_DIN    0x914
#define SM0_POLL   0x918
#define SM0_STAT   0x91C
#define SM0_START  0x920
#define SM0_CFG2   0x928
#define SM0_CTL0   0x940  /* = NEW_CTL0 */

/* NEW SM0 (manual mode) */
#define N_CTL0  0x940
#define N_CTL1  0x944
#define N_D0    0x950
#define N_D1    0x954

#define PIC  0x2A

static volatile uint32_t *B;
static uint32_t R(int o) { return B[o/4]; }
static void W(int o, uint32_t v) { B[o/4] = v; }

static void rst_sm0(void) {
    uint32_t r = R(RSTCTRL);
    W(RSTCTRL, r | 0x10000); usleep(10);
    W(RSTCTRL, r & ~0x10000); usleep(500);
}

static void hex8(const char *label, uint8_t *d) {
    printf("%-20s %02x %02x %02x %02x %02x %02x %02x %02x", label,
           d[0],d[1],d[2],d[3],d[4],d[5],d[6],d[7]);
    int allff=1, all00=1;
    for(int i=0;i<8;i++) { if(d[i]!=0xff) allff=0; if(d[i]!=0x00) all00=0; }
    if(!allff && !all00) printf("  *** DATA ***");
    printf("\n");
}

/* Method 1: NEW SM0 manual mode — exactly like kernel i2c-mt7621 driver */
static void read_new_manual(uint8_t *buf, int len, uint32_t ctl0_val) {
    int i, p;
    memset(buf, 0xFF, len);

    W(N_CTL0, ctl0_val); usleep(10);
    W(SM0_CFG2, 0x00); usleep(10);  /* manual mode */

    /* Wait idle */
    for(p=0;p<5000;p++) { if(!(R(N_CTL1)&1)) break; usleep(1); }

    /* START */
    W(N_CTL1, 0x10|0x01); /* START|TRI */
    for(p=0;p<5000;p++) { if(!(R(N_CTL1)&1)) break; usleep(1); }

    /* Write addr+R */
    W(N_D0, (PIC<<1)|1);
    W(N_CTL1, 0x20|0x01|(0<<8)); /* WRITE|TRI|PGLEN(1) */
    for(p=0;p<5000;p++) { if(!(R(N_CTL1)&1)) break; usleep(1); }

    /* Read in chunks */
    int off=0, rem=len;
    while(rem>0) {
        int chunk = (rem>8)?8:rem;
        uint32_t cmd = (rem>8)?0x50:0x40; /* READ or READ_LAST */
        uint32_t d0,d1;
        W(N_CTL1, cmd|0x01|((chunk-1)<<8));
        for(p=0;p<5000;p++) { if(!(R(N_CTL1)&1)) break; usleep(1); }
        d0=R(N_D0); d1=R(N_D1);
        memcpy(&buf[off], &d0, chunk>4?4:chunk);
        if(chunk>4) memcpy(&buf[off+4], &d1, chunk-4);
        off+=chunk; rem-=chunk;
    }

    /* STOP */
    W(N_CTL1, 0x30|0x01);
    for(p=0;p<5000;p++) { if(!(R(N_CTL1)&1)) break; usleep(1); }
}

/* Method 2: OLD SM0 auto mode read */
static void read_old_auto(uint8_t *buf, int len, uint32_t ctl0_val) {
    int i, p;
    memset(buf, 0xFF, len);

    W(SM0_CTL0, ctl0_val); usleep(10);
    W(SM0_CFG2, 0x01); usleep(10); /* auto mode ON */
    W(SM0_DATA, PIC);
    W(SM0_START, len-1);
    W(SM0_STAT, 1); /* READ */

    for(i=0;i<len;i++) {
        for(p=0;p<100000;p++) { if(R(SM0_POLL)&0x04) break; }
        usleep(10);
        buf[i] = R(SM0_DIN) & 0xFF;
    }
}

/* Method 3: D0/D1 direct read (cached from last HW operation) */
static void read_d0d1(uint8_t *buf) {
    uint32_t d0 = R(N_D0), d1 = R(N_D1);
    memcpy(buf, &d0, 4);
    memcpy(buf+4, &d1, 4);
}

/* Method 4: Single i2cget style (NEW manual, 1 byte only) */
static void read_single_new(uint8_t *val, uint32_t ctl0_val) {
    int p;
    W(N_CTL0, ctl0_val); usleep(10);
    W(SM0_CFG2, 0x00); usleep(10);
    for(p=0;p<5000;p++) { if(!(R(N_CTL1)&1)) break; usleep(1); }
    W(N_CTL1, 0x10|0x01);
    for(p=0;p<5000;p++) { if(!(R(N_CTL1)&1)) break; usleep(1); }
    W(N_D0, (PIC<<1)|1);
    W(N_CTL1, 0x20|0x01|(0<<8));
    for(p=0;p<5000;p++) { if(!(R(N_CTL1)&1)) break; usleep(1); }
    /* Read 1 byte with NACK */
    W(N_CTL1, 0x40|0x01|(0<<8)); /* READ_LAST|TRI|PGLEN(1) */
    for(p=0;p<5000;p++) { if(!(R(N_CTL1)&1)) break; usleep(1); }
    *val = R(N_D0) & 0xFF;
    W(N_CTL1, 0x30|0x01);
    for(p=0;p<5000;p++) { if(!(R(N_CTL1)&1)) break; usleep(1); }
}

/* Method 5: OLD auto single byte read */
static void read_old_single(uint8_t *val) {
    int p;
    W(SM0_CTL0, 0x90640042); usleep(10);
    W(SM0_CFG2, 0x01); usleep(10);
    W(SM0_DATA, PIC);
    W(SM0_START, 0); /* count-1 = 0 for 1 byte */
    W(SM0_STAT, 1);
    for(p=0;p<100000;p++) { if(R(SM0_POLL)&0x04) break; }
    usleep(10);
    *val = R(SM0_DIN) & 0xFF;
}

int main(void) {
    int fd = open("/dev/mem", O_RDWR|O_SYNC);
    if(fd<0) { perror("/dev/mem"); return 1; }
    B = mmap(NULL, MAP_SIZE, PROT_READ|PROT_WRITE, MAP_SHARED, fd, PALMBUS);
    if(B==MAP_FAILED) { perror("mmap"); return 1; }

    /* Unbind i2c */
    FILE *f = fopen("/sys/bus/platform/drivers/i2c-mt7621/unbind","w");
    if(f) { fprintf(f,"1e000900.i2c"); fclose(f); }
    usleep(100000);

    uint8_t buf[8];
    uint8_t val;
    uint32_t ctl0_variants[] = {0x01F3800F, 0x8064800E, 0x90640042, 0x80644002};
    const char *ctl0_names[] = {"kernel_default", "hw_modified", "stock_3.10", "stock_noOD"};

    printf("=== PIC BRUTEFORCE READ — all methods ===\n\n");

    for(int round=0; round<3; round++) {
        printf("--- Round %d (RSTCTRL reset) ---\n", round);
        rst_sm0();

        /* D0/D1 direct */
        read_d0d1(buf);
        hex8("D0/D1 direct:", buf);

        /* NEW manual mode with each CTL0 */
        for(int c=0;c<4;c++) {
            rst_sm0();
            read_new_manual(buf, 8, ctl0_variants[c]);
            char label[64];
            snprintf(label, sizeof(label), "NEW_%s:", ctl0_names[c]);
            hex8(label, buf);
        }

        /* OLD auto mode with each CTL0 */
        for(int c=0;c<4;c++) {
            rst_sm0();
            read_old_auto(buf, 8, ctl0_variants[c]);
            char label[64];
            snprintf(label, sizeof(label), "OLD_%s:", ctl0_names[c]);
            hex8(label, buf);
        }

        /* Single byte reads */
        rst_sm0();
        read_single_new(&val, 0x01F3800F);
        printf("SINGLE_NEW_kernel:   %02x\n", val);

        rst_sm0();
        read_single_new(&val, 0x8064800E);
        printf("SINGLE_NEW_hwmod:    %02x\n", val);

        rst_sm0();
        read_old_single(&val);
        printf("SINGLE_OLD_stock:    %02x\n", val);

        /* D0/D1 after all operations */
        read_d0d1(buf);
        hex8("D0/D1 after:", buf);

        printf("\n");
    }

    /* Rebind */
    f = fopen("/sys/bus/platform/drivers/i2c-mt7621/bind","w");
    if(f) { fprintf(f,"1e000900.i2c"); fclose(f); }

    munmap((void*)B, MAP_SIZE);
    close(fd);
    return 0;
}
