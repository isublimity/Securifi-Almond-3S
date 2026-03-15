/*
 * lcdlib.so — Lua C module for direct LCD access (zero fork)
 *
 * Provides:
 *   lcd.open()           — open /dev/lcd
 *   lcd.touch()          — read touch (x, y, pressed) via ioctl, ~0.1ms
 *   lcd.usleep(us)       — sleep microseconds (no fork)
 *   lcd.write(data)      — write framebuffer data
 *   lcd.close()          — close
 *
 * Build: zig cc -target mipsel-linux-musleabi -O2 -shared -o lcdlib.so lcdlib.c
 * Usage: local lcd = require("lcdlib"); lcd.open(); x,y,p = lcd.touch()
 */
#include <lua.h>
#include <lauxlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <string.h>

static int lcd_fd = -1;

static int l_open(lua_State *L)
{
    if (lcd_fd >= 0) close(lcd_fd);
    lcd_fd = open("/dev/lcd", O_RDWR);
    lua_pushboolean(L, lcd_fd >= 0);
    return 1;
}

static int l_close(lua_State *L)
{
    if (lcd_fd >= 0) { close(lcd_fd); lcd_fd = -1; }
    return 0;
}

static int l_touch(lua_State *L)
{
    int data[3] = {0, 0, 0};
    if (lcd_fd >= 0)
        ioctl(lcd_fd, 1, data);
    lua_pushinteger(L, data[0]);
    lua_pushinteger(L, data[1]);
    lua_pushinteger(L, data[2]);
    return 3;
}

static int l_usleep(lua_State *L)
{
    int us = luaL_checkinteger(L, 1);
    usleep(us);
    return 0;
}

/* backlight(0) = off, backlight(1) = on */
static int l_backlight(lua_State *L)
{
    int on = lua_toboolean(L, 1);
    if (lcd_fd >= 0)
        ioctl(lcd_fd, 4, on ? 1 : 0);
    return 0;
}

/* splash() — show 4PDA logo from kernel (bitmap, instant) */
static int l_splash(lua_State *L)
{
    if (lcd_fd >= 0)
        ioctl(lcd_fd, 4, 2);
    return 0;
}

static const struct luaL_Reg lcdlib[] = {
    {"open",      l_open},
    {"close",     l_close},
    {"touch",     l_touch},
    {"usleep",    l_usleep},
    {"backlight", l_backlight},
    {"splash",    l_splash},
    {NULL, NULL}
};

int luaopen_lcdlib(lua_State *L)
{
    luaL_register(L, "lcdlib", lcdlib);
    return 1;
}
