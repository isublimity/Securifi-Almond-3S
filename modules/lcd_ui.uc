#!/usr/bin/ucode
//
// lcd_ui.uc — Almond 3S LCD UI (ucode native)
//
// Архитектура: uloop (event loop) + ubus (system data) + uci (config)
// Данные: /tmp/lcd_data.json (от data_collector) + ubus supplement
// Рендер: JSON через unix socket → lcd_render (socat pipe)
// Тач: /tmp/.lcd_touch (от lcd_touch_poll)
//
// Build: scp lcd_ui.uc root@192.168.11.1:/usr/bin/lcd_ui.uc
// Run:   ucode /usr/bin/lcd_ui.uc &
//

'use strict';

let fs = require("fs");

// No PID lock needed — procd manages single instance (no auto-restart loop below)

// Optional modules — graceful degrade
let ubus_mod, uci_mod, uloop_mod;
try { ubus_mod = require("ubus"); } catch(e) {}
try { uci_mod = require("uci"); } catch(e) {}
try { uloop_mod = require("uloop"); } catch(e) {}

// --- Constants ---
let LCD_W = 320, LCD_H = 240;
let SOCK_PATH = "/tmp/lcd.sock";
let DATA_PATH = "/tmp/lcd_data.json";
let TOUCH_PATH = "/tmp/.lcd_touch";
let SCRIPTS = "/etc/lcd/scripts";  // shell scripts directory

// Colors (lcd_render accepts: #RRGGBB, #XXXX raw RGB565, named)
let C = {
    bg:      "#000000",
    hdr:     "#001F",
    white:   "white",
    green:   "#07E0",
    red:     "red",
    yellow:  "yellow",
    cyan:    "cyan",
    gray:    "#4208",
    btn:     "#1082",
    back:    "#4000",
    accent:  "#FFE0",
    dim:     "#2104",
};

// Timing (seconds)
let T = {
    data:   2,     // data refresh
    burnin: 30,    // anti-burn-in shift
    saver:  240,   // idle → screensaver (4 min)
    off:    300,   // idle → backlight off (5 min)
};

// Layout
let HDR_H   = 18;
let COLS    = 2;
let BTN_W   = 155;
let BTN_H   = 70;
let BTN_PAD = 3;
let START_Y = HDR_H + 2;
let BACK_Y  = LCD_H - 22;

// Touch: lcd_drv returns pixel coordinates directly (0-319, 0-239)
// No ADC mapping needed

// --- State ---
let st = {
    page:   "dashboard",
    mpg:    1,         // menu page (1 or 2)
    screen: "active",
    data:   {},        // sensor data from data_collector
    ltch:   time(),    // last touch time
    ldraw:  0,         // last draw time
    frame:  0,
    ox: 0, oy: 0,     // burn-in pixel offset
    tp:     false,     // touch was pressed (edge detection)
    saver_frame: 0,    // screensaver animation
};

// --- Connections ---
let uconn = null;
if (ubus_mod) {
    uconn = ubus_mod.connect();
    if (!uconn) warn("lcd_ui: ubus connect failed\n");
}

let ucur = null;
if (uci_mod) ucur = uci_mod.cursor();


// =============================================
//  LCD RENDER COMMUNICATION
// =============================================

let cmds = [];

function Q(j) {
    push(cmds, j);
}

function lcd_clear(c) {
    Q(sprintf('{"cmd":"clear","color":"%s"}', c ?? C.bg));
}

function lcd_rect(x, y, w, h, c) {
    Q(sprintf('{"cmd":"rect","x":%d,"y":%d,"w":%d,"h":%d,"color":"%s"}', x, y, w, h, c));
}

function lcd_text(x, y, text, color, bg, sz) {
    // Escape quotes and backslashes for JSON
    text = replace(replace(text ?? "", '\\', '\\\\'), '"', '\\"');
    Q(sprintf('{"cmd":"text","x":%d,"y":%d,"text":"%s","color":"%s","bg":"%s","size":%d}',
        x, y, text, color ?? C.white, bg ?? C.bg, sz ?? 2));
}

function lcd_flush() {
    if (!length(cmds)) return;
    push(cmds, '{"cmd":"flush"}');
    let payload = join("\n", cmds) + "\n";
    cmds = [];
    let p = fs.popen("socat -u - UNIX-CONNECT:" + SOCK_PATH + " 2>/dev/null", "w");
    if (p) {
        p.write(payload);
        p.close();
    }
}


// =============================================
//  HISTORY + TRAFFIC
// =============================================

let HIST_LEN = 60;

let hist = {
    csq:   [],   // LTE CSQ (0-31)
    rsrq:  [],   // LTE RSRQ (dB, -3 to -20)
    ping:  [],   // Google ping ms
    rx:    [],   // wwan0 RX bytes/sec
    tx:    [],   // wwan0 TX bytes/sec
    br_rx: [],   // br-lan RX bytes/sec
    br_tx: [],   // br-lan TX bytes/sec
};

let last_net = null;

function hist_push(arr, val) {
    push(arr, val);
    if (length(arr) > HIST_LEN)
        splice(arr, 0, 1);
}

function collect_traffic() {
    let raw = fs.readfile("/proc/net/dev");
    if (!raw) return;
    let now_net = {};
    for (let line in split(raw, "\n")) {
        let m = match(line, /^\s*(\S+):\s*(\d+)\s+\d+\s+\d+\s+\d+\s+\d+\s+\d+\s+\d+\s+\d+\s+(\d+)/);
        if (m)
            now_net[m[1]] = { rx: +m[2], tx: +m[3] };
    }
    if (last_net) {
        let delta = (iface, key) => {
            let cur = now_net[iface]?.[key] ?? 0;
            let prev = last_net[iface]?.[key] ?? 0;
            let d = cur - prev;
            return d >= 0 ? d : 0;
        };
        hist_push(hist.rx, delta("wwan0", "rx"));
        hist_push(hist.tx, delta("wwan0", "tx"));
        hist_push(hist.br_rx, delta("br-lan", "rx"));
        hist_push(hist.br_tx, delta("br-lan", "tx"));
    }
    last_net = now_net;
}

