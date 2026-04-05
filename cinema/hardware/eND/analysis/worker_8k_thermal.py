#!/usr/bin/env python3
"""Worker 3: 8K scenarios + thermal sustainability model.
Target: SM8650 (Snapdragon 8 Gen 3 / Pineapple) — Xiaomi 14 Ultra (aurora)
"""
import sys, json, math, random
sys.path.insert(0, ".")
from sim_core import simulate, summarise, all_clusters, demand_mpix_s, CHROMA_LOAD

N = 2000; RUNS = 5

# ── 8K Monte Carlo ────────────────────────────────────────────────────
results = {}
for fps in [24, 30]:
    for prof in ["422-10", "444-10"]:
        for sc, label in [(False, "all"), (True, "x4")]:
            headrooms = []
            for run in range(RUNS):
                headrooms.extend(simulate(7680, 4320, fps, prof, N,
                                          seed=run * 5555 + 2, single_core=sc))
            key = f"8K/{fps}fps/{prof}/{label}"
            results[key] = summarise(headrooms)

# ── Thermal model ─────────────────────────────────────────────────────
# Fixed-point calculation (no MC needed — thermal factors are the
# independent variable here, not a source of uncertainty)
SW_RATIO = 18.0; NEON_EFF = 5.5; OS_OH = 0.30

phases = [
    ("burst_0_30s",         1.00, 1.00, 1.00, 0.95),
    ("short_30_120s",       0.90, 0.88, 0.85, 0.92),
    ("sustained_120s_plus", 0.72, 0.70, 0.68, 0.88),
    ("worst_case",          0.60, 0.58, 0.55, 0.82),
]
targets = [
    ("4K/24fps/422-10", 3840, 2160, 24, "422-10"),
    ("4K/30fps/422-10", 3840, 2160, 30, "422-10"),
    ("4K/60fps/422-10", 3840, 2160, 60, "422-10"),
]
thermal = {}
clusters = all_clusters()
for phase_label, t_x3, t_mid, t_eff, t_thread in phases:
    thermal[phase_label] = {}
    for scen_label, w, h, fps, prof in targets:
        demand = demand_mpix_s(w, h, fps, CHROMA_LOAD[prof])
        cap = 0.0
        for c in clusters:
            tf = t_x3 if "X4" in c.name else (t_eff if "520" in c.name else t_mid)
            cap += c.neon_mpix_s(SW_RATIO, NEON_EFF) * tf
        cap *= (1 - OS_OH) * t_thread
        thermal[phase_label][scen_label] = round(cap / demand, 3)

# ── Storage check ─────────────────────────────────────────────────────
storage = {}
UFS_LIMIT = 2800   # MB/s (UFS 4.0 on SM8650 / Xiaomi 14 Ultra)
BW_TOTAL  = 68260  # MB/s LPDDR5X
for label, mbps in [
    ("4K/24fps/422-10/HQ",  720), ("4K/30fps/422-10/HQ",  900),
    ("4K/60fps/422-10/HQ", 1800), ("4K/24fps/422-10/LQ",  360),
    ("1080p/30fps/422-10",  200), ("8K/24fps/422-10/HQ", 2880),
]:
    mbs = mbps / 8
    storage[label] = {
        "rate_mbs": round(mbs, 1),
        "ufs40_pct": round(mbs / UFS_LIMIT * 100, 1),
        "lpddr_pct": round(mbs / BW_TOTAL * 100, 3),
        "over_ufs":  mbs > UFS_LIMIT,
    }

print(json.dumps({"8k": results, "thermal": thermal, "storage": storage}, indent=2))
