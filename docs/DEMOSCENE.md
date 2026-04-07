# Demoscene LCD Engine

Procedural animated scenes rendered in kernel (lcd_drv.ko) using integer-only math. 320x240 RGB565, ~0.3-0.5 fps via GPIO bit-bang. No textures, no bitmaps — pure math.

## Scenes

| ID | Name | Description |
|----|------|-------------|
| 0 | **plasma** | Classic 4-wave plasma with HSV palette cycling |
| 1 | **fire** | Bottom-up flame propagation with heat decay |
| 2 | **starfield** | 200 stars with 3D perspective projection |
| 3 | **interference** | Two moving wave centers creating Moire patterns |
| 4 | **rotozoom** | Rotating/zooming XOR pattern with color cycling |
| 5 | **dashboard** | Functional plasma — visualizes router state (see below) |

4PDA logo overlaid on all scenes. Dashboard uses 95% transparent alpha blend.

## API

### Lua (lcdlib.so)

```lua
local lcd = require("lcdlib")
lcd.open()

-- Switch scene
lcd.scene("plasma")      -- scene 0
lcd.scene("fire")        -- scene 1
lcd.scene("stars")       -- scene 2
lcd.scene("interference") -- scene 3
lcd.scene("rotozoom")    -- scene 4
lcd.scene("random")      -- random pick
lcd.scene("off")         -- stop animation, manual mode

-- Dashboard (scene 5) — functional router visualization
lcd.dashboard({
    lte = -85,          -- RSRP dBm (0 = no LTE)
    vpn = 45,           -- tunnel latency ms (-1 = no tunnel)
    clients = {
        {kbps=15000, signal=-40},  -- heavy user, close
        {kbps=500,   signal=-75},  -- light user, far
    }
})
```

### Kernel ioctl (/dev/lcd)

```
ioctl(fd, 5, scene_id)   -- 0-5: select scene, 99: random, 100: stop
ioctl(fd, 6, int[27])    -- dashboard params (see format below)
```

Animation stops automatically when userspace writes to framebuffer or calls `ioctl(0)` (flush).

## Dashboard Scene — Router Visualization

The dashboard plasma turns the display into a living map of the router's state. Each parameter maps to a visual property:

### Parameter Mapping

| Parameter | Visual Effect | Example |
|-----------|--------------|---------|
| **Client count** | Number of wave distortion points | 0 = calm, 8 = complex interference |
| **Client traffic (kbps)** | Wave amplitude — "pressure" / depth of distortion | 100 kbps = gentle ripple, 20000 = deep vortex |
| **Client WiFi signal (dBm)** | Distance from center | -35 dBm = center (close), -85 dBm = edge (far) |
| **LTE RSRP (dBm)** | Color palette temperature | >-80 = cyan/green, -95 = blue, -105 = orange, <-110 = red, 0 = gray |
| **VPN latency (ms)** | Pulsing concentric rings from center | <50ms = fast thin rings, >200ms = slow wide rings, -1 = no rings |

### How to Read the Display

**Colors tell LTE signal quality:**
- Cyan/green/white — excellent signal (>-80 dBm)
- Blue/cyan — good (-80 to -95)
- Yellow/orange — fair (-95 to -105)
- Red/orange — weak (<-105)
- Gray — no LTE connection

**Distortion points are WiFi clients:**
- Deep vortex near center = heavy user close to router
- Gentle ripple at edge = light user far from router
- Each client orbits at radius proportional to WiFi signal strength
- Wave speed of each client proportional to their traffic

**Rings show VPN tunnel:**
- Fast thin rings = low latency tunnel (good)
- Slow thick rings = high latency (congested)
- No rings = no VPN active

### Dashboard ioctl Format

Flat int array `[3 + N*2]`:

```
p[0]  = num_clients (0-12)
p[1]  = lte_rsrp (dBm, 0 = no LTE)
p[2]  = vpn_ms (-1 = no tunnel)
p[3]  = client 0 traffic (kbps)
p[4]  = client 0 WiFi signal (dBm)
p[5]  = client 1 traffic (kbps)
p[6]  = client 1 WiFi signal (dBm)
...
p[3+N*2-2] = client N-1 traffic
p[3+N*2-1] = client N-1 signal
```

### Example Scenarios

```lua
-- Morning: phone checking news
lcd.dashboard({lte=-82, vpn=30, clients={
    {kbps=2000, signal=-45},
}})

-- Work: laptop on VPN + phone idle
lcd.dashboard({lte=-88, vpn=40, clients={
    {kbps=15000, signal=-38},
    {kbps=100,   signal=-50},
}})

-- Evening: streaming + gaming + phones
lcd.dashboard({lte=-95, vpn=25, clients={
    {kbps=25000, signal=-35},
    {kbps=8000,  signal=-42},
    {kbps=3000,  signal=-55},
    {kbps=500,   signal=-70},
}})

-- Overload: weak signal, many devices
lcd.dashboard({lte=-112, vpn=300, clients={
    {kbps=10000, signal=-35},
    {kbps=8000,  signal=-45},
    {kbps=5000,  signal=-50},
    {kbps=3000,  signal=-60},
    {kbps=2000,  signal=-70},
    {kbps=1000,  signal=-80},
}})

-- Night: idle
lcd.dashboard({lte=-75, vpn=-1, clients={
    {kbps=5, signal=-55},
}})
```

## Implementation Notes

### Integer Math Only

No floating point in kernel. All effects use:
- **Sine LUT**: 256-entry table, values 0-255 (`sin(x)*127+128`)
- **Fixed-point arithmetic**: shifts instead of divides
- **HSV palette**: sector-based conversion (divide by 43, multiply by 6)

### Performance

- Frame generation: ~50-100ms (MIPS 880MHz, pure integer)
- GPIO bit-bang flush: ~2-3 seconds (320x240 pixels via DIR register)
- Effective framerate: ~0.3-0.5 fps
- `cond_resched()` every 1024 pixels to keep system responsive
- `msleep(100)` between frames for network/SSH

### Scene Selection at Boot

Random scene selected at `insmod` time based on `jiffies % NUM_SCENES`. Animation runs until userspace sends any write or flush command.
