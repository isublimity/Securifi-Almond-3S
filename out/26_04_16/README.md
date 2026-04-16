# Build 2026-04-16 — OpenWrt 24.10.6 (upstream, vermagic compatible)

## Firmware

| File | Size | Description |
|------|------|-------------|
| `openwrt-24.10.6-almond3s-sysupgrade.bin` | 7.6MB | Sysupgrade image |
| `kmod-lcd-gpio_6.6.127.2.0-r1_mipsel_24kc.ipk` | 20K | LCD kernel module |
| `lcd-ui_2.0-r1_mipsel_24kc.ipk` | 26K | LCD UI userspace (lcd_render + data_collector + touch_poll + lcd_ui.uc) |

## Specs

- **OpenWrt**: 24.10.6 stable (r29141-81be8a8869)
- **Kernel**: 6.6.127
- **Vermagic**: `f31f6f85a36836e510d64a18a9a5f1bf` (matches official upstream!)
- **Target**: ramips/mt7621, securifi_almond-3s
- **Default IP**: 192.168.11.1
- **Hostname**: Almond3S, TZ: MSK-3

## Included packages

- LCD: kmod-lcd-gpio, lcd-ui (from GitHub isublimity/Securifi-Almond-3S)
- LTE: kmod-usb-net-cdc-mbim, umbim, luci-proto-mbim, kmod-usb-acm
- LuCI: luci-ssl, luci-app-package-manager
- Utils: nano, curl, opkg
- ucode: ucode-mod-fs/ubus/uci/uloop/socket/digest

## NOT included (install via opkg)

```bash
opkg update
opkg install kmod-wireguard wireguard-tools luci-proto-wireguard
opkg install openvpn-openssl kmod-tun
opkg install xl2tpd kmod-l2tp kmod-pppol2tp ppp-mod-pppol2tp
```

opkg works with upstream repos — vermagic matches official 24.10.6.

**Note**: if `opkg update` fails with SSL errors, switch to HTTP:
```bash
sed -i 's|https://|http://|g' /etc/opkg/distfeeds.conf
```

## Build recipe

```bash
git clone --branch v24.10.6 --depth 1 https://github.com/openwrt/openwrt.git
cd openwrt
git apply almond3s.patch   # PR #22141
# Add lcd-gpio + lcd-ui packages, uci-defaults
wget config.buildinfo -O .config
# Single device + defconfig + packages
make -j$(nproc)
```

## Flash

```bash
sysupgrade -n -F /tmp/openwrt-24.10.6-almond3s-sysupgrade.bin
# After boot + jffs2 init (~11 min):
./install_to_router.sh
```
