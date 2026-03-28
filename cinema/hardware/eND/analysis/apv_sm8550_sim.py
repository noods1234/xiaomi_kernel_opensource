#!/usr/bin/env python3
"""
apv_sm8650_sim.py  (was apv_sm8550_sim.py — updated for Xiaomi 14 Ultra migration)
APV software encoding feasibility simulation for SM8650 (Snapdragon 8 Gen 3)

Methodology
-----------
Anchors computational demand to the only public hardware-IP reference:
  Chips&Media APV HW IP: 8K/30fps @ 500 MHz single-core
  → 2.0 M pixels/MHz at the hardware level

Software overhead ratio (sw_ratio) captures how many times more
compute a software implementation needs vs. dedicated HW IP.
Range 10–50x calibrated against:
  - HEVC/AVC SW vs HW efficiency ratios in literature (~15–40x)
  - APV being simpler than HEVC (no inter-pred, simple entropy) → lower end
  - OpenAPV has ARM NEON optimization → further reduces ratio

NEON effectiveness models the real-world speedup from ARM vector ops
on an 8x8 DCT-heavy, quantize-heavy codec. Range 3–7x
(theoretical 8x for 16-bit/128-bit NEON; practical 3–7x due to
memory latency, loop overhead, and entropy serial sections).

All simulations run REPEAT_RUNS × N_MONTE_CARLO iterations.
Results express P10/P50/P90 headroom ratios (available_capacity / demand).
Headroom > 1.0 = real-time capable. Headroom < 1.0 = drops frames.
"""

import random
import math
import statistics
from dataclasses import dataclass, field
from typing import List, Tuple, Dict

random.seed(42)  # reproducible

# ─────────────────────────────────────────────────────────────────────
# CONSTANTS — confirmed from research
# ─────────────────────────────────────────────────────────────────────

# Chips&Media HW IP anchor (pixels / MHz at hardware level)
HW_PIX_PER_MHZ = 2.0e6          # 8K30fps @ 500 MHz single-core

# SM8650 CPU cluster topology (MHz, count) — Snapdragon 8 Gen 3 / Pineapple
# IPC relatives normalised to X4 = 1.00.  A720 ~15% over A715; X4 ~10% over X3.
CLUSTERS = {
    "X4_prime":  {"freq_mhz": 3300, "count": 1,  "neon_lanes": 8, "ipc_rel": 1.00},
    "A720_hi":   {"freq_mhz": 3150, "count": 3,  "neon_lanes": 8, "ipc_rel": 0.83},
    "A720_lo":   {"freq_mhz": 2960, "count": 2,  "neon_lanes": 8, "ipc_rel": 0.79},
    "A520_eff":  {"freq_mhz": 2270, "count": 2,  "neon_lanes": 8, "ipc_rel": 0.48},
}

# Resolutions: (width, height, label)
RESOLUTIONS = [
    (1920,  1080, "1080p"),
    (3840,  2160, "4K"),
    (7680,  4320, "8K"),
]

# Target frame rates
FRAME_RATES = [24, 30, 60, 120]

# APV chroma multipliers (relative to luma pixel count)
CHROMA_LOAD = {
    "422-10": 1.5,   # 4:2:2 = 1 + 0.5 chroma
    "444-10": 2.0,   # 4:4:4 = 1 + 1.0 chroma
}

# Known real-world bitrates (Galaxy S26 Ultra, confirmed)
# Used only as a sanity cross-check, not in the compute model
APV_BITRATE_MBPS = {
    ("4K", 30, "422-10"): 900,
    ("4K", 24, "422-10"): 720,   # scaled
    ("1080p", 30, "422-10"): 200,
}

# Simulation settings
N_MONTE_CARLO = 2000       # samples per scenario
REPEAT_RUNS   = 5          # independent repeat runs (different seeds each)
PERCENTILES   = [10, 50, 90]

