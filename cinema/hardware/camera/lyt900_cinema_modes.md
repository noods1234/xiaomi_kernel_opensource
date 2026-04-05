# Sony LYT-900 Cinema Capture Modes — Xiaomi 14 Ultra (aurora)

**Sensor:** Sony LYT-900 (Lytia series)
**Package:** 1-inch stacked CMOS, 50MP (8192×6144), 4-lane MIPI CSI-2
**Device:** Xiaomi 14 Ultra (codename: aurora, SoC: SM8650 Pineapple)
**Driver:** Proprietary Qualcomm/Leica HAL — no upstream kernel driver required
**Community mod ref:** https://github.com/Jip-Hop/xiaomi-14-ultra-raw-video-mod-guide/

---

## 1. Sensor Specifications

| Parameter | Value |
|-----------|-------|
| Active area | 9.60 mm × 7.20 mm (1-inch diagonal: ~13.1 mm) |
| Resolution | 8192 × 6144 (50.3 MP) |
| Pixel size | ~1.6 µm |
| Readout | Stacked CMOS, dual conversion gain (DCG) |
| MIPI lanes | 4 × up to 2.5 Gbps/lane |
| Output formats | RAW16 (Bayer RGGB), RAW10 (Bayer RGGB) |
| ISO range | 50–6400 (native); ~50–25600 with DCG |
| Dynamic range | ~12.5 EV standard; ~14 EV with DCG4; ~16.5 EV with DCG16 |

---

## 2. Cinema Capture Modes

### Mode 0: 4K Binned (Default)

| Parameter | Value |
|-----------|-------|
| Output resolution | 3840 × 2160 |
| Binning | 2×2 on-chip binning |
| Max frame rate | 60fps (RAW16), 120fps (RAW10) |
| MIPI bandwidth | 4-lane @ 1.2 Gbps/lane |
| Storage rate | ~900 MB/s at 120fps RAW10 (4K/120) |
| Use case | Standard cinema capture, preview |

---

### Mode 1: Fullres 8K (no binning)

| Parameter | Value |
|-----------|-------|
| Output resolution | 8192 × 6144 |
| Binning | None (full pixel readout) |
| Max frame rate | 30fps (RAW10) |
| MIPI bandwidth | 4-lane @ 2.5 Gbps/lane |
| Storage rate | ~360 MB/s at 30fps RAW10 |
| UFS 4.0 headroom | 360 / 2800 MB/s = **12.9%** — well within limit |
| Use case | Stills-from-video, maximum resolution capture |

Community mod name: **"fullres"** — bypasses 2×2 on-chip binning to expose full 8192×6144 active area at reduced frame rates.

Reported by community: **8192×6144 RAW10 @ 30fps** confirmed on production hardware.

---

### Mode 2: 4K 120fps

| Parameter | Value |
|-----------|-------|
| Output resolution | 3840 × 2160 |
| Readout region | Centre crop (reduced vertical FOV) |
| Frame rate | 120fps RAW10 |
| MIPI bandwidth | 4-lane @ 2.5 Gbps/lane |
| Storage rate | 225 MB/s |
| UFS 4.0 headroom | 225 / 2800 MB/s = **8%** — no storage concern |
| CPU pin required | YES — cinema_mode must be active |
| Use case | Slow-motion 5× downsampled to 24fps |

**Kernel dependency:** Requires `cinema_mode` driver active (`persist.cinema.mode=1`) to pin all 4 SM8650 cpufreq domains to max freq. Without CPU pin the RAW encode pipeline will throttle and drop frames under sustained thermal load.

---

### Mode 3: 8K 30fps

| Parameter | Value |
|-----------|-------|
| Output resolution | 8192 × 6144 |
| Readout | Full sensor, accelerated scan |
| Frame rate | 30fps RAW10 |
| MIPI bandwidth | 4-lane @ 2.5 Gbps/lane |
| Storage rate | ~360 MB/s (same as fullres) |
| UFS 4.0 headroom | 12.9% — within limit |
| CPU pin required | YES |
| Use case | 8K cinema recording |

> **Note:** 8K/30fps software APV encoding is infeasible on SM8650 CPU alone (sim result: P50 < 1.0×). This mode requires writing uncompressed RAW10 directly to UFS — 360 MB/s is well within UFS 4.0 capacity.

---

## 3. Output Format Modes

### RAW16 (Standard)
- Bayer RGGB 16-bit packed
- Full 14-bit sensor dynamic range preserved in 16-bit container
- Larger files: ~190 MB/frame at 8K

