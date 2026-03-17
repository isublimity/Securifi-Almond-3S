/*
 * lcdlib.so — Lua C module for LCD: mmap framebuffer + rendering + touch
 *
 * Architecture A+C: zero-copy mmap framebuffer, all rendering in-process.
 * No sockets, no lcd_render, no fork. Single process does everything.
 *
 * API:
 *   lcd.open()                    — open /dev/lcd, mmap framebuffer
 *   lcd.close()                   — cleanup
 *   lcd.touch()                   — returns x, y, pressed
 *   lcd.flush()                   — push framebuffer to display
 *   lcd.backlight(on)             — 0=off, 1=on
 *   lcd.splash()                  — show kernel 4PDA logo
 *   lcd.rect(x,y,w,h, color)     — fill rectangle (RGB565 or 0xRRGGBB)
 *   lcd.text(x,y, str, color, scale) — draw text
 *   lcd.pixel(x,y, color)        — single pixel
 *   lcd.clear(color)              — fill entire screen
 *   lcd.usleep(us)                — sleep microseconds
 *
 * Build on server: requires OpenWrt Lua headers
 *   mipsel-openwrt-linux-musl-gcc -shared -O2 -o lcdlib.so lcdlib.c -I<lua-include> -llua
 */
#include <lua.h>
#include <lauxlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <stdlib.h>

#define LCD_W  320
#define LCD_H  240
#define FB_SIZE (LCD_W * LCD_H * 2)

static int lcd_fd = -1;
static unsigned short *fb = NULL;  /* mmap'd framebuffer */

/* === Framebuffer primitives === */

static inline void fb_pixel(int x, int y, unsigned short c)
{
    if ((unsigned)x < LCD_W && (unsigned)y < LCD_H)
        fb[y * LCD_W + x] = c;
}

static void fb_rect(int x, int y, int w, int h, unsigned short c)
{
    int x0 = x < 0 ? 0 : x;
    int y0 = y < 0 ? 0 : y;
    int x1 = x + w > LCD_W ? LCD_W : x + w;
    int y1 = y + h > LCD_H ? LCD_H : y + h;
    for (int j = y0; j < y1; j++)
        for (int i = x0; i < x1; i++)
            fb[j * LCD_W + i] = c;
}

