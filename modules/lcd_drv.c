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
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/i2c.h>

#include "splash_4pda.h"

#define DEVICE_NAME  "lcd"
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

static void __iomem *gpio_base;
static u32 shadow_dir;
static u8 *framebuffer;        /* kernel buffer */
static struct page **fb_pages; /* pages for mmap */
static int fb_npages;
static struct task_struct *render_thread;
static int target_fps = 10;
static int fb_dirty = 1;

/* === GPIO bit-bang (exact U-Boot replica) === */

static inline void gw(u32 off, u32 v) { __raw_writel(v, gpio_base + off); }
static inline u32 gr(u32 off) { return __raw_readl(gpio_base + off); }

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
    gw(GPIO_DIR_OFF, shadow_dir);
}

static void lcd_cmd(u8 cmd)
{
    shadow_dir |= BIT_CSX;  gw(GPIO_DIR_OFF, shadow_dir);
    shadow_dir |= BIT_WRX;  gw(GPIO_DIR_OFF, shadow_dir);
    shadow_dir |= BIT_DCX;  gw(GPIO_DIR_OFF, shadow_dir);
    shadow_dir &= ~BIT_WRX; gw(GPIO_DIR_OFF, shadow_dir);
    shadow_dir &= ~BIT_CSX; gw(GPIO_DIR_OFF, shadow_dir);
    gpio_set_byte(cmd);
    u32 a = shadow_dir & ~BIT_DCX;
    u32 b = a | BIT_DCX;
    u32 c = b | BIT_CSX;
    gw(GPIO_DIR_OFF, a);
    gw(GPIO_DIR_OFF, b);
    gw(GPIO_DIR_OFF, c);
    shadow_dir = c | BIT_WRX;
    gw(GPIO_DIR_OFF, shadow_dir);
}

static void lcd_dat(u8 dat)
{
    shadow_dir |= BIT_WRX;  gw(GPIO_DIR_OFF, shadow_dir);
    shadow_dir &= ~BIT_CSX; gw(GPIO_DIR_OFF, shadow_dir);
    gpio_set_byte(dat);
    u32 a = shadow_dir & ~BIT_DCX;
    gw(GPIO_DIR_OFF, a);
    a |= BIT_DCX; gw(GPIO_DIR_OFF, a);
    a |= BIT_CSX; shadow_dir = a;
    gw(GPIO_DIR_OFF, shadow_dir);
}

static void lcd_write_16d(u16 val)
{
    gpio_set_byte(val >> 8);
    shadow_dir &= ~BIT_DCX; gw(GPIO_DIR_OFF, shadow_dir);
    shadow_dir |= BIT_DCX;  gw(GPIO_DIR_OFF, shadow_dir);
    gpio_set_byte(val & 0xFF);
    shadow_dir &= ~BIT_DCX; gw(GPIO_DIR_OFF, shadow_dir);
    shadow_dir |= BIT_DCX;  gw(GPIO_DIR_OFF, shadow_dir);
}

static void lcd_write_mem(void)
{
    shadow_dir |= BIT_WRX; gw(GPIO_DIR_OFF, shadow_dir);
    shadow_dir |= BIT_CSX; gw(GPIO_DIR_OFF, shadow_dir);
    shadow_dir |= BIT_DCX; gw(GPIO_DIR_OFF, shadow_dir);
    gpio_set_byte(0x2C);
    shadow_dir &= ~BIT_WRX; shadow_dir &= ~BIT_CSX;
    gw(GPIO_DIR_OFF, shadow_dir);
    u32 a = shadow_dir & ~BIT_DCX;
    u32 b = a | BIT_DCX;
    gw(GPIO_DIR_OFF, shadow_dir);
    gw(GPIO_DIR_OFF, a);
    gw(GPIO_DIR_OFF, b);
    b |= BIT_WRX;
    u32 c = b | BIT_CSX;
    gw(GPIO_DIR_OFF, b);
    gw(GPIO_DIR_OFF, c);
    c &= ~BIT_CSX;
    shadow_dir = c;
    gw(GPIO_DIR_OFF, shadow_dir);
}

static void lcd_cs_deselect(void)
{
    shadow_dir |= BIT_CSX;
    gw(GPIO_DIR_OFF, shadow_dir);
}

