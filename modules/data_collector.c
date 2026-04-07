/*
 * data_collector V260401 by Sublimity
 *
 * Background daemon: collects LTE/WiFi/VPN/Battery/System stats.
 * Pushes JSON to connected clients via unix socket /tmp/lcd_data.sock every 2s.
 * Also writes /tmp/lcd_data.json for compatibility.
 *
 * Build: zig cc -target mipsel-linux-musleabi -Os -static -o data_collector data_collector.c
 */

#define VERSION "V260401"
#define MODNAME "data_collector"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <dirent.h>
#include <fcntl.h>
#include <signal.h>
#include <errno.h>
#include <sys/sysinfo.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>

#define SOCK_PATH  "/tmp/lcd_data.sock"
#define JSON_PATH  "/tmp/lcd_data.json"
#define INTERVAL   2
#define MAX_CLIENTS 4
#define JSON_BUF   8192

static volatile int running = 1;
static int clients[MAX_CLIENTS];
static int client_count = 0;

static void sig_handler(int sig) { (void)sig; running = 0; }

/* ======== PIC Battery ======== */
struct battery_info {
    int adc, percent, charging, valid;
};

static void get_battery(struct battery_info *bi) {
    unsigned char raw[17] = {0};
    bi->adc = 0; bi->percent = 0; bi->charging = 0; bi->valid = 0;
    int fd = open("/dev/lcd", O_RDWR);
    if (fd < 0) return;
    int ret = ioctl(fd, 2, raw);
    fprintf(stderr, MODNAME ": bat ioctl=%d raw=%02x %02x %02x %02x %02x %02x\n",
            ret, raw[0], raw[1], raw[2], raw[3], raw[4], raw[5]);
    if (ret == 0 && raw[3] == 0x02 && raw[4] == 0x04) {
        bi->adc = (raw[1] << 2) | (raw[2] >> 6);
        bi->charging = (raw[5] & 0x01) ? 1 : 0;
        bi->valid = 1;
        if (bi->adc < 401) bi->percent = 0;
        else if (bi->adc < 542) bi->percent = (bi->adc - 401) * 20 / 141;
        else bi->percent = 20 + (bi->adc - 542) * 80 / 481;
        if (bi->percent > 100) bi->percent = 100;
        fprintf(stderr, MODNAME ": bat ADC=%d pct=%d chg=%d\n",
                bi->adc, bi->percent, bi->charging);
    }
    close(fd);
}

/* ======== Shell helpers ======== */
static int run_cmd(const char *cmd, char *buf, int bufsz) {
    FILE *fp = popen(cmd, "r");
    if (!fp) { buf[0] = 0; return -1; }
    if (!fgets(buf, bufsz, fp)) buf[0] = 0;
    pclose(fp);
    char *nl = strchr(buf, '\n');
    if (nl) *nl = 0;
    return 0;
}

static int run_cmd_all(const char *cmd, char *buf, int bufsz) {
    FILE *fp = popen(cmd, "r");
    if (!fp) { buf[0] = 0; return -1; }
    int total = 0;
    while (total < bufsz - 1) {
        int n = fread(buf + total, 1, bufsz - 1 - total, fp);
        if (n <= 0) break;
        total += n;
    }
    buf[total] = 0;
    pclose(fp);
    return total;
}

/* ======== LTE ======== */
struct lte_info {
    int csq, ber, rsrp, rsrq, sinr, rssi, pci, earfcn;
    char oper[32], band[16], mode[16];
};

