/*
 * lcd_touch_poll — Background touch poller for LCD UI
 * Continuously reads touch from /dev/lcd ioctl, writes to /tmp/.lcd_touch
 * Format: "x y pressed\n" updated every 50ms
 *
 * Build: zig cc -target mipsel-linux-musleabi -O2 -static -o lcd_touch_poll lcd_touch_poll.c
 */
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <signal.h>

static volatile int running = 1;

static void sig_handler(int sig) { (void)sig; running = 0; }

int main(void)
{
    int fd, data[3];
    FILE *out;

    signal(SIGTERM, sig_handler);
    signal(SIGINT, sig_handler);

    fd = open("/dev/lcd", O_RDWR);
    if (fd < 0) { perror("/dev/lcd"); return 1; }

    while (running) {
        data[0] = 0; data[1] = 0; data[2] = 0;
        ioctl(fd, 1, data);

        out = fopen("/tmp/.lcd_touch", "w");
        if (out) {
            fprintf(out, "%d %d %d\n", data[0], data[1], data[2]);
            fclose(out);
        }

        usleep(50000);  /* 50ms = 20 polls/sec */
    }

    close(fd);
    return 0;
}
