# Mesa PanVK for ARM Mali-G57 on Android Termux

Prebuilt Mesa PanVK Vulkan driver for **ARM Mali-G57 MC2 (Valhall JM)** running on the proprietary `kbase` kernel driver, with X11 WSI for Termux. Also enables **Mesa Zink** (desktop OpenGL on Vulkan) via `robustness2`/`nullDescriptor`.

## Releases

| Release | Contents | Reference numbers (Mali-G57 MC2, Termux, X11) |
|---|---|---|
| [v1.0.0-async](https://github.com/apexspan-svg/mesa-panvk-mali-g57/releases/tag/v1.0.0-async) | PanVK + batch merging + **async submission** (`PANVK_ASYNC=1`) | **vkmark 94** full-suite, zero errors; `mipgen` ALL-DONE; aquarium ~30 fps / 500 fish; PPSSPP (Wine) clean |
| [v1.0.0-zink](https://github.com/apexspan-svg/mesa-panvk-mali-g57/releases/tag/v1.0.0-zink) | PanVK + Zink/OpenGL 3.2 + D3D notes | glmark2 cat 49–59 fps; `glxgears` ~126 fps; D3D9 37–47 fps; PPSSPP Mega Drops 60 fps |

Each release is a `.tar.gz` (+`.zip`) with `libvulkan_panfrost.so`, `install.sh`, and `README.md`.

## Quick install (async build)

```sh
curl -LO https://github.com/apexspan-svg/mesa-panvk-mali-g57/releases/download/v1.0.0-async/panvk-mali-g57-v1.0.0-async.tar.gz
tar -xzvf panvk-mali-g57-v1.0.0-async.tar.gz
cd panvk-mali-g57-v1.0.0-async
chmod +x install.sh
./install.sh
```

## Benchmark it

```sh
export DISPLAY=:0
export VK_ICD_FILENAMES=$PREFIX/share/vulkan/icd.d/panfrost_icd.aarch64.json
export PANVK_NO_AFBC=1
export PANVK_ASYNC=1
vkmark --winsys xcb -s 640x480
```

## Branches

- `main` — base snapshot + docs (this page).
- `g57-vkmark-94` — full source of the async build (opt-in merge + single-bag async engine). Rebuilding it on-device reproduces the release binary byte-for-byte (md5 `d883a28e…`).

## Notes

- Binaries are built on-device in Termux (ARM64); they can't be reproduced on generic Linux CI runners.
- WebGL/Chromium: use ANGLE-on-Vulkan with `PANVK_NO_AFBC=1 PANVK_SPLIT_SUBMIT=1`.
- Known limits: multisampled-render-to-single-sampled resolve stays off on JM (black output, use explicit resolve); async is per-device serialized, opt-in only.