static void get_lte_info_ext(struct lte_info *li) {
    int fd;
    char buf[2048];
    int total;
    char *line;
    char *ports[] = {"/dev/ttyACM2", "/dev/ttyACM1", "/dev/ttyACM0", NULL};
    int pi;

    memset(li, 0, sizeof(*li));
    fd = -1;
    for (pi = 0; ports[pi]; pi++) {
        fd = open(ports[pi], O_RDWR | O_NOCTTY | O_NONBLOCK);
        if (fd >= 0) break;
    }
    if (fd < 0) return;

    while (read(fd, buf, sizeof(buf)) > 0);
    write(fd, "AT+CSQ\r", 7); usleep(800000);
    write(fd, "AT+COPS?\r", 9); usleep(800000);
    write(fd, "AT+CESQ\r", 8); usleep(800000);
    write(fd, "AT+XCCINFO?\r", 12); usleep(800000);
    write(fd, "AT+XLEC?\r", 9); usleep(1000000);

    total = 0;
    { int retry;
      for (retry = 0; retry < 10 && total < (int)sizeof(buf) - 1; retry++) {
        int n = read(fd, buf + total, sizeof(buf) - 1 - total);
        if (n > 0) { total += n; retry = 0; } else usleep(100000);
    }}
    buf[total] = 0;
    close(fd);
    if (total == 0) return;

    line = strtok(buf, "\r\n");
    while (line) {
        if (strncmp(line, "+CSQ:", 5) == 0)
            sscanf(line, "+CSQ: %d,%d", &li->csq, &li->ber);
        if (strncmp(line, "+COPS:", 6) == 0) {
            char *q1 = strchr(line, '"');
            if (q1) { char *q2 = strchr(q1+1, '"');
                if (q2) { int len = q2-q1-1; if (len>31) len=31;
                    strncpy(li->oper, q1+1, len); li->oper[len]=0; }}
            char *comma = strrchr(line, ',');
            if (comma) { int act = atoi(comma+1);
                if (act==7) strcpy(li->mode,"LTE");
                else if (act==2) strcpy(li->mode,"3G");
                else strcpy(li->mode,"2G"); }
        }
        if (strncmp(line, "+CESQ:", 6) == 0) {
            int rxlev,ber2,rscp,ecno,rsrq_idx,rsrp_idx;
            if (sscanf(line, "+CESQ: %d,%d,%d,%d,%d,%d",
                       &rxlev,&ber2,&rscp,&ecno,&rsrq_idx,&rsrp_idx) >= 6) {
                if (rsrp_idx < 255) li->rsrp = -140 + rsrp_idx;
                if (rsrq_idx < 255) li->rsrq = -20 + rsrq_idx / 2;
            }
        }
        if (strncmp(line, "+XCCINFO:", 9) == 0) {
            int v[10]={0}; int n=0; char *p=line+9;
            while (*p && n < 10) {
                while (*p==' '||*p==',') p++;
                if (*p=='"') { while(*p&&*p!=',')p++; continue; }
                if (*p>='0'&&*p<='9') { v[n++]=atoi(p); while(*p&&*p!=',')p++; }
                else { while(*p&&*p!=',')p++; }
            }
            if (n >= 5) { snprintf(li->band,sizeof(li->band),"B%d",v[3]); li->pci=v[4]; }
        }
        line = strtok(NULL, "\r\n");
    }
    if (li->csq > 0 && li->csq < 32) li->rssi = 2 * li->csq - 113;
    if (li->rsrp == 0 && li->csq > 0) li->rsrp = li->rssi - 3;
}

