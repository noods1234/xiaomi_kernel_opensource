# Xiaomi USB-C Dock — Daughter Board Hardware Design Specification

**Document version:** 1.0
**Branch:** `claude/xiaomi-usb-dock-board-K0ILH`
**Target kernel branches:** `muyu-v-oss`, `sheng-u-oss`, `aurora-u-oss`, `xuanyuan-v-oss`, `goku-u-oss`

---

## 1. Overview

The Xiaomi USB-C Dock daughter board is a single-cable docking station that connects to any compatible Xiaomi smartphone or tablet via USB Type-C. It expands the device into a full desktop-class peripheral hub.

```
                    ┌──────────────────────────────────────────────────┐
                    │           XIAOMI USB-C DOCK (daughter board)     │
                    │                                                  │
  ┌──────────┐      │  ┌─────────┐   ┌───────┐   ┌────────────────┐   │
  │  Xiaomi  │ USB-C│  │ GL3523  │   │AG9311 │   │   HDMI 2.0     ├───┼─── HDMI out
  │  phone / ├──────┤  │ USB 3.1 ├───│DP→HDMI│   │    port        │   │
  │  tablet  │      │  │ Gen2 hub│   └───────┘   └────────────────┘   │
  └──────────┘      │  │ (5-port)│                                     │
                    │  │         ├─────────────────────────────────────┼─── USB-A ×4
                    │  │         │   ┌───────────┐                     │    (USB 3.1 G2)
                    │  └─────────┘   │ AX88179A  ├─────────────────────┼─── GbE RJ-45
                    │                │ USB3→GbE  │                     │
                    │                └───────────┘                     │
                    │                                                  │
                    │  ┌─────────┐   ┌───────────┐                     │
                    │  │FUSB302B │   │ RTS5328   ├─────────────────────┼─── SD/microSD
                    │  │  PD IC  │   │ USB3 card │                     │
                    │  └─────────┘   │  reader   │                     │
                    │                └───────────┘                     │
                    │                                                  │
                    │  ┌────────────────────────────────────────────┐  │
                    │  │    USB-C power input (upstream charger)    ├──┼─── USB-C PD in
                    │  │    TPS65987D PD controller                 │  │    (up to 100W)
                    │  └────────────────────────────────────────────┘  │
                    │                                                  │
                    │  ┌──────────────┐                                │
                    │  │ 3.5 mm audio │                                │
                    │  │ jack (TRRS)  ├────────────────────────────────┼─── Audio out
                    │  └──────────────┘                                │
                    └──────────────────────────────────────────────────┘
```

---

## 2. Compatible Devices

| Device | SoC | USB-C Spec | DP Alt Mode | PD Version | Kernel Branch |
|--------|-----|-----------|-------------|-----------|---------------|
| Xiaomi 14 Ultra | SM8650 | USB 3.2 Gen 2 | DP 1.4 (4-lane) | PD 3.0 PPS | `aurora-u-oss` |
| Xiaomi 15 Ultra | SM8750 | USB 3.2 Gen 2 | DP 1.4 (4-lane) | PD 3.1 EPR | `xuanyuan-v-oss` |
| Xiaomi Pad 6 Pro | SM8475 | USB 3.2 Gen 2 | DP 1.4 (4-lane) | PD 3.0 | `liuqin-t-oss` |
| Xiaomi Pad 6S Pro 12.4 | SM8550 | USB 3.2 Gen 2 | DP 1.4 (4-lane) | PD 3.0 PPS | `sheng-u-oss` |
| Xiaomi Pad 7 Pro | SM8750 | USB 3.2 Gen 2 | DP 1.4 (4-lane) | PD 3.1 EPR | `muyu-v-oss` |
| Xiaomi MIX Fold 4 | SM8650 | USB 3.2 Gen 2 | DP 1.4 | PD 3.0 PPS | `goku-u-oss` |
| Xiaomi 14T | MT6985 | USB 3.2 Gen 2 | DP 1.4 | PD 3.0 | `bsp-degas-u-oss` |

