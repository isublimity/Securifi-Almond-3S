# SX8650 Touchscreen Driver

## Overview

The Securifi Almond 3S uses a Semtech SX8650 4-wire resistive touchscreen controller connected via I2C to the MT7621 SoC. The touchscreen overlays the 2.8" ILI9341 LCD display.

## Hardware Specs

| Parameter | Value |
|-----------|-------|
| Controller | Semtech SX8650 |
| Type | 4-wire resistive |
| Interface | I2C, bus 0 |
| I2C Address | 0x48 (7-bit, A0=GND) |
| ADC | 12-bit (0–4095) |
| Supply | 1.65–3.7V |
| ESD | ±15kV HBM on touch pins |

## I2C Communication

The SX8650 uses a **custom I2C protocol** where the MT7621 I2C controller is accessed directly through palmbus memory-mapped registers (not through the Linux I2C subsystem).

### MT7621 I2C Registers (Palmbus SM0)

| Register | Offset | Description |
|----------|--------|-------------|
| SM0_CFG | 0x900 | Configuration |
| SM0_CLKDIV | 0x904 | Clock divider |
| SM0_DATA | 0x908 | Slave address |
| SM0_DATAOUT | 0x910 | Write data byte |
| SM0_DATAIN | 0x914 | Read data byte |
| SM0_STATUS | 0x91C | ACK/NACK (1=ACK, 2=NACK) |
| SM0_START | 0x920 | Start/Stop/Clock |
| SM0_CTL1 | 0x940 | Clock config |

### Why Direct Palmbus Access

The Linux I2C driver (`i2c-mt7621`) works for basic operations but some SX8650 commands only work through direct palmbus register manipulation. The SELECT(X) and SELECT(Y) commands specifically require the palmbus method.

## Initialization

### Register Configuration

From reverse engineering of the original firmware:

```c
// Soft Reset
i2c_write(0x1F, 0xDE);  // RegSoftReset = 0xDE
delay(50ms);

// Register writes (format: [000+RegAddr] [Value])
i2c_write_reg(0x00, 0x00);  // Reg 0
i2c_write_reg(0x01, 0x27);  // CTRL0: RATE=300cps, POWDLY=1.36ms
i2c_write_reg(0x02, 0x00);  // CTRL1: default
i2c_write_reg(0x03, 0x2D);  // CTRL2: settling + pen resistance
i2c_write_reg(0x04, 0xC0);  // ChanMask: XCONV + YCONV

// Enable pen trigger mode
i2c_cmd(0x80);  // PenTrg
i2c_cmd(0x90);  // CONDIRQ (conditional interrupt)
```

### Register Details

| Register | Addr | Value | Bits | Description |
|----------|------|-------|------|-------------|
| CTRL0 | 0x01 | 0x27 | RATE[7:4]=2 (300cps), POWDLY[3:0]=7 (1.36ms) | Conversion rate and settling time |
| CTRL1 | 0x02 | 0x00 | CONDIRQ=0, RPDNT=00(100K), FILT=00(disabled) | IRQ and filter config |
| CTRL2 | 0x03 | 0x2D | SETDLY=0xD (settling delay) | Conversion settling |
| ChanMask | 0x04 | 0xC0 | XCONV=1, YCONV=1 | Enable X and Y channels |

## Reading Coordinates

### Protocol

Two separate I2C transactions read X and Y:

```c
// Read X coordinate
i2c_start();
SM0_DATA = 0x48;          // SX8650 address
SM0_DATAOUT = 0x80;       // SELECT(X) command
SM0_STATUS = NACK;
// ... read 2 bytes → X raw value

// Read Y coordinate
i2c_start();
SM0_DATA = 0x48;
SM0_DATAOUT = 0x81;       // SELECT(Y) command
SM0_STATUS = NACK;
// ... read 2 bytes → Y raw value
```

### SX8650 Commands (Table 8 from datasheet)

| Command | Byte | Description |
|---------|------|-------------|
| SELECT(X) | 0x80 | Bias X channel (also PenTrg mode!) |
| SELECT(Y) | 0x81 | Bias Y channel |
| SELECT(Z1) | 0x82 | Bias Z1 (pressure) |
| SELECT(Z2) | 0x83 | Bias Z2 (pressure) |
| SELECT(SEQ) | 0x87 | Bias all channels from ChanMask |
| CONVERT(X) | 0x88 | Convert X channel |
| CONVERT(SEQ) | 0x8F | Convert all from ChanMask |
| PENDET | 0xC0 | Enter pen detect mode |
| PENTRG | 0xE0 | Enter pen trigger mode |

