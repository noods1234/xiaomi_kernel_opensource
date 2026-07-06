# oiw/nuwa

Staging area for One Inch Wonder (OIW-ROM) kernel-level integration. See
[`/docs/OIW_KERNEL_INTEGRATION.md`](../../docs/OIW_KERNEL_INTEGRATION.md) for why this repo has no
`nuwa` device branch yet and what happens if that changes.

- `configs/oiw_cinema.config` — the **verified** GKI defconfig delta (NTFS3/UDF footage-drive support +
  `-oiw-cinema` localversion), mirrored from `noods1234/xiaomi_13pro_sukisu-ultra_aosp`'s
  `kernel/oiw/nuwa/configs/`, where it is wired into CI as the `nuwa-oiw` build variant.
- `patch-queue/` — layout convention for future source-level patches once a real kernel tree is vendored.
