# oiw/nuwa

Staging area for One Inch Wonder (OIW-ROM) kernel-level integration. See
[`/docs/OIW_KERNEL_INTEGRATION.md`](../../docs/OIW_KERNEL_INTEGRATION.md) for why this repo has no
`nuwa` device branch yet and what happens if that changes.

- `configs/oiw_camera_media.config` — Kconfig fragment (camera/media/USB-storage/thermal), mirrored
  from `noods1234/xiaomi_13pro_sukisu-ultra_aosp`'s `kernel/oiw/nuwa/configs/`.
- `patch-queue/` — layout convention for future source-level patches once a real kernel tree is vendored.
