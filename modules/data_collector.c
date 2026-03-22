/*
 * data_collector — Background daemon collecting LTE/WiFi/VPN/system stats
 * Writes JSON to /tmp/lcd_data.json every 2 seconds
 * Build: zig cc -target mipsel-linux-musleabi -Os -static -o data_collector data_collector.c
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <dirent.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/sysinfo.h>

#define JSON_PATH "/tmp/lcd_data.json"
#define INTERVAL  2

/* Run command and capture first line of output */
static int run_cmd(const char *cmd, char *buf, int bufsz) {
    FILE *fp = popen(cmd, "r");
    if (!fp) { buf[0] = 0; return -1; }
    if (!fgets(buf, bufsz, fp)) buf[0] = 0;
    pclose(fp);
    /* strip newline */
    char *nl = strchr(buf, '\n');
    if (nl) *nl = 0;
    return 0;
}

/* Run command and capture ALL output */
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

/* LTE extended info */
struct lte_info {
    int csq, ber;
    int rsrp, rsrq, sinr, rssi;
    int pci, earfcn;
    char oper[32];
    char band[16];
    char mode[16];
};

/* Direct AT session via C open/read/write — no shell, no conflicts */
static void get_lte_info_ext(struct lte_info *li) {
    int fd;
    char buf[2048];
    int total;
    char *line;
    char *ports[] = {"/dev/ttyACM2", "/dev/ttyACM1", "/dev/ttyACM0", NULL};
    int pi;

    memset(li, 0, sizeof(*li));

    /* Find working AT port */
    fd = -1;
    for (pi = 0; ports[pi]; pi++) {
        fd = open(ports[pi], O_RDWR | O_NOCTTY | O_NONBLOCK);
        if (fd >= 0) break;
    }
    if (fd < 0) return;

    /* Drain any pending data */
    while (read(fd, buf, sizeof(buf)) > 0);

    /* Send AT commands with delays for modem to respond */
    write(fd, "AT+CSQ\r", 7);
    usleep(800000);
    write(fd, "AT+COPS?\r", 9);
    usleep(800000);
    write(fd, "AT+CESQ\r", 8);
    usleep(800000);
    write(fd, "AT+XCCINFO?\r", 12);
    usleep(800000);
    write(fd, "AT+XLEC?\r", 9);
    usleep(1000000);

    /* Read all responses — retry a few times for nonblock */
    total = 0;
    {
        int retry;
        for (retry = 0; retry < 10 && total < (int)sizeof(buf) - 1; retry++) {
            int n = read(fd, buf + total, sizeof(buf) - 1 - total);
            if (n > 0) { total += n; retry = 0; }  /* reset retry on data */
            else usleep(100000);  /* 100ms between retries */
        }
    }
    buf[total] = 0;
    close(fd);

    if (total == 0) return;

    /* Parse responses */
    line = strtok(buf, "\r\n");
    while (line) {
        if (strncmp(line, "+CSQ:", 5) == 0)
            sscanf(line, "+CSQ: %d,%d", &li->csq, &li->ber);

        if (strncmp(line, "+COPS:", 6) == 0) {
            char *q1 = strchr(line, '"');
            if (q1) {
                char *q2 = strchr(q1 + 1, '"');
                if (q2) {
                    int len = q2 - q1 - 1;
                    if (len > 31) len = 31;
                    strncpy(li->oper, q1 + 1, len);
                    li->oper[len] = 0;
                }
            }
            char *comma = strrchr(line, ',');
            if (comma) {
                int act = atoi(comma + 1);
                if (act == 7) strcpy(li->mode, "LTE");
                else if (act == 2) strcpy(li->mode, "3G");
                else strcpy(li->mode, "2G");
            }
        }

        /* +CESQ: rxlev,ber,rscp,ecno,rsrq,rsrp
         * rsrq: 0-34 → dB = -20 + rsrq*0.5
         * rsrp: 0-97 → dBm = -140 + rsrp */
        if (strncmp(line, "+CESQ:", 6) == 0) {
            int rxlev, ber2, rscp, ecno, rsrq_idx, rsrp_idx;
            if (sscanf(line, "+CESQ: %d,%d,%d,%d,%d,%d",
                       &rxlev, &ber2, &rscp, &ecno, &rsrq_idx, &rsrp_idx) >= 6) {
                if (rsrp_idx < 255) li->rsrp = -140 + rsrp_idx;
                if (rsrq_idx < 255) li->rsrq = -20 + rsrq_idx / 2;
            }
        }

        /* +XCCINFO: 0,MCC,MNC,"EARFCN_hex",band,PCI,"TAC",... */
        if (strncmp(line, "+XCCINFO:", 9) == 0) {
            int v[10] = {0};
            int n = 0;
            char *p = line + 9;
            /* Parse: skip hex strings in quotes, get integers */
            while (*p && n < 10) {
                while (*p == ' ' || *p == ',') p++;
                if (*p == '"') { while (*p && *p != ',') p++; continue; }
                if (*p >= '0' && *p <= '9') {
                    v[n++] = atoi(p);
                    while (*p && *p != ',') p++;
                } else {
                    while (*p && *p != ',') p++;
                }
            }
            /* v[0]=0, v[1]=MCC, v[2]=MNC, v[3]=band, v[4]=PCI */
            if (n >= 5) {
                snprintf(li->band, sizeof(li->band), "B%d", v[3]);
                li->pci = v[4];
            }
        }

        /* +XLEC: 0,CA_count,... — carrier aggregation */

        line = strtok(NULL, "\r\n");
    }

    /* CSQ → RSSI estimate */
    if (li->csq > 0 && li->csq < 32)
        li->rssi = 2 * li->csq - 113;

    /* If no RSRP from XMCI, estimate from CSQ */
    if (li->rsrp == 0 && li->csq > 0)
        li->rsrp = li->rssi - 3;
}

