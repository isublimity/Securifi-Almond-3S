/*
 * pic_newmode.c — Test NEW SM0 manual mode for BOTH write AND read
 * Key insight: NEW manual mode doesn't corrupt PIC!
 * OLD auto mode does. Let's try write through NEW mode too.
 *
 * From kernel i2c-mt7621 driver:
 *   START → WRITE addr → check ACK → WRITE data → check ACK → STOP
 *   Each operation: write to CTL1, wait TRI bit clear
 *
 * PIC NACKed write addr in manual mode before, but maybe
 * with RSTCTRL reset + correct CTL0 it will work.
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
#define RSTCTRL  0x034
#define SM0_CFG2 0x928
#define N_CTL0   0x940
#define N_CTL1   0x944
#define N_D0     0x950
#define N_D1     0x954
#define PIC      0x2A

static volatile uint32_t *B;
static uint32_t R(int o){return B[o/4];}
static void W(int o,uint32_t v){B[o/4]=v;}

static void rst(void){uint32_t r=R(RSTCTRL);W(RSTCTRL,r|0x10000);usleep(10);W(RSTCTRL,r&~0x10000);usleep(500);}

static int wait_idle(void){for(int p=0;p<10000;p++){if(!(R(N_CTL1)&1))return 1;usleep(1);}return 0;}

/* NEW manual mode: START→addr+W→data bytes→STOP */
static int new_write(uint32_t ctl0, uint8_t *data, int len) {
    int i;
    uint32_t ack;

    W(N_CTL0, ctl0); usleep(10);
    W(SM0_CFG2, 0); usleep(10); /* manual mode */
    wait_idle();

    /* START */
    W(N_CTL1, 0x10|0x01);
    if(!wait_idle()) { printf("  START timeout\n"); return -1; }

    /* Write address byte (write mode) — PGLEN=1 */
    W(N_D0, (PIC<<1)|0);
    W(N_CTL1, 0x20|0x01|(0<<8)); /* WRITE|TRI|PGLEN(1) */
    if(!wait_idle()) { printf("  ADDR timeout\n"); W(N_CTL1,0x31); wait_idle(); return -2; }

    /* Check ACK — bit 16 */
    ack = R(N_CTL1);
    printf("  addr+W ACK: CTL1=0x%08X ack=%s\n", ack, (ack&(1<<16))?"YES":"NO");

    if(!(ack & (1<<16))) {
        W(N_CTL1, 0x31); wait_idle(); /* STOP */
        return -3; /* NACK */
    }

    /* Write data bytes — up to 8 at a time via D0+D1 */
    int off = 0;
    while(off < len) {
        int chunk = (len-off > 8) ? 8 : (len-off);
        uint32_t d0=0, d1=0;
        memcpy(&d0, &data[off], chunk>4?4:chunk);
        if(chunk>4) memcpy(&d1, &data[off+4], chunk-4);
        W(N_D0, d0);
        if(chunk>4) W(N_D1, d1);
        W(N_CTL1, 0x20|0x01|((chunk-1)<<8)); /* WRITE|TRI|PGLEN(chunk) */
        if(!wait_idle()) { printf("  DATA timeout at %d\n", off); break; }

        ack = R(N_CTL1);
        int ack_ok = 1;
        for(i=0;i<chunk;i++) {
            if(!(ack & (1<<(16+i)))) { ack_ok = 0; break; }
        }
        if(!ack_ok) {
            printf("  DATA NACK at byte %d, CTL1=0x%08X\n", off+i, ack);
            break;
        }
        off += chunk;
    }

    /* STOP */
    W(N_CTL1, 0x31);
    wait_idle();

    return (off >= len) ? 0 : -4;
}

