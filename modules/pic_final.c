/*
 * pic_final.c — Full sequence: bit-bang write + NEW manual mode read
 * NO auto mode. NO power cycle.
 * Sequence from IDA: WAKE → empty calib → bat_read → read
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
#define GPIOMODE 0x060
#define SM0_CFG2 0x928
#define N_CTL0   0x940
#define N_CTL1   0x944
#define N_D0     0x950
#define N_D1     0x954
#define PIC      0x2A
#define SDA      515
#define SCL      516

static volatile uint32_t *B;
static uint32_t R(int o){return B[o/4];}
static void W(int o,uint32_t v){B[o/4]=v;}

/* GPIO sysfs bit-bang */
static void gex(int g){FILE*f=fopen("/sys/class/gpio/export","w");if(f){fprintf(f,"%d",g);fclose(f);}usleep(50000);
    char b[64];snprintf(b,64,"/sys/class/gpio/gpio%d/direction",g);f=fopen(b,"w");if(f){fprintf(f,"in");fclose(f);}}
static void gun(int g){FILE*f=fopen("/sys/class/gpio/unexport","w");if(f){fprintf(f,"%d",g);fclose(f);}}
static void gout(int g,int v){char b[64];snprintf(b,64,"/sys/class/gpio/gpio%d/direction",g);
    FILE*f=fopen(b,"w");if(f){fprintf(f,v?"high":"low");fclose(f);}}
static void gin(int g){char b[64];snprintf(b,64,"/sys/class/gpio/gpio%d/direction",g);
    FILE*f=fopen(b,"w");if(f){fprintf(f,"in");fclose(f);}}
static int gval(int g){char b[64],v[4]={0};snprintf(b,64,"/sys/class/gpio/gpio%d/value",g);
    FILE*f=fopen(b,"r");if(!f)return-1;fread(v,1,1,f);fclose(f);return v[0]=='1'?1:0;}

static void sda_lo(void){gout(SDA,0);usleep(1);}
static void sda_hi(void){gin(SDA);usleep(1);}
static void scl_lo(void){gout(SCL,0);usleep(10);}
static void scl_hi(void){gin(SCL);int t=10000;while(!gval(SCL)&&t--)usleep(1);usleep(10);}
static void i2c_start(void){sda_hi();scl_hi();usleep(10);sda_lo();usleep(10);scl_lo();}
static void i2c_stop(void){sda_lo();usleep(10);scl_hi();usleep(10);sda_hi();usleep(10);}

static int i2c_wr(uint8_t byte){
    int i,ack;
    for(i=7;i>=0;i--){if((byte>>i)&1)sda_hi();else sda_lo();scl_hi();scl_lo();}
    gin(SDA);usleep(5);scl_hi();usleep(2);ack=gval(SDA);scl_lo();usleep(5);
    return ack==0;
}

static int bb_write(uint8_t *d, int len){
    int i;
    uint32_t gm=R(GPIOMODE);
    W(GPIOMODE,gm|(1<<2));usleep(10000);
    gex(SDA);gex(SCL);usleep(50000);
    i2c_start();
    if(!i2c_wr((PIC<<1)|0)){i2c_stop();gun(SDA);gun(SCL);W(GPIOMODE,gm);return 0;}
    for(i=0;i<len;i++)if(!i2c_wr(d[i])){i2c_stop();gun(SDA);gun(SCL);W(GPIOMODE,gm);return 0;}
    i2c_stop();
    gun(SDA);gun(SCL);
    W(GPIOMODE,gm);
    return 1;
}

/* NEW manual mode read — doesn't corrupt PIC */
static int new_read(uint8_t *buf, int len){
    int p;
    memset(buf,0xFF,len);

    /* RSTCTRL reset */
    uint32_t r=R(RSTCTRL);W(RSTCTRL,r|0x10000);usleep(10);W(RSTCTRL,r&~0x10000);usleep(500);

    W(N_CTL0,0x01F3800F);usleep(10);
    W(SM0_CFG2,0);usleep(10);
    for(p=0;p<5000;p++)if(!(R(N_CTL1)&1))break;
    W(N_CTL1,0x11);
    for(p=0;p<5000;p++)if(!(R(N_CTL1)&1))break;
    W(N_D0,(PIC<<1)|1);
    W(N_CTL1,0x21);
    for(p=0;p<5000;p++)if(!(R(N_CTL1)&1))break;
    if(!(R(N_CTL1)&(1<<16))){W(N_CTL1,0x31);for(p=0;p<5000;p++)if(!(R(N_CTL1)&1))break;return-1;}
    int off=0,rem=len;
    while(rem>0){
        int ch=(rem>8)?8:rem;
        uint32_t cmd=(rem>8)?0x50:0x40;
        W(N_CTL1,cmd|1|((ch-1)<<8));
        for(p=0;p<5000;p++)if(!(R(N_CTL1)&1))break;
        uint32_t d0=R(N_D0),d1=R(N_D1);
        memcpy(&buf[off],&d0,ch>4?4:ch);
        if(ch>4)memcpy(&buf[off+4],&d1,ch-4);
        off+=ch;rem-=ch;
    }
    W(N_CTL1,0x31);for(p=0;p<5000;p++)if(!(R(N_CTL1)&1))break;
    return 0;
}

