/*
 * lcd_drv.ko — Минимальный LCD драйвер для ILI9341 на Almond 3S
 * Framebuffer в памяти + mmap для userspace + kernel thread для отрисовки
 *
 * /dev/lcd:
 *   mmap() — 320*240*2 = 153600 байт framebuffer (RGB565)
 *   write "flush" — принудительно отрисовать
 *   write "fps N" — установить fps (0=ручной flush)
 */

#include <linux/module.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/io.h>
#include <linux/delay.h>
#include <linux/uaccess.h>
#include <linux/kthread.h>
#include <linux/sched.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/i2c.h>
#include <linux/gpio.h>

#include "splash_4pda.h"
#include "pic_calib.h"

#define DEVICE_NAME  "lcd"
#define LCD_DRV_VERSION "0.37"
#define LCD_W        320
#define LCD_H        240
#define FB_SIZE      (LCD_W * LCD_H * 2)  /* RGB565 */

#define PALMBUS_BASE 0x1E000000
#define GPIOMODE_OFF 0x060
#define GPIO_DATA_OFF 0x600
#define GPIO_DIR_OFF  0x620

#define BIT_D0  (1u<<13)
#define BIT_D1  (1u<<18)
#define BIT_D2  (1u<<22)
#define BIT_D3  (1u<<23)
#define BIT_D4  (1u<<24)
#define BIT_D5  (1u<<25)
#define BIT_D6  (1u<<26)
#define BIT_D7  (1u<<27)
#define BIT_WRX (1u<<14)
#define BIT_RST (1u<<15)
#define BIT_CSX (1u<<16)
#define BIT_DCX (1u<<17)
#define BIT_BL  (1u<<31)

/* Mask of all LCD GPIO pins in bank0 — ONLY these may be touched */
#define LCD_PIN_MASK (BIT_D0|BIT_D1|BIT_D2|BIT_D3|BIT_D4|BIT_D5|BIT_D6|BIT_D7| \
                      BIT_WRX|BIT_RST|BIT_CSX|BIT_DCX|BIT_BL)

static void __iomem *gpio_base;
static u32 shadow_dir;
static u32 base_dir;           /* non-LCD DIR bits, preserved across writes */
static u8 *framebuffer;        /* kernel buffer */
static struct page **fb_pages; /* pages for mmap */
static int fb_npages;
static struct task_struct *render_thread;
static int target_fps = 0;  /* manual flush only — userspace controls refresh */
static int fb_dirty = 1;
static int splash_active = 1; /* demoscene animation until userspace takes over */

/* === GPIO bit-bang (exact U-Boot replica) === */

static inline void gw(u32 off, u32 v) { __raw_writel(v, gpio_base + off); }
static inline u32 gr(u32 off) { return __raw_readl(gpio_base + off); }

/*
 * Write GPIO DIR register preserving non-LCD pins.
 * Only LCD_PIN_MASK bits come from lcd_bits, rest from base_dir.
 * bb_lock: when set, skip DIR writes (bit-bang I2C in progress)
 */
static volatile int bb_lock;
static inline void gw_dir(u32 lcd_bits) {
    if (bb_lock) return;  /* bit-bang I2C owns DIR register */
    gw(GPIO_DIR_OFF, base_dir | (lcd_bits & LCD_PIN_MASK));
}

static void gpio_set_byte(u8 val)
{
    if (val & 0x01) shadow_dir |= BIT_D0; else shadow_dir &= ~BIT_D0;
    if (val & 0x02) shadow_dir |= BIT_D1; else shadow_dir &= ~BIT_D1;
    if (val & 0x04) shadow_dir |= BIT_D2; else shadow_dir &= ~BIT_D2;
    if (val & 0x08) shadow_dir |= BIT_D3; else shadow_dir &= ~BIT_D3;
    if (val & 0x10) shadow_dir |= BIT_D4; else shadow_dir &= ~BIT_D4;
    if (val & 0x20) shadow_dir |= BIT_D5; else shadow_dir &= ~BIT_D5;
    if (val & 0x40) shadow_dir |= BIT_D6; else shadow_dir &= ~BIT_D6;
    if (val & 0x80) shadow_dir |= BIT_D7; else shadow_dir &= ~BIT_D7;
    gw_dir(shadow_dir);
}

static void lcd_cmd(u8 cmd)
{
    shadow_dir |= BIT_CSX;  gw_dir(shadow_dir);
    shadow_dir |= BIT_WRX;  gw_dir(shadow_dir);
    shadow_dir |= BIT_DCX;  gw_dir(shadow_dir);
    shadow_dir &= ~BIT_WRX; gw_dir(shadow_dir);
    shadow_dir &= ~BIT_CSX; gw_dir(shadow_dir);
    gpio_set_byte(cmd);
    u32 a = shadow_dir & ~BIT_DCX;
    u32 b = a | BIT_DCX;
    u32 c = b | BIT_CSX;
    gw_dir(a);
    gw_dir(b);
    gw_dir(c);
    shadow_dir = c | BIT_WRX;
    gw_dir(shadow_dir);
}

static void lcd_dat(u8 dat)
{
    shadow_dir |= BIT_WRX;  gw_dir(shadow_dir);
    shadow_dir &= ~BIT_CSX; gw_dir(shadow_dir);
    gpio_set_byte(dat);
    u32 a = shadow_dir & ~BIT_DCX;
    gw_dir(a);
    a |= BIT_DCX; gw_dir(a);
    a |= BIT_CSX; shadow_dir = a;
    gw_dir(shadow_dir);
}

static void lcd_write_16d(u16 val)
{
    gpio_set_byte(val >> 8);
    shadow_dir &= ~BIT_DCX; gw_dir(shadow_dir);
    shadow_dir |= BIT_DCX;  gw_dir(shadow_dir);
    gpio_set_byte(val & 0xFF);
    shadow_dir &= ~BIT_DCX; gw_dir(shadow_dir);
    shadow_dir |= BIT_DCX;  gw_dir(shadow_dir);
}

static void lcd_write_mem(void)
{
    shadow_dir |= BIT_WRX; gw_dir(shadow_dir);
    shadow_dir |= BIT_CSX; gw_dir(shadow_dir);
    shadow_dir |= BIT_DCX; gw_dir(shadow_dir);
    gpio_set_byte(0x2C);
    shadow_dir &= ~BIT_WRX; shadow_dir &= ~BIT_CSX;
    gw_dir(shadow_dir);
    u32 a = shadow_dir & ~BIT_DCX;
    u32 b = a | BIT_DCX;
    gw_dir(shadow_dir);
    gw_dir(a);
    gw_dir(b);
    b |= BIT_WRX;
    u32 c = b | BIT_CSX;
    gw_dir(b);
    gw_dir(c);
    c &= ~BIT_CSX;
    shadow_dir = c;
    gw_dir(shadow_dir);
}

static void lcd_cs_deselect(void)
{
    shadow_dir |= BIT_CSX;
    gw_dir(shadow_dir);
}

/* === LCD Hardware Init === */

static void lcd_gpio_init(void)
{
    u32 data;
    /*
     * GPIOMODE is now configured via DTS pinctrl (jtag/wdt/rgmii2 → gpio).
     * DO NOT write GPIOMODE here — it would clobber MDIO and kill MT7530 LAN!
     * Old value 0x95A8 had bit 12 set = MDIO→GPIO = LAN dead.
     */

    /* Save non-LCD DIR bits to preserve other GPIO settings */
    base_dir = gr(GPIO_DIR_OFF) & ~LCD_PIN_MASK;

    data = gr(GPIO_DATA_OFF);
    data |= BIT_CSX; gw(GPIO_DATA_OFF, data);
    data |= BIT_RST; gw(GPIO_DATA_OFF, data);
    data |= BIT_DCX; gw(GPIO_DATA_OFF, data);
    data |= BIT_WRX; gw(GPIO_DATA_OFF, data);
    data |= BIT_D0;  gw(GPIO_DATA_OFF, data);
    data |= BIT_D1;  gw(GPIO_DATA_OFF, data);
    data |= BIT_D2;  gw(GPIO_DATA_OFF, data);
    data |= BIT_D3;  gw(GPIO_DATA_OFF, data);
    data |= BIT_D4;  gw(GPIO_DATA_OFF, data);
    data |= BIT_D5;  gw(GPIO_DATA_OFF, data);
    data |= BIT_D6;  gw(GPIO_DATA_OFF, data);
    data |= BIT_D7;  gw(GPIO_DATA_OFF, data);
    udelay(10);
    shadow_dir = 0;
}

static void lcd_hw_reset(void)
{
    udelay(100000);
    shadow_dir |= BIT_RST; gw_dir(shadow_dir); udelay(10000);
    shadow_dir &= ~BIT_RST; gw_dir(shadow_dir); udelay(10000);
    shadow_dir |= BIT_RST; gw_dir(shadow_dir); udelay(120000);
    shadow_dir |= BIT_CSX; gw_dir(shadow_dir);
    shadow_dir |= BIT_DCX; gw_dir(shadow_dir);
    udelay(5000);
    shadow_dir &= ~BIT_CSX; gw_dir(shadow_dir);
}

