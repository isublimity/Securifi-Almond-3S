/*
 * touch_poll — Touch demo: read touch, draw directly to /dev/lcd framebuffer
 * No lcd_render needed — writes RGB565 pixels directly.
 * Build: zig cc -target mipsel-linux-musleabi -O2 -static -o touch_poll touch_poll.c
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>

#define LCD_W 320
#define LCD_H 240
#define FB_SIZE (LCD_W * LCD_H * 2)

static int lcd_fd;
static unsigned short fb[LCD_W * LCD_H];

static void fb_pixel(int x, int y, unsigned short color)
{
    if (x >= 0 && x < LCD_W && y >= 0 && y < LCD_H)
        fb[y * LCD_W + x] = color;
}

static void fb_rect(int x, int y, int w, int h, unsigned short color)
{
    for (int j = y; j < y + h && j < LCD_H; j++)
        for (int i = x; i < x + w && i < LCD_W; i++)
            if (i >= 0 && j >= 0)
                fb[j * LCD_W + i] = color;
}

/* Simple 8x8 font - digits and basic chars */
static const unsigned char font5x7[][5] = {
    {0x3E,0x51,0x49,0x45,0x3E}, /* 0 */
    {0x00,0x42,0x7F,0x40,0x00}, /* 1 */
    {0x42,0x61,0x51,0x49,0x46}, /* 2 */
    {0x21,0x41,0x45,0x4B,0x31}, /* 3 */
    {0x18,0x14,0x12,0x7F,0x10}, /* 4 */
    {0x27,0x45,0x45,0x45,0x39}, /* 5 */
    {0x3C,0x4A,0x49,0x49,0x30}, /* 6 */
    {0x01,0x71,0x09,0x05,0x03}, /* 7 */
    {0x36,0x49,0x49,0x49,0x36}, /* 8 */
    {0x06,0x49,0x49,0x29,0x1E}, /* 9 */
    {0x7F,0x02,0x0C,0x02,0x7F}, /* W (index 10) */
    {0x00,0x00,0x00,0x00,0x00}, /* space (11) */
    {0x7E,0x11,0x11,0x11,0x7E}, /* A (12) */
    {0x7F,0x09,0x09,0x09,0x01}, /* F (13) */
    {0x41,0x41,0x7F,0x41,0x41}, /* T (14) */
    {0x3E,0x41,0x41,0x41,0x3E}, /* O (15) */
    {0x3E,0x41,0x41,0x41,0x22}, /* C (16) */
    {0x7F,0x08,0x14,0x22,0x41}, /* K (17) */
    {0x7F,0x01,0x01,0x01,0x7F}, /* U (18) */
    {0x7F,0x08,0x08,0x08,0x7F}, /* H (19) */
    {0x7F,0x41,0x41,0x22,0x1C}, /* D (20) */
    {0x7F,0x49,0x49,0x49,0x41}, /* E (21) */
    {0x7F,0x04,0x08,0x04,0x7F}, /* M (22) */
    {0x36,0x49,0x49,0x49,0x36}, /* 8 dup */
    {0x7C,0x08,0x04,0x04,0x78}, /* Y (24) */
    {0x44,0x64,0x54,0x4C,0x44}, /* X=% (25) */
    {0x00,0x36,0x36,0x00,0x00}, /* = (26) */
    {0x08,0x08,0x3E,0x08,0x08}, /* + (27) */
    {0x20,0x10,0x08,0x04,0x02}, /* / (28) */
    {0x7F,0x40,0x40,0x40,0x40}, /* L (29) */
    {0x7F,0x08,0x14,0x22,0x41}, /* N (30 - reuse K shape, close enough) */
};