> **Note:** Devices running Android Q and earlier typically lack DP Alt Mode support. Target Android T (13) and newer for full dock functionality.

---

## 3. Connector & Interface Specification

### 3.1 Phone-side USB-C Connector (J1)

| Signal | USB-C Pin | Description |
|--------|-----------|-------------|
| VBUS | A4, A9, B4, B9 | 5 V (from dock, up to 900 mA) or negotiated PD |
| GND | A1, A12, B1, B12 | Common ground |
| TX1+/TX1− | A2, A3 | USB 3.x / DP TX SuperSpeed differential pair |
| RX1+/RX1− | B10, B11 | USB 3.x / DP RX SuperSpeed differential pair |
| TX2+/TX2− | B2, B3 | DP TX lane 2 (4-lane DP) or USB 3.x TX (2-lane DP) |
| RX2+/RX2− | A10, A11 | DP RX lane 2 (4-lane DP) or USB 3.x RX (2-lane DP) |
| D+ | A6 | USB 2.0 data + |
| D− | A7 | USB 2.0 data − |
| CC1 | A5 | Configuration channel 1 (PD, orientation, Alt Mode) |
| CC2 | B5 | Configuration channel 2 |
| SBU1 | A8 | Sideband channel 1 (audio, proprietary) |
| SBU2 | B8 | Sideband channel 2 |

**Mating connector:** USB Type-C receptacle, 24-pin, right-angle SMD
**Recommended part:** Amphenol 12401610E4#2A or equivalent
**Cable:** Passive USB-C cable, ≥ 10 Gbps rated, ≤ 1 m

### 3.2 Upstream Charger USB-C Input (J2)

Same pin assignment as J1 but acts as a PD sink port for the dock.
Supports USB PD 3.0/3.1, up to 100 W input (20 V / 5 A).

---

## 4. Key IC Selection

### 4.1 USB Hub — GL3523 (Genesys Logic)

| Parameter | Value |
|-----------|-------|
| USB version | USB 3.1 Gen 2 (10 Gbps) |
| Downstream ports | 4× USB-A + 1× USB-C |
| Upstream port | 1× USB-C (connects to phone) |
| Transaction Translator | Per-port (isolates USB 2.0 devices) |
| Power management | USB 3.x LTM, U1/U2 link states |
| Supply voltage | 3.3 V core, 5 V I/O |
| Package | QFN-64 |
| Interface | USB 3.1 Gen 2 upstream; USB 3.1/2.0 downstream |

**Alternative:** VL822 (VIA Labs), CH569 (WCH) for cost-reduced variants.

### 4.2 DP-to-HDMI Bridge — AG9311 (Analogix)

| Parameter | Value |
|-----------|-------|
| Input | DisplayPort 1.4 (HBR3, 8.1 Gbps / lane) |
| Output | HDMI 2.0 (18 Gbps) |
| Max resolution | 4K UHD (3840×2160) @ 60 Hz, 4:4:4 |
| HDR support | HDR10, HLG |
| Audio | HDMI ARC, multi-channel |
| I²C config | Yes (firmware via SPI flash) |
| Supply | 1.2 V core, 3.3 V I/O |
| Package | BGA-64 |

**Alternative:** PTN3460 (NXP) for DP 1.2 → HDMI 1.4 (lower cost).

### 4.3 USB3-to-GbE — AX88179A (ASIX)

| Parameter | Value |
|-----------|-------|
| USB interface | USB 3.2 Gen 1 (5 Gbps) |
| Ethernet | 10/100/1000BASE-T |
| Wake-on-LAN | Yes |
| Checksum offload | TCP/UDP/IP IPv4/IPv6 |
| Supply | 3.3 V |
| Package | QFN-48 |

### 4.4 SD Card Reader — RTS5328 (Realtek)

| Parameter | Value |
|-----------|-------|
| USB interface | USB 3.2 Gen 1 |
| Card types | SD 3.0 (UHS-I), microSD 3.0 |
| Max speed | UHS-I SDR104 (104 MB/s) |
| Supply | 3.3 V |
| Package | QFN-48 |

