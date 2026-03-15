/*
 * lcd_touch_read — Read touch from /dev/lcd ioctl(1)
 * Output: "x y pressed" on stdout
 * Usage: lcd_touch_read
 * Build: zig cc -target mipsel-linux-musleabi -O2 -static -o lcd_touch_read lcd_touch_read.c
 */
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>

int main(void)
{
    int fd = open("/dev/lcd", O_RDWR);
    if (fd < 0) return 1;

    int data[3] = {0, 0, 0};
    if (ioctl(fd, 1, data) < 0) {
        close(fd);
        return 1;
    }
    close(fd);

    printf("%d %d %d\n", data[0], data[1], data[2]);
    return 0;
}