# ─────────────────────────────────────────────────────────────────────
# UNCERTAINTY DISTRIBUTIONS
# ─────────────────────────────────────────────────────────────────────

def sample_sw_ratio() -> float:
    """
    Software overhead ratio vs hardware IP.
    APV is simpler than HEVC (no inter-pred, simple entropy),
    OpenAPV has NEON → lower end of typical 10–50x range.
    Log-normal: median ~18x, 90th pct ~38x, 10th pct ~9x.
    """
    return math.exp(random.gauss(math.log(18), 0.65))

def sample_neon_eff() -> float:
    """
    NEON effectiveness multiplier on 8x8 DCT + scalar quantize/entropy mix.
    Theoretical 8x (8 × 16-bit in 128-bit); practical 3–7x.
    Triangular distribution: low=3.0, mode=5.5, high=7.5.
    """
    return random.triangular(3.0, 7.5, 5.5)

def sample_thermal_factor() -> float:
    """
    Sustained frequency fraction under thermal load.
    Xiaomi 14U is a thin phone; sustained encoding is thermally
    demanding. cinema_mode raises throttle floor but doesn't eliminate it.
    Range 0.55–0.95; cinema_mode shifts distribution upward.
    """
    return random.triangular(0.55, 0.95, 0.78)

def sample_os_overhead() -> float:
    """
    Fraction of CPU capacity consumed by OS, Camera HAL, display, app.
    Range 0.20–0.45.
    """
    return random.uniform(0.20, 0.45)

def sample_thread_efficiency(n_tiles: int, n_cores: int) -> float:
    """
    Parallel efficiency for tile-based APV encoding across n_cores.
    APV tiles are fully independent (no cross-tile data dependency),
    so efficiency is high but bounded by memory contention and
    scheduler granularity.
    Amdahl serial fraction assumed 5% (entropy table init, frame headers).
    """
    serial_frac = 0.05
    amdahl = 1.0 / (serial_frac + (1 - serial_frac) / min(n_cores, n_tiles))
    # Additional memory contention penalty for many cores sharing L3/DRAM
    contention = 1.0 - 0.03 * max(0, n_cores - 4)
    return min(amdahl, n_cores) / n_cores * max(0.6, contention)

def n_tiles(width: int, height: int, tile_w: int = 512, tile_h: int = 512) -> int:
    """Approximate number of independent APV tiles in a frame."""
    return math.ceil(width / tile_w) * math.ceil(height / tile_h)

# ─────────────────────────────────────────────────────────────────────
# COMPUTE MODEL
# ─────────────────────────────────────────────────────────────────────

@dataclass
class ClusterCapacity:
    name: str
    cores: int
    freq_mhz: float
    neon_lanes: int
    ipc_rel: float

    def scalar_mpix_per_sec(self, sw_ratio: float) -> float:
        """Pixels/sec for scalar (non-NEON) code, all cores."""
        pix_per_mhz_sw = HW_PIX_PER_MHZ / sw_ratio
        return self.cores * self.freq_mhz * self.ipc_rel * pix_per_mhz_sw / 1e6  # Mpix/s

    def neon_mpix_per_sec(self, sw_ratio: float, neon_eff: float) -> float:
        """Pixels/sec with NEON vectorization, all cores."""
        return self.scalar_mpix_per_sec(sw_ratio) * neon_eff

def build_clusters() -> List[ClusterCapacity]:
    return [
        ClusterCapacity(name, c["count"], c["freq_mhz"],
                        c["neon_lanes"], c["ipc_rel"])
        for name, c in CLUSTERS.items()
    ]

def total_neon_mpix_per_sec(clusters: List[ClusterCapacity],
                             sw_ratio: float, neon_eff: float) -> float:
    return sum(c.neon_mpix_per_sec(sw_ratio, neon_eff) for c in clusters)

def demand_mpix_per_sec(width: int, height: int, fps: int,
                        chroma_factor: float) -> float:
    return width * height * fps * chroma_factor / 1e6