### RAW10 "LN2 mode"
- Bayer RGGB 10-bit packed (MIPI DATA_TYPE = 0x2B)
- Stacked CMOS architecture: improved per-pixel noise at ISO 50–400
- ~37% smaller files than RAW16
- Preferred format for MotionCam

---

## 4. DCG (Dual Conversion Gain) Modes

The LYT-900 uses in-sensor dual-exposure blending to extend dynamic range without software HDR merging.

| Mode | Gain Ratio | DR Extension | ISO Blend |
|------|-----------|-------------|-----------|
| Off | 1× | baseline ~12.5 EV | single readout |
| DCG4 | ~4× | +2 stops ≈ 14.5 EV | ISO 50 + ISO 200 |
| DCG16 | ~16× | +4 stops ≈ 16.5 EV | ISO 50 + ISO 800 |

Both exposures are read out simultaneously in a single frame; no inter-frame ghosting.

---

## 5. Binning Zoom (Unbinned Digital Zoom)

Reads an unbinned crop of the full sensor; achieves optical-quality zoom without a separate telephoto lens.

| Setting | Crop region | Effective zoom |
|---------|------------|----------------|
| 1× | Full 8192×6144 | 1× (native) |
| 2× | 4096×3072 centre | 2× |
| 5× | 1638×1229 centre | 5× |

Output is upscaled to target resolution by the ISP.

---

## 6. MotionCam Integration

### Camera2 HAL3 Requirements

| Capability | Status | Property |
|------------|--------|----------|
| FULL hardware level | Required | `persist.vendor.camera.HAL3.enabled=1` |
| RAW stream (RAW16/RAW10) | Required | `persist.camera.raw.enable=1` |
| Manual sensor control | Required | Camera2 MANUAL_SENSOR |
| In-flight burst requests | 8 frames | `persist.camera.maxinflight=8` |
| Disable EIS | Required | `persist.camera.eis.enable=0` |
| Disable TNR | Required | `persist.camera.tnr.enable=0` |
| Disable MFNR | Required | `persist.camera.mfnr.enable=0` |

### Android Property Gate

Extended cinema modes are gated by properties in `cinema/system.prop`:

```
persist.camera.video.4k120.enable=1   # 4K/120fps RAW mode
persist.camera.video.8k30.enable=1    # 8K/30fps RAW mode
persist.camera.res.fullres.enable=1   # fullres 8K stills
persist.camera.dcg.enable=1           # DCG mode
persist.camera.unbinned_zoom.enable=1 # unbinned 2×/5× zoom
persist.camera.raw10.enable=1         # RAW10 "LN2" output
```

### MotionCam App Flow

```
MotionCam app
  → Camera2 CaptureRequest (manual exposure/gain/WB)
  → CameraHAL3 (Qualcomm Leica HAL, aurora firmware)
    → APatch HAL interception (community mod layer)
      → Mode unlock: fullres / DCG / LN2 / 4K120 / 8K30
    → Qualcomm CAMSS (camss-csid, camss-vfe)
    → MIPI CSI-2 PHY → LYT-900 sensor
  → DMA buffer (ION/videobuf2-dma-contig) → RAW frame
  → MotionCam GPU pipeline (Adreno 750 Vulkan compute)
  → UFS 4.0 write (225–360 MB/s)
```

---

## 7. cinema_mode Kernel Driver Interaction

| Mode | cinema_mode required | Reason |
|------|---------------------|--------|
| 4K binned ≤60fps | Optional | Enough CPU headroom without pin |
| 4K 120fps | **Mandatory** | A720 + X4 must stay at max freq |
| 8K 30fps uncompressed | **Mandatory** | Sustained UFS write + ISP throughput |
| Fullres 8K ≤30fps | Recommended | ISP pipeline bandwidth |

Enable with: `adb shell setprop persist.cinema.mode 1`
Or via Cinema app toggle → `init.cinema.rc` `on property:persist.cinema.mode=1` trigger.

---

## 8. TODO / Pending

- [ ] Confirm SM8650 MIPI D-PHY timing parameters (camcc-kalama / pineapple clock config)
- [ ] Validate 4K/120fps mode against actual MIPI bandwidth on aurora DT
- [ ] Measure CRA at LYT-900 corner pixels vs LC-Tec eND angular tolerance (eND integration risk)
- [ ] Test DCG modes via MotionCam custom capture request
- [ ] Map APatch HAL hook locations for community mod compatibility layer
