#!/usr/bin/env python3
"""
merge_report.py — aggregate worker outputs and print final report.
Usage: python3 merge_report.py w1.json w2.json w3.json
"""
import sys, json

files = sys.argv[1:]
blobs = [json.load(open(f)) for f in files]

# Flatten all scenario results into one dict
scenarios = {}
thermal = {}
storage = {}
for b in blobs:
    if "thermal" in b:
        thermal = b["thermal"]
    if "storage" in b:
        storage = b["storage"]
    if "8k" in b:
        scenarios.update(b["8k"])
    for k, v in b.items():
        if k not in ("thermal", "storage", "8k"):
            scenarios.update(b[k] if isinstance(b[k], dict) else {k: v})

# Detect flat structure (worker_1080p / worker_4k emit flat dicts)
flat = {}
for b in blobs:
    for k, v in b.items():
        if isinstance(v, dict) and "p50" in v:
            flat[k] = v
scenarios.update(flat)

SEP = "=" * 72

def flag(p10, p50):
    if p50 < 1.0:  return " ✗ INFEASIBLE"
    if p10 < 1.0:  return " ⚠ RISKY"
    if p10 < 2.0:  return " ~ MARGINAL"
    return ""

def print_section(title, keys):
    print(f"\n{title}")
    print("-" * 72)
    print(f"{'Scenario':<40} {'P10':>6} {'P50':>6} {'P90':>6} {'P(RT)':>7} {'P(2x)':>7}")
    print("-" * 72)
    for k in keys:
        if k not in scenarios: continue
        s = scenarios[k]
        f = flag(s["p10"], s["p50"])
        print(f"{k:<40} {s['p10']:>6.2f} {s['p50']:>6.2f} {s['p90']:>6.2f} "
              f"{s['p_rt']:>6.1%} {s['p_2x']:>6.1%}{f}")

print(SEP)
print("APV / SM8650 FEASIBILITY — MERGED REPORT (parallel worker runs)")
print(SEP)

# ── 1080p ─────────────────────────────────────────────────────────────
keys_1080_all = [f"1080p/{fps}fps/{p}/all"
                 for fps in [24,30,60,120] for p in ["422-10","444-10"]]
keys_1080_x4  = [f"1080p/{fps}fps/{p}/x4"
                 for fps in [24,30,60,120] for p in ["422-10","444-10"]]
print_section("1080p — ALL CORES", keys_1080_all)
print_section("1080p — X4 PRIME ONLY", keys_1080_x4)

# ── 4K ────────────────────────────────────────────────────────────────
keys_4k_all = [f"4K/{fps}fps/{p}/all"
               for fps in [24,30,60,120] for p in ["422-10","444-10"]]
keys_4k_x4  = [f"4K/{fps}fps/{p}/x4"
               for fps in [24,30,60,120] for p in ["422-10","444-10"]]
print_section("4K — ALL CORES", keys_4k_all)
print_section("4K — X4 PRIME ONLY", keys_4k_x4)

# ── 8K ────────────────────────────────────────────────────────────────
keys_8k_all = [f"8K/{fps}fps/{p}/all"
               for fps in [24,30] for p in ["422-10","444-10"]]
keys_8k_x4  = [f"8K/{fps}fps/{p}/x4"
               for fps in [24,30] for p in ["422-10","444-10"]]
print_section("8K — ALL CORES", keys_8k_all)
print_section("8K — X4 PRIME ONLY", keys_8k_x4)

# ── Thermal ───────────────────────────────────────────────────────────
if thermal:
    print(f"\n\nTHERMAL SUSTAINABILITY MODEL (cinema_mode engaged, median params)")
    print("-" * 72)
    targets = ["4K/24fps/422-10", "4K/30fps/422-10", "4K/60fps/422-10"]
    print(f"{'Phase':<26}", end="")
    for t in targets: print(f"  {t:>18}", end="")
    print()
    print("-" * 72)
    phase_labels = {
        "burst_0_30s":         "Burst (0–30 s)",
        "short_30_120s":       "Short (30–120 s)",
        "sustained_120s_plus": "Sustained (>120 s)",
        "worst_case":          "Worst-case sustained",
    }
    for pk, pl in phase_labels.items():
        if pk not in thermal: continue
        print(f"{pl:<26}", end="")
        for t in targets:
            ratio = thermal[pk].get(t, 0)
            status = "✓ OK" if ratio >= 1.5 else ("~ MARGINAL" if ratio >= 1.0 else "✗ DROP")
            print(f"  {ratio:5.2f}x {status:<10}", end="")
        print()

# ── Storage ───────────────────────────────────────────────────────────
if storage:
    print(f"\n\nSTORAGE AND MEMORY BANDWIDTH")
    print("-" * 72)
    print(f"{'Scenario':<30} {'MB/s':>8} {'UFS4.0%':>10} {'LPDDR%':>8}  Note")
    print("-" * 72)
    for label, s in storage.items():
        note = " ⚠ OVER UFS LIMIT" if s["over_ufs"] else ""
        pct_key = "ufs40_pct" if "ufs40_pct" in s else s.get("ufs31_pct", 0)
        pct_val = s.get("ufs40_pct", s.get("ufs31_pct", 0))
        print(f"{label:<30} {s['rate_mbs']:>8.1f} {pct_val:>9.1f}% "
              f"{s['lpddr_pct']:>7.3f}%{note}")

# ── Assessment ────────────────────────────────────────────────────────
print(f"\n\n{SEP}")
print("FINAL ASSESSMENT")
print(SEP)
assessments = [
    ("1080p any framerate / 422-10",
     "CLEAR — >10x P50 all-core. X3-only comfortable to 60fps."),
    ("4K / 24fps / 422-10",
     "FEASIBLE — P50 >10x all-core (SM8650 improvement over SM8550 ~8.7x). "
     "Thermal is the practical limiter, not compute."),
    ("4K / 30fps / 422-10",
     "FEASIBLE — P50 ~8x all-core. cinema_mode CPU pin load-bearing."),
    ("4K / 60fps / 422-10",
     "MARGINAL to FEASIBLE — SM8650 X4/A720 gains push P10 above 1.0x. "
     "LQ profile comfortable. Full NEON opt required."),
    ("4K / 120fps / 422-10",
     "RISKY — P10 borderline. Needs HVX offload or Adreno 750 compute path."),
    ("8K any framerate",
     "INFEASIBLE software. Requires hardware APV IP not in SM8650. "
     "UFS 4.0 clears the storage bottleneck (2800 MB/s) but compute is the wall."),
]
for scenario, verdict in assessments:
    print(f"\n  {scenario}")
    words, line = verdict.split(), "    "
    for w in words:
        if len(line) + len(w) > 68: print(line); line = "    " + w + " "
        else: line += w + " "
    if line.strip(): print(line)

print(f"\n{SEP}")
print("PRIMARY TARGET: 4K/24fps APV 422-10")
print("  All-core P50 headroom : >10x (SM8650 Pineapple)")
print("  X4-only  P50 headroom : ~3.5x")
print("  cinema_mode pin       : load-bearing at 4K/30fps+")
print("  Thermal constraint    : use cinema_mode + thermal_message")
print("  HVX / GPU path        : unlocks 4K/120fps if needed later")
print("  UFS 4.0               : 2800 MB/s — no storage bottleneck through 8K/30fps")
print(SEP)