/* === LCD Hardware Init === */

static void lcd_gpio_init(void)
{
    u32 data;
    gw(GPIOMODE_OFF, 0x95A8);
    udelay(10);
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
    shadow_dir |= BIT_RST; gw(GPIO_DIR_OFF, shadow_dir); udelay(10000);
    shadow_dir &= ~BIT_RST; gw(GPIO_DIR_OFF, shadow_dir); udelay(10000);
    shadow_dir |= BIT_RST; gw(GPIO_DIR_OFF, shadow_dir); udelay(120000);
    shadow_dir |= BIT_CSX; gw(GPIO_DIR_OFF, shadow_dir);
    shadow_dir |= BIT_DCX; gw(GPIO_DIR_OFF, shadow_dir);
    udelay(5000);
    shadow_dir &= ~BIT_CSX; gw(GPIO_DIR_OFF, shadow_dir);
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

    lcd_cmd(0x2A); lcd_dat(0); lcd_dat(0); lcd_dat(1); lcd_dat(0x3F);
    lcd_cmd(0x2B); lcd_dat(0); lcd_dat(0); lcd_dat(0); lcd_dat(0xEF);

    lcd_write_mem();
    for (i = 0; i < LCD_W * LCD_H; i++)
        lcd_write_16d(pixels[i]);
    lcd_cs_deselect();
}