**Important**: Only `SELECT(X)=0x80` and `SELECT(Y)=0x81` work reliably on this hardware. CONVERT commands return 0xFF.

### Data Format

Each channel read returns 2 bytes:

```
Byte 0: [0 | CHAN(2:0) | D(11:8)]
Byte 1: [D(7:0)]

CHAN: 0=X, 1=Y, 2=Z1, 3=Z2
D: 12-bit ADC value (0-4095)
```

### Calibration

Raw ADC values are converted to screen coordinates:

```c
// From original firmware's libTouch.so
screen_x = (4096 - raw_x) * 320 / 4096;  // X is inverted
screen_y = raw_y * 240 / 4096;
```

More precise calibration from the original firmware:
```c
// X axis: inverted
float screen_x = (float)(raw_x - 4096) / (-11.83f) - 11.0f;

// Y axis: with X/Y ratio correction
float temp_y = (float)(raw_y) / 15.256f - 11.0f;
float correction = (float)(raw_y / raw_x) * 0.31f;
float screen_y = temp_y + correction;
```

## I2C Read Sequence (Palmbus)

Complete sequence for reading one channel:

```c
// 1. Setup
SM0_CTL1 = 0x90644042;    // Clock config
SM0_DATA = 0x48;           // SX8650 address
SM0_START = 0;             // Clear
delay(10us);

// 2. Send SELECT command
SM0_DATAOUT = cmd;         // 0x80 for X, 0x81 for Y
SM0_STATUS = 2;            // NACK
delay(150us);

// 3. Switch to read
SM0_START = 0;
delay(10us);
SM0_DATAOUT = 0x91;        // Read address (0x48<<1 | 1)
SM0_STATUS = 2;            // NACK
delay(150us);

// 4. Read mode
SM0_CFG = 0xFA;
SM0_START = 0;
delay(10us);

// 5. Clock in byte 1 (high)
SM0_START = 1;
SM0_START = 1;
delay(10us);
SM0_STATUS = 1;            // ACK
delay(150us);
high_byte = SM0_DATAIN;

// 6. Clock in byte 2 (low)
delay(150us);
low_byte = SM0_DATAIN;     // Read same register again!

// 7. Stop
SM0_START = 0;
delay(10us);
SM0_START = 1;
SM0_CTL1 = 0x8064800E;    // Restore clock
```

**Note**: The low byte is read from `SM0_DATAIN` without additional clock pulses. This is a quirk of the MT7621 I2C controller — the second read of the same register returns the next byte.

## Touch Polling

The kernel module polls touch at 50ms intervals:

```c
while (!kthread_should_stop()) {
    if (sx8650_read_xy(&x, &y)) {
        // Touch detected
        touch_x = x;
        touch_y = y;
        touch_pressed = 1;
    } else {
        // No touch (0xFFFF returned)
        if (no_touch_count++ > 3)
            touch_pressed = 0;
    }
    msleep(50);
}
```

## Userspace Access

### Via ioctl

```c
int fd = open("/dev/lcd", O_RDWR);
int data[3];  // {x, y, pressed}

ioctl(fd, 1, data);  // cmd=1: read touch

printf("x=%d y=%d pressed=%d\n", data[0], data[1], data[2]);
```

### NIRQ Pin

GPIO 0 is connected to SX8650 NIRQ (interrupt, active LOW). Currently not used — polling is used instead. NIRQ stays LOW constantly on OpenWrt, possibly due to I2C controller state.

## Troubleshooting

| Issue | Cause | Fix |
|-------|-------|-----|
| All reads return 0xFFFF | SX8650 not initialized | Check I2C init sequence, verify addr 0x48 on bus |
| Only Y axis works | Using wrong command | Must use SELECT(X)=0x80 AND SELECT(Y)=0x81 separately |
| CONVERT commands return 0xFF | MT7621 I2C timing | Use SELECT commands instead of CONVERT |
| Touch stops working | I2C bus locked | Restart lcd_drv module |
| Coordinates inverted | Wrong calibration | X axis is inverted: `screen_x = (4096 - raw) * 320 / 4096` |
