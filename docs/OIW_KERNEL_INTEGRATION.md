# OIW-ROM Kernel Integration Notes (this repo)

This document explains why this multi-device Xiaomi kernel source mirror does **not** currently have
`nuwa` (Xiaomi 13 Pro) content, and what's staged under `oiw/nuwa/` for when/if that changes.

## Why there's no `nuwa-*-oss` branch here

This repo mirrors Xiaomi's per-device kernel source releases, one branch per device/Android-version
combination (see the `README` branch's device/branch table). As of this change, **no branch for `nuwa`
exists** — Xiaomi has not published a full kernel source release for the Xiaomi 13 Pro through this
channel, consistent with common practice of withholding full source for some recent flagship SoC
generations while still meeting GPL obligations for the GKI-common portions elsewhere.

The companion project, `noods1234/xiaomi_13pro_sukisu-ultra_aosp`, builds a rooted (SukiSU Ultra) GKI
kernel for `nuwa` from a **different, community-maintained source**:
`crdroidandroid/android_kernel_xiaomi_sm8550` (branch `15.0`) — see that repo's
`Kernel/configs/nuwa-13.config.json`. That is the actual buildable kernel source for this device today,
not this repo.

## What's staged here (`oiw/nuwa/`)

- `configs/oiw_cinema.config` — the same **verified** OIW cinema kernel delta as
  `noods1234/xiaomi_13pro_sukisu-ultra_aosp`'s `kernel/oiw/nuwa/configs/oiw_cinema.config` (kept
  in both places so this repo has a self-contained record of the intended kernel-level changes, in case
  a real `nuwa` branch is added here later and needs the same fragment merged against it).
- `patch-queue/0000-README.md` — layout convention for source-level patches, unused until this repo (or
  the crdroid source) is actually vendored/checked out somewhere this project controls.

## Trigger to revisit

If a `nuwa-*-oss` branch appears on this repo in the future:
1. Re-run the Path A feasibility check in `noods1234/xiaomi_13pro_sukisu-ultra_aosp`'s
   `docs/ARCHITECTURE.md` §2 — a real Xiaomi-released kernel source (and possibly an accompanying
   device/vendor tree) would materially change what's buildable.
2. Merge `oiw/nuwa/configs/oiw_cinema.config` into that branch's defconfig and confirm it still
   builds (see that fragment's inline comments — it was authored against generic GKI/SM8550-era Kconfig
   symbols, not against this specific branch's tree, so re-verify symbol availability).
