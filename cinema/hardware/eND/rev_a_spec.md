# eND Rev A Integration Specification
## Cinema phone conversion — Sony LYT-900 / Xiaomi 14 Ultra

**Revision:** A-0 (bench mule) → A-1 (integrated prototype)
**Sensor target:** Sony LYT-900 (1-inch stacked BSI, 9.6 mm × 7.2 mm active area, 13.1 mm diagonal, 50MP 8192×6144)
**Optical element:** LC-Tec PolarView eND(NBf2.0) custom miniaturized cartridge
**Architecture:** rear-of-lens, pre-sensor, removable cartridge

---

## 1. Scope and constraints

### 1.1 Rev A goals

| Priority | Goal |
|----------|------|
| 1 | Stable optical behavior across 2–4 stop working band |
| 2 | Repeatable electronic control (setpoint → transmittance) |
| 3 | Removable / serviceable cartridge — no permanent bond to sensor |
| 4 | Calibration workflow (LUT generation, temperature-aware correction) |
| 5 | Android-side control interface (manual ND, exposure hold, DOF hold) |

### 1.2 Rev A non-goals

- Sony full cinema range (1/4 → 1/128, ~5 stops) — chased in Rev B once color
  shift and angular response are characterized on the actual cell
- Sensor-bonded permanent installation
- Perfect OEM-level color neutrality from first sample

### 1.3 Integration constraints

The eND cartridge must not:
- Add tilt exceeding the shim-correction range of the carrier
- Touch the LYT-900 cover glass (minimum 0.10 mm clearance)
- Obstruct module removal or flex routing
- Require mainboard rework in Rev A-0

The eND cartridge must:
- Be removable without disturbing the sensor module
- Expose all AR faces (front and rear)
- Have blackened edges and an integral light trap
- Accept a replacement cell without re-gluing the carrier frame

---

## 2. Optical stack definition

Placement: between lens rear element and LYT-900 cover glass, as close to sensor as
practical while maintaining the clearance constraint above.

```
LENS SIDE
─────────────────────────────────────────────────────
  Lens exit pupil / rear element

  [Air gap — existing optical path]

┌─────────────────────────────────────────────────────┐
│  AR protective window (front face)     ~0.30 mm     │
│  Optical cement or air gap             ~0.10 mm     │
│  LC eND cell — dual-cell guest-host    ~0.90 mm     │  CARTRIDGE
│  Optical cement or air gap             ~0.10 mm     │
│  AR protective window (rear face)      ~0.30 mm     │
└─────────────────────────────────────────────────────┘

  Precision spacer / gasket               0.10 mm min clearance

  LYT-900 cover glass (existing)           ~0.30 mm
  LYT-900 sensor array                     —
─────────────────────────────────────────────────────
SENSOR SIDE
```

**Total added optical path length (cartridge):** ~1.80 mm nominal
**Total cartridge mechanical thickness including frame:** 2.20–2.50 mm target

Window material preference: N-BK7 or fused silica, λ/4 flatness, AR coating both
faces, Ravg < 0.3% per surface at the sensor's primary wavelengths (400–700 nm).

---

## 3. Clear aperture specification

```
LYT-900 active area:          9.60 mm (H) × 7.20 mm (V)
Required overscan margin:    +1.20 mm each axis minimum
─────────────────────────────────────────────────────
Clear aperture (CA) target: 12.00 mm (H) × 9.60 mm (V)
CA diagonal:                 15.4 mm
```

The rectangular CA is preferred over a circular aperture.  A circular aperture
sized to pass the full diagonal (≥ 15.4 mm diameter) is also acceptable if the
cell vendor requires it, but rectangular minimizes cell material waste on small
formats and reduces the corner leakage and uniformity problem.

Chief-ray angle (CRA) at LYT-900 corner pixels: verify against Sony datasheet before
finalizing CA.  If CRA > 20° the angular transmittance uniformity claim in LC-Tec
literature must be validated on-bench before relying on it.

---

## 4. Cartridge frame specification

### 4.1 Material

- Primary: black-anodized 6061-T6 aluminum (Type III hard anodize)
- Alternative: black PEEK (lower thermal conductivity — useful if thermal isolation
  from the metal phone chassis is desirable)

Edges and all non-optical faces: matte black, Ra ≤ 1.6 µm to suppress scatter.

### 4.2 Envelope (target)