function update_history() {
    let d = st.data;
    hist_push(hist.csq, int(+(d?.lte?.csq ?? 0)));
    hist_push(hist.rsrq, int(+(d?.lte?.rsrq ?? 0)));
    hist_push(hist.ping, int(+(d?.ping?.google_ms ?? 0)));
    collect_traffic();
}

// Line graph with scale, thresholds, and labels
// thresholds: [{val, color, label}, ...] — horizontal reference lines
function draw_graph(x, y, w, h, data, color, mn, mx, thresholds) {
    let n = length(data);
    if (n < 2) return;
    if (mx <= mn) mx = mn + 1;
    let range = mx - mn;

    // Background
    lcd_rect(x, y, w, h, "#0841");

    // Threshold lines (dashed — draw every 4px)
    if (thresholds) {
        for (let t in thresholds) {
            let ty2 = y + h - int((t.val - mn) / range * h);
            if (ty2 > y && ty2 < y + h) {
                for (let dx = 0; dx < w; dx += 8)
                    lcd_rect(x + dx, ty2, 4, 1, t.color ?? C.gray);
                // Label on right
                lcd_text(x + w - 30, ty2 - 4, t.label ?? "", t.color ?? C.gray, "#0841", 1);
            }
        }
    }

    // Scale labels (left: max, bottom: min)
    lcd_text(x + 1, y + 1, sprintf("%d", mx), C.gray, "#0841", 1);
    lcd_text(x + 1, y + h - 9, sprintf("%d", mn), C.gray, "#0841", 1);

    // Plot line: connect points
    let pts = n > HIST_LEN ? HIST_LEN : n;
    let start = n - pts;
    let step_x = (w - 2) / (pts - 1);

    let prev_px = -1, prev_py = -1;
    for (let i = 0; i < pts; i++) {
        let val = data[start + i];
        let px = x + 1 + int(i * step_x);
        let py = y + h - 1 - int((val - mn) / range * (h - 2));
        if (py < y) py = y;
        if (py >= y + h) py = y + h - 1;

        // Draw dot
        lcd_rect(px, py, 2, 2, color);

        // Connect to previous with vertical line segments
        if (prev_px >= 0) {
            let dy = py - prev_py;
            let steps = (dy > 0 ? dy : -dy);
            if (steps > 0) {
                let y_start = dy > 0 ? prev_py : py;
                lcd_rect(px, y_start, 1, steps, color);
            }
        }
        prev_px = px;
        prev_py = py;
    }

    // Current value — bright dot
    if (pts > 0) {
        let last_val = data[n - 1];
        let last_py = y + h - 1 - int((last_val - mn) / range * (h - 2));
        let last_px = x + w - 3;
        lcd_rect(last_px - 1, last_py - 1, 4, 4, C.white);
    }
}

function arr_minmax(arr) {
    if (length(arr) == 0) return { min: 0, max: 1 };
    let mn = 999999, mx = -999999;
    for (let v in arr) {
        if (v < mn) mn = v;
        if (v > mx) mx = v;
    }
    return { min: mn, max: mx };
}


// =============================================
//  DATA COLLECTION
// =============================================

function refresh_data() {
    // Primary: data_collector JSON
    let raw = fs.readfile(DATA_PATH);
    let d = raw ? json(raw) : {};

    // Supplement: ubus system info (more accurate uptime/mem/load)
    if (uconn) {
        let si = uconn.call("system", "info", {});
        if (si) {
            if (si.uptime) d.uptime = si.uptime;
            let mem = si.memory;
            if (mem) d.mem_free_mb = int((mem.available ?? mem.free ?? 0) / 1048576);
            if (si.load) d.cpu_load_raw = si.load[0];
        }
    }

    st.data = d;
    update_history();
}


// =============================================
//  TOUCH INPUT
// =============================================

function read_touch() {
    // touch_poll daemon writes file only on press edge (latch).
    // Coordinates are already in pixels (lcd_drv maps internally).
    // We read and unlink — next tap creates a new file.
    let raw = fs.readfile(TOUCH_PATH);
    if (!raw) return null;
    fs.unlink(TOUCH_PATH);
    let m = match(trim(raw), /^(\d+)\s+(\d+)/);
    if (!m) return null;
    return { x: +m[1], y: +m[2] };
}


// =============================================
//  HELPERS
// =============================================

function lte_quality(csq) {
    if (csq > 25) return { label: "Excellent", color: C.green, bg: "#002000" };
    if (csq > 15) return { label: "Good",      color: C.green, bg: "#001000" };
    if (csq > 5)  return { label: "Fair",       color: C.yellow, bg: "#080800" };
    return { label: "Weak", color: C.red, bg: "#100000" };
}

function fmt_bytes(b) {
    b = +(b ?? 0);
    if (b >= 1073741824) return sprintf("%.1fG", b / 1073741824);
    if (b >= 1048576) return sprintf("%.1fM", b / 1048576);
    if (b >= 1024) return sprintf("%.0fK", b / 1024);
    return sprintf("%d", b);
}

function fmt_uptime(s) {
    s = int(+(s ?? 0));
    let d = int(s / 86400);
    let h = int((s % 86400) / 3600);
    let m = int((s % 3600) / 60);
    if (d > 0) return sprintf("%dd%dh%dm", d, h, m);
    if (h > 0) return sprintf("%dh%dm", h, m);
    return sprintf("%dm", m);
}

function clock_str() {
    let t = localtime();
    return t ? sprintf("%02d:%02d", t.hour, t.min) : "--:--";
}

function btn_pos(idx) {
    let col = (idx - 1) % COLS;
    let row = int((idx - 1) / COLS);
    return {
        x: BTN_PAD + col * (BTN_W + BTN_PAD),
        y: START_Y + row * (BTN_H + BTN_PAD),
        w: BTN_W,
        h: BTN_H,
    };
}

function in_rect(tx, ty, bx, by, bw, bh) {
    return tx >= bx && tx <= bx + bw && ty >= by && ty <= by + bh;
}


// =============================================
//  DRAWING: COMMON
// =============================================

