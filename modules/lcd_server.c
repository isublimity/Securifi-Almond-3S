/*
 * lcd_server V260401 by Sublimity
 *
 * Single process: render + touch + data + socket server.
 * Replaces: lcd_render + touch_poll (no more daemon hacks)
 *
 * Threads:
 *   main    — socket server /tmp/lcd.sock, accept clients, dispatch
 *   render  — mmap /dev/lcd framebuffer, draw commands
 *   touch   — ioctl poll /dev/lcd, push events to clients
 *   data    — connect /tmp/lcd_data.sock, push to clients
 *
 * Build: zig cc -target mipsel-linux-musleabi -Os -static -lpthread -o lcd_server lcd_server.c
 */

#define VERSION "V260401"
#define MODNAME "lcd_server"

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/ioctl.h>
#include <sys/stat.h>

#define LCD_DEV       "/dev/lcd"
#define SOCK_PATH     "/tmp/lcd.sock"
#define DATA_SOCK     "/tmp/lcd_data.sock"
#define FB_W          320
#define FB_H          240
#define FB_BPP        2       /* RGB565 */
#define FB_SIZE       (FB_W * FB_H * FB_BPP)
#define MAX_CLIENTS   4
#define CMD_BUF       4096

/* ======== Globals ======== */
static int lcd_fd = -1;
static uint16_t *framebuffer;
static volatile int running = 1;

/* Client tracking */
static int clients[MAX_CLIENTS];
static pthread_mutex_t client_lock = PTHREAD_MUTEX_INITIALIZER;
static int client_count = 0;

/* Latest data from data_collector */
static char latest_data[CMD_BUF];
static pthread_mutex_t data_lock = PTHREAD_MUTEX_INITIALIZER;

/* ======== Client management ======== */
static void client_add(int fd) {
    pthread_mutex_lock(&client_lock);
    if (client_count < MAX_CLIENTS) {
        clients[client_count++] = fd;
    } else {
        close(fd);
    }
    pthread_mutex_unlock(&client_lock);
}

static void client_remove(int fd) {
    pthread_mutex_lock(&client_lock);
    for (int i = 0; i < client_count; i++) {
        if (clients[i] == fd) {
            clients[i] = clients[--client_count];
            break;
        }
    }
    pthread_mutex_unlock(&client_lock);
    close(fd);
}

static void broadcast_to_clients(const char *msg, int len) {
    pthread_mutex_lock(&client_lock);
    for (int i = 0; i < client_count; i++) {
        if (write(clients[i], msg, len) < 0) {
            /* Client disconnected — mark for removal */
            close(clients[i]);
            clients[i] = clients[--client_count];
            i--;
        }
    }
    pthread_mutex_unlock(&client_lock);
}

/* ======== RGB565 helpers ======== */
static uint16_t rgb565(int r, int g, int b) {
    return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
}

static uint16_t parse_color(const char *hex) {
    if (!hex || hex[0] != '#' || strlen(hex) < 7) return 0;
    unsigned int c = strtoul(hex + 1, NULL, 16);
    return rgb565((c >> 16) & 0xFF, (c >> 8) & 0xFF, c & 0xFF);
}

/* ======== Drawing ======== */
static void fb_clear(uint16_t color) {
    for (int i = 0; i < FB_W * FB_H; i++)
        framebuffer[i] = color;
}

static void fb_rect(int x, int y, int w, int h, uint16_t color) {
    for (int j = y; j < y + h && j < FB_H; j++)
        for (int i = x; i < x + w && i < FB_W; i++)
            if (i >= 0 && j >= 0)
                framebuffer[j * FB_W + i] = color;
}

static void fb_flush(void) {
    if (lcd_fd >= 0) {
        lseek(lcd_fd, 0, SEEK_SET);
        write(lcd_fd, framebuffer, FB_SIZE);
        ioctl(lcd_fd, 0, 0); /* flush ioctl */
    }
}

/* ======== Simple JSON parser ======== */
static int json_str(const char *json, const char *key, char *out, int outsz) {
    char pattern[64];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    const char *p = strstr(json, pattern);
    if (!p) return -1;
    p = strchr(p + strlen(pattern), ':');
    if (!p) return -1;
    while (*p == ' ' || *p == ':' || *p == '"') p++;
    int i = 0;
    while (*p && *p != '"' && *p != ',' && *p != '}' && i < outsz - 1)
        out[i++] = *p++;
    out[i] = 0;
    return 0;
}

static int json_int(const char *json, const char *key) {
    char buf[32];
    if (json_str(json, key, buf, sizeof(buf)) < 0) return -1;
    return atoi(buf);
}

/* ======== Process draw command ======== */
static void process_cmd(const char *line) {
    char cmd[32] = "", color[16] = "#000000", text[256] = "";
    int x = 0, y = 0, w = 0, h = 0, size = 16;

    json_str(line, "cmd", cmd, sizeof(cmd));

    if (strcmp(cmd, "clear") == 0) {
        json_str(line, "color", color, sizeof(color));
        fb_clear(parse_color(color));
    }
    else if (strcmp(cmd, "rect") == 0) {
        x = json_int(line, "x");
        y = json_int(line, "y");
        w = json_int(line, "w");
        h = json_int(line, "h");
        json_str(line, "color", color, sizeof(color));
        fb_rect(x, y, w, h, parse_color(color));
    }
    else if (strcmp(cmd, "text") == 0) {
        /* TODO: font rendering */
        x = json_int(line, "x");
        y = json_int(line, "y");
        json_str(line, "text", text, sizeof(text));
        /* placeholder: draw small rect where text would be */
        json_str(line, "color", color, sizeof(color));
        fb_rect(x, y, strlen(text) * 8, 16, parse_color(color));
    }
    else if (strcmp(cmd, "flush") == 0) {
        fb_flush();
    }
}