/* Get WiFi clients from iw */
static int get_wifi_clients(char *json_array, int bufsz) {
    char buf[4096];
    int n = 0;

    n += snprintf(json_array + n, bufsz - n, "[");

    /* Parse iw station dump for each phy */
    for (int phy = 0; phy <= 1; phy++) {
        char cmd[256];
        snprintf(cmd, sizeof(cmd),
                 "iw dev phy%d-ap0 station dump 2>/dev/null | "
                 "awk '/Station/{if(mac)print mac,sig,rx,tx;"
                 "mac=$2;sig=0;rx=0;tx=0} /signal:/{sig=$2} "
                 "/rx bytes:/{rx=$3} /tx bytes:/{tx=$3} "
                 "END{if(mac)print mac,sig,rx,tx}'",
                 phy);

        if (run_cmd_all(cmd, buf, sizeof(buf)) > 0) {
            char *line = strtok(buf, "\n");
            while (line) {
                char mac[20] = {0};
                int sig = 0;
                long long rx = 0, tx = 0;
                if (sscanf(line, "%19s %d %lld %lld", mac, &sig, &rx, &tx) >= 2) {
                    /* Lookup hostname + IP from DHCP leases */
                    char name[64] = "unknown";
                    char ip[20] = "";
                    char lcmd[128];
                    snprintf(lcmd, sizeof(lcmd),
                             "grep -i '%s' /tmp/dhcp.leases | awk '{print $4}'", mac);
                    run_cmd(lcmd, name, sizeof(name));
                    if (name[0] == 0 || name[0] == '*') strcpy(name, "unknown");
                    snprintf(lcmd, sizeof(lcmd),
                             "grep -i '%s' /tmp/dhcp.leases | awk '{print $3}'", mac);
                    run_cmd(lcmd, ip, sizeof(ip));

                    char *band = (phy == 0) ? "5G" : "2G";

                    if (n > 2) n += snprintf(json_array + n, bufsz - n, ",");
                    n += snprintf(json_array + n, bufsz - n,
                                 "{\"mac\":\"%s\",\"name\":\"%s\",\"ip\":\"%s\","
                                 "\"band\":\"%s\",\"signal\":%d,"
                                 "\"rx_bytes\":%lld,\"tx_bytes\":%lld}",
                                 mac, name, ip, band, sig, rx, tx);
                }
                line = strtok(NULL, "\n");
            }
        }
    }

    n += snprintf(json_array + n, bufsz - n, "]");
    return n;
}