function draw_header(title, bg_c) {
    bg_c ??= C.hdr;
    lcd_rect(0, 0, LCD_W, HDR_H, bg_c);
    lcd_text(4, 2, title ?? "ALMOND 3S", C.white, bg_c, 2);
    lcd_text(LCD_W - 60, 2, clock_str(), C.cyan, bg_c, 2);
}

function draw_back() {
    lcd_rect(0, BACK_Y, LCD_W, 22, C.back);
    lcd_text(120, BACK_Y + 3, "< BACK", C.white, C.back, 2);
}

function draw_btn(idx, title, subtitle, title_c, sub_c, bg_c) {
    let b = btn_pos(idx);
    lcd_rect(b.x, b.y, b.w, b.h, bg_c ?? C.btn);
    lcd_text(b.x + 4, b.y + 4, title, title_c ?? C.white, bg_c ?? C.btn, 2);
    if (subtitle)
        lcd_text(b.x + 4, b.y + 28, subtitle, sub_c ?? C.gray, bg_c ?? C.btn, 2);
}


// =============================================
//  DRAWING: DASHBOARD
// =============================================

function draw_dashboard() {
    let d = st.data;
    let csq = int(+(d?.lte?.csq ?? 0));
    let lq = lte_quality(csq);
    let ox = st.ox, oy = st.oy;
    let bg = lq.bg;

    lcd_clear(bg);
    draw_header(sprintf("LTE:%s CSQ:%d", lq.label, csq), bg);

    // VPN status bar
    let vpn = d?.vpn?.active;
    let vtype = d?.vpn?.type ?? "";
    let vbg = vpn ? "#002000" : "#200000";
    lcd_rect(0, 20, LCD_W, 16, vbg);
    let vpn_label = vpn ? (vtype + " ON") : "VPN OFF";
    lcd_text(4 + ox, 22 + oy, vpn_label, vpn ? C.green : C.red, vbg, 2);
    let ping = int(+(d?.vpn?.ping_ms ?? d?.ping?.google_ms ?? 0));
    lcd_text(160 + ox, 22 + oy, sprintf("%dms", ping), C.white, vbg, 2);
    let eip = d?.vpn?.external_ip;
    if (vpn && eip)
        lcd_text(220 + ox, 22 + oy, eip, C.accent, vbg, 1);

    // LTE info
    let oper = d?.lte?.operator ?? "?";
    let lip  = d?.lte?.ip ?? "";
    lcd_text(4 + ox, 42 + oy, sprintf("%s %s", oper, lip), C.cyan, bg, 1);

    // External IP (eip already defined in VPN bar above)
    if (!vpn && eip && eip != "")
        lcd_text(4 + ox, 55 + oy, "IP:" + eip, C.gray, bg, 1);

    // WiFi clients
    let y = 72 + oy;
    let clients = d?.wifi?.clients;
    let nc = type(clients) == "array" ? length(clients) : 0;
    lcd_text(4 + ox, y, sprintf("WiFi: %d clients", nc), C.white, bg, 1);
    y += 12;

    if (type(clients) == "array") {
        for (let cl in clients) {
            if (y > 208) break;
            let name = cl.name ?? "?";
            let ip = cl.ip ?? "";
            let sig = int(+(cl.signal ?? 0));
            let band = cl.band ?? "";
            lcd_text(4 + ox, y,
                sprintf("%s %s %ddB %s", name, ip, sig, band),
                C.gray, bg, 1);
            y += 12;
        }
    }

    // Bottom status bar
    let load = d?.cpu_load_raw ? sprintf("%.2f", d.cpu_load_raw / 65536.0)
             : (d?.cpu_load ?? "?");
    lcd_text(4 + ox, 222 + oy,
        sprintf("UP:%s MEM:%dM CPU:%s",
            fmt_uptime(d?.uptime),
            int(+(d?.mem_free_mb ?? 0)),
            load),
        C.accent, bg, 1);

    lcd_flush();
}


// =============================================
//  DRAWING: MAIN MENU
// =============================================

function draw_menu() {
    let d = st.data;
    lcd_clear(C.bg);
    draw_header();

    if (st.mpg == 1) {
        // 1: VPN
        let vpn = d?.vpn?.active;
        draw_btn(1, "VPN",
            vpn ? "Active" : "OFF",
            C.green, vpn ? C.green : C.red);

        // 2: LTE
        let csq = int(+(d?.lte?.csq ?? 0));
        let lq = lte_quality(csq);
        draw_btn(2, sprintf("LTE %d", csq),
            d?.lte?.operator ?? "?",
            lq.color, C.gray);

        // 3: WiFi
        let nc = type(d?.wifi?.clients) == "array" ? length(d.wifi.clients) : 0;
        draw_btn(3, "WiFi",
            sprintf("%d clients", nc),
            C.cyan, C.green);

        // 4: Info
        draw_btn(4, "Info",
            fmt_uptime(d?.uptime),
            C.white, C.gray);

        // 5: Traffic
        let rx_last = length(hist.rx) > 0 ? hist.rx[length(hist.rx) - 1] : 0;
        let tx_last = length(hist.tx) > 0 ? hist.tx[length(hist.tx) - 1] : 0;
        draw_btn(5, "Traffic",
            sprintf("R:%s T:%s", fmt_bytes(rx_last), fmt_bytes(tx_last)),
            C.white, C.green);

        // 6: MORE
        let b = btn_pos(6);
        lcd_rect(b.x, b.y, b.w, b.h, C.hdr);
        lcd_text(b.x + 20, b.y + 20, "MORE >>>", C.white, C.hdr, 2);
    } else {
        // Page 2
        // 1: Reboot (with confirmation)
        draw_btn(1, "Reboot", "System", C.red, C.gray);

        // 2: LTE Reset
        draw_btn(2, "LTE Reset", "Modem restart", C.yellow, C.gray);

        // 3: IP
        let eip = d?.vpn?.external_ip ?? "";
        draw_btn(3, "IP",
            eip != "" ? eip : "...",
            C.white, eip != "" ? C.accent : C.gray);

        // 4: Dashboard
        draw_btn(4, "Dashboard", "Back to dash", C.cyan, C.gray);

        // 6: <<< BACK
        let b = btn_pos(6);
        lcd_rect(b.x, b.y, b.w, b.h, C.hdr);
        lcd_text(b.x + 20, b.y + 20, "<<< BACK", C.white, C.hdr, 2);
    }

    lcd_flush();
}