/* ======== WiFi ======== */
static int get_wifi_clients(char *json_array, int bufsz) {
    char buf[4096];
    int n = 0;
    n += snprintf(json_array+n, bufsz-n, "[");
    for (int phy = 0; phy <= 1; phy++) {
        char cmd[256];
        snprintf(cmd, sizeof(cmd),
            "iw dev phy%d-ap0 station dump 2>/dev/null | "
            "awk '/Station/{if(mac)print mac,sig,rx,tx;"
            "mac=$2;sig=0;rx=0;tx=0} /signal:/{sig=$2} "
            "/rx bytes:/{rx=$3} /tx bytes:/{tx=$3} "
            "END{if(mac)print mac,sig,rx,tx}'", phy);
        if (run_cmd_all(cmd, buf, sizeof(buf)) > 0) {
            char *line = strtok(buf, "\n");
            while (line) {
                char mac[20]={0}; int sig=0; long long rx=0,tx=0;
                if (sscanf(line, "%19s %d %lld %lld", mac, &sig, &rx, &tx) >= 2) {
                    char name[64]="unknown", ip[20]="", lcmd[128];
                    snprintf(lcmd,sizeof(lcmd),"grep -i '%s' /tmp/dhcp.leases | awk '{print $4}'",mac);
                    run_cmd(lcmd,name,sizeof(name));
                    if (name[0]==0||name[0]=='*') strcpy(name,"unknown");
                    snprintf(lcmd,sizeof(lcmd),"grep -i '%s' /tmp/dhcp.leases | awk '{print $3}'",mac);
                    run_cmd(lcmd,ip,sizeof(ip));
                    char *band = (phy==0) ? "5G" : "2G";
                    if (n>2) n += snprintf(json_array+n, bufsz-n, ",");
                    n += snprintf(json_array+n, bufsz-n,
                        "{\"mac\":\"%s\",\"name\":\"%s\",\"ip\":\"%s\","
                        "\"band\":\"%s\",\"signal\":%d,\"rx_bytes\":%lld,\"tx_bytes\":%lld}",
                        mac,name,ip,band,sig,rx,tx);
                }
                line = strtok(NULL, "\n");
            }
        }
    }
    n += snprintf(json_array+n, bufsz-n, "]");
    return n;
}

/* ======== VPN ======== */
static void get_vpn_info(int *active, int *ping_ms, char *ext_ip, int ip_sz,
                         char *vpn_type, int type_sz) {
    char buf[128];
    *active=0; *ping_ms=0; ext_ip[0]=0; vpn_type[0]=0;
    if (run_cmd("wg show wg0 latest-handshakes 2>/dev/null | awk '{print $2}'",
                buf,sizeof(buf))==0 && buf[0]) {
        if (time(NULL) - atol(buf) < 180) { *active=1; strncpy(vpn_type,"WG",type_sz-1); }
    }
    if (!*active && run_cmd("ip link show tun0 2>/dev/null | grep -c UP",buf,sizeof(buf))==0 && buf[0]=='1') {
        *active=1; strncpy(vpn_type,"OVPN",type_sz-1);
    }
    if (!*active && run_cmd("ip link show l2tp-l2tp_tina 2>/dev/null | grep -c UP",buf,sizeof(buf))==0 && buf[0]=='1') {
        *active=1; strncpy(vpn_type,"L2TP",type_sz-1);
    }
    run_cmd("wget -qO- --timeout=3 http://ifconfig.me/ip 2>/dev/null || "
            "curl -s --max-time 3 ifconfig.me/ip 2>/dev/null", ext_ip, ip_sz);
    if (*active) {
        char ping_cmd[128];
        const char *dev = strcmp(vpn_type,"WG")==0?"wg0":strcmp(vpn_type,"OVPN")==0?"tun0":"l2tp-l2tp_tina";
        snprintf(ping_cmd,sizeof(ping_cmd),"ping -c1 -W2 -I %s 8.8.8.8 2>/dev/null | grep 'time=' | sed 's/.*time=//;s/ .*//'",dev);
        if (run_cmd(ping_cmd,buf,sizeof(buf))==0 && buf[0] && atof(buf)>0) *ping_ms=(int)atof(buf);
        else *ping_ms=-1;
    }
}

/* ======== Socket: push to clients ======== */
static void push_to_clients(const char *json, int len) {
    for (int i = 0; i < client_count; i++) {
        if (write(clients[i], json, len) < 0) {
            close(clients[i]);
            clients[i] = clients[--client_count];
            i--;
        }
    }
}

static void accept_clients(int srv) {
    /* Non-blocking accept */
    while (client_count < MAX_CLIENTS) {
        int cfd = accept(srv, NULL, NULL);
        if (cfd < 0) break;
        fcntl(cfd, F_SETFL, O_NONBLOCK);
        clients[client_count++] = cfd;
    }
}

