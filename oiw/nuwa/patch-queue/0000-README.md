# Patch queue layout (for future use once kernel source is vendored directly)

Currently empty — this repo does not vendor the kernel source (it's fetched by the external CI per
`Kernel/configs/nuwa-13.config.json`), so there is nowhere to apply a source-level patch against in-tree today. See
`kernel/oiw/nuwa/README.md` for why config fragments (`configs/oiw_camera_media.config`) are used instead.

If this project ever vendors the kernel source directly (e.g., as a git submodule or subtree), patches should be
laid out as:

```
patch-queue/
  0001-short-description.patch   # git format-patch style, one logical change per file
  0002-short-description.patch
  apply.sh                       # applies 000N-*.patch in numeric order, stops on first failure
```

Numbering is sequential and gapless; a patch that's later dropped should be removed and the rest renumbered, not
left as a gap, so the queue always reads as an ordered, current changeset rather than a historical diff of intent.