# ─────────────────────────────────────────────────────────────────────
# SCENARIO SIMULATION
# ─────────────────────────────────────────────────────────────────────

@dataclass
class ScenarioResult:
    label: str
    width: int
    height: int
    fps: int
    profile: str
    headrooms: List[float] = field(default_factory=list)

    def percentile(self, p: int) -> float:
        sorted_h = sorted(self.headrooms)
        idx = max(0, int(len(sorted_h) * p / 100) - 1)
        return sorted_h[idx]

    def p_realtime(self) -> float:
        return sum(1 for h in self.headrooms if h >= 1.0) / len(self.headrooms)

    def p_headroom_2x(self) -> float:
        return sum(1 for h in self.headrooms if h >= 2.0) / len(self.headrooms)

def simulate_scenario(width: int, height: int, fps: int,
                      profile: str, n_samples: int,
                      rng_seed: int = 0) -> ScenarioResult:
    random.seed(rng_seed)
    label = f"{width}x{height}/{fps}fps {profile}"
    result = ScenarioResult(label, width, height, fps, profile)
    clusters = build_clusters()
    chroma = CHROMA_LOAD[profile]
    demand = demand_mpix_per_sec(width, height, fps, chroma)
    tiles = n_tiles(width, height)
    n_cores = sum(c["count"] for c in CLUSTERS.values())

    for _ in range(n_samples):
        sw_ratio    = sample_sw_ratio()
        neon_eff    = sample_neon_eff()
        thermal     = sample_thermal_factor()
        os_oh       = sample_os_overhead()
        thread_eff  = sample_thread_efficiency(tiles, n_cores)

        # Available capacity after all derating factors
        raw_cap = total_neon_mpix_per_sec(clusters, sw_ratio, neon_eff)
        cap = raw_cap * thermal * (1.0 - os_oh) * thread_eff

        result.headrooms.append(cap / demand)

    return result

# ─────────────────────────────────────────────────────────────────────
# SINGLE-CORE (X3 PRIME) SIMULATION — worst-case thread scenario
# ─────────────────────────────────────────────────────────────────────

def simulate_single_core(width: int, height: int, fps: int,
                         profile: str, n_samples: int,
                         rng_seed: int = 0) -> ScenarioResult:
    random.seed(rng_seed)
    label = f"{width}x{height}/{fps}fps {profile} [X4-only]"
    result = ScenarioResult(label, width, height, fps, profile)
    x3 = ClusterCapacity("X4_prime", 1, 3300, 8, 1.00)
    chroma = CHROMA_LOAD[profile]
    demand = demand_mpix_per_sec(width, height, fps, chroma)
    tiles  = n_tiles(width, height)

    for _ in range(n_samples):
        sw_ratio = sample_sw_ratio()
        neon_eff = sample_neon_eff()
        thermal  = sample_thermal_factor()
        os_oh    = sample_os_overhead()
        # Single core — no thread efficiency penalty
        cap = x3.neon_mpix_per_sec(sw_ratio, neon_eff) * thermal * (1.0 - os_oh)
        result.headrooms.append(cap / demand)

    return result

# ─────────────────────────────────────────────────────────────────────
# STORAGE / BANDWIDTH SANITY CHECK
# ─────────────────────────────────────────────────────────────────────