/* Render thread */
static int render_fn(void *data)
{
    while (!kthread_should_stop()) {
        if (target_fps > 0) {
            if (fb_dirty) {
                lcd_flush_fb();
                fb_dirty = 0;
            }
            msleep(1000 / target_fps);
            fb_dirty = 1; /* periodic refresh */
        } else {
            /* manual mode: wait for flush command */
            msleep(50);
            if (fb_dirty) {
                lcd_flush_fb();
                fb_dirty = 0;
            }
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

    /* Если offset 0 и size = FB_SIZE — полный кадр */
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

/* === SX8650 Touchscreen via direct I2C palmbus === */

#define SM0_CFG     0x900
#define SM0_CLKDIV  0x904
#define SM0_DATA    0x908
#define SM0_DATAOUT 0x910
#define SM0_DATAIN  0x914
#define SM0_STATUS  0x91C
#define SM0_START   0x920
#define SM0_CTL1    0x940

static int touch_x, touch_y;
static int touch_pressed;
static struct task_struct *touch_thread;

/* === PIC16 Battery via palmbus I2C === */
#define PIC_ADDR  0x2A
#define PIC_BATTERY_LEN  17  /* max bytes to read from PIC */

static u8 pic_battery_raw[PIC_BATTERY_LEN];
static int pic_battery_valid;

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
    gw(SM0_DATA, 0x48);   /* SX8650 addr */
    gw(SM0_START, 0);
    udelay(150);
}

static void i2c_raw_stop(void)
{
    gw(SM0_STATUS, 2);    /* NACK */
    udelay(150);
    gw(SM0_START, 0);
    udelay(150);
}

static void sx8650_hw_init(void)
{
    /* I2C controller init (from original kernel) */
    u32 rst = gr(0x034);
    rst |= 0x10000;  gw(0x034, rst);
    rst &= ~0x10000; gw(0x034, rst);
    udelay(500);

    gw(SM0_CTL1, 0x90644042);
    gw(0x928, 1);  /* SM0_D0 enable */

    /* Оригинальные регистры из заводской прошивки + CONVERT(SEQ) */

    /* 1. Soft Reset */
    i2c_raw_start(); i2c_raw_write(0x1F); i2c_raw_write(0xDE); i2c_raw_stop();
    mdelay(50);

    /* 2. Регистры из оригинального ядра 3.10.14 */
    i2c_raw_start(); i2c_raw_write(0x00); i2c_raw_write(0x00); i2c_raw_stop(); udelay(150);
    i2c_raw_start(); i2c_raw_write(0x01); i2c_raw_write(0x27); i2c_raw_stop(); udelay(150);
    i2c_raw_start(); i2c_raw_write(0x02); i2c_raw_write(0x00); i2c_raw_stop(); udelay(150);
    i2c_raw_start(); i2c_raw_write(0x03); i2c_raw_write(0x2D); i2c_raw_stop(); udelay(150);
    i2c_raw_start(); i2c_raw_write(0x04); i2c_raw_write(0xC0); i2c_raw_stop(); udelay(150);

    /* 3. PenTrg mode (0x80 потом 0x90 как в оригинале) */
    i2c_raw_start();
    gw(SM0_DATAOUT, 0x80); gw(SM0_STATUS, 2); udelay(150);
    gw(SM0_START, 0); udelay(150);
    i2c_raw_start();
    gw(SM0_DATAOUT, 0x90); gw(SM0_STATUS, 2); udelay(150);

    gw(SM0_CFG, 0xFA);

    /* GPIO 0 = input (NIRQ) */
    u32 dir = gr(GPIO_DIR_OFF);
    dir &= ~1;
    gw(GPIO_DIR_OFF, dir);

    pr_info("lcd_drv: SX8650 init done (mainline-style v2)\n");
}

/*
 * I2C Read Channels (даташит Figure 26):
 * В Auto mode после NIRQ — просто I2C read.
 * Данные: X(hi,lo), Y(hi,lo) = 4 байта.
 * Format: [0|CHAN(2:0)|D(11:8)] [D(7:0)]
 */
static int sx8650_read_xy(int *rx, int *ry)
{
    int raw_x = 0, raw_y = 0;
    u8 h, l;

    /*
     * Читаем X: cmd 0x80 = SELECT(X), потом read 2 bytes
     * Читаем Y: cmd 0x81 = SELECT(Y), потом read 2 bytes
     * Между ними — полный I2C stop/start цикл
     */

    /* --- Read X: cmd 0x80 --- */
    gw(SM0_CTL1, 0x90644042); udelay(10);
    gw(SM0_DATA, 0x48);
    gw(SM0_START, 0); udelay(10);
    gw(SM0_DATAOUT, 0x80);  /* SELECT(X) */
    gw(SM0_STATUS, 2); udelay(150);
    gw(SM0_START, 0); udelay(10);
    gw(SM0_DATAOUT, 0x91);  /* read addr */
    gw(SM0_STATUS, 2); udelay(150);
    gw(SM0_CFG, 0xFA);
    gw(SM0_START, 0); udelay(10);
    gw(SM0_START, 1); gw(SM0_START, 1); udelay(10);
    gw(SM0_STATUS, 1); udelay(150);
    h = gr(SM0_DATAIN) & 0xFF; udelay(150);
    l = gr(SM0_DATAIN) & 0xFF; udelay(150);
    gw(SM0_START, 0); udelay(10);
    gw(SM0_START, 1);
    gw(SM0_CTL1, 0x8064800E); udelay(10);

    if (h != 0xFF) {
        int ch = (h >> 4) & 7;
        int val = ((h & 0x0F) << 8) | l;
        if (ch == 0) raw_x = val;
        if (ch == 1) raw_y = val;
    }

    /* --- Read Y: cmd 0x81 --- */
    gw(SM0_CTL1, 0x90644042); udelay(10);
    gw(SM0_DATA, 0x48);
    gw(SM0_START, 0); udelay(10);
    gw(SM0_DATAOUT, 0x81);  /* SELECT(Y) */
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
    gw(SM0_CTL1, 0x8064800E); udelay(10);

    if (h != 0xFF) {
        int ch = (h >> 4) & 7;
        int val = ((h & 0x0F) << 8) | l;
        if (ch == 0) raw_x = val;
        if (ch == 1) raw_y = val;
    }

    if (raw_x > 0 || raw_y > 0) {
        /* Landscape mode (MADCTL=0xA8): axes swapped + both inverted */
        *rx = (raw_y > 0) ? (4096 - raw_y) * 320 / 4096 : 160;
        *ry = (raw_x > 0) ? raw_x * 240 / 4096 : 120;
        return 1;
    }
    return 0;
}

/* === PIC16 I2C via Linux I2C subsystem === */

static struct i2c_adapter *pic_i2c_adap;

static int pic_i2c_read(u8 *buf, int len)
{
    struct i2c_msg msg = {
        .addr = PIC_ADDR,
        .flags = I2C_M_RD,
        .len = len,
        .buf = buf,
    };
    int ret;

    if (!pic_i2c_adap)
        return -ENODEV;

    ret = i2c_transfer(pic_i2c_adap, &msg, 1);
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

    if (!pic_i2c_adap)
        return -ENODEV;

    ret = i2c_transfer(pic_i2c_adap, &msg, 1);
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
static int pic_read_battery(void)
{
    int ret;

    /* PIC returns test pattern (AA 54 A8...) without calibration.
     * Battery monitoring blocked until PICkit programmer or
     * stock firmware calibration data is available.
     * Send wake-up command 0x33 first, then read. */
    {
        u8 wake[3] = { 0x33, 0x00, 0x01 };
        pic_i2c_write(wake, 3);
        mdelay(50);
    }
    ret = pic_i2c_read(pic_battery_raw, PIC_BATTERY_LEN);
    if (!ret) {
        pr_info("lcd_drv: PIC alive, data: %02x %02x %02x %02x %02x %02x %02x\n",
                pic_battery_raw[0], pic_battery_raw[1], pic_battery_raw[2],
                pic_battery_raw[3], pic_battery_raw[4], pic_battery_raw[5],
                pic_battery_raw[6]);
        pic_battery_valid = 1;
    } else {
        pr_info("lcd_drv: PIC not responding (%d)\n", ret);
    }

    return 0;
}

/* Touch polling thread */
static int touch_fn(void *data)
{
    int x, y, was_pressed = 0;
    int no_touch_count = 0;

    while (!kthread_should_stop()) {
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
            if (no_touch_count > 10 && was_pressed) {  /* 500ms hold */
                touch_pressed = 0;
                was_pressed = 0;
                pr_info("lcd_drv: touch UP\n");
            }
        }

        msleep(50);
    }
    return 0;
}

/* ioctl: 0=flush, 1=read touch, 2=read battery, 3=raw PIC read, 4=backlight */
static long lcd_ioctl(struct file *f, unsigned int cmd, unsigned long arg)
{
    if (cmd == 0) {
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
        /* Read battery via PIC I2C command 0x2F */
        int ret = pic_read_battery();
        if (ret)
            return ret;
        if (copy_to_user((void __user *)arg, pic_battery_raw, PIC_BATTERY_LEN))
            return -EFAULT;
        return 0;
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
        gw(GPIO_DIR_OFF, shadow_dir);
        return 0;
    }
    return -ENOTTY;
}


static const struct file_operations lcd_fops = {
    .owner          = THIS_MODULE,
    .write          = lcd_fb_write,
    .llseek         = default_llseek,
    .unlocked_ioctl = lcd_ioctl,
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
    shadow_dir |= BIT_BL; gw(GPIO_DIR_OFF, shadow_dir);

    /* Splash screen: 4PDA logo, 1 second */
    {
        u16 *fb16 = (u16 *)framebuffer;
        int i, j = 0;
        for (i = 0; i < SPLASH_RLE_LEN && j < LCD_W * LCD_H; i++) {
            int k;
            for (k = 0; k < splash_cnt[i] && j < LCD_W * LCD_H; k++)
                fb16[j++] = splash_clr[i];
        }
        lcd_flush_fb();
        mdelay(1500);
    }

    /* Clear to black after splash */
    memset(framebuffer, 0, FB_SIZE);
    lcd_flush_fb();

    /* SX8650 touchscreen init */
    sx8650_hw_init();

    /* PIC16 battery: disabled at boot.
     * i2c_get_adapter() and i2c_transfer() reconfigure SM0 registers
     * which breaks palmbus-based touch reads.
     * PIC will be initialized on-demand via ioctl. */
    pr_info("lcd_drv: PIC battery disabled (no i2c_get_adapter at boot)\n");

    /* Start render thread */
    render_thread = kthread_run(render_fn, NULL, "lcd_render");

    /* Start touch thread */
    touch_thread = kthread_run(touch_fn, NULL, "lcd_touch");

    pr_info("lcd_drv: /dev/lcd ready (fb=%dx%d, %d bytes, fps=%d)\n",
            LCD_W, LCD_H, FB_SIZE, target_fps);
    return 0;
}

static void __exit lcd_drv_exit(void)
{
    if (touch_thread) kthread_stop(touch_thread);
    if (render_thread) kthread_stop(render_thread);
    if (pic_i2c_adap) i2c_put_adapter(pic_i2c_adap);
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