/* ======== Main ======== */
int main(void) {
    fprintf(stderr, MODNAME " " VERSION " by Sublimity — START\n");

    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);
    signal(SIGPIPE, SIG_IGN);

    /* PID file: kill old instance */
    {
        FILE *pf = fopen("/tmp/data_collector.pid", "r");
        if (pf) { int old=0; fscanf(pf,"%d",&old); fclose(pf);
            if (old>0 && kill(old,0)==0) { kill(old,9); usleep(500000); } }
        pf = fopen("/tmp/data_collector.pid", "w");
        if (pf) { fprintf(pf,"%d\n",getpid()); fclose(pf); }
    }

    /* Create socket server */
    unlink(SOCK_PATH);
    int srv = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    strncpy(addr.sun_path, SOCK_PATH, sizeof(addr.sun_path) - 1);
    bind(srv, (struct sockaddr *)&addr, sizeof(addr));
    listen(srv, MAX_CLIENTS);
    chmod(SOCK_PATH, 0666);
    fcntl(srv, F_SETFL, O_NONBLOCK);

    fprintf(stderr, MODNAME ": socket %s ready\n", SOCK_PATH);

    while (running) {
        struct lte_info li;
        int vpn_active=0, vpn_ping=0;
        char ext_ip[32]="", vpn_type[8]="", wifi_json[2048]="[]", lte_ip[32]="";
        struct sysinfo si;
        struct battery_info bat;
        char ping_buf[32]="";
        int google_ping;
        char json[JSON_BUF];
        int len;

        /* Collect */
        get_lte_info_ext(&li);
        get_vpn_info(&vpn_active, &vpn_ping, ext_ip, sizeof(ext_ip), vpn_type, sizeof(vpn_type));
        get_wifi_clients(wifi_json, sizeof(wifi_json));
        sysinfo(&si);
        run_cmd("ip -4 addr show wwan0 2>/dev/null | grep inet | awk '{print $2}' | cut -d/ -f1", lte_ip, sizeof(lte_ip));
        get_battery(&bat);
        run_cmd("ping -c1 -W2 8.8.8.8 2>/dev/null | grep 'time=' | sed 's/.*time=//;s/ .*//'", ping_buf, sizeof(ping_buf));
        google_ping = (ping_buf[0] && atof(ping_buf)>0) ? (int)atof(ping_buf) : -1;

        /* Format JSON */
        len = snprintf(json, sizeof(json),
            "{\"ts\":%ld,"
            "\"lte\":{\"csq\":%d,\"ber\":%d,\"rsrp\":%d,\"rsrq\":%d,"
            "\"sinr\":%d,\"rssi\":%d,\"pci\":%d,"
            "\"band\":\"%s\",\"mode\":\"%s\",\"operator\":\"%s\",\"ip\":\"%s\"},"
            "\"vpn\":{\"active\":%s,\"type\":\"%s\",\"ping_ms\":%d,\"external_ip\":\"%s\"},"
            "\"wifi\":{\"clients\":%s},"
            "\"ping\":{\"google_ms\":%d},"
            "\"battery\":{\"adc\":%d,\"percent\":%d,\"charging\":%s,\"valid\":%s},"
            "\"uptime\":%ld,\"mem_free_mb\":%ld,\"cpu_load\":%.2f}\n",
            (long)time(NULL),
            li.csq,li.ber,li.rsrp,li.rsrq,li.sinr,li.rssi,li.pci,
            li.band,li.mode,li.oper,lte_ip,
            vpn_active?"true":"false",vpn_type,vpn_ping,ext_ip,
            wifi_json, google_ping,
            bat.adc,bat.percent,bat.charging?"true":"false",bat.valid?"true":"false",
            si.uptime, si.freeram/1024/1024, si.loads[0]/65536.0);

        /* Push to socket clients */
        accept_clients(srv);
        push_to_clients(json, len);

        /* Also write file for compatibility */
        FILE *fp = fopen(JSON_PATH ".tmp", "w");
        if (fp) { fwrite(json, 1, len, fp); fclose(fp); rename(JSON_PATH ".tmp", JSON_PATH); }

        sleep(INTERVAL);
    }

    close(srv);
    unlink(SOCK_PATH);
    unlink("/tmp/data_collector.pid");
    fprintf(stderr, MODNAME " " VERSION " — STOP\n");
    return 0;
}
