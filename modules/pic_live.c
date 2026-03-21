/*
 * pic_live.c — Find working LIVE PIC read method
 * Tries all combinations in a loop until data changes
 * NO power cycle needed!
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

#define RSTCTRL   0x034
#define SM0_DATA  0x908
#define SM0_DOUT  0x910
#define SM0_DIN   0x914
#define SM0_POLL  0x918
#define SM0_STAT  0x91C
#define SM0_START 0x920
#define SM0_CFG2  0x928
#define SM0_CTL0  0x940
#define N_CTL1    0x944
#define N_D0      0x950
#define N_D1      0x954
#define GPIOMODE  0x060

#define PIC 0x2A

static volatile uint32_t *B;
static uint32_t R(int o) { return B[o/4]; }
static void W(int o, uint32_t v) { B[o/4] = v; }

static void rst(void) {
    uint32_t r=R(RSTCTRL);
    W(RSTCTRL,r|0x10000); usleep(10);
    W(RSTCTRL,r&~0x10000); usleep(500);
}

/* NEW manual mode read */
static int read_new(uint8_t *buf, int len, uint32_t ctl0) {
    int p;
    memset(buf,0xFF,len);
    W(SM0_CTL0,ctl0); usleep(10);
    W(SM0_CFG2,0); usleep(10);
    for(p=0;p<5000;p++) if(!(R(N_CTL1)&1)) break;
    W(N_CTL1,0x11); /* START|TRI */
    for(p=0;p<5000;p++) if(!(R(N_CTL1)&1)) break;
    W(N_D0,(PIC<<1)|1);
    W(N_CTL1,0x21); /* WRITE|TRI|PGLEN(1) */
    for(p=0;p<5000;p++) if(!(R(N_CTL1)&1)) break;
    /* Check ACK */
    uint32_t ack = R(N_CTL1);
    if(!(ack & (1<<16))) { /* NACK */
        W(N_CTL1,0x31); for(p=0;p<5000;p++) if(!(R(N_CTL1)&1)) break;
        return -1;
    }
    int off=0,rem=len;
    while(rem>0) {
        int ch=(rem>8)?8:rem;
        uint32_t cmd=(rem>8)?0x50:0x40;
        W(N_CTL1,cmd|1|((ch-1)<<8));
        for(p=0;p<5000;p++) if(!(R(N_CTL1)&1)) break;
        uint32_t d0=R(N_D0),d1=R(N_D1);
        memcpy(&buf[off],&d0,ch>4?4:ch);
        if(ch>4) memcpy(&buf[off+4],&d1,ch-4);
        off+=ch; rem-=ch;
    }
    W(N_CTL1,0x31); for(p=0;p<5000;p++) if(!(R(N_CTL1)&1)) break;
    return 0;
}

/* OLD auto mode read with CORRECT bits: START=count-1, STAT=1, poll 0x04 */
static int read_old(uint8_t *buf, int len, uint32_t ctl0) {
    int i,p;
    memset(buf,0xFF,len);
    W(SM0_CTL0,ctl0); usleep(10);
    W(SM0_CFG2,1); usleep(10);
    W(SM0_DATA,PIC);
    W(SM0_START,len-1);
    W(SM0_STAT,1);
    for(i=0;i<len;i++) {
        for(p=0;p<100000;p++) if(R(SM0_POLL)&0x04) break;
        if(p>=100000) { buf[i]=0xFF; continue; }
        usleep(10);
        buf[i]=R(SM0_DIN)&0xFF;
    }
    return 0;
}

/* BB write via GPIO sysfs */
static void gpio_export(int g) {
    char b[64]; FILE *f;
    f=fopen("/sys/class/gpio/export","w"); if(f){fprintf(f,"%d",g);fclose(f);}
    usleep(50000);
    snprintf(b,64,"/sys/class/gpio/gpio%d/direction",g);
    f=fopen(b,"w"); if(f){fprintf(f,"in");fclose(f);}
}
static void gpio_unexport(int g) {
    FILE *f=fopen("/sys/class/gpio/unexport","w"); if(f){fprintf(f,"%d",g);fclose(f);}
}
static void gpio_out(int g, int v) {
    char b[64]; FILE *f;
    snprintf(b,64,"/sys/class/gpio/gpio%d/direction",g);
    f=fopen(b,"w"); if(f){fprintf(f,v?"high":"low");fclose(f);}
}
static void gpio_in(int g) {
    char b[64]; FILE *f;
    snprintf(b,64,"/sys/class/gpio/gpio%d/direction",g);
    f=fopen(b,"w"); if(f){fprintf(f,"in");fclose(f);}
}
static int gpio_val(int g) {
    char b[64],v[4]={0}; FILE *f;
    snprintf(b,64,"/sys/class/gpio/gpio%d/value",g);
    f=fopen(b,"r"); if(!f) return -1;
    fread(v,1,1,f); fclose(f);
    return v[0]=='1'?1:0;
}

#define SDA 515
#define SCL 516
#define DLY 10