static void lcd_init_ili9341(void)
{
    lcd_cmd(0xCF); lcd_dat(0x00); lcd_dat(0xC1); lcd_dat(0x30);
    lcd_cmd(0xED); lcd_dat(0x64); lcd_dat(0x03); lcd_dat(0x12); lcd_dat(0x81);
    lcd_cmd(0xE8); lcd_dat(0x85); lcd_dat(0x00); lcd_dat(0x78);
    lcd_cmd(0xCB); lcd_dat(0x39); lcd_dat(0x2C); lcd_dat(0x00); lcd_dat(0x34); lcd_dat(0x02);
    lcd_cmd(0xF7); lcd_dat(0x20);
    lcd_cmd(0xEA); lcd_dat(0x00); lcd_dat(0x00);
    lcd_cmd(0xC0); lcd_dat(0x1B);
    lcd_cmd(0xC1); lcd_dat(0x11);
    lcd_cmd(0xC5); lcd_dat(0x3F); lcd_dat(0x3C);
    lcd_cmd(0xC7); lcd_dat(0x8E);
    lcd_cmd(0x36); lcd_dat(0xA8);
    lcd_cmd(0x3A); lcd_dat(0x55);
    lcd_cmd(0xB1); lcd_dat(0x00); lcd_dat(0x15);
    lcd_cmd(0xB6); lcd_dat(0x0A); lcd_dat(0xA2);
    lcd_cmd(0xF2); lcd_dat(0x00);
    lcd_cmd(0x26); lcd_dat(0x01);
    lcd_cmd(0xE0);
    lcd_dat(0x0F); lcd_dat(0x0C); lcd_dat(0x0B); lcd_dat(0x07);
    lcd_dat(0x09); lcd_dat(0x00); lcd_dat(0x41); lcd_dat(0x67);
    lcd_dat(0x37); lcd_dat(0x07); lcd_dat(0x12); lcd_dat(0x06);
    lcd_dat(0x0F); lcd_dat(0x09); lcd_dat(0x00);
    lcd_cmd(0xE1);
    lcd_dat(0x00); lcd_dat(0x0B); lcd_dat(0x0E); lcd_dat(0x03);
    lcd_dat(0x0F); lcd_dat(0x04); lcd_dat(0x2C); lcd_dat(0x16);
    lcd_dat(0x43); lcd_dat(0x02); lcd_dat(0x0B); lcd_dat(0x0A);
    lcd_dat(0x2F); lcd_dat(0x30); lcd_dat(0x0F);
    lcd_cmd(0x11); mdelay(120);
    lcd_cmd(0x29);
}

/* === Framebuffer → Display === */

static void lcd_flush_fb(void)
{
    u16 *pixels = (u16 *)framebuffer;
    int i;

    /* Refresh non-LCD DIR bits in case other drivers changed them */
    base_dir = gr(GPIO_DIR_OFF) & ~LCD_PIN_MASK;

    lcd_cmd(0x2A); lcd_dat(0); lcd_dat(0); lcd_dat(1); lcd_dat(0x3F);
    lcd_cmd(0x2B); lcd_dat(0); lcd_dat(0); lcd_dat(0); lcd_dat(0xEF);

    lcd_write_mem();
    for (i = 0; i < LCD_W * LCD_H; i++) {
        lcd_write_16d(pixels[i]);
        if ((i & 0x3FF) == 0) cond_resched(); /* yield every 1024 pixels */
    }
    lcd_cs_deselect();
}

/* === Demoscene animated splash (plasma + palette cycling) === */

/* Sine LUT: 256 entries, values 0-255 (fixed-point sin*127+128) */
static const u8 sin_lut[256] = {
    128,131,134,137,140,143,146,149,152,155,158,162,165,167,170,173,
    176,179,182,185,188,190,193,196,198,201,203,206,208,211,213,215,
    218,220,222,224,226,228,230,232,234,235,237,238,240,241,243,244,
    245,246,248,249,250,250,251,252,253,253,254,254,254,255,255,255,
    255,255,255,255,254,254,254,253,253,252,251,250,250,249,248,246,
    245,244,243,241,240,238,237,235,234,232,230,228,226,224,222,220,
    218,215,213,211,208,206,203,201,198,196,193,190,188,185,182,179,
    176,173,170,167,165,162,158,155,152,149,146,143,140,137,134,131,
    128,125,122,119,116,113,110,107,104,101,98,94,91,89,86,83,
    80,77,74,71,68,66,63,60,58,55,53,50,48,45,43,41,
    38,36,34,32,30,28,26,24,22,21,19,18,16,15,13,12,
    11,10,8,7,6,6,5,4,3,3,2,2,2,1,1,1,
    1,1,1,1,2,2,2,3,3,4,5,6,6,7,8,10,
    11,12,13,15,16,18,19,21,22,24,26,28,30,32,34,36,
    38,41,43,45,48,50,53,55,58,60,63,66,68,71,74,77,
    80,83,86,89,91,94,98,101,104,107,110,113,116,119,122,125,
};

/* HSV-like palette: hue cycling through RGB565 */
static u16 plasma_color(u8 val, u8 phase)
{
    u8 h = val + phase;  /* rotate hue */
    u8 r, g, b;
    u8 sector = h / 43;  /* 0-5 */
    u8 frac = (h % 43) * 6;

    switch (sector) {
    case 0:  r = 255;     g = frac;     b = 0;       break;
    case 1:  r = 255-frac; g = 255;     b = 0;       break;
    case 2:  r = 0;       g = 255;      b = frac;    break;
    case 3:  r = 0;       g = 255-frac; b = 255;     break;
    case 4:  r = frac;    g = 0;        b = 255;     break;
    default: r = 255;     g = 0;        b = 255-frac; break;
    }
    return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
}

/* === Scene 1: Plasma === */
static void scene_plasma(int t)
{
    u16 *fb = (u16 *)framebuffer;
    int x, y;
    u8 phase = (u8)(t * 7);

    for (y = 0; y < LCD_H; y++) {
        for (x = 0; x < LCD_W; x++) {
            u8 v = sin_lut[(x * 3 + t * 11) & 0xFF]
                 + sin_lut[(y * 5 + t * 7) & 0xFF]
                 + sin_lut[((x + y) * 2 + t * 3) & 0xFF]
                 + sin_lut[((x * x + y * y) / 64 + t * 5) & 0xFF];
            fb[y * LCD_W + x] = plasma_color(v, phase);
        }
    }
}

/* === Scene 2: Fire === */
static void scene_fire(int t)
{
    u16 *fb = (u16 *)framebuffer;
    int x, y;
    /* Fire: propagate heat upward, random sparks at bottom */
    /* Seed bottom row with random heat */
    for (x = 0; x < LCD_W; x++) {
        u8 spark = sin_lut[(x * 7 + t * 13) & 0xFF]
                 + sin_lut[(x * 3 + t * 37) & 0xFF];
        fb[(LCD_H - 1) * LCD_W + x] = spark > 200 ? 0xFFE0 : 0;
    }
    /* Propagate upward with cooling */
    for (y = 0; y < LCD_H - 1; y++) {
        for (x = 1; x < LCD_W - 1; x++) {
            /* Average of 3 pixels below + decay */
            u16 below = fb[(y + 1) * LCD_W + x - 1];
            u16 belowc = fb[(y + 1) * LCD_W + x];
            u16 belowr = fb[(y + 1) * LCD_W + x + 1];
            /* Extract red channel as heat (top 5 bits of RGB565) */
            int heat_val = ((below >> 11) + (belowc >> 11) * 2 + (belowr >> 11)) / 4;
            if (heat_val > 0) heat_val--;
            /* Heat to fire color: black→red→yellow→white */
            u8 r, g, b;
            if (heat_val > 24) { r = 31; g = 63; b = (heat_val - 24) * 4; }
            else if (heat_val > 12) { r = 31; g = (heat_val - 12) * 5; b = 0; }
            else { r = heat_val * 2; g = 0; b = 0; }
            fb[y * LCD_W + x] = (r << 11) | (g << 5) | b;
        }
    }
}

/* === Scene 3: Starfield 3D === */
#define NUM_STARS 200
static struct { int x, y, z; } stars[NUM_STARS];
static int stars_init;

static void scene_starfield(int t)
{
    u16 *fb = (u16 *)framebuffer;
    int i, sx, sy;

    if (!stars_init) {
        for (i = 0; i < NUM_STARS; i++) {
            stars[i].x = (sin_lut[(i * 7) & 0xFF] - 128) * 16;
            stars[i].y = (sin_lut[(i * 13 + 80) & 0xFF] - 128) * 12;
            stars[i].z = (sin_lut[(i * 3 + 40) & 0xFF]) + 1;
        }
        stars_init = 1;
    }

    memset(fb, 0, FB_SIZE);

    for (i = 0; i < NUM_STARS; i++) {
        stars[i].z -= 3;
        if (stars[i].z <= 0) {
            stars[i].x = (sin_lut[(t * 3 + i * 7) & 0xFF] - 128) * 16;
            stars[i].y = (sin_lut[(t * 5 + i * 13) & 0xFF] - 128) * 12;
            stars[i].z = 255;
        }
        sx = stars[i].x * 128 / (stars[i].z + 1) + LCD_W / 2;
        sy = stars[i].y * 128 / (stars[i].z + 1) + LCD_H / 2;
        if ((unsigned)sx < LCD_W && (unsigned)sy < LCD_H) {
            u8 bright = 255 - stars[i].z;
            u16 c = ((bright >> 3) << 11) | ((bright >> 2) << 5) | (bright >> 3);
            fb[sy * LCD_W + sx] = c;
            /* Bigger stars are closer */
            if (stars[i].z < 100 && sx + 1 < LCD_W)
                fb[sy * LCD_W + sx + 1] = c;
            if (stars[i].z < 50 && sy + 1 < LCD_H)
                fb[(sy + 1) * LCD_W + sx] = c;
        }
    }
}