/* NEW manual mode write — works with CTL0=0x01F3800F */
static int new_write(uint8_t *data, int len){
    int p;

    uint32_t r=R(RSTCTRL);W(RSTCTRL,r|0x10000);usleep(10);W(RSTCTRL,r&~0x10000);usleep(500);
    W(N_CTL0,0x01F3800F);usleep(10);
    W(SM0_CFG2,0);usleep(10);
    for(p=0;p<5000;p++)if(!(R(N_CTL1)&1))break;

    W(N_CTL1,0x11);
    for(p=0;p<5000;p++)if(!(R(N_CTL1)&1))break;
    W(N_D0,(PIC<<1)|0);
    W(N_CTL1,0x21);
    for(p=0;p<5000;p++)if(!(R(N_CTL1)&1))break;
    if(!(R(N_CTL1)&(1<<16))){W(N_CTL1,0x31);for(p=0;p<5000;p++)if(!(R(N_CTL1)&1))break;return-1;}

    int off=0;
    while(off<len){
        int ch=(len-off>8)?8:(len-off);
        uint32_t d0=0,d1=0;
        memcpy(&d0,&data[off],ch>4?4:ch);
        if(ch>4)memcpy(&d1,&data[off+4],ch-4);
        W(N_D0,d0);if(ch>4)W(N_D1,d1);
        W(N_CTL1,0x20|1|((ch-1)<<8));
        for(p=0;p<5000;p++)if(!(R(N_CTL1)&1))break;
        off+=ch;
    }
    W(N_CTL1,0x31);for(p=0;p<5000;p++)if(!(R(N_CTL1)&1))break;
    return 0;
}

static void hex8(const char*l,uint8_t*d){
    printf("%-12s %02x %02x %02x %02x %02x %02x %02x %02x\n",l,d[0],d[1],d[2],d[3],d[4],d[5],d[6],d[7]);
}

int main(void){
    int fd=open("/dev/mem",O_RDWR|O_SYNC);
    if(fd<0){perror("/dev/mem");return 1;}
    B=mmap(NULL,MAP_SIZE,PROT_READ|PROT_WRITE,MAP_SHARED,fd,PALMBUS);
    if(B==MAP_FAILED){perror("mmap");return 1;}

    FILE*f=fopen("/sys/bus/platform/drivers/i2c-mt7621/unbind","w");
    if(f){fprintf(f,"1e000900.i2c");fclose(f);}
    usleep(100000);

    uint8_t buf[8];
    uint8_t wake[]={0x33,0x00,0x00};
    uint8_t tab1[]={0x2D};
    uint8_t tab2[]={0x2E};
    uint8_t bat[]={0x2F,0x00,0x01};

    printf("=== PIC FINAL — full stock sequence, no auto mode ===\n\n");

    /* Step 1: Read before anything */
    printf("Step 1: Read BEFORE any write\n");
    new_read(buf,8); hex8("before:",buf);

    /* Step 2: bit-bang WAKE + empty calib + bat_read (IDA stock sequence) */
    printf("\nStep 2: bit-bang full stock init\n");
    printf("  WAKE {33,00,00}: %s\n", bb_write(wake,3)?"ACK":"NACK");
    usleep(5000);
    printf("  Table1 {2D}:     %s\n", bb_write(tab1,1)?"ACK":"NACK");
    usleep(5000);
    printf("  Table2 {2E}:     %s\n", bb_write(tab2,1)?"ACK":"NACK");
    usleep(5000);
    printf("  bat_read {2F}:   %s\n", bb_write(bat,3)?"ACK":"NACK");

    /* Step 3: Read immediately */
    printf("\nStep 3: Read immediately\n");
    new_read(buf,8); hex8("immed:",buf);

    /* Step 4: Wait 500ms + read */
    usleep(500000);
    printf("\nStep 4: Read after 500ms\n");
    new_read(buf,8); hex8("500ms:",buf);

    /* Step 5: NEW manual mode write bat_read (proven to work!) */
    printf("\nStep 5: NEW mode write bat_read\n");
    int rc=new_write(bat,3);
    printf("  result: %d\n",rc);

    /* Step 6: Read after NEW write */
    usleep(500000);
    printf("\nStep 6: Read after NEW write + 500ms\n");
    new_read(buf,8); hex8("new_wr:",buf);

    /* Step 7: Loop 30 reads — watch for changes */
    printf("\nStep 7: 30 reads every 1s — watching for changes\n");
    uint8_t prev[8]={0};
    for(int i=0;i<30;i++){
        new_read(buf,8);
        int changed=memcmp(buf,prev,8)!=0;
        int allff=1;for(int j=0;j<8;j++)if(buf[j]!=0xFF)allff=0;
        if(!allff){
            printf("[%2d] %02x %02x %02x %02x %02x %02x %02x %02x%s\n",
                   i,buf[0],buf[1],buf[2],buf[3],buf[4],buf[5],buf[6],buf[7],
                   changed?" *** CHANGED":"");
            memcpy(prev,buf,8);
        }
        /* Every 10 reads, send bat_read again */
        if(i%10==9){
            printf("[%2d] Sending bat_read (NEW mode)...\n",i);
            new_write(bat,3);
            usleep(500000);
        }
        sleep(1);
    }

    f=fopen("/sys/bus/platform/drivers/i2c-mt7621/bind","w");
    if(f){fprintf(f,"1e000900.i2c");fclose(f);}
    munmap((void*)B,MAP_SIZE);close(fd);
    return 0;
}