### 4.5 USB PD Controller — FUSB302B (onsemi) + TPS65987D (TI)

Two-IC approach:
- **FUSB302B**: CC logic and low-level PD PHY (phone-side port J1)
- **TPS65987D**: Full-featured PD policy engine with PPS for charger port J2

| Parameter | Value |
|-----------|-------|
| PD revision | USB PD 3.0 (FUSB302B), PD 3.1 EPR (TPS65987D) |
| PPS | Yes (Programmable Power Supply) |
| Max power | 100 W (J2 input), 65 W forwarded to phone |
| Interface | I²C to MCU |
| Power path | Separate from hub (clean power delivery) |

### 4.6 System MCU — STM32G0B1 (STMicroelectronics)

| Parameter | Value |
|-----------|-------|
| Core | ARM Cortex-M0+ @ 64 MHz |
| Flash | 512 KB |
| RAM | 144 KB |
| I²C | 3× (connects to FUSB302B, TPS65987D, AG9311) |
| GPIO | 30+ (hub reset, power switches, LEDs) |
| USB | USB 2.0 FS (for firmware update) |
| Supply | 3.3 V |
| Package | LQFP-48 |

Firmware responsibilities:
- PD negotiation state machine
- Power budget calculation and allocation
- DP Alt Mode VDO exchange coordination
- Hub/bridge power sequencing on dock attach/detach
- LED status indicators
- Thermal monitoring (NTC thermistor)

---

## 5. Power Architecture

```
USB-C charger input (J2)
       │
       ▼
  TPS65987D PD controller
  (negotiate up to 100 W)
       │
       ├──────────────────────┐
       │                      │
       ▼                      ▼
  5 V / 3 A buck          Phone VBUS output
  (dock hub + bridge)     (via J1, negotiated PD)
       │                  Up to 65 W
       ├── GL3523 (hub)   (20V/3.25A or 15V/4A etc.)
       ├── AG9311 (DP bridge)
       ├── AX88179A (GbE)
       └── RTS5328 (SD)

If NO charger connected (phone-powered mode):
  Phone supplies 5V/0.9A on J1 VBUS
  Dock operates hub + bridge only
  GbE and SD reader function at lower power
  No phone charging
```

### 5.1 Power Rails

| Rail | Voltage | Source | Consumers |
|------|---------|--------|-----------|
| VBUS_IN | 5–20 V | J2 charger | TPS65987D |
| VBUS_PHONE | 5–20 V negotiated | TPS65987D | J1 phone |
| VDD_HUB | 3.3 V | Buck from VBUS_IN/VBUS_PHONE | GL3523 |
| VDD_BRIDGE | 3.3 V / 1.2 V | LDO from VDD_HUB | AG9311 |
| VDD_GBE | 3.3 V | VDD_HUB | AX88179A |
| VDD_SD | 3.3 V | VDD_HUB | RTS5328 |
| VDD_MCU | 3.3 V | LDO | STM32G0B1 |
| VBUS_USB_A | 5 V | Dedicated 5V/1.5A per port | USB-A host ports |

### 5.2 Power Sequencing

```
J1 detect (CC) → MCU wakes → VBUS_IN negotiated (J2) → VDD_HUB on →
GL3523 reset released → AG9311 reset released → DP Alt Mode entered →
VBUS_PHONE negotiated → Phone charges
```

---

## 6. PCB Layout Guidelines

### 6.1 Board Dimensions

| Variant | L × W | Height (with connectors) |
|---------|-------|--------------------------|
| Basic (hub only) | 100 × 40 mm | 14 mm |
| Pro (hub + DP + LAN) | 130 × 50 mm | 16 mm |
| Ultra (full) | 160 × 60 mm | 18 mm |

**Layer stack (4-layer minimum, 6-layer recommended for Ultra):**
```
Layer 1: Signal (USB SS traces, DP traces — 100 Ω diff pairs)
Layer 2: GND plane (solid, unbroken under all high-speed traces)
Layer 3: Power planes (VBUS, VDD_HUB, VDD_BRIDGE)
Layer 4: Signal (I²C, MCU GPIO, HDMI signals)
[Layer 5: GND pour]               ← 6-layer only
[Layer 6: HDMI / audio signals]   ← 6-layer only
```