/* NEW manual mode read */
static int new_read(uint32_t ctl0, uint8_t *buf, int len) {
    int p;
    memset(buf, 0xFF, len);
    W(N_CTL0, ctl0); usleep(10);
    W(SM0_CFG2, 0); usleep(10);
    wait_idle();
    W(N_CTL1, 0x11);
    if(!wait_idle()) return -1;
    W(N_D0, (PIC<<1)|1);
    W(N_CTL1, 0x21);
    if(!wait_idle()) { W(N_CTL1,0x31); wait_idle(); return -2; }

    uint32_t ack = R(N_CTL1);
    if(!(ack & (1<<16))) { W(N_CTL1,0x31); wait_idle(); return -3; }

    int off=0, rem=len;
    while(rem>0) {
        int ch=(rem>8)?8:rem;
        uint32_t cmd=(rem>8)?0x50:0x40;
        W(N_CTL1, cmd|1|((ch-1)<<8));
        if(!wait_idle()) break;
        uint32_t d0=R(N_D0), d1=R(N_D1);
        memcpy(&buf[off], &d0, ch>4?4:ch);
        if(ch>4) memcpy(&buf[off+4], &d1, ch-4);
        off+=ch; rem-=ch;
    }
    W(N_CTL1, 0x31); wait_idle();
    return 0;
}

int main(void) {
    int fd=open("/dev/mem",O_RDWR|O_SYNC);
    if(fd<0){perror("/dev/mem");return 1;}
    B=mmap(NULL,MAP_SIZE,PROT_READ|PROT_WRITE,MAP_SHARED,fd,PALMBUS);
    if(B==MAP_FAILED){perror("mmap");return 1;}

    FILE *f=fopen("/sys/bus/platform/drivers/i2c-mt7621/unbind","w");
    if(f){fprintf(f,"1e000900.i2c");fclose(f);}
    usleep(100000);

    uint8_t buf[8];
    uint32_t ctls[] = {0x01F3800F, 0x8064800E, 0x90640042};
    const char *cn[] = {"kern", "hwmod", "stock"};

    printf("=== NEW SM0 manual mode WRITE test ===\n\n");

    for(int c=0; c<3; c++) {
        printf("--- CTL0=%s (0x%08X) ---\n", cn[c], ctls[c]);

        /* RSTCTRL reset */
        rst();

        /* Try write bat_read {0x2F, 0x00, 0x01} */
        uint8_t bat[] = {0x2F, 0x00, 0x01};
        printf("WRITE bat_read:\n");
        int rc = new_write(ctls[c], bat, 3);
        printf("  result: %d\n", rc);

        if(rc == 0) {
            /* Write succeeded! Wait and read */
            printf("WRITE OK! Reading after 500ms...\n");
            usleep(500000);
            rst();
            new_read(ctls[c], buf, 8);
            printf("  READ: %02x %02x %02x %02x %02x %02x %02x %02x\n",
                   buf[0],buf[1],buf[2],buf[3],buf[4],buf[5],buf[6],buf[7]);
        }

        /* Try write WAKE {0x33, 0x00, 0x00} */
        rst();
        uint8_t wake[] = {0x33, 0x00, 0x00};
        printf("WRITE wake:\n");
        rc = new_write(ctls[c], wake, 3);
        printf("  result: %d\n\n", rc);
    }

    /* Now: continuous read loop — 20 iterations, show only valid */
    printf("=== READ LOOP (20 iterations) ===\n");
    for(int i=0; i<20; i++) {
        rst();
        for(int c=0; c<3; c++) {
            new_read(ctls[c], buf, 8);
            int allff=1;
            for(int j=0;j<8;j++) if(buf[j]!=0xFF) allff=0;
            if(!allff) {
                printf("[%2d] %s: %02x %02x %02x %02x %02x %02x %02x %02x\n",
                       i, cn[c], buf[0],buf[1],buf[2],buf[3],buf[4],buf[5],buf[6],buf[7]);
            }
            rst();
        }
        usleep(200000);
    }

    f=fopen("/sys/bus/platform/drivers/i2c-mt7621/bind","w");
    if(f){fprintf(f,"1e000900.i2c");fclose(f);}
    munmap((void*)B,MAP_SIZE); close(fd);
    return 0;
}