static void fb_char(int x, int y, char c, unsigned short color, int scale)
{
    int idx = -1;
    if (c >= '0' && c <= '9') idx = c - '0';
    else if (c == ' ') idx = 11;
    else if (c == 'X' || c == 'x') idx = 25;
    else if (c == 'Y' || c == 'y') idx = 24;
    else if (c == '=') idx = 26;
    else if (c == '#') idx = 27;
    else if (c == 'T') idx = 14;
    else if (c == 'A') idx = 12;
    else if (c == 'P') idx = 13; /* reuse F */
    else if (c == 'D') idx = 20;
    else if (c == 'E') idx = 21;
    else if (c == 'M') idx = 22;
    else if (c == 'O') idx = 15;
    else if (c == 'H') idx = 19;
    else if (c == 'U') idx = 18;
    else if (c == 'C') idx = 16;
    else if (c == 'K') idx = 17;
    else if (c == 'W') idx = 10;
    else if (c == 'L') idx = 29;
    else if (c == 'N') idx = 30;
    if (idx < 0 || idx >= 31) return;

    for (int col = 0; col < 5; col++) {
        unsigned char bits = font5x7[idx][col];
        for (int row = 0; row < 7; row++) {
            if (bits & (1 << row)) {
                for (int sy = 0; sy < scale; sy++)
                    for (int sx = 0; sx < scale; sx++)
                        fb_pixel(x + col * scale + sx, y + row * scale + sy, color);
            }
        }
    }
}

static void fb_string(int x, int y, const char *s, unsigned short color, int scale)
{
    while (*s) {
        fb_char(x, y, *s, color, scale);
        x += 6 * scale;
        s++;
    }
}

static void fb_flush(void)
{
    lseek(lcd_fd, 0, SEEK_SET);
    write(lcd_fd, fb, FB_SIZE);
    ioctl(lcd_fd, 0, 0); /* flush */
}

static void draw_crosshair(int x, int y, unsigned short color)
{
    fb_rect(x - 12, y, 25, 1, color);
    fb_rect(x, y - 12, 1, 25, color);
    /* Corner marks */
    fb_rect(x - 8, y - 8, 6, 1, 0x07E0);
    fb_rect(x + 3, y - 8, 6, 1, 0x07E0);
    fb_rect(x - 8, y + 8, 6, 1, 0x07E0);
    fb_rect(x + 3, y + 8, 6, 1, 0x07E0);
    fb_rect(x - 8, y - 8, 1, 6, 0x07E0);
    fb_rect(x + 8, y - 8, 1, 6, 0x07E0);
    fb_rect(x - 8, y + 3, 1, 6, 0x07E0);
    fb_rect(x + 8, y + 3, 1, 6, 0x07E0);
}

int main(void)
{
    lcd_fd = open("/dev/lcd", O_RDWR);
    if (lcd_fd < 0) { perror("/dev/lcd"); return 1; }

    /* Initial screen */
    memset(fb, 0, FB_SIZE);
    fb_string(30, 10, "TOUCH DEMO", 0xFFE0, 3);
    fb_string(30, 45, "TAP ANYWHERE", 0xFFFF, 2);
    fb_flush();

    printf("Touch demo running. Ctrl+C to exit.\n");

    int count = 0, was_pressed = 0;
    char info[64];

    while (1) {
        int data[3] = {0};
        if (ioctl(lcd_fd, 1, data) < 0) { usleep(50000); continue; }

        int x = data[0], y = data[1], pressed = data[2];

        if (pressed && !was_pressed) {
            count++;
            /* Clear tap area */
            fb_rect(0, 60, LCD_W, 180, 0x0000);
            /* Draw crosshair */
            draw_crosshair(x, y, 0xF800);
            /* Info text */
            snprintf(info, sizeof(info), "X=%3d Y=%3d #%d", x, y, count);
            fb_string(40, 215, info, 0xFFFF, 2);
            fb_flush();
            printf("TAP #%d x=%d y=%d\n", count, x, y);
        }
        was_pressed = pressed;
        usleep(30000);
    }

    close(lcd_fd);
    return 0;
}