/* === Scene 4: Interference / Moire === */
static void scene_interference(int t)
{
    u16 *fb = (u16 *)framebuffer;
    int x, y;
    /* Two moving center points */
    int cx1 = 160 + sin_lut[(t * 5) & 0xFF] / 2 - 64;
    int cy1 = 120 + sin_lut[(t * 7 + 64) & 0xFF] / 2 - 64;
    int cx2 = 160 + sin_lut[(t * 3 + 128) & 0xFF] / 2 - 64;
    int cy2 = 120 + sin_lut[(t * 4 + 192) & 0xFF] / 2 - 64;

    for (y = 0; y < LCD_H; y++) {
        for (x = 0; x < LCD_W; x++) {
            int dx1 = x - cx1, dy1 = y - cy1;
            int dx2 = x - cx2, dy2 = y - cy2;
            /* isqrt approximation: use sum of abs as cheap distance */
            int d1 = (dx1 * dx1 + dy1 * dy1) >> 5;
            int d2 = (dx2 * dx2 + dy2 * dy2) >> 5;
            u8 v = sin_lut[(d1 + t * 3) & 0xFF]
                 + sin_lut[(d2 + t * 5) & 0xFF];
            fb[y * LCD_W + x] = plasma_color(v, (u8)(t * 3));
        }
    }
}

/* === Scene 5: Rotozoom XOR === */
static void scene_rotozoom(int t)
{
    u16 *fb = (u16 *)framebuffer;
    int x, y;
    int angle = t * 4;
    int cosA = (int)sin_lut[(angle + 64) & 0xFF] - 128;
    int sinA = (int)sin_lut[angle & 0xFF] - 128;
    int zoom = sin_lut[(t * 3) & 0xFF] / 2 + 32;

    for (y = 0; y < LCD_H; y++) {
        for (x = 0; x < LCD_W; x++) {
            int cx = x - LCD_W / 2, cy = y - LCD_H / 2;
            int u = (cx * cosA - cy * sinA) / zoom + t * 2;
            int v = (cx * sinA + cy * cosA) / zoom + t * 3;
            u8 pattern = (u ^ v) & 0xFF;
            fb[y * LCD_W + x] = plasma_color(pattern, (u8)(t * 5));
        }
    }
}

/* === Logo overlay (4PDA from splash RLE, transparent black) === */
static void overlay_logo(void)
{
    u16 *fb = (u16 *)framebuffer;
    int i, j = 0;
    for (i = 0; i < SPLASH_RLE_LEN && j < LCD_W * LCD_H; i++) {
        int k;
        for (k = 0; k < splash_cnt[i] && j < LCD_W * LCD_H; k++, j++) {
            if (splash_clr[i] != 0x0000)  /* non-black = logo pixel */
                fb[j] = splash_clr[i];
        }
    }
}

/* === Scene 6: Dashboard Plasma — functional router visualization === */
/*
 * Each client has individual params:
 *   traffic_kbps: wave amplitude / "pressure" (heavy user = deep distortion)
 *   signal_dbm:   WiFi signal = distance from router (strong = close = center)
 *
 * Global: lte_rsrp = color palette, vpn_ms = pulsing rings
 */
#define MAX_DASH_CLIENTS 12

static struct {
    int num_clients;
    int lte_rsrp;       /* dBm, 0=no LTE */
    int vpn_ms;         /* -1=no tunnel */
    struct {
        int kbps;       /* traffic: amplitude of distortion */
        int signal;     /* WiFi dBm: -30(close) to -90(far) */
    } cl[MAX_DASH_CLIENTS];
} dash_params = { .lte_rsrp = -100, .vpn_ms = -1 };

/* LTE-based palette */
static u16 dash_color(u8 val, int rsrp, u8 phase)
{
    u8 h = val + phase;
    u8 r, g, b;

    if (rsrp == 0) {
        u8 gray = h >> 2;
        return ((gray >> 3) << 11) | ((gray >> 2) << 5) | (gray >> 3);
    }
    if (rsrp > -80) {
        r = h / 4; g = 128 + h / 2; b = 128 + h / 3;
    } else if (rsrp > -95) {
        r = h / 6; g = 64 + h / 2; b = 128 + h / 2;
    } else if (rsrp > -105) {
        r = 128 + h / 3; g = 96 + h / 4; b = h / 4;
    } else {
        r = 160 + h / 3; g = h / 4; b = h / 8;
    }
    return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
}

static void scene_dashboard(int t)
{
    u16 *fb = (u16 *)framebuffer;
    int x, y, i;
    int nc = dash_params.num_clients;
    int rsrp = dash_params.lte_rsrp;
    int vpn = dash_params.vpn_ms;

    /* Total traffic for global animation speed */
    int total_kbps = 0;
    for (i = 0; i < nc && i < MAX_DASH_CLIENTS; i++)
        total_kbps += dash_params.cl[i].kbps;
    int gspeed = total_kbps / 200 + 1;  /* global time scale */
    if (gspeed > 30) gspeed = 30;
    int ts = t * gspeed;
    u8 phase = (u8)(t * gspeed / 3);

    /* Precompute client centers + amplitude */
    int cx[MAX_DASH_CLIENTS], cy[MAX_DASH_CLIENTS], amp[MAX_DASH_CLIENTS], cspeed[MAX_DASH_CLIENTS];
    for (i = 0; i < nc && i < MAX_DASH_CLIENTS; i++) {
        /* Distance from center: signal -30=0px(center), -90=140px(edge) */
        int radius = ((-dash_params.cl[i].signal) - 30) * 140 / 60;
        if (radius < 0) radius = 0;
        if (radius > 140) radius = 140;
        /* Orbit angle: each client at different position, slowly drifting */
        int angle_idx = (i * 256 / (nc + 1) + t * 2) & 0xFF;
        cx[i] = 160 + ((int)sin_lut[angle_idx] - 128) * radius / 128;
        cy[i] = 120 + ((int)sin_lut[(angle_idx + 64) & 0xFF] - 128) * radius / 128;
        /* Amplitude: traffic → pressure (0..128) */
        amp[i] = dash_params.cl[i].kbps / 100;
        if (amp[i] > 128) amp[i] = 128;
        if (amp[i] < 5) amp[i] = 5;
        /* Individual speed: more traffic = faster local waves */
        cspeed[i] = dash_params.cl[i].kbps / 500 + 1;
        if (cspeed[i] > 20) cspeed[i] = 20;
    }

    for (y = 0; y < LCD_H; y++) {
        for (x = 0; x < LCD_W; x++) {
            /* Ambient base: gentle waves */
            int v = sin_lut[(x * 2 + ts / 3) & 0xFF]
                  + sin_lut[(y * 3 + ts / 4) & 0xFF];

            /* Each client: wave source with individual pressure + speed */
            for (i = 0; i < nc && i < MAX_DASH_CLIENTS; i++) {
                int dx = x - cx[i], dy = y - cy[i];
                int dist_sq = dx * dx + dy * dy;
                int dist = dist_sq >> 5;
                /* Wave from this client: freq based on distance, speed individual */
                int wave = sin_lut[(dist + t * cspeed[i]) & 0xFF];
                /* Pressure: amplitude falls off with distance (gravity well) */
                int falloff = 256 - (dist_sq >> 8);
                if (falloff < 0) falloff = 0;
                v += (wave * amp[i] * falloff) >> 15;
            }

            /* VPN tunnel: concentric rings */
            if (vpn >= 0) {
                int dx = x - 160, dy = y - 120;
                int dist = (dx * dx + dy * dy) >> 5;
                int rspeed = vpn < 10 ? 20 : (vpn < 50 ? 10 : (vpn < 200 ? 5 : 2));
                u8 ring = sin_lut[(dist - t * rspeed) & 0xFF];
                if (ring > 200)
                    v += (vpn < 50) ? 40 : 20;
            }

            /* Clamp */
            if (v < 0) v = 0;
            if (v > 255) v = 255;
            fb[y * LCD_W + x] = dash_color((u8)v, rsrp, phase);
        }
    }
}

/* Logo overlay with alpha blending (95% background, 5% logo) */
static void overlay_logo_alpha(void)
{
    u16 *fb = (u16 *)framebuffer;
    int i, j = 0;
    for (i = 0; i < SPLASH_RLE_LEN && j < LCD_W * LCD_H; i++) {
        int k;
        for (k = 0; k < splash_cnt[i] && j < LCD_W * LCD_H; k++, j++) {
            u16 logo = splash_clr[i];
            if (logo != 0x0000) {
                u16 bg = fb[j];
                /* 95%/5% blend in RGB565 */
                int br = (bg >> 11) & 0x1F, bg2 = (bg >> 5) & 0x3F, bb = bg & 0x1F;
                int lr = (logo >> 11) & 0x1F, lg = (logo >> 5) & 0x3F, lb = logo & 0x1F;
                int r = (br * 19 + lr) / 20;
                int g = (bg2 * 19 + lg) / 20;
                int b = (bb * 19 + lb) / 20;
                fb[j] = (r << 11) | (g << 5) | b;
            }
        }
    }
}