// =============================================
//  DRAWING: SUB-PAGES
// =============================================

// VPN page — large buttons like main menu (2 columns)
let VPN_TYPES = ["WG", "OVPN", "L2TP", ""];  // match data_collector vpn.type

function draw_vpn_page() {
    let d = st.data;
    let vpn = d?.vpn?.active;
    let vtype = d?.vpn?.type ?? "";
    lcd_clear(C.bg);
    draw_header("VPN");

    // Status bar
    let eip = d?.vpn?.external_ip ?? "";
    let ping_v = int(+(d?.vpn?.ping_ms ?? 0));
    if (vpn) {
        let vbg = "#002000";
        lcd_rect(0, 20, LCD_W, 16, vbg);
        lcd_text(4, 22, vtype + " ON", C.green, vbg, 2);
        lcd_text(120, 22, sprintf("%dms", ping_v), C.white, vbg, 2);
        lcd_text(200, 24, eip, C.accent, vbg, 1);
    } else {
        lcd_rect(0, 20, LCD_W, 16, "#200000");
        lcd_text(4, 22, "VPN OFF — Direct", C.red, "#200000", 2);
    }

    // 4 big buttons (2x2 grid) — same layout as main menu
    let vpns = [
        { name: "WireGuard", key: "WG",   color: C.cyan },
        { name: "OpenVPN",   key: "OVPN", color: C.green },
        { name: "L2TP",      key: "L2TP", color: C.yellow },
        { name: "VPN OFF",   key: "",     color: C.red },
    ];

    for (let i = 0; i < 4; i++) {
        let b = btn_pos(i + 1);  // reuse main menu layout
        let v = vpns[i];
        let active = vpn && vtype == v.key;
        let bg = active ? "#0841" : (i == 3 ? "#300000" : C.btn);

        lcd_rect(b.x, b.y, b.w, b.h, bg);
        lcd_text(b.x + 8, b.y + 8, v.name, v.color, bg, 2);

        if (active) {
            lcd_text(b.x + 8, b.y + 32, "ACTIVE", C.green, bg, 2);
            lcd_text(b.x + 90, b.y + 34, sprintf("%dms", ping_v), C.white, bg, 1);
        } else if (i == 3 && !vpn) {
            lcd_text(b.x + 8, b.y + 32, "Direct", C.gray, bg, 2);
        }
    }

    draw_back();
    lcd_flush();
}

function draw_wifi_page() {
    let d = st.data;
    lcd_clear(C.bg);
    draw_header("WiFi");
    let y = 24;

    // SSIDs from UCI
    if (ucur) {
        let ssid_2g = ucur.get("wireless", "default_radio0", "ssid") ?? "?";
        let ssid_5g = ucur.get("wireless", "default_radio1", "ssid") ?? "?";
        let enc_2g  = ucur.get("wireless", "default_radio0", "encryption") ?? "?";
        let enc_5g  = ucur.get("wireless", "default_radio1", "encryption") ?? "?";

        lcd_text(4, y, "2.4G: " + ssid_2g, C.green, C.bg, 1);
        y += 10;
        lcd_text(4, y, "Enc: " + enc_2g, C.gray, C.bg, 1);
        y += 12;
        lcd_text(4, y, "5G: " + ssid_5g, C.cyan, C.bg, 1);
        y += 10;
        lcd_text(4, y, "Enc: " + enc_5g, C.gray, C.bg, 1);
        y += 14;
    }

    // Connected clients
    let clients = d?.wifi?.clients;
    let nc = type(clients) == "array" ? length(clients) : 0;
    lcd_text(4, y, sprintf("Clients: %d", nc), C.white, C.bg, 2);
    y += 20;

    if (type(clients) == "array") {
        for (let cl in clients) {
            if (y > BACK_Y - 24) break;
            let name = cl.name ?? "unknown";
            let ip = cl.ip ?? "";
            let sig = int(+(cl.signal ?? 0));
            let band = cl.band ?? "";
            let sig_c = sig > -50 ? C.green : (sig > -70 ? C.yellow : C.red);
            // Line 1: name + band
            lcd_text(4, y, sprintf("%s %s", band, name), C.white, C.bg, 1);
            y += 10;
            // Line 2: IP + signal + traffic
            lcd_text(4, y,
                sprintf("%s %ddB R:%s T:%s",
                    ip, sig,
                    fmt_bytes(cl.rx_bytes ?? 0),
                    fmt_bytes(cl.tx_bytes ?? 0)),
                sig_c, C.bg, 1);
            y += 12;
        }
    }

    draw_back();
    lcd_flush();
}

function draw_info_page() {
    let d = st.data;
    lcd_clear(C.bg);
    draw_header("System Info");
    let y = 24;

    lcd_text(4, y, "Almond 3S + Fibocom L860-GL", C.accent, C.bg, 1);
    y += 14;

    // System info
    lcd_text(4, y, sprintf("Uptime: %s", fmt_uptime(d?.uptime)), C.white, C.bg, 2);
    y += 20;
    lcd_text(4, y, sprintf("Memory: %d MB free", int(+(d?.mem_free_mb ?? 0))), C.green, C.bg, 2);
    y += 20;

    let load = d?.cpu_load_raw ? sprintf("%.2f", d.cpu_load_raw / 65536.0)
             : (d?.cpu_load ?? "?");
    lcd_text(4, y, "CPU Load: " + load, C.cyan, C.bg, 2);
    y += 22;

    // LTE details
    let csq = int(+(d?.lte?.csq ?? 0));
    let ber = int(+(d?.lte?.ber ?? 0));
    lcd_text(4, y, sprintf("LTE CSQ:%d BER:%d", csq, ber), C.yellow, C.bg, 1);
    y += 12;
    lcd_text(4, y, sprintf("Operator: %s", d?.lte?.operator ?? "?"), C.gray, C.bg, 1);
    y += 12;
    lcd_text(4, y, sprintf("LTE IP: %s", d?.lte?.ip ?? "?"), C.gray, C.bg, 1);
    y += 12;
    lcd_text(4, y, sprintf("Google ping: %dms", int(+(d?.ping?.google_ms ?? 0))), C.gray, C.bg, 1);
    y += 14;

    // Board info from ubus
    if (uconn) {
        let board = uconn.call("system", "board", {});
        if (board) {
            lcd_text(4, y, sprintf("Kernel: %s", board?.kernel ?? "?"), C.dim, C.bg, 1);
            y += 10;
            lcd_text(4, y, sprintf("OpenWrt: %s", board?.release?.version ?? "?"), C.dim, C.bg, 1);
            y += 12;
        }
    }

    // lcd_drv version (via touch_poll version helper)
    let drv_ver = "?";
    let p = fs.popen("touch_poll version 2>/dev/null", "r");
    if (p) {
        drv_ver = trim(p.read("all") ?? "?");
        p.close();
    }
    lcd_text(4, y, "lcd_drv: " + drv_ver, C.dim, C.bg, 1);

    draw_back();
    lcd_flush();
}