```
                        ┌──────────────────────┐
                        │                      │ 13.0 mm
       flex exit ─────► │  ┌──────────────┐   │
       (one edge)       │  │  clear       │   │
                        │  │  aperture    │   │
                        │  │  12 × 9.6 mm │   │
                        │  └──────────────┘   │
                        │                      │
                        └──────────────────────┘
                                16.0 mm

  Thickness (Z, lens-to-sensor axis):   2.20–2.50 mm
  Frame wall minimum:                   1.50 mm each side
  Flex connector exit width:            4.0 mm (6-pin, 0.5 mm pitch FFC/FPC)
```

Corners: radiused, R ≥ 1.0 mm to clear housing molding features.

### 4.3 Light trap

A knife-edge or stepped rabbet on the inner frame perimeter, blackened with
light-absorbing coating (Acktar Fractal Black or equivalent), reduces stray light
that bypasses the LC cell at the edges.  This is mandatory.  Omitting it causes
visible corner fogging at long shutter angles under high-contrast scenes.

### 4.4 Tilt shim provision

Carrier must include ≥ 3 shim seats (one per non-flex-exit corner plus one
center rear) accepting 0.05 mm shim stock.  Tilt budget for Rev A:
≤ 0.1 mrad after shimming.

### 4.5 Serviceability

Cell retention: two M1.2 or equivalent micro-fasteners, accessible from the front
(lens side) without removing the sensor module.  Replaceable without re-bonding the
carrier frame to the housing.

---

## 5. Electrical interface

### 5.1 LC drive requirements

| Parameter | Typical LC camera eND | Rev A target |
|-----------|----------------------|--------------|
| Drive waveform | AC square wave, low-frequency | 1–100 Hz, characterize on bench |
| Amplitude (Vrms) | 0–15 VRMS | 0–15 VRMS (match LCC-230 output range) |
| DC offset | < 0.1 V (avoid electrolysis) | enforce in driver firmware |
| Number of drive channels | 2 (dual-cell) | 2 |
| Control interface | programmable setpoint | serial (UART/SPI) or USB |

**Do not drive with DC.**  All LC films degrade under sustained DC bias.  The AC
square wave condition specified in LC-Tec characterization data is the correct
bench-start point.

### 5.2 Flex harness

Minimum 6 conductors:
- 2 × LC cell drive (one per cell, differential or referenced)
- 1 × cell temperature sense (TMP117 I²C SDA)
- 1 × I²C SCL
- 1 × GND
- 1 × +3.3 V (temperature sensor supply only — drive supply separate)

Connector: Molex PicoBlade 1.25 mm or equivalent, friction-lock, rated for
vibration in a handheld device.

---

## 6. Control subsystem

### 6.1 Rev A-0 bench mule

External controller: **LC-Tec LCC-230**
- Two independent programmable LC drive channels
- Up to 30 VRMS output
- USB control interface
- Use for all optical characterization before committing to a custom board

MCU bridge (Rev A-0): STM32U5 dev board (Nucleo-U575ZI-Q or equivalent)
- Acts as USB-to-serial bridge between Android host and LCC-230
- Runs calibration LUT logic and temperature polling
- No custom PCB required in Rev A-0

Temperature sensor: TMP117 on small breakout, taped or clamped to cartridge frame
near the cell.  Log temperature for every calibration sweep.

### 6.2 Rev A-1 integrated driver board

Custom PCB targeting: **25 mm × 12 mm × 1.2 mm** (fits inside grip sidecar or
along the phone chassis inner wall adjacent to the camera module).

Component partitions — see `schematic_partitions.txt` for full signal-level
breakdown.

Key ICs:

| Function | Part | Package |
|----------|------|---------|
| Supervisor MCU | STM32U5 (e.g. STM32U575CIT6) | UFBGA132, 7×7 mm |
| Temperature sense | TI TMP117 | WSON-8, 2×2 mm |
| Precision DAC | ADI AD5696R | TSSOP-16, 5×4.4 mm |
| Split-rail supply | TI TPS65131 | VQFN-24, 4×4 mm |
| Precision op amp | TI OPA2197 | SOIC-8, 5×4 mm |
| Analog switch | ADI ADG1419 | MSOP-8, 3×3 mm |

### 6.3 Operating modes (firmware)

Three modes must be implemented before any field test:

**Manual ND**
- Operator sets target ND value (stop or EV offset)
- Slew rate limited: ≤ 0.25 stop per frame at 24 fps (prevents visible stepping)
- Minimum step size: 0.1 stop virtual increment (LUT interpolated)