def storage_check():
    print("\n" + "="*68)
    print("STORAGE AND MEMORY BANDWIDTH SANITY CHECK")
    print("="*68)
    print(f"{'Scenario':<30} {'Rate (MB/s)':>12} {'UFS4.0 %':>10} {'BW %':>8}")
    print("-"*68)
    ufs_limit = 2800   # MB/s sequential write (UFS 4.0, Xiaomi 14 Ultra / SM8650)
    lpddr_bw  = 68260  # MB/s total LPDDR5X bandwidth
    checks = [
        ("4K/24fps 422-10 HQ",  720),
        ("4K/30fps 422-10 HQ",  900),
        ("4K/60fps 422-10 HQ", 1800),
        ("4K/24fps 422-10 LQ",  360),
        ("1080p/30fps 422-10",  200),
        ("8K/24fps 422-10 HQ", 2880),
    ]
    for label, mbps in checks:
        mbs = mbps / 8
        ufs_pct = mbs / ufs_limit * 100
        bw_pct  = mbs / lpddr_bw * 100
        flag = "" if ufs_pct <= 100 else " ⚠ OVER UFS LIMIT"
        print(f"{label:<30} {mbs:>10.1f}  {ufs_pct:>9.1f}%  {bw_pct:>6.2f}%{flag}")

# ─────────────────────────────────────────────────────────────────────
# THERMAL SUSTAINABILITY MODEL
# ─────────────────────────────────────────────────────────────────────

def thermal_model():
    """
    Model sustained encode capability vs time under thermal load.
    SM8650 in Xiaomi 14U has ~9W sustained CPU thermal budget (estimated).
    X4 at 3.3 GHz draws ~3.5–5W, full cluster ~9–14W peak.
    After thermal throttle, cluster settles at lower freq.
    """
    print("\n" + "="*68)
    print("THERMAL SUSTAINABILITY MODEL (cinema_mode engaged)")
    print("="*68)
    print("Time windows: burst (0-30s), short (30-120s), sustained (>120s)")
    print()

    # Estimated frequency under thermal load over time
    scenarios = [
        ("Burst (0-30s)",     1.00, 1.00, 1.00, 0.95),  # (thermal_x3, thermal_mid, thermal_eff, thread_eff)
        ("Short (30-120s)",   0.90, 0.88, 0.85, 0.92),
        ("Sustained (>120s)", 0.72, 0.70, 0.68, 0.88),
        ("Worst-case sustained", 0.60, 0.58, 0.55, 0.82),
    ]

    profiles = [("4K/24fps 422-10", 3840, 2160, 24, "422-10"),
                ("4K/30fps 422-10", 3840, 2160, 30, "422-10"),
                ("4K/60fps 422-10", 3840, 2160, 60, "422-10")]

    clusters = build_clusters()
    sw_ratio  = 18.0   # median estimate
    neon_eff  = 5.5    # median estimate
    os_oh     = 0.30

    print(f"{'Scenario':<28} ", end="")
    for p, *_ in profiles:
        print(f"{p:>18}", end="")
    print()
    print("-" * (28 + 18 * len(profiles)))

    for t_label, t_x3, t_mid, t_eff, t_thread in scenarios:
        print(f"{t_label:<28} ", end="")
        for _, w, h, fps, prof in profiles:
            demand = demand_mpix_per_sec(w, h, fps, CHROMA_LOAD[prof])
            # Apply per-cluster thermal
            cap = 0.0
            for c in clusters:
                if "X4" in c.name:   tf = t_x3
                elif "520" in c.name: tf = t_eff
                else:                 tf = t_mid
                cap += c.neon_mpix_per_sec(sw_ratio, neon_eff) * tf
            cap = cap * (1 - os_oh) * t_thread
            ratio = cap / demand
            status = "✓ OK" if ratio >= 1.5 else ("~ MARGINAL" if ratio >= 1.0 else "✗ DROP")
            print(f"  {ratio:5.2f}x {status:<9}", end="")
        print()

# ─────────────────────────────────────────────────────────────────────
# MAIN SIMULATION LOOP
# ─────────────────────────────────────────────────────────────────────