function draw_ip_page() {
    let d = st.data;
    lcd_clear(C.bg);
    draw_header("External IP");
    let y = 30;

    let eip = d?.vpn?.external_ip ?? "unknown";
    lcd_text(4, y, "Exit IP:", C.cyan, C.bg, 2);
    y += 22;
    lcd_text(4, y, eip, C.accent, C.bg, 3);
    y += 30;

    let vpn = d?.vpn?.active;
    lcd_text(4, y, vpn ? "via VPN (WireGuard)" : "Direct (no VPN)",
        vpn ? C.green : C.red, C.bg, 2);
    y += 24;

    let ping_g = int(+(d?.ping?.google_ms ?? 0));
    let ping_v = int(+(d?.vpn?.ping_ms ?? 0));
    lcd_text(4, y, sprintf("Google: %dms  VPN: %dms", ping_g, ping_v), C.white, C.bg, 1);
    y += 14;

    // LTE IP for reference
    let lip = d?.lte?.ip ?? "?";
    lcd_text(4, y, sprintf("LTE IP: %s", lip), C.gray, C.bg, 1);

    draw_back();
    lcd_flush();
}

function draw_lte_page() {
    let d = st.data;
    lcd_clear(C.bg);
    draw_header("LTE Signal");

    let rsrp = int(+(d?.lte?.rsrp ?? 0));
    let rsrq = int(+(d?.lte?.rsrq ?? 0));
    let sinr = int(+(d?.lte?.sinr ?? 0));
    let csq  = int(+(d?.lte?.csq ?? 0));
    let pci  = int(+(d?.lte?.pci ?? 0));
    let band = d?.lte?.band ?? "";
    let mode = d?.lte?.mode ?? "";

    // Row A: RSRP + RSRQ + SINR
    let rsrp_c = rsrp > -90 ? C.green : (rsrp > -105 ? C.yellow : C.red);
    lcd_text(4, 22, sprintf("RSRP:%d", rsrp), rsrp_c, C.bg, 1);
    let rsrq_c = rsrq > -10 ? C.green : (rsrq > -15 ? C.yellow : C.red);
    lcd_text(100, 22, sprintf("RSRQ:%d", rsrq), rsrq_c, C.bg, 1);
    let sinr_c = sinr > 10 ? C.green : (sinr > 0 ? C.yellow : C.red);
    lcd_text(196, 22, sprintf("SINR:%d", sinr), sinr_c, C.bg, 1);
    lcd_text(275, 22, sprintf("CSQ:%d", csq), C.gray, C.bg, 1);

    // Row B: Band + PCI + Mode + Operator
    lcd_text(4, 34, sprintf("%s %s PCI:%d %s",
        mode, band, pci, d?.lte?.operator ?? ""),
        C.cyan, C.bg, 1);

    // Graph 1: RSRQ (Quality) — -3 (excellent) to -20 (bad)
    lcd_text(4, 47, "RSRQ (Quality)", C.gray, C.bg, 1);
    draw_graph(4, 57, LCD_W - 8, 42, hist.rsrq, C.cyan, -20, 0, [
        { val: -10, color: C.green,  label: "-10" },
        { val: -15, color: C.yellow, label: "-15" },
    ]);

    // Graph 2: Traffic (RX/TX)
    let rx_last = length(hist.rx) > 0 ? hist.rx[length(hist.rx) - 1] : 0;
    let tx_last = length(hist.tx) > 0 ? hist.tx[length(hist.tx) - 1] : 0;
    lcd_text(4, 102, "Traffic", C.gray, C.bg, 1);
    lcd_rect(56, 102, 6, 8, C.green);
    lcd_text(66, 102, fmt_bytes(rx_last) + "/s", C.green, C.bg, 1);
    lcd_rect(150, 102, 6, 8, C.red);
    lcd_text(160, 102, fmt_bytes(tx_last) + "/s", C.red, C.bg, 1);

    let rm = arr_minmax(hist.rx);
    let tm = arr_minmax(hist.tx);
    let mx = rm.max > tm.max ? rm.max : tm.max;
    if (mx < 10240) mx = 10240;
    draw_graph(4, 112, LCD_W - 8, 35, hist.rx, C.green, 0, mx, null);
    // Overlay TX
    if (length(hist.tx) >= 2) {
        let pts = length(hist.tx) > HIST_LEN ? HIST_LEN : length(hist.tx);
        let start = length(hist.tx) - pts;
        let sx = (LCD_W - 10) / (pts - 1);
        let ppx = -1, ppy = -1;
        for (let i = 0; i < pts; i++) {
            let px = 5 + int(i * sx);
            let py = 146 - int(hist.tx[start + i] / mx * 33);
            if (py < 112) py = 112;
            if (py > 146) py = 146;
            lcd_rect(px, py, 2, 2, C.red);
            if (ppx >= 0) { let dy = py - ppy; lcd_rect(px, dy > 0 ? ppy : py, 1, dy > 0 ? dy : -dy, C.red); }
            ppx = px; ppy = py;
        }
    }

    // Graph 3: Ping
    let ping = int(+(d?.ping?.google_ms ?? 0));
    let pm = arr_minmax(hist.ping);
    let ping_c = ping < 50 ? C.green : (ping < 150 ? C.yellow : C.red);
    lcd_text(4, 150, sprintf("Ping: %dms", ping), ping_c, C.bg, 1);
    lcd_text(120, 150, sprintf("min:%d max:%d", pm.min, pm.max), C.gray, C.bg, 1);

    let ping_max = pm.max > 200 ? pm.max : 200;
    draw_graph(4, 160, LCD_W - 8, 35, hist.ping, C.yellow, 0, ping_max, [
        { val: 50,  color: C.green,  label: "50" },
        { val: 150, color: C.red,    label: "150" },
    ]);

    // Bottom: IP
    lcd_text(4, 198, d?.lte?.ip ?? "", C.gray, C.bg, 1);

    draw_back();
    lcd_flush();
}

