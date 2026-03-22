/*
 * memdebug — Remote debug agent over TCP
 * Listens on port 5555, accepts text commands:
 *   r <addr>           — read 32-bit at physical addr
 *   w <addr> <value>   — write 32-bit
 *   d <addr> <count>   — dump count dwords
 *   sm0                — dump all SM0 I2C registers
 *   poll <seconds>     — poll SM0 catching PIC transactions
 *   upload <path> <size> — receive binary file (size bytes raw after newline)
 *   exec <cmd>         — run shell command, return stdout
 *   q                  — quit
 *
 * Build: zig cc -target mipsel-linux-musleabi -Os -static -o memdebug memdebug.c
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static int memfd;

static unsigned int mem_read(unsigned long addr) {
    unsigned long page = addr & ~0xFFFUL;
    unsigned long off = addr & 0xFFF;
    volatile unsigned int *p = mmap(NULL, 0x1000, PROT_READ|PROT_WRITE, MAP_SHARED, memfd, page);
    if (p == MAP_FAILED) return 0xDEADBEEF;
    unsigned int val = p[off/4];
    munmap((void*)p, 0x1000);
    return val;
}

static void mem_write(unsigned long addr, unsigned int val) {
    unsigned long page = addr & ~0xFFFUL;
    unsigned long off = addr & 0xFFF;
    volatile unsigned int *p = mmap(NULL, 0x1000, PROT_READ|PROT_WRITE, MAP_SHARED, memfd, page);
    if (p == MAP_FAILED) return;
    p[off/4] = val;
    munmap((void*)p, 0x1000);
}

#define PB 0x1E000000

static void cmd_sm0(int fd) {
    char buf[2048];
    int n = 0;
    struct { int off; const char *name; } regs[] = {
        {0x034, "RSTCTRL"}, {0x060, "GPIOMODE"},
        {0x900, "SM0_CFG"}, {0x904, "SM0_904"},
        {0x908, "SM0_DATA"}, {0x90C, "SM0_SLAVE"},
        {0x910, "SM0_DOUT"}, {0x914, "SM0_DIN"},
        {0x918, "SM0_POLL"}, {0x91C, "SM0_STAT"},
        {0x920, "SM0_STRT"}, {0x928, "SM0_CFG2"},
        {0x940, "N_CTL0"}, {0x944, "N_CTL1"},
        {0x948, "N_948"}, {0x94C, "N_94C"},
        {0x950, "N_D0"}, {0x954, "N_D1"},
        {0, NULL}
    };
    int i;
    for (i = 0; regs[i].name; i++) {
        n += sprintf(buf+n, "%-10s (0x%03X) = 0x%08X\n",
                     regs[i].name, regs[i].off, mem_read(PB + regs[i].off));
    }
    write(fd, buf, n);
}

static void cmd_poll(int fd, int seconds) {
    char buf[256];
    int n, i;
    int total = seconds * 100;
    unsigned int last_din = 0, last_data = 0;

    n = sprintf(buf, "Polling %ds (10ms interval), showing changes + PIC addr...\n", seconds);
    write(fd, buf, n);

    for (i = 0; i < total; i++) {
        unsigned int data = mem_read(PB + 0x908);
        unsigned int din  = mem_read(PB + 0x914);
        unsigned int poll = mem_read(PB + 0x918);
        unsigned int stat = mem_read(PB + 0x91C);
        unsigned int strt = mem_read(PB + 0x920);
        unsigned int cfg  = mem_read(PB + 0x900);
        unsigned int cfg2 = mem_read(PB + 0x928);
        unsigned int ctl0 = mem_read(PB + 0x940);
        unsigned int d0   = mem_read(PB + 0x950);
        unsigned int d1   = mem_read(PB + 0x954);

        /* Log when: addr changes, DIN changes, or addr=PIC(0x2A) */
        if (data != last_data || din != last_din ||
            (data & 0xFF) == 0x2A || (data & 0xFF) == 0x48) {
            n = sprintf(buf, "[%d.%02d] DATA=%02X DIN=%08X POLL=%02X STAT=%02X "
                        "STRT=%04X CFG=%08X CFG2=%X CTL0=%08X D0=%08X D1=%08X\n",
                        i/100, i%100, data & 0xFF, din, poll & 0xFF, stat & 0xFF,
                        strt & 0xFFFF, cfg, cfg2, ctl0, d0, d1);
            write(fd, buf, n);
            last_data = data;
            last_din = din;
        }
        usleep(10000);
    }
    n = sprintf(buf, "Poll done.\n");
    write(fd, buf, n);
}