/* 5x7 bitmap font (printable ASCII 32-127) */
static const unsigned char font5x7[96][5] = {
    {0x00,0x00,0x00,0x00,0x00}, /*   */
    {0x00,0x00,0x5F,0x00,0x00}, /* ! */
    {0x00,0x07,0x00,0x07,0x00}, /* " */
    {0x14,0x7F,0x14,0x7F,0x14}, /* # */
    {0x24,0x2A,0x7F,0x2A,0x12}, /* $ */
    {0x23,0x13,0x08,0x64,0x62}, /* % */
    {0x36,0x49,0x55,0x22,0x50}, /* & */
    {0x00,0x05,0x03,0x00,0x00}, /* ' */
    {0x00,0x1C,0x22,0x41,0x00}, /* ( */
    {0x00,0x41,0x22,0x1C,0x00}, /* ) */
    {0x08,0x2A,0x1C,0x2A,0x08}, /* * */
    {0x08,0x08,0x3E,0x08,0x08}, /* + */
    {0x00,0x50,0x30,0x00,0x00}, /* , */
    {0x08,0x08,0x08,0x08,0x08}, /* - */
    {0x00,0x60,0x60,0x00,0x00}, /* . */
    {0x20,0x10,0x08,0x04,0x02}, /* / */
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
    {0x00,0x36,0x36,0x00,0x00}, /* : */
    {0x00,0x56,0x36,0x00,0x00}, /* ; */
    {0x00,0x08,0x14,0x22,0x41}, /* < */
    {0x14,0x14,0x14,0x14,0x14}, /* = */
    {0x41,0x22,0x14,0x08,0x00}, /* > */
    {0x02,0x01,0x51,0x09,0x06}, /* ? */
    {0x32,0x49,0x79,0x41,0x3E}, /* @ */
    {0x7E,0x11,0x11,0x11,0x7E}, /* A */
    {0x7F,0x49,0x49,0x49,0x36}, /* B */
    {0x3E,0x41,0x41,0x41,0x22}, /* C */
    {0x7F,0x41,0x41,0x22,0x1C}, /* D */
    {0x7F,0x49,0x49,0x49,0x41}, /* E */
    {0x7F,0x09,0x09,0x01,0x01}, /* F */
    {0x3E,0x41,0x41,0x51,0x32}, /* G */
    {0x7F,0x08,0x08,0x08,0x7F}, /* H */
    {0x00,0x41,0x7F,0x41,0x00}, /* I */
    {0x20,0x40,0x41,0x3F,0x01}, /* J */
    {0x7F,0x08,0x14,0x22,0x41}, /* K */
    {0x7F,0x40,0x40,0x40,0x40}, /* L */
    {0x7F,0x02,0x04,0x02,0x7F}, /* M */
    {0x7F,0x04,0x08,0x10,0x7F}, /* N */
    {0x3E,0x41,0x41,0x41,0x3E}, /* O */
    {0x7F,0x09,0x09,0x09,0x06}, /* P */
    {0x3E,0x41,0x51,0x21,0x5E}, /* Q */
    {0x7F,0x09,0x19,0x29,0x46}, /* R */
    {0x46,0x49,0x49,0x49,0x31}, /* S */
    {0x01,0x01,0x7F,0x01,0x01}, /* T */
    {0x3F,0x40,0x40,0x40,0x3F}, /* U */
    {0x1F,0x20,0x40,0x20,0x1F}, /* V */
    {0x7F,0x20,0x18,0x20,0x7F}, /* W */
    {0x63,0x14,0x08,0x14,0x63}, /* X */
    {0x03,0x04,0x78,0x04,0x03}, /* Y */
    {0x61,0x51,0x49,0x45,0x43}, /* Z */
    {0x00,0x00,0x7F,0x41,0x41}, /* [ */
    {0x02,0x04,0x08,0x10,0x20}, /* \ */
    {0x41,0x41,0x7F,0x00,0x00}, /* ] */
    {0x04,0x02,0x01,0x02,0x04}, /* ^ */
    {0x40,0x40,0x40,0x40,0x40}, /* _ */
    {0x00,0x01,0x02,0x04,0x00}, /* ` */
    {0x20,0x54,0x54,0x54,0x78}, /* a */
    {0x7F,0x48,0x44,0x44,0x38}, /* b */
    {0x38,0x44,0x44,0x44,0x20}, /* c */
    {0x38,0x44,0x44,0x48,0x7F}, /* d */
    {0x38,0x54,0x54,0x54,0x18}, /* e */
    {0x08,0x7E,0x09,0x01,0x02}, /* f */
    {0x08,0x14,0x54,0x54,0x3C}, /* g */
    {0x7F,0x08,0x04,0x04,0x78}, /* h */
    {0x00,0x44,0x7D,0x40,0x00}, /* i */
    {0x20,0x40,0x44,0x3D,0x00}, /* j */
    {0x00,0x7F,0x10,0x28,0x44}, /* k */
    {0x00,0x41,0x7F,0x40,0x00}, /* l */
    {0x7C,0x04,0x18,0x04,0x78}, /* m */
    {0x7C,0x08,0x04,0x04,0x78}, /* n */
    {0x38,0x44,0x44,0x44,0x38}, /* o */
    {0x7C,0x14,0x14,0x14,0x08}, /* p */
    {0x08,0x14,0x14,0x18,0x7C}, /* q */
    {0x7C,0x08,0x04,0x04,0x08}, /* r */
    {0x48,0x54,0x54,0x54,0x20}, /* s */
    {0x04,0x3F,0x44,0x40,0x20}, /* t */
    {0x3C,0x40,0x40,0x20,0x7C}, /* u */
    {0x1C,0x20,0x40,0x20,0x1C}, /* v */
    {0x3C,0x40,0x30,0x40,0x3C}, /* w */
    {0x44,0x28,0x10,0x28,0x44}, /* x */
    {0x0C,0x50,0x50,0x50,0x3C}, /* y */
    {0x44,0x64,0x54,0x4C,0x44}, /* z */
    {0x00,0x08,0x36,0x41,0x00}, /* { */
    {0x00,0x00,0x7F,0x00,0x00}, /* | */
    {0x00,0x41,0x36,0x08,0x00}, /* } */
    {0x08,0x08,0x2A,0x1C,0x08}, /* ~ */
    {0x08,0x1C,0x2A,0x08,0x08}, /* DEL */
};

static void fb_char(int x, int y, unsigned char c, unsigned short color, int s)
{
    if (c < 32 || c > 127) c = '?';
    const unsigned char *glyph = font5x7[c - 32];
    for (int col = 0; col < 5; col++) {
        unsigned char bits = glyph[col];
        for (int row = 0; row < 7; row++) {
            if (bits & (1 << row)) {
                if (s == 1)
                    fb_pixel(x + col, y + row, color);
                else
                    fb_rect(x + col*s, y + row*s, s, s, color);
            }
        }
    }
}

static void fb_text(int x, int y, const char *str, unsigned short color, int scale)
{
    while (*str) {
        fb_char(x, y, *str, color, scale);
        x += 6 * scale;
        str++;
    }
}

/* Convert 0xRRGGBB or raw RGB565 to RGB565 */
static unsigned short to_rgb565(unsigned int c)
{
    if (c <= 0xFFFF) return (unsigned short)c;  /* already RGB565 */
    /* 0xRRGGBB → RGB565 */
    unsigned int r = (c >> 16) & 0xFF;
    unsigned int g = (c >> 8) & 0xFF;
    unsigned int b = c & 0xFF;
    return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
}