/* Check VPN status — WireGuard, OpenVPN, L2TP */
static void get_vpn_info(int *active, int *ping_ms, char *ext_ip, int ip_sz,
                         char *vpn_type, int type_sz) {
    char buf[128];
    *active = 0; *ping_ms = 0;
    ext_ip[0] = 0;
    vpn_type[0] = 0;

    /* Check WireGuard */
    if (run_cmd("wg show wg0 latest-handshake 2>/dev/null | awk '{print $2}'",
                buf, sizeof(buf)) == 0 && buf[0]) {
        long hs = atol(buf);
        long now = time(NULL);
        if (now - hs < 180) {
            *active = 1;
            strncpy(vpn_type, "WG", type_sz - 1);
        }
    }

    /* Check OpenVPN (tun0 interface) */
    if (!*active) {
        if (run_cmd("ip link show tun0 2>/dev/null | grep -c UP",
                    buf, sizeof(buf)) == 0 && buf[0] == '1') {
            *active = 1;
            strncpy(vpn_type, "OVPN", type_sz - 1);
        }
    }

    /* Check L2TP (l2tp interface) */
    if (!*active) {
        if (run_cmd("ip link show l2tp-l2tp_tina 2>/dev/null | grep -c UP",
                    buf, sizeof(buf)) == 0 && buf[0] == '1') {
            *active = 1;
            strncpy(vpn_type, "L2TP", type_sz - 1);
        }
    }

    /* External IP */
    run_cmd("curl -s --max-time 3 ifconfig.me 2>/dev/null", ext_ip, ip_sz);

    /* Ping through VPN tunnel */
    if (*active) {
        char ping_cmd[128];
        const char *dev = strcmp(vpn_type, "WG") == 0 ? "wg0" :
                          strcmp(vpn_type, "OVPN") == 0 ? "tun0" : "l2tp-l2tp_tina";
        snprintf(ping_cmd, sizeof(ping_cmd),
                 "ping -c1 -W2 -I %s 8.8.8.8 2>/dev/null | "
                 "grep 'time=' | sed 's/.*time=//;s/ .*//'", dev);
        if (run_cmd(ping_cmd, buf, sizeof(buf)) == 0 && buf[0]) {
            *ping_ms = (int)atof(buf);
        }
    }
}

int main(void) {
    /* PID file: prevent double-start */
    {
        FILE *pf = fopen("/tmp/data_collector.pid", "r");
        if (pf) {
            int old_pid = 0;
            fscanf(pf, "%d", &old_pid);
            fclose(pf);
            if (old_pid > 0 && kill(old_pid, 0) == 0) {
                /* Already running — kill old instance */
                kill(old_pid, 9);
                usleep(500000);
            }
        }
        pf = fopen("/tmp/data_collector.pid", "w");
        if (pf) { fprintf(pf, "%d\n", getpid()); fclose(pf); }
    }

    while (1) {
        struct lte_info li;
        int vpn_active = 0, vpn_ping = 0;
        char ext_ip[32] = "";
        char vpn_type[8] = "";
        char wifi_json[2048] = "[]";
        struct sysinfo si;
        char lte_ip[32] = "";

        /* Collect data */
        get_lte_info_ext(&li);
        get_vpn_info(&vpn_active, &vpn_ping, ext_ip, sizeof(ext_ip),
                     vpn_type, sizeof(vpn_type));
        get_wifi_clients(wifi_json, sizeof(wifi_json));
        sysinfo(&si);

        /* LTE IP */
        run_cmd("ip -4 addr show wwan0 2>/dev/null | grep inet | awk '{print $2}' | cut -d/ -f1",
                lte_ip, sizeof(lte_ip));

        /* Google ping */
        char ping_buf[32] = "0";
        run_cmd("ping -c1 -W2 8.8.8.8 2>/dev/null | grep 'time=' | sed 's/.*time=//;s/ .*//'",
                ping_buf, sizeof(ping_buf));
        int google_ping = (int)atof(ping_buf);

        /* Write JSON atomically */
        FILE *fp = fopen(JSON_PATH ".tmp", "w");
        if (fp) {
            fprintf(fp,
                "{\n"
                "  \"ts\": %ld,\n"
                "  \"lte\": {\"csq\": %d, \"ber\": %d, \"rsrp\": %d, \"rsrq\": %d, "
                "\"sinr\": %d, \"rssi\": %d, \"pci\": %d, "
                "\"band\": \"%s\", \"mode\": \"%s\", "
                "\"operator\": \"%s\", \"ip\": \"%s\"},\n"
                "  \"vpn\": {\"active\": %s, \"type\": \"%s\", \"ping_ms\": %d, \"external_ip\": \"%s\"},\n"
                "  \"wifi\": {\"clients\": %s},\n"
                "  \"ping\": {\"google_ms\": %d},\n"
                "  \"uptime\": %ld,\n"
                "  \"mem_free_mb\": %ld,\n"
                "  \"cpu_load\": %.2f\n"
                "}\n",
                (long)time(NULL),
                li.csq, li.ber, li.rsrp, li.rsrq,
                li.sinr, li.rssi, li.pci,
                li.band, li.mode,
                li.oper, lte_ip,
                vpn_active ? "true" : "false", vpn_type, vpn_ping, ext_ip,
                wifi_json,
                google_ping,
                si.uptime,
                si.freeram / 1024 / 1024,
                si.loads[0] / 65536.0);
            fclose(fp);
            rename(JSON_PATH ".tmp", JSON_PATH);
        }

        sleep(INTERVAL);
    }

    return 0;
}