/* Scene dispatch */
#define NUM_SCENES 6
static int current_scene = -1;  /* -1 = random at boot */

static void render_scene(int scene, int t)
{
    switch (scene) {
    case 0: scene_plasma(t); break;
    case 1: scene_fire(t); break;
    case 2: scene_starfield(t); break;
    case 3: scene_interference(t); break;
    case 4: scene_rotozoom(t); break;
    case 5: scene_dashboard(t); break;
    default: scene_plasma(t); break;
    }
    if (scene == 5)
        overlay_logo_alpha();
    else
        overlay_logo();
}

/* Render thread */
static int render_fn(void *data)
{
    int frame = 0;

    /* Random scene at boot (based on jiffies) */
    if (current_scene < 0)
        current_scene = jiffies % NUM_SCENES;

    while (!kthread_should_stop()) {
        if (splash_active) {
            render_scene(current_scene, frame++);
            lcd_flush_fb();
            msleep(100); /* breathe — let network/SSH work */
            if (kthread_should_stop()) break;
        } else if (fb_dirty) {
            lcd_flush_fb();
            fb_dirty = 0;
        } else {
            msleep(50);
        }
    }
    return 0;
}

/* === /dev/lcd file operations === */

/* Raw write: записать пиксели напрямую в framebuffer */
static ssize_t lcd_fb_write(struct file *f, const char __user *buf,
                             size_t cnt, loff_t *p)
{
    loff_t pos = *p;

    splash_active = 0;  /* userspace took over — stop animation */

    if (pos >= FB_SIZE) return 0;
    if (pos + cnt > FB_SIZE) cnt = FB_SIZE - pos;

    if (copy_from_user(framebuffer + pos, buf, cnt))
        return -EFAULT;

    *p = pos + cnt;

    /* Если записан полный кадр или конец — отметить dirty */
    if (pos + cnt >= FB_SIZE)
        fb_dirty = 1;

    return cnt;
}

/* === SX8650 Touchscreen via palmbus I2C (SM0 direct) === */
/*
 * SX8650 requires SM0_CTL1=0x90644042 (raw master mode) for touch reads.
 * Linux I2C (SM0_CTL1=0x8064800E) returns FF for SELECT(X/Y) commands.
 * We save/restore SM0_CTL1 around each palmbus access to coexist with
 * the Linux i2c-mt7621 driver.
 */

#define SX8650_ADDR  0x48

/* SM0 I2C controller registers */
#define SM0_CFG     0x900
#define SM0_CFG2    0x928  /* bit 0 = auto mode enable */
#define SM0_DATA    0x908
#define SM0_DATAOUT 0x910
#define SM0_DATAIN  0x914
#define SM0_STATUS  0x91C
#define SM0_START   0x920
#define SM0_CTL1    0x940

/* New SM0 registers (kernel 6.12 i2c-mt7621) */
#define NEW_CTL0  0x940
#define NEW_CTL1  0x944
#define NEW_D0    0x950
#define NEW_D1    0x954
#define N_TRI     0x01
#define N_START   0x10
#define N_WRITE   0x20
#define N_STOP    0x30
#define N_READ_L  0x40
#define N_READ    0x50
#define N_PGLEN(x) ((((x)-1)<<8) & 0x700)

static int touch_x, touch_y;
static int touch_pressed;
static struct task_struct *touch_thread;
static struct i2c_adapter *touch_i2c_adap;

/* === PIC16 Battery via Linux I2C === */
#define PIC_ADDR  0x2A
#define PIC_BATTERY_LEN  17

static u8 pic_battery_raw[PIC_BATTERY_LEN];
static int pic_battery_valid;

static DEFINE_MUTEX(i2c_bus_mutex);  /* protects SM0 + I2C pins between touch and battery */

/* === GPIO bit-bang I2C for PIC ===
 * SM0 auto mode corrupts PIC on kernel 6.12!
 * Bypass SM0 entirely — bit-bang I2C via GPIO 3 (SDA) and GPIO 4 (SCL).
 * Uses DIR register: DIR=1+DATA=0 → drive LOW, DIR=0 → float HIGH (pull-up)
 * Must temporarily switch GPIOMODE to put I2C pins in GPIO mode.
 */
#define BB_SDA_GPIO  515  /* gpiochip0 base=512 + pin 3 */
#define BB_SCL_GPIO  516  /* gpiochip0 base=512 + pin 4 */
#define BB_DELAY 10  /* us, ~50kHz — slower for gpio API overhead */

static void bb_sda_low(void)  { gpio_direction_output(BB_SDA_GPIO, 0); udelay(1); }
static void bb_sda_high(void) { gpio_direction_input(BB_SDA_GPIO); udelay(1); }
static void bb_scl_low(void)  { gpio_direction_output(BB_SCL_GPIO, 0); udelay(BB_DELAY); }
static void bb_scl_high(void) {
    int timeout = 1000;
    gpio_direction_input(BB_SCL_GPIO);
    /* Wait for slave to release SCL (clock stretching support) */
    while (!gpio_get_value(BB_SCL_GPIO) && timeout--)
        udelay(1);
    udelay(BB_DELAY);
}
static int __maybe_unused bb_sda_read(void) { gpio_direction_input(BB_SDA_GPIO); udelay(2); return gpio_get_value(BB_SDA_GPIO); }

static void bb_i2c_start(void)
{
    bb_sda_high(); bb_scl_high(); udelay(BB_DELAY);
    bb_sda_low(); udelay(BB_DELAY);  /* SDA↓ while SCL HIGH = START */
    bb_scl_low();
}

static void bb_i2c_stop(void)
{
    bb_sda_low(); bb_scl_high(); udelay(BB_DELAY);
    bb_sda_high(); udelay(BB_DELAY);  /* SDA↑ while SCL HIGH = STOP */
}

static void bb_i2c_restart(void)
{
    /* Restart = Start without Stop. SCL is LOW after last byte. */
    bb_sda_high(); udelay(BB_DELAY);   /* SDA HIGH first */
    bb_scl_high(); udelay(BB_DELAY);   /* SCL HIGH (wait stretch) */
    bb_sda_low();  udelay(BB_DELAY);   /* SDA↓ while SCL HIGH = RESTART */
    bb_scl_low();                       /* SCL LOW — ready for address */
}

static int bb_i2c_write_byte(u8 byte)
{
    int i, ack;
    for (i = 7; i >= 0; i--) {
        if ((byte >> i) & 1) bb_sda_high(); else bb_sda_low();
        bb_scl_high(); bb_scl_low();
    }
    /* Read ACK: release SDA, slave pulls LOW = ACK */
    gpio_direction_input(BB_SDA_GPIO);
    udelay(5);
    bb_scl_high();
    udelay(5);
    ack = gpio_get_value(BB_SDA_GPIO);
    bb_scl_low();
    udelay(10);  /* settling time before next byte */
    return (ack == 0);  /* 1 = ACK received */
}

static u8 bb_i2c_read_byte(int send_ack)
{
    u8 byte = 0;
    int i;
    gpio_direction_input(BB_SDA_GPIO);
    udelay(5);

    for (i = 7; i >= 0; i--) {
        bb_scl_high();
        udelay(3);
        if (gpio_get_value(BB_SDA_GPIO)) byte |= (1 << i);
        bb_scl_low();

        /* On LAST data bit: set ACK IMMEDIATELY while SCL still LOW.
         * gpio_direction_output takes time through pinctrl —
         * start early so SDA is LOW before 9th SCL rises. */
        if (i == 0 && send_ack) {
            gpio_direction_output(BB_SDA_GPIO, 0);
            udelay(30);  /* LONG setup — ensure SDA physically LOW */
        } else {
            udelay(3);
        }
    }

    if (!send_ack) {
        /* NACK — ensure SDA HIGH */
        gpio_direction_input(BB_SDA_GPIO);
        udelay(10);
    }

    /* 9th clock — PIC samples ACK/NACK */
    bb_scl_high();
    udelay(5);
    bb_scl_low();
    udelay(5);

    /* Release SDA, wait for PIC to load next byte (clock stretch) */
    gpio_direction_input(BB_SDA_GPIO);
    udelay(20);
    return byte;
}

/* Acquire I2C GPIO pins — must also fix base_dir to prevent LCD gw_dir() overwrite */
static u32 bb_saved_gpiomode;

static int bb_sda_ok, bb_scl_ok;

static void bb_acquire(void)
{
    bb_saved_gpiomode = gr(GPIOMODE_OFF);
    bb_lock = 1;
    wmb();

    /* Switch pinmux to GPIO + request via kernel API */
    gw(GPIOMODE_OFF, bb_saved_gpiomode | (1 << 2));
    udelay(100);
    bb_sda_ok = (gpio_request(BB_SDA_GPIO, "bb_sda") == 0);
    bb_scl_ok = (gpio_request(BB_SCL_GPIO, "bb_scl") == 0);

    /* Both HIGH (input, pull-up) */
    gpio_direction_input(BB_SDA_GPIO);
    gpio_direction_input(BB_SCL_GPIO);
    udelay(200);

    pr_info("lcd_drv: BB acquire: sda_req=%d scl_req=%d SDA=%d SCL=%d\n",
            bb_sda_ok, bb_scl_ok,
            gpio_get_value(BB_SDA_GPIO), gpio_get_value(BB_SCL_GPIO));
}

