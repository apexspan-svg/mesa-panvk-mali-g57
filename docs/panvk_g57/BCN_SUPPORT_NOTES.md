# BCn Texture Compression on Mali-G57 MC2 (Dimensity 6300) — Observations

> Status: observations from one device/firmware, not a conformance claim.
> Everything below was measured on-device; it may vary with firmware lot.

## TL;DR

- `textureCompressionBC` reports **true**, and BC1/BC2/BC3 appear to
  sample and transfer correctly (round-trip tested on real DDS payloads).
- BC4/BC5/BC6H/BC7 report **zero** format features on this unit.
- The `textureCompressionBC` *feature bit* in this tree is currently
  **hardcoded to true** (`ADHOC-DXVK-FL override` in
  `panvk_vX_physical_device.c`, added for a DXVK probe) — do not trust
  the feature bit alone; always query per-format features.

## The strange part

Mali community wisdom says BCn is disabled on Mali (fuse bits off on most
shipped devices). This Dimensity 6300 unit appears to be an exception:
its firmware capability word lists BC1–BC3 as present. Raw readout from
`/dev/mali0` (`TEXTURE_FEATURES_0`, kbase props key 9):

```
TEXTURE_FEATURES_0 (key 9)  = 0xf7fe03fe
bit  7 BC1      on
bit  8 BC2      on
bit  9 BC3      on
bit 10 BC4      off
bit 11 BC4s     off
bit 12 BC5      off
bit 13 BC5s     off
bit 14 BC6Hu    off
bit 15 BC6Hs    off
bit 16 BC7      off
bit 22 ASTC-LDR on
```

Full log: [`fuse_mali_g57_mc2.log`](./fuse_mali_g57_mc2.log).
No public ARM/MediaTek document lists per-chip BC fuse state that we could
find, so this readout plus the tests below are currently the only public
record for this chip. It may differ on other units/firmware.

## What was tested (PanVK, kbase JM 11.38)

| Test | Result |
|---|---|
| `textureCompressionBC` feature query | true (note hardcoded override above) |
| Per-format features BC1/BC2/BC3 (optimal tiling) | `SAMPLED XFER DST` present |
| Per-format features BC4/BC5/BC6H/BC7 | `0x0` (cleanly refused) |
| Native round-trip, real DDS payloads (optimal tiling, fence-verified) | BC1 131072 B ok, BC3 262144 B ok, BC2 65536 B ok (aras-p test images) |
| WebGL S3TC decode (ANGLE → PanVK BC1 image) | decoded to expected color, no device loss |
| PanVK source audit for CPU BC emulation paths | none found; unsupported formats fail at creation instead |

Interpretation (ours, open to correction): decode appears to happen in the
texture unit — there is no emulation code path in PanVK, the capability
gating tracks the firmware bits exactly, and device-local optimal-tiling
memory used in these tests is not CPU-mappable, leaving no room for a
silent CPU fallback. BC4+ on this chip would need compute-shader
decompression (e.g. Granite-style kernels); that work is not started.