int main(void) {
    int srv, cli;
    struct sockaddr_in addr;
    char line[256], resp[1024];

    printf("opening /dev/mem...\n"); fflush(stdout);
    memfd = open("/dev/mem", O_RDWR | O_SYNC);
    if (memfd < 0) { perror("/dev/mem"); return 1; }
    printf("ok fd=%d\n", memfd); fflush(stdout);

    printf("socket...\n"); fflush(stdout);
    srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv < 0) { perror("socket"); return 1; }
    int opt = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(5555);
    addr.sin_addr.s_addr = INADDR_ANY;

    printf("bind...\n"); fflush(stdout);
    if (bind(srv, (struct sockaddr*)&addr, sizeof(addr)) < 0) { perror("bind"); return 1; }
    printf("listen...\n"); fflush(stdout);
    listen(srv, 1);

    printf("memdebug listening on port 5555\n");
    fflush(stdout);

    while (1) {
        cli = accept(srv, NULL, NULL);
        if (cli < 0) continue;

        write(cli, "memdebug ready. Commands: r/w/d/sm0/poll/q\n", 44);

        while (1) {
            write(cli, "> ", 2);
            int n = read(cli, line, sizeof(line)-1);
            if (n <= 0) break;
            line[n] = 0;
            /* Strip newline */
            while (n > 0 && (line[n-1] == '\n' || line[n-1] == '\r')) line[--n] = 0;

            if (line[0] == 'q') break;

            if (line[0] == 'r' && line[1] == ' ') {
                unsigned long a = strtoul(line+2, NULL, 0);
                n = sprintf(resp, "0x%08lX = 0x%08X\n", a, mem_read(a));
                write(cli, resp, n);
            }
            else if (line[0] == 'w' && line[1] == ' ') {
                unsigned long a;
                unsigned int v;
                if (sscanf(line+2, "%lx %x", &a, &v) == 2) {
                    mem_write(a, v);
                    n = sprintf(resp, "0x%08lX <- 0x%08X (readback: 0x%08X)\n",
                                a, v, mem_read(a));
                    write(cli, resp, n);
                }
            }
            else if (line[0] == 'd' && line[1] == ' ') {
                unsigned long a;
                int cnt = 16;
                sscanf(line+2, "%lx %d", &a, &cnt);
                if (cnt > 64) cnt = 64;
                int i;
                for (i = 0; i < cnt; i++) {
                    n = sprintf(resp, "0x%08lX: 0x%08X\n", a + i*4, mem_read(a + i*4));
                    write(cli, resp, n);
                }
            }
            else if (strncmp(line, "sm0", 3) == 0) {
                cmd_sm0(cli);
            }
            else if (strncmp(line, "poll", 4) == 0) {
                int secs = 10;
                if (line[4] == ' ') secs = atoi(line+5);
                if (secs < 1) secs = 1;
                if (secs > 300) secs = 300;
                cmd_poll(cli, secs);
            }
            else if (strncmp(line, "upload ", 7) == 0) {
                /* upload <path> <size> — then send <size> raw bytes */
                char path[200];
                int fsize = 0;
                if (sscanf(line+7, "%199s %d", path, &fsize) == 2 && fsize > 0 && fsize < 10*1024*1024) {
                    int ufd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0755);
                    if (ufd < 0) {
                        n = sprintf(resp, "ERR: open %s failed\n", path);
                        write(cli, resp, n);
                    } else {
                        n = sprintf(resp, "READY %d\n", fsize);
                        write(cli, resp, n);
                        int got = 0;
                        char fbuf[4096];
                        while (got < fsize) {
                            int want = fsize - got;
                            if (want > (int)sizeof(fbuf)) want = sizeof(fbuf);
                            int rd = read(cli, fbuf, want);
                            if (rd <= 0) break;
                            write(ufd, fbuf, rd);
                            got += rd;
                        }
                        close(ufd);
                        chmod(path, 0755);
                        n = sprintf(resp, "OK %d bytes -> %s\n", got, path);
                        write(cli, resp, n);
                    }
                } else {
                    write(cli, "Usage: upload <path> <size>\n", 27);
                }
            }
            else if (strncmp(line, "exec ", 5) == 0) {
                /* exec <cmd> — run command, pipe stdout/stderr back */
                char cmd_buf[240];
                snprintf(cmd_buf, sizeof(cmd_buf), "%s 2>&1", line+5);
                FILE *pp = popen(cmd_buf, "r");
                if (pp) {
                    char pbuf[512];
                    while (fgets(pbuf, sizeof(pbuf), pp)) {
                        write(cli, pbuf, strlen(pbuf));
                    }
                    int rc = pclose(pp);
                    n = sprintf(resp, "[exit %d]\n", WEXITSTATUS(rc));
                    write(cli, resp, n);
                } else {
                    write(cli, "ERR: popen failed\n", 18);
                }
            }
            else {
                write(cli, "Commands: r/w/d/sm0/poll/upload/exec/q\n", 39);
            }
        }
        close(cli);
    }
}