static void bb_release(void)
{
    gpio_direction_input(BB_SDA_GPIO);
    gpio_direction_input(BB_SCL_GPIO);
    udelay(10);
    if (bb_sda_ok) gpio_free(BB_SDA_GPIO);
    if (bb_scl_ok) gpio_free(BB_SCL_GPIO);
    gw(GPIOMODE_OFF, bb_saved_gpiomode);
    udelay(50);
    bb_lock = 0;
    wmb();
}

/* Combined Write+Restart+Read — the correct I2C protocol for PIC.
 * Sends cmd bytes, then Restart, then reads response. */
static int __maybe_unused bb_pic_combined(const u8 *cmd, int cmd_len, u8 *resp, int resp_len)
{
    int i, ok = 1;

    bb_acquire();

    /* Write phase: START → addr+W → cmd bytes */
    bb_i2c_start();
    if (!bb_i2c_write_byte((PIC_ADDR << 1) | 0)) {
        pr_info("lcd_drv: BB combined: addr+W NACK\n");
        ok = 0; goto done;
    }
    for (i = 0; i < cmd_len; i++) {
        if (!bb_i2c_write_byte(cmd[i])) {
            pr_info("lcd_drv: BB combined: data[%d] NACK\n", i);
            ok = 0; goto done;
        }
    }

    /* RESTART (not Stop!) — PIC resets slave logic, ready for Read */
    bb_i2c_restart();

    /* Read phase: addr+R → read bytes → NACK last → STOP */
    if (!bb_i2c_write_byte((PIC_ADDR << 1) | 1)) {
        pr_info("lcd_drv: BB combined: addr+R NACK\n");
        ok = 0; goto done;
    }
    for (i = 0; i < resp_len; i++)
        resp[i] = bb_i2c_read_byte(i < resp_len - 1);  /* ACK all, NACK last */

done:
    bb_i2c_stop();
    bb_release();
    return ok;
}

/* Write buffer to PIC via bit-bang I2C. Returns 1 on success. */
static int __maybe_unused bb_pic_write(const u8 *data, int len)
{
    int i, ok = 1;

    bb_acquire();

    /* I2C transaction */
    bb_i2c_start();
    if (!bb_i2c_write_byte((PIC_ADDR << 1) | 0)) {  /* write address */
        pr_info("lcd_drv: BB PIC addr NACK!\n");
        ok = 0;
    }
    if (ok) {
        for (i = 0; i < len; i++) {
            if (!bb_i2c_write_byte(data[i])) {
                pr_info("lcd_drv: BB PIC data[%d] NACK!\n", i);
                ok = 0; break;
            }
        }
    }
    bb_i2c_stop();
    bb_release();

    return ok;
}

/* Read bytes from PIC via bit-bang I2C. Returns bytes read. */
static int __maybe_unused bb_pic_read(u8 *buf, int len)
{
    int i, ok = 1;

    bb_acquire();

    bb_i2c_start();
    if (!bb_i2c_write_byte((PIC_ADDR << 1) | 1)) {  /* read address */
        pr_info("lcd_drv: BB PIC read addr NACK!\n");
        ok = 0;
    }
    if (ok) {
        for (i = 0; i < len; i++)
            buf[i] = bb_i2c_read_byte(i < len - 1);  /* ACK all except last */
    }
    bb_i2c_stop();
    bb_release();

    return ok ? len : 0;
}

/* Palmbus I2C raw helpers */
static void i2c_raw_write(u8 val)
{
    gw(SM0_DATAOUT, val);
    gw(SM0_STATUS, 0);
    udelay(150);
    gw(SM0_START, 0);
    udelay(150);
}

static void i2c_raw_start(void)
{
    gw(SM0_DATA, SX8650_ADDR);
    gw(SM0_START, 0);
    udelay(150);
}

static void i2c_raw_stop(void)
{
    gw(SM0_STATUS, 2);
    udelay(150);
    gw(SM0_START, 0);
    udelay(150);
}

static void sx8650_hw_init(void)
{
    u32 saved_ctl1 = gr(SM0_CTL1);

    /* i2c adapter already obtained in early PIC read */

    /* SX8650 init via palmbus (needs SM0_CTL1=0x90644042) */
    gw(SM0_CTL1, 0x90644042);
    gw(0x928, 1);

    /* 1. Soft Reset */
    i2c_raw_start(); i2c_raw_write(0x1F); i2c_raw_write(0xDE); i2c_raw_stop();
    mdelay(50);

    /* 2. Registers from stock firmware */
    i2c_raw_start(); i2c_raw_write(0x00); i2c_raw_write(0x00); i2c_raw_stop(); udelay(150);
    i2c_raw_start(); i2c_raw_write(0x01); i2c_raw_write(0x27); i2c_raw_stop(); udelay(150);
    i2c_raw_start(); i2c_raw_write(0x02); i2c_raw_write(0x00); i2c_raw_stop(); udelay(150);
    i2c_raw_start(); i2c_raw_write(0x03); i2c_raw_write(0x2D); i2c_raw_stop(); udelay(150);
    i2c_raw_start(); i2c_raw_write(0x04); i2c_raw_write(0xC0); i2c_raw_stop(); udelay(150);

    /* 3. PenTrg mode */
    i2c_raw_start();
    gw(SM0_DATAOUT, 0x80); gw(SM0_STATUS, 2); udelay(150);
    gw(SM0_START, 0); udelay(150);
    i2c_raw_start();
    gw(SM0_DATAOUT, 0x90); gw(SM0_STATUS, 2); udelay(150);

    gw(SM0_CFG, 0xFA);

    /* Restore SM0_CTL1 for Linux I2C driver */
    gw(SM0_CTL1, saved_ctl1); udelay(10);

    pr_info("lcd_drv: SX8650 init done (palmbus + SM0 save/restore)\n");
}

/*
 * Read touch X/Y via palmbus direct (SM0_CTL1 saved/restored).
 * Format: [0|CHAN(2:0)|D(11:8)] [D(7:0)]
 */
static int sx8650_read_xy(int *rx, int *ry)
{
    int raw_x = 0, raw_y = 0;
    u8 h, l;
    u32 saved_ctl1 = gr(SM0_CTL1);

    /* --- Read X: SELECT(X)=0x80 --- */
    gw(SM0_CTL1, 0x90644042); udelay(10);
    gw(SM0_DATA, SX8650_ADDR);
    gw(SM0_START, 0); udelay(10);
    gw(SM0_DATAOUT, 0x80);
    gw(SM0_STATUS, 2); udelay(150);
    gw(SM0_START, 0); udelay(10);
    gw(SM0_DATAOUT, 0x91);
    gw(SM0_STATUS, 2); udelay(150);
    gw(SM0_CFG, 0xFA);
    gw(SM0_START, 0); udelay(10);
    gw(SM0_START, 1); gw(SM0_START, 1); udelay(10);
    gw(SM0_STATUS, 1); udelay(150);
    h = gr(SM0_DATAIN) & 0xFF; udelay(150);
    l = gr(SM0_DATAIN) & 0xFF; udelay(150);
    gw(SM0_START, 0); udelay(10);
    gw(SM0_START, 1);

    if (h != 0xFF) {
        int ch = (h >> 4) & 7;
        int val = ((h & 0x0F) << 8) | l;
        if (ch == 0) raw_x = val;
        if (ch == 1) raw_y = val;
    }

    /* --- Read Y: SELECT(Y)=0x81 --- */
    gw(SM0_CTL1, 0x90644042); udelay(10);
    gw(SM0_DATA, SX8650_ADDR);
    gw(SM0_START, 0); udelay(10);
    gw(SM0_DATAOUT, 0x81);
    gw(SM0_STATUS, 2); udelay(150);
    gw(SM0_START, 0); udelay(10);
    gw(SM0_DATAOUT, 0x91);
    gw(SM0_STATUS, 2); udelay(150);
    gw(SM0_CFG, 0xFA);
    gw(SM0_START, 0); udelay(10);
    gw(SM0_START, 1); gw(SM0_START, 1); udelay(10);
    gw(SM0_STATUS, 1); udelay(150);
    h = gr(SM0_DATAIN) & 0xFF; udelay(150);
    l = gr(SM0_DATAIN) & 0xFF; udelay(150);
    gw(SM0_START, 0); udelay(10);
    gw(SM0_START, 1);

    /* Restore SM0_CTL1 for Linux I2C driver */
    gw(SM0_CTL1, saved_ctl1); udelay(10);

    if (h != 0xFF) {
        int ch = (h >> 4) & 7;
        int val = ((h & 0x0F) << 8) | l;
        if (ch == 0) raw_x = val;
        if (ch == 1) raw_y = val;
    }

    if (raw_x > 0 || raw_y > 0) {
        *rx = (raw_y > 0) ? (4096 - raw_y) * 320 / 4096 : 160;
        *ry = (raw_x > 0) ? raw_x * 240 / 4096 : 120;
        return 1;
    }
    return 0;
}