**Exposure hold**
- Shutter angle and ISO locked by operator
- ND value adjusted continuously to maintain metered exposure
- Useful for controlled-light indoor cinema work

**DOF hold** (aligned with Sony eND positioning)
- Operator selects aperture freely
- Controller compensates with ND to maintain target exposure
- Allows shooting at preferred f-stop (e.g., f/1.9 for bokeh) regardless of
  lighting level without affecting shutter angle discipline

---

## 7. Android software interface

The eND controller communicates with Android userspace over:
- Rev A-0: USB (STM32U5 USB-CDC to ADB shell or cinema app)
- Rev A-1: USB-C auxiliary or UART over dedicated pin on mod grip connector

Android-side responsibilities:
- Send ND setpoint commands to MCU
- Poll MCU telemetry: current transmittance, cell temperature, drive voltage
- Receive metered EV from Camera2 API for closed-loop modes
- Update kernel `cinema_mode` driver (write to `/sys/kernel/cinema_mode/enable`)
  to synchronize CPU/GPU/I/O performance state with eND activation

The kernel driver and init.rc tuning stack already present in this repository
handle all performance-layer coordination.  The eND hardware control is a parallel
hardware path — it does not depend on or interfere with those software layers.

---

## 8. Characterization protocol (Rev A-0)

All results must be collected before Rev A-1 PCB layout begins.

### 8.1 Required sweeps

1. **Transmittance vs voltage** at 3 temperatures: 0°C, 23°C, 50°C
   - Step voltage in 0.5 VRMS increments from 0 to 15 VRMS
   - Measure T at each step with calibrated integrating sphere or power meter
   - Record as raw LUT, then fit a model for firmware interpolation

2. **Spectral transmittance** at intermediate states
   - Use a spectrometer, 400–700 nm, 5 nm steps
   - Identify color shift peaks — typically at ~480 nm and ~620 nm in guest-host cells
   - Determine the Δ_ab corridor across the working ND band

3. **Angular uniformity**
   - Measure transmittance at 0°, ±10°, ±20° incidence
   - Compare to LC-Tec claim; if Δ > 0.15 stop at the sensor CRA, the cell
     may require re-evaluation or tilt optimization

4. **Response time**
   - Rising (transparent → dark): measure τ at 10–90% transmittance
   - Falling (dark → transparent): measure τ
   - Verify smooth ND pull is achievable within a single video frame at 24 fps

5. **Flicker under video shutter**
   - Set cell to a mid-state, drive with 180° / 360° shutter angle
   - Check for sync between LC drive frequency and sensor rolling shutter
   - Any visible banding at 24/25/30/48/60 fps must be resolved before Rev A-1

### 8.2 Go/no-go criteria for Rev A-1 commitment

| Criterion | Requirement |
|-----------|-------------|
| Working ND band | ≥ 2.0 stops at 23°C |
| Color shift (Δab) across band | ≤ 3.0 (correctable in post) |
| Angular uniformity | ≤ 0.25 stop variation at ±20° |
| Rise time (worst case) | < 100 ms |
| Flicker at 24/25/30 fps | Not visible at 1/50 s shutter |
| Thermal drift 0–50°C | ≤ 0.5 stop compensable with LUT |

---

## 9. Known risks

| Risk | Likelihood | Mitigation |
|------|-----------|------------|
| LYT-900 cover glass clearance too tight for 2.5 mm cartridge | Medium | Survey actual module height from sensor reference plane; plan shim-out of lens mount if needed |
| CRA at LYT-900 corner exceeds LC-Tec angular spec | Medium | Measure CRA from Sony LYT-900 datasheet; request on-axis sample test before custom order |
| Guest-host cell Δab > 3.0 in mid-states | Low-medium | Color correction LUT in ISP pipeline (existing Leica tuning path); worst case swap to different cell chemistry |
| LC drive frequency beating with rolling shutter | Medium | Characterize at all target frame rates; adjust drive frequency; isolate drive supply from MIPI clock |
| Dust ingestion during cartridge swap | High (field) | Positive-pressure purge procedure; nitrogen blanketing during swap; consider sealed cartridge with gasket |
| TPS65131 EMI coupling into MIPI lanes | Medium | Shielding can; spread-spectrum switching; route split-rail supply away from camera flex path |

---

## 10. Revision history

| Rev | Date | Notes |
|-----|------|-------|
| A | 2026-03-25 | Initial integration spec — bench mule and integrated prototype |