/* === Lua bindings === */

static int l_open(lua_State *L)
{
    if (lcd_fd >= 0) close(lcd_fd);
    lcd_fd = open("/dev/lcd", O_RDWR);
    if (lcd_fd < 0) { lua_pushboolean(L, 0); return 1; }

    fb = mmap(NULL, FB_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, lcd_fd, 0);
    if (fb == MAP_FAILED) {
        fb = NULL;
        /* Fallback: allocate local buffer, use write() */
        close(lcd_fd); lcd_fd = -1;
        lua_pushboolean(L, 0);
        return 1;
    }
    lua_pushboolean(L, 1);
    return 1;
}

static int l_close(lua_State *L)
{
    if (fb) { munmap(fb, FB_SIZE); fb = NULL; }
    if (lcd_fd >= 0) { close(lcd_fd); lcd_fd = -1; }
    return 0;
}

static int l_touch(lua_State *L)
{
    int data[3] = {0, 0, 0};
    if (lcd_fd >= 0) ioctl(lcd_fd, 1, data);
    lua_pushinteger(L, data[0]);
    lua_pushinteger(L, data[1]);
    lua_pushinteger(L, data[2]);
    return 3;
}

static int l_flush(lua_State *L)
{
    if (lcd_fd >= 0) ioctl(lcd_fd, 0, 0);
    return 0;
}

static int l_backlight(lua_State *L)
{
    int on = lua_toboolean(L, 1);
    if (lcd_fd >= 0) ioctl(lcd_fd, 4, on ? 1 : 0);
    return 0;
}

static int l_splash(lua_State *L)
{
    if (lcd_fd >= 0) ioctl(lcd_fd, 4, 2);
    return 0;
}

static int l_usleep(lua_State *L)
{
    usleep(luaL_checkinteger(L, 1));
    return 0;
}

/* lcd.rect(x, y, w, h, color) */
static int l_rect(lua_State *L)
{
    if (!fb) return 0;
    int x = luaL_checkinteger(L, 1);
    int y = luaL_checkinteger(L, 2);
    int w = luaL_checkinteger(L, 3);
    int h = luaL_checkinteger(L, 4);
    unsigned short c = to_rgb565(luaL_checkinteger(L, 5));
    fb_rect(x, y, w, h, c);
    return 0;
}

/* lcd.text(x, y, str, color, scale) */
static int l_text(lua_State *L)
{
    if (!fb) return 0;
    int x = luaL_checkinteger(L, 1);
    int y = luaL_checkinteger(L, 2);
    const char *str = luaL_checkstring(L, 3);
    unsigned short c = to_rgb565(luaL_checkinteger(L, 4));
    int scale = luaL_optinteger(L, 5, 1);
    fb_text(x, y, str, c, scale);
    return 0;
}

/* lcd.pixel(x, y, color) */
static int l_pixel(lua_State *L)
{
    if (!fb) return 0;
    int x = luaL_checkinteger(L, 1);
    int y = luaL_checkinteger(L, 2);
    unsigned short c = to_rgb565(luaL_checkinteger(L, 3));
    fb_pixel(x, y, c);
    return 0;
}

/* lcd.clear(color) */
static int l_clear(lua_State *L)
{
    if (!fb) return 0;
    unsigned short c = to_rgb565(luaL_optinteger(L, 1, 0));
    if (c == 0) {
        memset(fb, 0, FB_SIZE);
    } else {
        for (int i = 0; i < LCD_W * LCD_H; i++) fb[i] = c;
    }
    return 0;
}

/* lcd.line(x0, y0, x1, y1, color) — Bresenham */
static int l_line(lua_State *L)
{
    if (!fb) return 0;
    int x0 = luaL_checkinteger(L, 1);
    int y0 = luaL_checkinteger(L, 2);
    int x1 = luaL_checkinteger(L, 3);
    int y1 = luaL_checkinteger(L, 4);
    unsigned short c = to_rgb565(luaL_checkinteger(L, 5));
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    while (1) {
        fb_pixel(x0, y0, c);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
    return 0;
}

static const struct luaL_Reg lcdlib[] = {
    {"open",      l_open},
    {"close",     l_close},
    {"touch",     l_touch},
    {"flush",     l_flush},
    {"backlight", l_backlight},
    {"splash",    l_splash},
    {"usleep",    l_usleep},
    {"rect",      l_rect},
    {"text",      l_text},
    {"pixel",     l_pixel},
    {"clear",     l_clear},
    {"line",      l_line},
    {NULL, NULL}
};

int luaopen_lcdlib(lua_State *L)
{
    luaL_register(L, "lcdlib", lcdlib);
    /* Export constants */
    lua_pushinteger(L, LCD_W); lua_setfield(L, -2, "W");
    lua_pushinteger(L, LCD_H); lua_setfield(L, -2, "H");
    return 1;
}