/* === PIC16 I2C via Linux I2C subsystem === */

static int pic_i2c_read(u8 *buf, int len)
{
    struct i2c_msg msg = {
        .addr = PIC_ADDR,
        .flags = I2C_M_RD,
        .len = len,
        .buf = buf,
    };
    int ret;

    if (!touch_i2c_adap)
        return -ENODEV;

    ret = i2c_transfer(touch_i2c_adap, &msg, 1);
    if (ret < 0)
        return ret;
    return (ret == 1) ? 0 : -EIO;
}

static int __maybe_unused pic_i2c_write(u8 *data, int len)
{
    struct i2c_msg msg = {
        .addr = PIC_ADDR,
        .flags = 0,
        .len = len,
        .buf = data,
    };
    int ret;

    if (!touch_i2c_adap)
        return -ENODEV;

    ret = i2c_transfer(touch_i2c_adap, &msg, 1);
    if (ret < 0)
        return ret;
    return (ret == 1) ? 0 : -EIO;
}

/*
 * Read battery data from PIC16:
 * Try multiple approaches:
 * 1. Write {0x2F, 0x00, 0x02} then read
 * 2. Simple read (no command)
 */
static int __maybe_unused pic_read_battery(void)
{
    int ret;
    {
        u8 wake[3] = { 0x33, 0x00, 0x01 };
        pic_i2c_write(wake, 3);
        mdelay(50);
    }
    ret = pic_i2c_read(pic_battery_raw, PIC_BATTERY_LEN);
    if (!ret) {
        pic_battery_valid = 1;
    } else {
        pr_info("lcd_drv: PIC not responding (%d)\n", ret);
    }

    return 0;
}

/* Touch polling thread */
/*
 * Read PIC battery via new SM0 registers (0x944/0x950/0x954).
 * Called from touch thread with SM0 in known state (after touch restore).
 */
static void __maybe_unused pic_read_battery_palmbus(void)
{
    u8 resp[17] = {0};
    u32 saved_ctl1 = gr(SM0_CTL1);
    int i;

    /* EXACT stock kernel protocol (from IDA_DATA_TRACE.md):
     * SM0_DATA = 0x2A (kept from previous write — DO NOT change!)
     * SM0_START = count - 1 (NOT count!)
     * SM0_STATUS = 1 (READ mode)
     * Poll SM0_POLLSTA bit 0x04 (NOT 0x02!)
     * udelay(10) after ready
     * Read SM0_DATAIN */

    gw(SM0_CTL1, 0x90640042); udelay(10);  /* Stock CTL1 value */
    gw(SM0_DATA, PIC_ADDR);                 /* Ensure PIC addr set */

    gw(SM0_START, 8 - 1);                   /* count-1 = 7 for 8 bytes! */
    gw(SM0_STATUS, 1);                       /* READ mode */

    for (i = 0; i < 8; i++) {
        int p;
        /* Poll POLLSTA bit 0x04 (bit 2) — NOT 0x02! */
        for (p = 0; p < 100000; p++) {
            if (gr(0x918) & 0x04) break;
        }
        udelay(10);
        resp[i] = gr(SM0_DATAIN) & 0xFF;
    }

    /* Restore */
    gw(SM0_CTL1, saved_ctl1); udelay(10);

    /* Log and update battery data */
    /* Byte 0 = SM0 echo (0xFF), bytes 1+ = PIC data.
     * Bytes 2-3 = raw ADC value. Thresholds: <401=CRIT, 401-541=LOW, >=542=NORMAL */
    {
        int adc_raw = ((resp[2] & 0xFF) << 8) | (resp[3] & 0xFF);
        const char *level = (adc_raw < 401) ? "CRIT" : (adc_raw < 542) ? "LOW" : "OK";
        pr_info("lcd_drv: PIC BAT: ADC=%d (%s) [%02x %02x %02x %02x %02x %02x %02x %02x]\n",
                adc_raw, level,
                resp[0], resp[1], resp[2], resp[3],
                resp[4], resp[5], resp[6], resp[7]);

        if (resp[1] != 0x00 && resp[1] != 0xFF) {
            memcpy(pic_battery_raw, resp, 8);
            pic_battery_valid = 1;
        }
    }
}

static int touch_fn(void *data)
{
    int x, y, was_pressed = 0;
    int no_touch_count = 0;
    int battery_counter = 0;
    /* bat_write_counter removed — no writes in loop */

    while (!kthread_should_stop()) {

        /* Battery: i2c_transfer through PATCHED kernel i2c-mt7621.
         * Patch: byte-by-byte reads (supports PIC clock stretching).
         * Try all methods every cycle. */
        battery_counter++;
        if (battery_counter >= 100) {  /* every 5 sec */
            battery_counter = 0;

            if (touch_i2c_adap) {
                u8 resp[8] = {0};
                int ret;

                /* Method 1: i2c_transfer read-only 6 bytes */
                ret = pic_i2c_read(resp, 6);
                pr_info("lcd_drv: PIC i2c_rd6: ret=%d "
                        "[%02x %02x %02x %02x %02x %02x]\n", ret,
                        resp[0], resp[1], resp[2], resp[3],
                        resp[4], resp[5]);

                /* Method 2: i2c_transfer read 1 byte */
                {
                    u8 one[1] = {0};
                    int r1 = pic_i2c_read(one, 1);
                    pr_info("lcd_drv: PIC i2c_rd1: ret=%d [%02x]\n",
                            r1, one[0]);
                }

                /* Method 3: i2c_transfer write {2F} + read 6 (2 msgs) */
                {
                    u8 cmd = 0x2F;
                    u8 rd[6] = {0};
                    struct i2c_msg msgs[2] = {
                        { .addr = PIC_ADDR, .flags = 0,
                          .len = 1, .buf = &cmd },
                        { .addr = PIC_ADDR, .flags = I2C_M_RD,
                          .len = 6, .buf = rd },
                    };
                    int r2 = i2c_transfer(touch_i2c_adap, msgs, 2);
                    pr_info("lcd_drv: PIC i2c_wr1rd6: ret=%d "
                            "[%02x %02x %02x %02x %02x %02x]\n", r2,
                            rd[0], rd[1], rd[2], rd[3], rd[4], rd[5]);
                    if (r2 >= 0) memcpy(resp, rd, 6);
                }

                if (resp[0] != 0x00 && resp[0] != 0xFF &&
                    resp[0] != 0xAA && resp[0] != 0x54) {
                    memcpy(pic_battery_raw, resp, 6);
                    pic_battery_valid = 1;
                    pr_info("lcd_drv: *** PIC REAL DATA! ***\n");
                }
            }
        }

        /* Touch read DISABLED — no palmbus SM0 access, only i2c_transfer for PIC */
        if (0 && mutex_trylock(&i2c_bus_mutex)) {
            if (sx8650_read_xy(&x, &y)) {
                touch_x = x;
                touch_y = y;
                touch_pressed = 1;
                no_touch_count = 0;
                if (!was_pressed)
                    pr_info("lcd_drv: touch DOWN x=%d y=%d (raw data logged)\n", x, y);
                was_pressed = 1;
            } else {
                no_touch_count++;
                if (no_touch_count > 10 && was_pressed) {
                    touch_pressed = 0;
                    was_pressed = 0;
                    pr_info("lcd_drv: touch UP\n");
                }
            }
            mutex_unlock(&i2c_bus_mutex);
        }

        msleep(50);
    }
    return 0;
}

/* ioctl: 0=flush, 1=read touch, 2=read battery, 3=raw PIC read, 4=backlight */
static long lcd_ioctl(struct file *f, unsigned int cmd, unsigned long arg)
{
    if (cmd == 0) {
        splash_active = 0;  /* userspace flush — stop animation */
        fb_dirty = 1;
        return 0;
    }
    if (cmd == 1) {
        int data[3] = { touch_x, touch_y, touch_pressed };
        if (copy_to_user((void __user *)arg, data, sizeof(data)))
            return -EFAULT;
        return 0;
    }
    if (cmd == 2) {
        /* Return latest battery data from periodic palmbus read */
        if (copy_to_user((void __user *)arg, pic_battery_raw, PIC_BATTERY_LEN))
            return -EFAULT;
        return pic_battery_valid ? 0 : -ENODATA;
    }
    if (cmd == 3) {
        /* Raw PIC read (no write command, just read) */
        u8 buf[PIC_BATTERY_LEN];
        int ret = pic_i2c_read(buf, PIC_BATTERY_LEN);
        if (ret)
            return ret;
        pr_info("lcd_drv: PIC raw: %02x %02x %02x %02x %02x %02x %02x %02x "
                "%02x %02x %02x %02x %02x %02x %02x %02x %02x\n",
                buf[0], buf[1], buf[2], buf[3], buf[4], buf[5],
                buf[6], buf[7], buf[8], buf[9], buf[10], buf[11],
                buf[12], buf[13], buf[14], buf[15], buf[16]);
        if (copy_to_user((void __user *)arg, buf, PIC_BATTERY_LEN))
            return -EFAULT;
        return 0;
    }
    if (cmd == 4) {
        /* Backlight control: arg=0 off, arg=1 on, arg=2 show splash */
        if (arg == 2) {
            /* Redraw splash screen (4PDA logo) */
            u16 *fb16 = (u16 *)framebuffer;
            int i, j = 0;
            for (i = 0; i < SPLASH_RLE_LEN && j < LCD_W * LCD_H; i++) {
                int k;
                for (k = 0; k < splash_cnt[i] && j < LCD_W * LCD_H; k++)
                    fb16[j++] = splash_clr[i];
            }
            fb_dirty = 1;
            return 0;
        }
        /* Backlight control: arg=0 off, arg=1 on */
        if (arg)
            shadow_dir |= BIT_BL;
        else
            shadow_dir &= ~BIT_BL;
        gw_dir(shadow_dir);
        return 0;
    }
    if (cmd == 5) {
        /* Scene control: arg=0..5 select scene, arg=99 random, arg=100 stop */
        if (arg == 100) {
            splash_active = 0;
        } else {
            current_scene = (arg == 99) ? (jiffies % NUM_SCENES) : (arg % NUM_SCENES);
            splash_active = 1;
        }
        return 0;
    }
    if (cmd == 6) {
        /* Dashboard params: [nc, lte_rsrp, vpn_ms, kbps0, sig0, kbps1, sig1, ...] */
        int p[3 + MAX_DASH_CLIENTS * 2];
        int nc, i;
        if (copy_from_user(p, (void __user *)arg, sizeof(p)))
            return -EFAULT;
        nc = p[0];
        if (nc > MAX_DASH_CLIENTS) nc = MAX_DASH_CLIENTS;
        dash_params.num_clients = nc;
        dash_params.lte_rsrp = p[1];
        dash_params.vpn_ms = p[2];
        for (i = 0; i < nc; i++) {
            dash_params.cl[i].kbps = p[3 + i * 2];
            dash_params.cl[i].signal = p[3 + i * 2 + 1];
        }
        return 0;
    }
    return -ENOTTY;
}


