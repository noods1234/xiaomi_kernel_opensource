#!/usr/bin/env python3
"""Worker 2: 4K scenarios, all-core + X4-only, 5 repeat runs.
Target: SM8650 (Snapdragon 8 Gen 3 / Pineapple) — Xiaomi 14 Ultra (aurora)
"""
import sys, json
sys.path.insert(0, ".")
from sim_core import simulate, summarise

N = 2000; RUNS = 5
results = {}
for fps in [24, 30, 60, 120]:
    for prof in ["422-10", "444-10"]:
        for sc, label in [(False, "all"), (True, "x4")]:
            headrooms = []
            for run in range(RUNS):
                headrooms.extend(simulate(3840, 2160, fps, prof, N,
                                          seed=run * 7777 + 1, single_core=sc))
            key = f"4K/{fps}fps/{prof}/{label}"
            results[key] = summarise(headrooms)

print(json.dumps(results, indent=2))
