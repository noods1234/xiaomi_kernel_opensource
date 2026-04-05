"""
sim_core.py — shared constants, distributions, and compute model
Imported by all simulation worker scripts.

Target: SM8650 (Snapdragon 8 Gen 3 / Pineapple) — Xiaomi 14 Ultra (aurora)
CPU topology:
  1× Cortex-X4  @ 3.30 GHz  (Prime)
  3× Cortex-A720 @ 3.15 GHz  (Performance-Hi)
  2× Cortex-A720 @ 2.96 GHz  (Performance-Lo)
  2× Cortex-A520 @ 2.27 GHz  (Efficiency)
  Total: 8 cores, 4 cpufreq policies

IPC relatives are normalised to X4 = 1.00.
A720 is ~15% IPC improvement over A715; X4 is ~10% over X3.
A520 is ~15% improvement over A510.
"""
import random, math, statistics
from dataclasses import dataclass, field
from typing import List, Dict

HW_PIX_PER_MHZ = 2.0e6   # Chips&Media APV HW IP anchor

CLUSTERS = {
    "X4_prime":   {"freq_mhz": 3300, "count": 1, "ipc_rel": 1.00},
    "A720_hi":    {"freq_mhz": 3150, "count": 3, "ipc_rel": 0.83},
    "A720_lo":    {"freq_mhz": 2960, "count": 2, "ipc_rel": 0.79},
    "A520_eff":   {"freq_mhz": 2270, "count": 2, "ipc_rel": 0.48},
}
CHROMA_LOAD = {"422-10": 1.5, "444-10": 2.0}

# ── Uncertainty samplers ──────────────────────────────────────────────
def sample_sw_ratio()      -> float: return math.exp(random.gauss(math.log(18), 0.65))
def sample_neon_eff()      -> float: return random.triangular(3.0, 7.5, 5.5)
def sample_thermal()       -> float: return random.triangular(0.55, 0.95, 0.78)
def sample_os_overhead()   -> float: return random.uniform(0.20, 0.45)

def sample_thread_eff(n_tiles: int, n_cores: int) -> float:
    sf = 0.05
    amdahl = 1.0 / (sf + (1 - sf) / min(n_cores, n_tiles))
    contention = 1.0 - 0.03 * max(0, n_cores - 4)
    return min(amdahl, n_cores) / n_cores * max(0.6, contention)

def n_tiles(w: int, h: int) -> int:
    return math.ceil(w / 512) * math.ceil(h / 512)

# ── Compute model ─────────────────────────────────────────────────────
@dataclass
class Cluster:
    name: str; cores: int; freq_mhz: float; ipc_rel: float

    def neon_mpix_s(self, sw_ratio: float, neon_eff: float) -> float:
        return (self.cores * self.freq_mhz * self.ipc_rel
                * HW_PIX_PER_MHZ / sw_ratio * neon_eff / 1e6)

def all_clusters() -> List[Cluster]:
    return [Cluster(n, c["count"], c["freq_mhz"], c["ipc_rel"])
            for n, c in CLUSTERS.items()]

def demand_mpix_s(w, h, fps, chroma) -> float:
    return w * h * fps * chroma / 1e6

# ── Single-scenario Monte Carlo ───────────────────────────────────────
def simulate(w, h, fps, profile, n_samples, seed,
             single_core=False) -> List[float]:
    random.seed(seed)
    clusters = all_clusters()
    if single_core:
        clusters = [c for c in clusters if "X4" in c.name]
    n_cores  = sum(c.cores for c in clusters)
    tiles    = n_tiles(w, h)
    demand   = demand_mpix_s(w, h, fps, CHROMA_LOAD[profile])
    headrooms = []
    for _ in range(n_samples):
        sw, ne, th, os = (sample_sw_ratio(), sample_neon_eff(),
                          sample_thermal(), sample_os_overhead())
        te = (1.0 if single_core else
              sample_thread_eff(tiles, n_cores))
        cap = sum(c.neon_mpix_s(sw, ne) for c in clusters) * th * (1 - os) * te
        headrooms.append(cap / demand)
    return headrooms

# ── Stats helpers ─────────────────────────────────────────────────────
def pct(lst, p):
    s = sorted(lst); return s[max(0, int(len(s) * p / 100) - 1)]

def summarise(headrooms: List[float]) -> dict:
    return {
        "p10": pct(headrooms, 10),
        "p50": pct(headrooms, 50),
        "p90": pct(headrooms, 90),
        "p_rt": sum(1 for x in headrooms if x >= 1.0) / len(headrooms),
        "p_2x": sum(1 for x in headrooms if x >= 2.0) / len(headrooms),
    }