static void bb_sda_lo(void){gpio_out(SDA,0);usleep(1);}
static void bb_sda_hi(void){gpio_in(SDA);usleep(1);}
static void bb_scl_lo(void){gpio_out(SCL,0);usleep(DLY);}
static void bb_scl_hi(void){gpio_in(SCL);int t=10000;while(!gpio_val(SCL)&&t--)usleep(1);usleep(DLY);}
static void bb_start(void){bb_sda_hi();bb_scl_hi();usleep(DLY);bb_sda_lo();usleep(DLY);bb_scl_lo();}
static void bb_stop(void){bb_sda_lo();usleep(DLY);bb_scl_hi();usleep(DLY);bb_sda_hi();usleep(DLY);}

static int bb_write(uint8_t byte) {
    int i,ack;
    for(i=7;i>=0;i--){if((byte>>i)&1)bb_sda_hi();else bb_sda_lo();bb_scl_hi();bb_scl_lo();}
    gpio_in(SDA); usleep(5);
    bb_scl_hi(); usleep(2);
    ack=gpio_val(SDA);
    bb_scl_lo(); usleep(5);
    return ack==0;
}

static int bb_pic_write(uint8_t *d, int len) {
    int i;
    uint32_t gm=R(GPIOMODE);
    W(GPIOMODE,gm|(1<<2)); usleep(10000);
    gpio_export(SDA); gpio_export(SCL); usleep(50000);
    bb_start();
    if(!bb_write((PIC<<1)|0)){bb_stop();gpio_unexport(SDA);gpio_unexport(SCL);W(GPIOMODE,gm);return 0;}
    for(i=0;i<len;i++) if(!bb_write(d[i])){bb_stop();gpio_unexport(SDA);gpio_unexport(SCL);W(GPIOMODE,gm);return 0;}
    bb_stop();
    gpio_unexport(SDA); gpio_unexport(SCL);
    W(GPIOMODE,gm);
    return 1;
}

static int not_ff(uint8_t *b, int n) {
    for(int i=0;i<n;i++) if(b[i]!=0xFF) return 1;
    return 0;
}
static int not_zero(uint8_t *b, int n) {
    for(int i=0;i<n;i++) if(b[i]!=0x00) return 1;
    return 0;
}
static int valid(uint8_t *b) { return not_ff(b,8) && not_zero(b,8); }

int main(void) {
    int fd=open("/dev/mem",O_RDWR|O_SYNC);
    if(fd<0){perror("/dev/mem");return 1;}
    B=mmap(NULL,MAP_SIZE,PROT_READ|PROT_WRITE,MAP_SHARED,fd,PALMBUS);
    if(B==MAP_FAILED){perror("mmap");return 1;}

    FILE *f=fopen("/sys/bus/platform/drivers/i2c-mt7621/unbind","w");
    if(f){fprintf(f,"1e000900.i2c");fclose(f);}
    usleep(100000);

    uint8_t buf[8], prev[8]={0};
    uint32_t ctls[]={0x01F3800F, 0x8064800E, 0x90640042};
    const char *cn[]={"kern","hwmod","stock"};
    uint8_t bat_cmd[]={0x2F,0x00,0x01};
    uint8_t wake_cmd[]={0x33,0x00,0x00};
    int iter=0, found=0;

    printf("=== PIC LIVE SEARCH — running until data changes ===\n");
    printf("Sending WAKE + bat_read via bit-bang first...\n");
    bb_pic_write(wake_cmd,3); usleep(5000);
    bb_pic_write(bat_cmd,3); usleep(5000);
    printf("Done. Now reading in loop...\n\n");

    for(iter=0; iter<100; iter++) {
        rst();

        /* Try each CTL0 with NEW manual mode */
        for(int c=0;c<3;c++) {
            if(read_new(buf,8,ctls[c])==0 && valid(buf)) {
                printf("[%3d] NEW %-6s: %02x %02x %02x %02x %02x %02x %02x %02x",
                       iter,cn[c],buf[0],buf[1],buf[2],buf[3],buf[4],buf[5],buf[6],buf[7]);
                if(memcmp(buf,prev,8)!=0) { printf(" <<< CHANGED!"); found=1; }
                printf("\n");
                memcpy(prev,buf,8);
            }
            rst();
        }

        /* Try each CTL0 with OLD auto mode */
        for(int c=0;c<3;c++) {
            if(read_old(buf,8,ctls[c])==0 && valid(buf)) {
                printf("[%3d] OLD %-6s: %02x %02x %02x %02x %02x %02x %02x %02x",
                       iter,cn[c],buf[0],buf[1],buf[2],buf[3],buf[4],buf[5],buf[6],buf[7]);
                if(memcmp(buf,prev,8)!=0) { printf(" <<< CHANGED!"); found=1; }
                printf("\n");
                memcpy(prev,buf,8);
            }
            rst();
        }

        /* Every 10 iterations, send bat_read again */
        if(iter%10==9) {
            printf("[%3d] Sending bat_read via bit-bang...\n", iter);
            bb_pic_write(bat_cmd,3);
            usleep(500000); /* 500ms for PIC ADC */
        }

        usleep(100000); /* 100ms between iterations */
    }

    if(!found) printf("\nNo changes detected in %d iterations.\n", iter);

    f=fopen("/sys/bus/platform/drivers/i2c-mt7621/bind","w");
    if(f){fprintf(f,"1e000900.i2c");fclose(f);}
    munmap((void*)B,MAP_SIZE);
    close(fd);
    return 0;
}