### 6.2 Signal Integrity Rules

| Signal | Trace width | Spacing | Impedance |
|--------|-------------|---------|-----------|
| USB 3.x SS diff pair | 0.1 mm | 0.1 mm | 85 Ω ± 10% |
| DP 1.4 diff pair | 0.1 mm | 0.1 mm | 85 Ω ± 10% |
| HDMI diff pair | 0.1 mm | 0.1 mm | 100 Ω ± 10% |
| USB 2.0 D+/D− | 0.15 mm | 0.15 mm | 90 Ω ± 10% |
| I²C (MCU) | 0.2 mm | — | — |
| Power (VDD_HUB) | 0.8 mm min | — | — |
| VBUS traces | 1.5 mm min | — | — |

### 6.3 ESD Protection

All external connectors must have TVS arrays:
- J1 (phone USB-C): PRTR5V0U2X (5-line, 0.5 pF) on all 24 pins
- J2 (charger USB-C): PRTR5V0U2X on CC/SBU; heavier TVS on VBUS
- USB-A ports: USBLC6-2SC6 (4-line) per port
- HDMI: TPD12S521 or equivalent on all HDMI differential pairs
- RJ-45: Integrated magnetics module with 2 kV isolation

---

## 7. Firmware Interface (MCU ↔ Kernel Driver)

The STM32G0B1 MCU presents as a USB HID device to the host kernel and
communicates via the dock driver using vendor-specific HID reports.

### 7.1 HID Report Format

| Byte | Field | Description |
|------|-------|-------------|
| 0 | Report ID | 0x01 = status; 0x02 = command |
| 1 | Dock state | 0=idle, 1=detecting, 2=ready, 3=error |
| 2 | PD voltage[7:0] | Contracted voltage (units: 50 mV) |
| 3 | PD voltage[15:8] | High byte |
| 4 | PD current[7:0] | Contracted current (units: 10 mA) |
| 5 | PD current[15:8] | High byte |
| 6 | Capabilities | Bitmap: hub/dp/hdmi/lan/pd/sd/audio |
| 7 | DP lanes | 0=none, 2=2-lane, 4=4-lane |
| 8 | Error code | 0=none, 1=PD fail, 2=hub fail, 3=thermal |

### 7.2 Kernel Driver ↔ MCU Commands

| Command | Byte 1 | Description |
|---------|--------|-------------|
| SET_HOST_MODE | 0x10 | Switch phone USB to host |
| SET_DEVICE_MODE | 0x11 | Switch phone USB to device |
| SET_DP_LANES | 0x12 | Configure DP lane count |
| SET_PD_LIMIT | 0x13 | Set max phone charging power |
| MCU_FW_UPDATE | 0xFF | Enter DFU mode for MCU firmware update |

---

## 8. Compliance & Certification Requirements

| Standard | Applies To | Requirement |
|----------|-----------|-------------|
| USB 3.2 Gen 2 | Hub, SS ports | USB-IF certification |
| USB Power Delivery 3.0 | PD IC | USB-IF PD certification |
| DisplayPort 1.4 | DP Alt Mode | VESA DP compliance |
| HDMI 2.0 | HDMI port | HDMI LA compliance |
| CE (Europe) | Entire dock | EMC Directive 2014/30/EU |
| FCC (USA) | Entire dock | Part 15 Class B |
| RoHS | PCB + components | WEEE / RoHS Directive |
| IEC 62368-1 | Power handling | Audio/Video safety |

---

## 9. Kernel Configuration

Add to your device's `defconfig`:

```
CONFIG_USB=y
CONFIG_USB_XHCI_HCD=y
CONFIG_USB_EHCI_HCD=y
CONFIG_USB_HUB=y
CONFIG_TYPEC=y
CONFIG_TYPEC_TCPM=y
CONFIG_TYPEC_FUSB302=y
CONFIG_TYPEC_DP_ALTMODE=y
CONFIG_USB_ROLE_SWITCH=y
CONFIG_EXTCON=y
CONFIG_EXTCON_USB_GPIO=y
CONFIG_USB_NET_AX88179_178A=y       # GbE adapter
CONFIG_MMC=y
CONFIG_MMC_REALTEK_USB=y            # SD card reader
CONFIG_SND_USB_AUDIO=y              # USB audio
CONFIG_XIAOMI_USB_DOCK=m            # Dock driver (module)
```

---

## 10. Build & Flash Instructions

### 10.1 Build the kernel module

```bash
# From kernel root (e.g. muyu-v-oss branch)
make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- \
     menuconfig   # enable CONFIG_XIAOMI_USB_DOCK

make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- \
     drivers/usb/xiaomi-dock/

# Resulting module: drivers/usb/xiaomi-dock/xiaomi_usb_dock.ko
```

### 10.2 Build DTB overlay

```bash
make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- \
     arch/arm64/boot/dts/xiaomi/xiaomi-pad7-pro-dock.dtb
```

### 10.3 Push to device (Android)

```bash
adb root
adb push drivers/usb/xiaomi-dock/xiaomi_usb_dock.ko \
         /vendor/lib/modules/
adb push arch/arm64/boot/dts/xiaomi/xiaomi-pad7-pro-dock.dtbo \
         /data/local/tmp/
adb shell insmod /vendor/lib/modules/xiaomi_usb_dock.ko
```

### 10.4 Verify

```bash
# Check driver loaded
adb shell dmesg | grep "xiaomi-dock"

# Check dock state (after connecting dock)
adb shell cat /sys/bus/platform/devices/usb-dock/state
# Expected: connected

# Check capabilities
adb shell cat /sys/bus/platform/devices/usb-dock/capabilities
# Expected: usb_hub displayport hdmi ethernet pd_passthru sd_reader audio

# Check DP lanes
adb shell cat /sys/bus/platform/devices/usb-dock/dp_lanes
# Expected: 4

# Check PD contract
adb shell cat /sys/bus/platform/devices/usb-dock/pd_contract
# Expected: 20000mV / 3250mA  (when 65W charger connected)
```

---

## 11. Bill of Materials (Abbreviated)

| Ref | Part | Description | Qty |
|-----|------|-------------|-----|
| U1 | GL3523-MPTG2 | USB 3.1 Gen 2 hub IC | 1 |
| U2 | AG9311 | DP 1.4 → HDMI 2.0 bridge | 1 |
| U3 | AX88179A | USB3 → GbE controller | 1 |
| U4 | RTS5328 | USB3 → SD/microSD reader | 1 |
| U5 | FUSB302B | USB PD controller (phone port) | 1 |
| U6 | TPS65987D | USB PD controller (charger port) | 1 |
| U7 | STM32G0B1CEU6 | System MCU | 1 |
| U8 | TLV62568 | 5V/3A synchronous buck (hub supply) | 1 |
| U9 | TPS2061C | USB-A power switch, 1.5A (×4) | 4 |
| J1 | USB-C receptacle 24P | Phone USB-C port | 1 |
| J2 | USB-C receptacle 24P | Charger USB-C port | 1 |
| J3–J6 | USB-A 3.0 receptacle | USB-A host ports | 4 |
| J7 | HDMI type-A | HDMI output | 1 |
| J8 | RJ-45 + magnetics | GbE with integrated transformer | 1 |
| J9 | SD card slot | Full-size SD (push-push) | 1 |
| J10 | microSD slot | microSD (push-push) | 1 |
| J11 | 3.5mm TRRS jack | Headset audio | 1 |
| D1–D4 | WS2812B-Mini | RGB status LEDs (port activity) | 4 |
| F1 | 3A PTC fuse | VBUS_IN polyfuse | 1 |
| Various | Passives | Caps, resistors, ferrites | ~120 |

**Estimated BOM cost (Ultra variant, 1k units):** ~$18–22 USD

---

*End of Hardware Design Specification v1.0*