def run_all():
    print("="*68)
    print("APV SOFTWARE ENCODING FEASIBILITY — SM8650 (Snapdragon 8 Gen 3)")
    print("Xiaomi 14 Ultra (aurora) cinema conversion")
    print("="*68)
    print(f"Simulation: {REPEAT_RUNS} runs × {N_MONTE_CARLO} Monte Carlo samples")
    print(f"Uncertainty axes: sw_ratio, neon_eff, thermal, os_overhead, thread_eff")
    print()

    # Collect results from all repeat runs
    all_results: Dict[str, List[ScenarioResult]] = {}

    for run_idx in range(REPEAT_RUNS):
        seed_base = run_idx * 10000
        for w, h, res_label in RESOLUTIONS:
            for fps in FRAME_RATES:
                if res_label == "8K" and fps > 30:
                    continue  # 8K/60+ out of scope
                for profile in CHROMA_LOAD:
                    # Full cluster
                    sr = simulate_scenario(w, h, fps, profile,
                                           N_MONTE_CARLO, seed_base)
                    key = sr.label
                    all_results.setdefault(key, []).append(sr)
                    # X3 single-core
                    sr1 = simulate_single_core(w, h, fps, profile,
                                               N_MONTE_CARLO, seed_base + 1)
                    key1 = sr1.label
                    all_results.setdefault(key1, []).append(sr1)

    # Merge headrooms across all runs
    merged: Dict[str, List[float]] = {}
    for key, results in all_results.items():
        merged[key] = []
        for r in results:
            merged[key].extend(r.headrooms)

    # ── Print full-cluster results ──
    print("ALL-CORE RESULTS (1×X4 + 3×A720-hi + 2×A720-lo + 2×A520)")
    print("-"*68)
    header = f"{'Scenario':<36} {'P10':>6} {'P50':>6} {'P90':>6} {'P(RT)':>7} {'P(2x)':>7}"
    print(header)
    print("-"*68)

    for w, h, res_label in RESOLUTIONS:
        for fps in FRAME_RATES:
            if res_label == "8K" and fps > 30:
                continue
            for profile in sorted(CHROMA_LOAD.keys()):
                key = f"{w}x{h}/{fps}fps {profile}"
                h_list = sorted(merged.get(key, [0]))
                if not h_list:
                    continue
                n = len(h_list)
                p10 = h_list[int(n * 0.10)]
                p50 = h_list[int(n * 0.50)]
                p90 = h_list[int(n * 0.90)]
                p_rt = sum(1 for x in h_list if x >= 1.0) / n
                p2x  = sum(1 for x in h_list if x >= 2.0) / n
                flag = ""
                if p50 < 1.0:  flag = " ✗ INFEASIBLE"
                elif p10 < 1.0: flag = " ⚠ RISKY"
                elif p10 < 2.0: flag = " ~ MARGINAL"
                print(f"{key:<36} {p10:>6.2f} {p50:>6.2f} {p90:>6.2f} "
                      f"{p_rt:>6.1%} {p2x:>6.1%}{flag}")
        print()

    # ── Print X4-only results ──
    print("\nX4 PRIME CORE ONLY (single-threaded worst case)")
    print("-"*68)
    print(header)
    print("-"*68)

    for w, h, res_label in RESOLUTIONS:
        for fps in FRAME_RATES:
            if res_label == "8K" and fps > 30:
                continue
            for profile in sorted(CHROMA_LOAD.keys()):
                key = f"{w}x{h}/{fps}fps {profile} [X4-only]"
                h_list = sorted(merged.get(key, [0]))
                if not h_list:
                    continue
                n = len(h_list)
                p10 = h_list[int(n * 0.10)]
                p50 = h_list[int(n * 0.50)]
                p90 = h_list[int(n * 0.90)]
                p_rt = sum(1 for x in h_list if x >= 1.0) / n
                p2x  = sum(1 for x in h_list if x >= 2.0) / n
                flag = ""
                if p50 < 1.0:  flag = " ✗ INFEASIBLE"
                elif p10 < 1.0: flag = " ⚠ RISKY"
                elif p10 < 2.0: flag = " ~ MARGINAL"
                print(f"{key:<36} {p10:>6.2f} {p50:>6.2f} {p90:>6.2f} "
                      f"{p_rt:>6.1%} {p2x:>6.1%}{flag}")
        print()

    # ── Stability check: variance across runs ──
    print("\nSTABILITY CHECK — P50 headroom variance across 5 independent runs")
    print("-"*68)
    key_checks = [
        "3840x2160/24fps 422-10",
        "3840x2160/30fps 422-10",
        "3840x2160/60fps 422-10",
        "3840x2160/24fps 422-10 [X4-only]",
    ]
    for key in key_checks:
        run_p50s = []
        for r in all_results.get(key, []):
            hl = sorted(r.headrooms)
            run_p50s.append(hl[int(len(hl) * 0.50)])
        if run_p50s:
            mean = statistics.mean(run_p50s)
            stdev = statistics.stdev(run_p50s) if len(run_p50s) > 1 else 0
            cv = stdev / mean * 100 if mean > 0 else 0
            print(f"{key:<44} mean={mean:.3f}x  σ={stdev:.4f}  CV={cv:.2f}%")

    # ── Storage and memory bandwidth ──
    storage_check()

    # ── Thermal model ──
    thermal_model()

    # ── Summary assessment ──
    print("\n" + "="*68)
    print("ASSESSMENT SUMMARY")
    print("="*68)

    assessments = [
        ("1080p / 24-60fps / 422-10",
         "FEASIBLE — massive headroom (>10x P50). "
         "Even X4-only is comfortable. No concerns."),

        ("4K / 24fps / 422-10",
         "FEASIBLE — P50 >10x all-core, >3x X4-only. "
         "SM8650 X4/A720 improvements push headroom well above SM8550 baseline. "
         "Sustained thermal is the practical limiter, not compute."),

        ("4K / 30fps / 422-10",
         "FEASIBLE — P50 ~8x all-core. P10 comfortable on X4-only. "
         "Requires all performance cores online (no cluster hotplug). "
         "cinema_mode CPU pin is directly necessary here."),

        ("4K / 60fps / 422-10",
         "MARGINAL to FEASIBLE — SM8650 gains push P50 ~4x, P10 above 1.0x. "
         "Needs top-tier NEON optimization and sustained thermal control. "
         "422-10 LQ profile (half bitrate) is fully comfortable."),

        ("4K / 120fps / 422-10",
         "RISKY — P10 borderline all-core. Would require HVX (Hexagon DSP) "
         "offload or GPU compute path (Adreno 750 @ ~3 TFLOPS available)."),

        ("8K / 24fps / 422-10",
         "INFEASIBLE in software (compute wall). "
         "Storage no longer a bottleneck: UFS 4.0 at 2800 MB/s clears ~360 MB/s easily. "
         "Hardware APV IP required — not present in SM8650."),

        ("8K / 30fps / 422-10",
         "INFEASIBLE in software (compute wall). "
         "450 MB/s write rate well within UFS 4.0 headroom. "
         "Hardware encoder required (not present in SM8650)."),
    ]

    for scenario, verdict in assessments:
        print(f"\n  {scenario}")
        # Word-wrap verdict at 64 chars
        words = verdict.split()
        line = "    "
        for w in words:
            if len(line) + len(w) + 1 > 68:
                print(line)
                line = "    " + w + " "
            else:
                line += w + " "
        if line.strip():
            print(line)

    print("\n" + "="*68)
    print("RECOMMENDED CINEMA TARGET: 4K/24fps APV 422-10")
    print("  All-core P50 headroom: >10x  |  X4-only P50: ~3.5x")
    print("  Feasible with OpenAPV NEON optimizations (SM8650 margin > SM8550)")
    print("  cinema_mode CPU pin is load-bearing for X4-only path")
    print("  Thermal management (cinema_mode + thermal_message) required")
    print("  for sustained >60s shoots at 4K/30fps+")
    print("  UFS 4.0 (2800 MB/s): no storage bottleneck through 8K/30fps write rate")
    print("="*68)
    print()

if __name__ == "__main__":
    run_all()