function draw_traffic_page() {
    lcd_clear(C.bg);
    draw_header("Traffic");

    // wwan0 (LTE) — current speeds
    let rx_last = length(hist.rx) > 0 ? hist.rx[length(hist.rx) - 1] : 0;
    let tx_last = length(hist.tx) > 0 ? hist.tx[length(hist.tx) - 1] : 0;
    lcd_text(4, 22, "LTE", C.cyan, C.bg, 2);
    lcd_rect(50, 22, 6, 14, C.green);
    lcd_text(60, 22, fmt_bytes(rx_last) + "/s", C.green, C.bg, 2);
    lcd_rect(180, 22, 6, 14, C.red);
    lcd_text(190, 22, fmt_bytes(tx_last) + "/s", C.red, C.bg, 2);

    // wwan0 line graph (RX green + TX red, overlaid)
    let rm = arr_minmax(hist.rx);
    let tm = arr_minmax(hist.tx);
    let mx1 = rm.max > tm.max ? rm.max : tm.max;
    if (mx1 < 10240) mx1 = 10240;
    draw_graph(4, 40, LCD_W - 8, 55, hist.rx, C.green, 0, mx1, null);
    // Overlay TX on same graph
    let n = length(hist.tx);
    if (n >= 2) {
        let pts = n > HIST_LEN ? HIST_LEN : n;
        let start = n - pts;
        let step_x = (LCD_W - 10) / (pts - 1);
        let prev_px = -1, prev_py = -1;
        for (let i = 0; i < pts; i++) {
            let val = hist.tx[start + i];
            let px = 5 + int(i * step_x);
            let py = 94 - int(val / mx1 * 53);
            if (py < 40) py = 40;
            if (py > 94) py = 94;
            lcd_rect(px, py, 2, 2, C.red);
            if (prev_px >= 0) {
                let dy = py - prev_py;
                let ys = dy > 0 ? prev_py : py;
                lcd_rect(px, ys, 1, (dy > 0 ? dy : -dy), C.red);
            }
            prev_px = px; prev_py = py;
        }
    }
    // Scale label
    lcd_text(4, 96, sprintf("max: %s/s", fmt_bytes(mx1)), C.gray, C.bg, 1);

    // br-lan (WiFi clients) — current speeds
    let br_rx = length(hist.br_rx) > 0 ? hist.br_rx[length(hist.br_rx) - 1] : 0;
    let br_tx = length(hist.br_tx) > 0 ? hist.br_tx[length(hist.br_tx) - 1] : 0;
    lcd_text(4, 110, "LAN", C.white, C.bg, 2);
    lcd_rect(50, 110, 6, 14, C.green);
    lcd_text(60, 110, fmt_bytes(br_rx) + "/s", C.green, C.bg, 2);
    lcd_rect(180, 110, 6, 14, C.red);
    lcd_text(190, 110, fmt_bytes(br_tx) + "/s", C.red, C.bg, 2);

    // br-lan line graph
    let brm = arr_minmax(hist.br_rx);
    let btm = arr_minmax(hist.br_tx);
    let mx2 = brm.max > btm.max ? brm.max : btm.max;
    if (mx2 < 10240) mx2 = 10240;
    draw_graph(4, 128, LCD_W - 8, 50, hist.br_rx, C.green, 0, mx2, null);
    // Overlay TX
    let n2 = length(hist.br_tx);
    if (n2 >= 2) {
        let pts = n2 > HIST_LEN ? HIST_LEN : n2;
        let start = n2 - pts;
        let step_x = (LCD_W - 10) / (pts - 1);
        let prev_px = -1, prev_py = -1;
        for (let i = 0; i < pts; i++) {
            let val = hist.br_tx[start + i];
            let px = 5 + int(i * step_x);
            let py = 177 - int(val / mx2 * 48);
            if (py < 128) py = 128;
            if (py > 177) py = 177;
            lcd_rect(px, py, 2, 2, C.red);
            if (prev_px >= 0) {
                let dy = py - prev_py;
                let ys = dy > 0 ? prev_py : py;
                lcd_rect(px, ys, 1, (dy > 0 ? dy : -dy), C.red);
            }
            prev_px = px; prev_py = py;
        }
    }
    lcd_text(4, 180, sprintf("max: %s/s", fmt_bytes(mx2)), C.gray, C.bg, 1);

    // Legend
    lcd_rect(4, 194, 8, 8, C.green);
    lcd_text(16, 194, "RX (download)", C.green, C.bg, 1);
    lcd_rect(160, 194, 8, 8, C.red);
    lcd_text(172, 194, "TX (upload)", C.red, C.bg, 1);

    draw_back();
    lcd_flush();
}


// =============================================
//  PAGE DRAWING DISPATCH
// =============================================

function draw_current() {
    switch (st.page) {
    case "dashboard": draw_dashboard(); break;
    case "menu":      draw_menu(); break;
    case "vpn":       draw_vpn_page(); break;
    case "wifi":      draw_wifi_page(); break;
    case "info":      draw_info_page(); break;
    case "ip":        draw_ip_page(); break;
    case "lte":       draw_lte_page(); break;
    case "traffic":   draw_traffic_page(); break;
    }
}


// =============================================
//  SCREENSAVER
// =============================================