/* mmap: map framebuffer to userspace (zero-copy rendering) */
static int lcd_mmap(struct file *f, struct vm_area_struct *vma)
{
    unsigned long size = vma->vm_end - vma->vm_start;
    int i;

    if (size > fb_npages * PAGE_SIZE)
        return -EINVAL;

    for (i = 0; i < fb_npages && i * PAGE_SIZE < size; i++) {
        if (vm_insert_page(vma, vma->vm_start + i * PAGE_SIZE, fb_pages[i]))
            return -EAGAIN;
    }
    return 0;
}

static const struct file_operations lcd_fops = {
    .owner          = THIS_MODULE,
    .write          = lcd_fb_write,
    .llseek         = default_llseek,
    .unlocked_ioctl = lcd_ioctl,
    .mmap           = lcd_mmap,
};

static struct miscdevice lcd_dev = {
    .minor = MISC_DYNAMIC_MINOR,
    .name  = DEVICE_NAME,
    .fops  = &lcd_fops,
    .mode  = 0666,
};

/* === Module init/exit === */

static int __init lcd_drv_init(void)
{
    int ret, i;

    gpio_base = ioremap(PALMBUS_BASE, 0x1000);
    if (!gpio_base) return -ENOMEM;

    /* Allocate framebuffer */
    fb_npages = (FB_SIZE + PAGE_SIZE - 1) / PAGE_SIZE;
    fb_pages = kmalloc(fb_npages * sizeof(struct page *), GFP_KERNEL);
    if (!fb_pages) { iounmap(gpio_base); return -ENOMEM; }

    framebuffer = vzalloc(fb_npages * PAGE_SIZE);
    if (!framebuffer) { kfree(fb_pages); iounmap(gpio_base); return -ENOMEM; }

    for (i = 0; i < fb_npages; i++)
        fb_pages[i] = vmalloc_to_page(framebuffer + i * PAGE_SIZE);

    /* Register device */
    ret = misc_register(&lcd_dev);
    if (ret) { kfree(framebuffer); kfree(fb_pages); iounmap(gpio_base); return ret; }

    /* Init LCD hardware */
    lcd_gpio_init();
    lcd_hw_reset();
    lcd_init_ili9341();
    shadow_dir |= BIT_BL; gw_dir(shadow_dir);

    /* First scene frame + logo — render thread continues animation */
    current_scene = jiffies % NUM_SCENES;
    render_scene(current_scene, 0);
    lcd_flush_fb();

    /* === VERY FIRST: Read PIC BEFORE any SM0 modification ===
     * Get i2c adapter first, then read PIC while SM0 is untouched. */
    touch_i2c_adap = i2c_get_adapter(0);
    if (!touch_i2c_adap)
        pr_warn("lcd_drv: cannot get I2C adapter 0\n");

    {
        u8 early_buf[8] = {0};
        int early_ret = -1;

        pr_info("lcd_drv: EARLY SM0: DATA=0x%08X D0=0x%08X D1=0x%08X "
                "DIN=0x%08X CTL0=0x%08X CTL1=0x%08X CFG=0x%08X "
                "POLL=0x%08X START=0x%08X STAT=0x%08X CFG2=0x%08X\n",
                gr(SM0_DATA), gr(0x950), gr(0x954), gr(SM0_DATAIN),
                gr(0x940), gr(SM0_CTL1), gr(SM0_CFG),
                gr(0x918), gr(0x920), gr(SM0_STATUS), gr(0x928));

        /* Try kernel i2c read — SM0 still in kernel i2c-mt7621 state */
        if (touch_i2c_adap) {
            early_ret = pic_i2c_read(early_buf, 8);
            pr_info("lcd_drv: EARLY i2c_read: ret=%d "
                    "[%02x %02x %02x %02x %02x %02x %02x %02x]\n",
                    early_ret,
                    early_buf[0], early_buf[1], early_buf[2], early_buf[3],
                    early_buf[4], early_buf[5], early_buf[6], early_buf[7]);
        }

        /* D0/D1 after i2c_read — did kernel i2c write new data? */
        pr_info("lcd_drv: AFTER-READ D0=0x%08X D1=0x%08X DIN=0x%08X\n",
                gr(0x950), gr(0x954), gr(SM0_DATAIN));
    }

    /* SX8650 touchscreen init */
    sx8650_hw_init();

    /* PIC16 battery init — IDA deep analysis protocol (2026-03-19)
     * 1. SM0 RSTCTRL hardware reset
     * 2. SM0_CTL1 = 0x90640042 (NOT 0x90644042!)
     * 3. WAKE {0x33, 0x00, 0x00} — no calibration
     * 4. bat_read {0x2F, 0x00, 0x01} — POLLING mode (not 0x02!)
     * 5. Wait 500ms
     * 6. SM0 read (SEPARATE transaction, not Combined!)
     */
    {
        int ok;
        u8 wake_cmd[3] = { 0x33, 0x00, 0x00 };
        u8 bat_cmd[3]  = { 0x2F, 0x00, 0x01 };

        pr_info("lcd_drv: PIC init v2 (IDA protocol)...\n");

        /* GPIOMODE trick: stock U-Boot sets 0x95A8 (I2C→GPIO mode)
         * then stock kernel switches back to I2C mode.
         * This GPIO→I2C transition may be required for SM0 to work with PIC.
         * Replicate: set I2C pins to GPIO mode, delay, switch back to I2C. */
        {
            u32 gmode = gr(GPIOMODE_OFF);
            pr_info("lcd_drv: GPIOMODE before: 0x%08X\n", gmode);

            /* Set bit 2 = I2C→GPIO mode (like stock U-Boot 0x95A8) */
            gw(GPIOMODE_OFF, gmode | (1 << 2));
            udelay(1000);
            pr_info("lcd_drv: GPIOMODE GPIO mode: 0x%08X\n", gr(GPIOMODE_OFF));

            /* Switch back to I2C peripheral mode */
            gw(GPIOMODE_OFF, gmode & ~(1 << 2));
            udelay(1000);
            pr_info("lcd_drv: GPIOMODE I2C mode: 0x%08X\n", gr(GPIOMODE_OFF));
        }

        /* FIRST: Read D0/D1 BEFORE any reset — kernel i2c probe may have data */
        pr_info("lcd_drv: PRE-RESET D0=0x%08X D1=0x%08X DIN=0x%08X "
                "CTL0=0x%08X CTL1=0x%08X POLL=0x%08X\n",
                gr(0x950), gr(0x954), gr(SM0_DATAIN),
                gr(0x940), gr(SM0_CTL1), gr(0x918));

        /* EXPERIMENT: SM0_CFG write + NEW manual mode PIC read */
        {
            u32 cfg_before, cfg_after;
            u8 resp[8] = {0};
            int i;

            /* Test 1: SM0_CFG write — try MANY approaches */
            cfg_before = gr(SM0_CFG);

            /* 1a. Direct write */
            gw(SM0_CFG, 0xFA);
            cfg_after = gr(SM0_CFG);
            pr_info("lcd_drv: CFG direct: 0x%08X→0x%08X\n", cfg_before, cfg_after);

            /* 1b. With RSTCTRL reset first (like stock kernel!) */
            {
                u32 rst = gr(0x034);
                gw(0x034, rst | 0x10000);  /* assert I2C reset */
                udelay(10);
                gw(0x034, rst & ~0x10000); /* deassert */
                udelay(500);
            }
            gw(SM0_CFG, 0xFA);
            pr_info("lcd_drv: CFG after RSTCTRL: 0x%08X\n", gr(SM0_CFG));

            /* 1c. Try CTL1=stock FIRST, then CFG */
            gw(SM0_CTL1, 0x90640042);
            udelay(10);
            gw(SM0_CFG, 0xFA);
            pr_info("lcd_drv: CFG after CTL1=stock: 0x%08X\n", gr(SM0_CFG));

            /* 1d. Try CFG2=1 (auto), then CFG */
            gw(SM0_CFG2, 1);
            gw(SM0_CFG, 0xFA);
            pr_info("lcd_drv: CFG after CFG2=1: 0x%08X\n", gr(SM0_CFG));

            /* 1e. Try writing as 8-bit via iowrite8 */
            {
                void __iomem *p = ioremap(0x1E000900, 4);
                if (p) {
                    iowrite8(0xFA, p);
                    pr_info("lcd_drv: CFG after iowrite8: 0x%08X\n", gr(SM0_CFG));
                    iounmap(p);
                }
            }

            /* 1f. Try writing 32-bit with different values */
            {
                u32 vals[] = {0xFA, 0x000000FA, 0xFA000000, 0x00FA0000, 0x0000FA00};
                int j;
                for (j = 0; j < 5; j++) {
                    gw(SM0_CFG, vals[j]);
                    if (gr(SM0_CFG) != 0)
                        pr_info("lcd_drv: CFG=0x%08X → 0x%08X !!!\n", vals[j], gr(SM0_CFG));
                }
            }

            pr_info("lcd_drv: SM0_CFG final: 0x%08X (all attempts done)\n", gr(SM0_CFG));

            /* Test 2: NEW manual mode read (N_CTL1 commands)
             * This is a different HW path than OLD auto mode.
             * i2c-mt7621 kernel driver uses this mode. */
            gw(SM0_CFG2, 0);  /* manual mode */
            gw(0x940, 0x01F3800F);  /* kernel default CTL0 */
            udelay(100);

            /* Write bat_read via NEW manual mode */
            gw(0x950, (PIC_ADDR << 1) | 0);  /* N_D0 = write addr */
            gw(0x944, 0x11);  /* START */
            for (i = 0; i < 10000; i++) if (gr(0x918) & 0x01) break;

            gw(0x950, (PIC_ADDR << 1) | 0);
            gw(0x944, 0x21);  /* WRITE addr */
            for (i = 0; i < 10000; i++) if (gr(0x918) & 0x01) break;

            gw(0x950, 0x2F);
            gw(0x944, 0x21);  /* WRITE 0x2F */
            for (i = 0; i < 10000; i++) if (gr(0x918) & 0x01) break;

            gw(0x950, 0x00);
            gw(0x944, 0x21);  /* WRITE 0x00 */
            for (i = 0; i < 10000; i++) if (gr(0x918) & 0x01) break;

            gw(0x950, 0x01);
            gw(0x944, 0x21);  /* WRITE 0x01 */
            for (i = 0; i < 10000; i++) if (gr(0x918) & 0x01) break;

            gw(0x944, 0x31);  /* STOP */
            for (i = 0; i < 10000; i++) if (gr(0x918) & 0x01) break;

            pr_info("lcd_drv: NEW manual write bat_read done\n");
            mdelay(500);

            /* Read via NEW manual mode */
            gw(0x944, 0x11);  /* START */
            for (i = 0; i < 10000; i++) if (gr(0x918) & 0x01) break;

            gw(0x950, (PIC_ADDR << 1) | 1);  /* read addr */
            gw(0x944, 0x21);  /* WRITE read-addr */
            for (i = 0; i < 10000; i++) if (gr(0x918) & 0x01) break;

            for (i = 0; i < 8; i++) {
                int p;
                gw(0x944, (i < 7) ? 0x51 : 0x41);  /* READ / READ_LAST */
                for (p = 0; p < 10000; p++) if (gr(0x918) & 0x01) break;
                resp[i] = gr(0x950) & 0xFF;
            }

            gw(0x944, 0x31);  /* STOP */
            for (i = 0; i < 10000; i++) if (gr(0x918) & 0x01) break;

            pr_info("lcd_drv: NEW manual read: "
                    "[%02x %02x %02x %02x %02x %02x %02x %02x]\n",
                    resp[0], resp[1], resp[2], resp[3],
                    resp[4], resp[5], resp[6], resp[7]);

            gw(SM0_CFG2, 1);  /* restore auto mode */
        }

        /* Step 1: SM0 RSTCTRL hardware reset (from IDA sub_411770) */
        {
            u32 rst = gr(0x034);
            gw(0x034, rst | 0x10000);    /* Assert I2C reset */
            udelay(10);
            gw(0x034, rst & ~0x10000);   /* Deassert I2C reset */
            udelay(500);
        }

        /* Step 2: SM0 init — CORRECT value from IDA! */
        gw(SM0_CTL1, 0x90640042);       /* NOT 0x90644042! bit 14 must be 0 */
        udelay(10);
        gw(SM0_CFG2, 0x01);             /* auto mode ON (0x928 = debug/enable) */
        gw(0x90C, 0);                   /* SM0_SLAVE = 0 */
        pr_info("lcd_drv: SM0 reset done, CTL1=0x%08X\n", gr(SM0_CTL1));

        /* Step 3: WAKE {0x33, 0x00, 0x00} — no calibration, count=0 */
        ok = bb_pic_write(wake_cmd, 3);
        pr_info("lcd_drv: PIC WAKE {33,00,00}: %s\n", ok ? "ACK" : "NACK");
        mdelay(5);

        /* Step 4: Empty calibration tables {0x2D} + {0x2E} — REQUIRED!
         * Stock kernel sends these even with count=0.
         * Without them PIC doesn't enter battery monitoring mode.
         * With them PIC plays startup melody = enters monitoring! */
        {
            u8 tab1[1] = { 0x2D };
            u8 tab2[1] = { 0x2E };
            ok = bb_pic_write(tab1, 1);
            pr_info("lcd_drv: PIC Table1 {2D}: %s\n", ok ? "ACK" : "NACK");
            mdelay(5);
            ok = bb_pic_write(tab2, 1);
            pr_info("lcd_drv: PIC Table2 {2E}: %s\n", ok ? "ACK" : "NACK");
            mdelay(5);
        }

        /* Step 5: Stop melody immediately */
        {
            u8 buz_off[3] = { 0x34, 0x00, 0x00 };
            ok = bb_pic_write(buz_off, 3);
            pr_info("lcd_drv: PIC buzzer off: %s\n", ok ? "ACK" : "NACK");
            mdelay(5);
        }

        /* Step 6: bat_read via bit-bang */
        ok = bb_pic_write(bat_cmd, 3);
        pr_info("lcd_drv: PIC bat_read {2F,00,01} bb: %s\n", ok ? "ACK" : "NACK");
        mdelay(500);

        /* Step 7: SM0 auto mode WRITE bat_read (THIS triggers live ADC!)
         * v0.28 proved: SM0 auto write = live ADC (591/423).
         * bit-bang write = ACK but static ADC.
         * SM0 auto write has different I2C timing that PIC needs. */
        {
            gw(SM0_DATA, PIC_ADDR);
            gw(SM0_START, 3);
            gw(SM0_DATAOUT, 0x2F);
            gw(SM0_STATUS, 0);  /* WRITE mode */
            { int p; for (p = 0; p < 100000; p++) { if (gr(0x918) & 0x02) break; } }
            udelay(5000);
            gw(SM0_DATAOUT, 0x00);
            { int p; for (p = 0; p < 100000; p++) { if (gr(0x918) & 0x02) break; } }
            udelay(5000);
            gw(SM0_DATAOUT, 0x01);
            { int p; for (p = 0; p < 100000; p++) { if (gr(0x918) & 0x01) break; } }
            pr_info("lcd_drv: PIC bat_read SM0 auto write: POLLSTA=0x%02X\n", gr(0x918));
        }
        mdelay(500);

        /* Step 8: SM0 auto mode READ */
        pr_info("lcd_drv: PIC SM0 read after bat_read...\n");
        pic_read_battery_palmbus();
    }

    /* Start render thread */
    render_thread = kthread_run(render_fn, NULL, "lcd_render");

    /* Start touch+battery thread (touch reads disabled, battery via i2c_transfer) */
    touch_thread = kthread_run(touch_fn, NULL, "lcd_touch");
    pr_info("lcd_drv: touch thread started (touch palmbus DISABLED)\n");

    pr_info("lcd_drv v%s: /dev/lcd ready (fb=%dx%d, %d bytes, fps=%d)\n",
            LCD_DRV_VERSION, LCD_W, LCD_H, FB_SIZE, target_fps);
    return 0;
}

static void __exit lcd_drv_exit(void)
{
    if (touch_thread) kthread_stop(touch_thread);
    if (render_thread) kthread_stop(render_thread);
    if (touch_i2c_adap) i2c_put_adapter(touch_i2c_adap);
    misc_deregister(&lcd_dev);
    vfree(framebuffer);
    kfree(fb_pages);
    if (gpio_base) iounmap(gpio_base);
    pr_info("lcd_drv: unloaded\n");
}

module_init(lcd_drv_init);
module_exit(lcd_drv_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("ILI9341 framebuffer driver for Almond 3S");
