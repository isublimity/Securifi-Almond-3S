#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <termios.h>
#include <errno.h>
#include <poll.h>

static void drain_input(int fd) {
    char junk[1024];
    struct pollfd pfd = { .fd = fd, .events = POLLIN };
    while (poll(&pfd, 1, 200) > 0 && (pfd.revents & POLLIN)) {
        read(fd, junk, sizeof(junk));
    }
}

int main(int argc, char *argv[]) {
    if (argc < 3) {
        fprintf(stderr, "Usage: %s <port> <AT command> [timeout_ms]\n", argv[0]);
        return 1;
    }

    const char *port = argv[1];
    const char *cmd = argv[2];
    int timeout_ms = argc > 3 ? atoi(argv[3]) : 3000;

    int fd = open(port, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) { perror("open"); return 1; }

    /* Clear O_NONBLOCK after open */
    int flags = fcntl(fd, F_GETFL);
    fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);

    struct termios tio;
    tcgetattr(fd, &tio);
    cfmakeraw(&tio);
    cfsetispeed(&tio, B115200);
    cfsetospeed(&tio, B115200);
    tio.c_cc[VMIN] = 0;
    tio.c_cc[VTIME] = 1;
    tcsetattr(fd, TCSAFLUSH, &tio);

    /* Aggressive flush */
    tcflush(fd, TCIOFLUSH);
    drain_input(fd);
    tcflush(fd, TCIOFLUSH);

    /* Send command */
    char buf[512];
    int len = snprintf(buf, sizeof(buf), "%s\r", cmd);
    write(fd, buf, len);

    /* Read response */
    int total = 0;
    char resp[4096];
    memset(resp, 0, sizeof(resp));

    struct pollfd pfd = { .fd = fd, .events = POLLIN };
    int elapsed = 0;

    while (elapsed < timeout_ms) {
        int rc = poll(&pfd, 1, 100);
        if (rc > 0 && (pfd.revents & POLLIN)) {
            int n = read(fd, resp + total, sizeof(resp) - total - 1);
            if (n > 0) {
                total += n;
                resp[total] = '\0';
                if (strstr(resp, "\r\nOK\r\n") || strstr(resp, "\r\nERROR\r\n"))
                    break;
            }
        }
        elapsed += 100;
    }

    /* Print response, skip echo line */
    resp[total] = '\0';
    char *p = resp;
    /* Find end of echo (first \r\n after command) */
    char *nl = strstr(p, "\r\n");
    if (nl) p = nl + 2;
    /* Print cleaned */
    while (*p) {
        if (*p == '\r') { p++; continue; }
        putchar(*p);
        p++;
    }

    close(fd);
    return 0;
}