/* ======== Touch thread ======== */
static void *touch_thread(void *arg) {
    (void)arg;
    int prev_pressed = 0;
    char msg[128];

    while (running) {
        int data[3] = {0, 0, 0};
        if (ioctl(lcd_fd, 1, data) == 0) {
            int x = data[0], y = data[1], pressed = data[2];
            if (pressed != prev_pressed || pressed) {
                int n = snprintf(msg, sizeof(msg),
                    "{\"event\":\"touch\",\"x\":%d,\"y\":%d,\"state\":\"%s\"}\n",
                    x, y, pressed ? "down" : "up");
                broadcast_to_clients(msg, n);
                prev_pressed = pressed;
            }
        }
        usleep(50000); /* 50ms */
    }
    return NULL;
}

/* ======== Data reader thread ======== */
static void *data_thread(void *arg) {
    (void)arg;

    while (running) {
        /* Connect to data_collector socket */
        int dfd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (dfd < 0) { sleep(2); continue; }

        struct sockaddr_un addr = { .sun_family = AF_UNIX };
        strncpy(addr.sun_path, DATA_SOCK, sizeof(addr.sun_path) - 1);

        if (connect(dfd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
            close(dfd);
            sleep(2);
            continue;
        }

        /* Read data updates */
        char buf[CMD_BUF];
        while (running) {
            int n = read(dfd, buf, sizeof(buf) - 1);
            if (n <= 0) break;
            buf[n] = 0;

            /* Store latest */
            pthread_mutex_lock(&data_lock);
            strncpy(latest_data, buf, sizeof(latest_data) - 1);
            pthread_mutex_unlock(&data_lock);

            /* Broadcast to UI clients */
            char msg[CMD_BUF + 32];
            int len = snprintf(msg, sizeof(msg), "{\"event\":\"data\",%s}\n", buf + 1);
            /* Skip opening { from data, merge with event wrapper */
            broadcast_to_clients(msg, len);
        }
        close(dfd);
        sleep(2); /* reconnect delay */
    }
    return NULL;
}

/* ======== Client handler ======== */
static void *client_handler(void *arg) {
    int fd = *(int *)arg;
    free(arg);
    char buf[CMD_BUF];

    client_add(fd);

    /* Send current data snapshot */
    pthread_mutex_lock(&data_lock);
    if (latest_data[0]) {
        char msg[CMD_BUF + 32];
        int len = snprintf(msg, sizeof(msg), "{\"event\":\"data\",%s}\n", latest_data + 1);
        write(fd, msg, len);
    }
    pthread_mutex_unlock(&data_lock);

    /* Read draw commands from client */
    int total = 0;
    while (running) {
        int n = read(fd, buf + total, sizeof(buf) - total - 1);
        if (n <= 0) break;
        total += n;
        buf[total] = 0;

        /* Process complete lines */
        char *line = buf;
        char *nl;
        while ((nl = strchr(line, '\n')) != NULL) {
            *nl = 0;
            if (line[0] == '{')
                process_cmd(line);
            line = nl + 1;
        }

        /* Move remaining to start */
        int remaining = total - (line - buf);
        if (remaining > 0) memmove(buf, line, remaining);
        total = remaining;
    }

    client_remove(fd);
    return NULL;
}

/* ======== Signal handler ======== */
static void sig_handler(int sig) {
    (void)sig;
    running = 0;
}

/* ======== Main ======== */
int main(void) {
    fprintf(stderr, MODNAME " " VERSION " by Sublimity — START\n");

    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);
    signal(SIGPIPE, SIG_IGN);

    /* Open LCD device */
    lcd_fd = open(LCD_DEV, O_RDWR);
    if (lcd_fd < 0) {
        perror(LCD_DEV);
        return 1;
    }

    /* mmap framebuffer */
    framebuffer = mmap(NULL, FB_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, lcd_fd, 0);
    if (framebuffer == MAP_FAILED) {
        perror("mmap");
        close(lcd_fd);
        return 1;
    }

    /* Clear screen */
    fb_clear(0);
    fb_flush();

    /* Create socket */
    unlink(SOCK_PATH);
    int srv = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    strncpy(addr.sun_path, SOCK_PATH, sizeof(addr.sun_path) - 1);
    bind(srv, (struct sockaddr *)&addr, sizeof(addr));
    listen(srv, MAX_CLIENTS);
    chmod(SOCK_PATH, 0666);

    /* Start threads */
    pthread_t t_touch, t_data;
    pthread_create(&t_touch, NULL, touch_thread, NULL);
    pthread_create(&t_data, NULL, data_thread, NULL);

    fprintf(stderr, MODNAME ": socket %s, framebuffer %dx%d, ready\n",
            SOCK_PATH, FB_W, FB_H);

    /* Accept loop */
    while (running) {
        int cfd = accept(srv, NULL, NULL);
        if (cfd < 0) {
            if (errno == EINTR) continue;
            break;
        }
        int *pfd = malloc(sizeof(int));
        *pfd = cfd;
        pthread_t t;
        pthread_create(&t, NULL, client_handler, pfd);
        pthread_detach(t);
    }

    /* Cleanup */
    running = 0;
    pthread_join(t_touch, NULL);
    pthread_join(t_data, NULL);
    close(srv);
    unlink(SOCK_PATH);
    munmap(framebuffer, FB_SIZE);
    close(lcd_fd);

    fprintf(stderr, MODNAME " " VERSION " — STOP\n");
    return 0;
}