function draw_screensaver() {
    lcd_clear(C.bg);
    let ts = clock_str();
    // Bouncing position (anti-burn-in)
    let x = 40 + ((st.saver_frame * 17) % (LCD_W - 120));
    let y = 40 + ((st.saver_frame * 11) % (LCD_H - 100));
    lcd_text(x, y, ts, C.dim, C.bg, 4);

    // Dim status line
    let d = st.data;
    let vpn = d?.vpn?.active;
    lcd_text(x, y + 40, vpn ? "VPN" : "---", vpn ? "#0320" : "#2000", C.bg, 2);

    st.saver_frame++;
    lcd_flush();
}


// =============================================
//  TOUCH HANDLING
// =============================================

// Run shell script from SCRIPTS dir (non-blocking with &)
function run_script(name, bg) {
    let cmd = SCRIPTS + "/" + name;
    if (bg) cmd += " &";
    system(cmd);
}

function go_page(p) {
    st.page = p;
    draw_current();
}

// Toast notification — overlay message with auto-dismiss
function toast(msg, color, bg_color, wait_sec) {
    color ??= C.white;
    bg_color ??= "#1082";
    wait_sec ??= 0;

    // Draw toast bar at bottom
    lcd_rect(0, LCD_H - 36, LCD_W, 36, bg_color);
    lcd_rect(0, LCD_H - 37, LCD_W, 1, color);  // top border
    lcd_text(10, LCD_H - 30, msg, color, bg_color, 2);
    lcd_flush();

    if (wait_sec > 0)
        system(sprintf("sleep %d", wait_sec));
}

// Full-screen action splash with progress dots
function action_splash(title, subtitle, color) {
    color ??= C.accent;
    lcd_clear(C.bg);
    lcd_rect(0, 0, LCD_W, HDR_H, C.hdr);
    lcd_text(4, 2, title, C.white, C.hdr, 2);
    lcd_text(LCD_W - 60, 2, clock_str(), C.cyan, C.hdr, 2);

    // Centered subtitle
    lcd_text(20, 90, subtitle, color, C.bg, 3);

    // Animated dots
    lcd_rect(130, 140, 60, 8, C.bg);
    lcd_text(130, 140, ". . . .", C.gray, C.bg, 2);
    lcd_flush();
}

// Button press animation — invert colors briefly
function flash_btn(bx, by, bw, bh, label) {
    lcd_rect(bx, by, bw, bh, C.white);
    lcd_text(bx + 4, by + 4, label ?? "", C.bg, C.white, 2);
    lcd_flush();
}

function handle_touch(tx, ty) {
    // Dashboard → Menu on any touch
    if (st.page == "dashboard") {
        go_page("menu");
        return;
    }

    // Back button (all sub-pages except menu)
    if (st.page != "menu" && ty >= BACK_Y - 10) {
        go_page("menu");
        return;
    }

    // Menu button detection
    if (st.page == "menu") {
        for (let i = 1; i <= 6; i++) {
            let b = btn_pos(i);
            if (in_rect(tx, ty, b.x, b.y, b.w, b.h)) {
                // Flash button with label
                let labels = st.mpg == 1
                    ? ["VPN", "LTE", "WiFi", "Info", "Traffic", ">>>"]
                    : ["Reboot", "LTE Reset", "IP", "Dashboard", "", "<<<"];
                flash_btn(b.x, b.y, b.w, b.h, labels[i - 1] ?? "");
                system("usleep 150000");

                if (st.mpg == 1) {
                    switch (i) {
                    case 1: go_page("vpn"); return;
                    case 2: go_page("lte"); return;
                    case 3: go_page("wifi"); return;
                    case 4: go_page("info"); return;
                    case 5: go_page("traffic"); return;
                    case 6: st.mpg = 2; draw_menu(); return;
                    }
                } else {
                    switch (i) {
                    case 1:
                        // Reboot with confirmation dialog
                        lcd_clear("#200000");
                        lcd_rect(30, 60, 260, 120, "#300000");
                        lcd_rect(30, 60, 260, 1, C.red);
                        lcd_text(80, 75, "REBOOT?", C.red, "#300000", 3);
                        lcd_rect(50, 120, 100, 35, C.red);
                        lcd_text(62, 128, "YES", C.white, C.red, 2);
                        lcd_rect(170, 120, 100, 35, "#0841");
                        lcd_text(190, 128, "NO", C.white, "#0841", 2);
                        // Countdown
                        for (let sec = 5; sec > 0; sec--) {
                            lcd_rect(120, 165, 80, 16, "#200000");
                            lcd_text(120, 165, sprintf("(%ds)", sec), C.gray, "#200000", 2);
                            lcd_flush();
                            system("sleep 1");
                            let ct = read_touch();
                            if (ct) {
                                if (ct.x < 160) {
                                    // YES
                                    action_splash("System", "Rebooting...", C.red);
                                    lcd_flush();
                                    run_script("reboot.sh");
                                    return;
                                } else {
                                    // NO
                                    toast("Cancelled", C.gray, "#1082", 1);
                                    draw_menu();
                                    return;
                                }
                            }
                        }
                        toast("Cancelled (timeout)", C.gray, "#1082", 1);
                        draw_menu();
                        return;
                    case 2:
                        // LTE Reset — скрипт делает ifdown+GPIO+ifup
                        action_splash("LTE", "Resetting modem...", C.yellow);
                        run_script("lte_reset.sh");
                        // Ждём завершения скрипта (~14 сек)
                        for (let step = 0; step < 7; step++) {
                            system("sleep 2");
                            let msgs = ["Disconnecting...", "GPIO reset...", "Waiting...",
                                       "Reconnecting...", "Waiting...", "Checking...", "Done"];
                            lcd_rect(20, 140, 280, 20, C.bg);
                            lcd_text(20, 140, msgs[step], C.gray, C.bg, 2);
                            lcd_flush();
                        }
                        refresh_data();
                        draw_menu();
                        let csq = int(+(st.data?.lte?.csq ?? 0));
                        toast(csq > 0 ? sprintf("LTE OK  CSQ:%d", csq) : "LTE: no signal",
                              csq > 0 ? C.green : C.red,
                              csq > 0 ? "#002000" : "#200000", 2);
                        draw_menu();
                        return;
                    case 3:
                        go_page("ip");
                        return;
                    case 4:
                        go_page("dashboard");
                        return;
                    case 6:
                        st.mpg = 1;
                        draw_menu();
                        return;
                    }
                }
                draw_menu();
                return;
            }
        }
        return;
    }

    // VPN page buttons
    if (st.page == "vpn") {
        let vpn_names = ["WireGuard", "OpenVPN", "L2TP", "VPN OFF"];
        let vpn_scripts = ["vpn_wg_on.sh", "vpn_ovpn_on.sh", "vpn_l2tp_on.sh", "vpn_off.sh"];
        let vpn_colors = [C.cyan, C.green, C.yellow, C.red];

        for (let i = 0; i < 4; i++) {
            let b = btn_pos(i + 1);
            if (in_rect(tx, ty, b.x, b.y, b.w, b.h)) {
                flash_btn(b.x, b.y, b.w, b.h, vpn_names[i]);

                if (i == 3) {
                    action_splash("VPN", "Disconnecting...", C.red);
                    run_script("vpn_off.sh");
                    system("sleep 3");
                    refresh_data();
                    toast("VPN OFF", C.red, "#200000", 2);
                } else {
                    action_splash("VPN", vpn_names[i] + "...", vpn_colors[i]);
                    run_script("vpn_off.sh");
                    system("sleep 2");
                    run_script(vpn_scripts[i]);

                    for (let step = 0; step < 10; step++) {
                        system("sleep 1");
                        lcd_rect(20, 140, 280, 20, C.bg);
                        lcd_text(20, 140, sprintf("%s... %ds", vpn_names[i], step + 1), C.gray, C.bg, 2);
                        lcd_flush();
                    }
                    refresh_data();
                    let ok = st.data?.vpn?.active;
                    toast(ok ? vpn_names[i] + " Connected" : vpn_names[i] + ": failed",
                          ok ? C.green : C.red,
                          ok ? "#002000" : "#200000", 2);
                }
                draw_vpn_page();
                return;
            }
        }
    }
}


// =============================================
//  SCREEN STATE MACHINE
// =============================================

function set_screen(s) {
    if (s == st.screen) return;
    st.screen = s;

    if (s == "active") {
        // Backlight on
        run_script("backlight.sh 1");
        st.page = "dashboard";
        st.mpg = 1;
        refresh_data();
        draw_dashboard();
    } else if (s == "screensaver") {
        st.saver_frame = 0;
        draw_screensaver();
    } else if (s == "off") {
        lcd_clear(C.bg);
        lcd_flush();
        run_script("backlight.sh 0");
    }
}


// =============================================
//  MAIN
// =============================================

function main() {
    warn(sprintf("lcd_ui: starting (ucode) ubus=%s uci=%s uloop=%s\n",
        uconn ? "OK" : "NO",
        ucur  ? "OK" : "NO",
        uloop_mod ? "OK" : "NO"));

    // Wait for lcd_drv splash logo
    system("sleep 3");

    // Stop splash: ioctl(0) via flush
    run_script("backlight.sh 1");
    system("printf '\\0' > /dev/lcd 2>/dev/null");

    // Initial data + draw
    refresh_data();
    draw_dashboard();

    // === uloop event-driven mode ===
    if (uloop_mod) {
        uloop_mod.init();

        // Data refresh + redraw (every 2s)
        let data_t;
        data_t = uloop_mod.timer(T.data * 1000, function() {
            refresh_data();
            if (st.screen == "active")
                draw_current();
            else if (st.screen == "screensaver")
                draw_screensaver();
            data_t.set(T.data * 1000);
        });

        // Touch polling (every 100ms)
        let touch_t;
        touch_t = uloop_mod.timer(100, function() {
            let t = read_touch();
            if (t) {
                st.ltch = time();
                if (st.screen != "active")
                    set_screen("active");
                else
                    handle_touch(t.x, t.y);
            }
            // Poll slower when screen is off
            touch_t.set(st.screen == "off" ? 500 : 100);
        });

        // Idle check (every 1s)
        let idle_t;
        idle_t = uloop_mod.timer(1000, function() {
            let idle = time() - st.ltch;
            if (st.screen == "active" && idle >= T.saver)
                set_screen("screensaver");
            else if (st.screen == "screensaver" && idle >= T.off)
                set_screen("off");
            idle_t.set(1000);
        });

        // Anti-burn-in shift (every 30s)
        let burnin_t;
        burnin_t = uloop_mod.timer(T.burnin * 1000, function() {
            st.ox = (st.frame % 5) - 2;
            st.oy = (int(st.frame / 3) % 5) - 2;
            st.frame++;
            burnin_t.set(T.burnin * 1000);
        });

        warn("lcd_ui: uloop running\n");
        uloop_mod.run();

    // === Fallback: poll loop ===
    } else {
        warn("lcd_ui: fallback poll loop (no uloop)\n");
        let last_data = 0;
        let last_burnin = time();

        while (true) {
            let now = time();

            // Data refresh
            if (now - last_data >= T.data) {
                refresh_data();
                last_data = now;
            }

            // Touch
            let t = read_touch();
            if (t) {
                st.ltch = now;
                if (st.screen != "active")
                    set_screen("active");
                else
                    handle_touch(t.x, t.y);
            }

            // Idle
            let idle = now - st.ltch;
            if (st.screen == "active" && idle >= T.saver)
                set_screen("screensaver");
            else if (st.screen == "screensaver" && idle >= T.off)
                set_screen("off");

            // Burn-in
            if (now - last_burnin >= T.burnin) {
                st.ox = (st.frame % 5) - 2;
                st.oy = (int(st.frame / 3) % 5) - 2;
                st.frame++;
                last_burnin = now;
            }

            // Redraw
            if (st.screen == "active" && now - st.ldraw >= T.data) {
                draw_current();
                st.ldraw = now;
            } else if (st.screen == "screensaver") {
                draw_screensaver();
            }

            // Sleep (usleep via system call)
            let us = st.screen == "off" ? 500000 : 100000;
            system(sprintf("usleep %d", us));
        }
    }
}

// Single run — procd handles respawn on crash
main();
