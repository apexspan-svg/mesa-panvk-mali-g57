# Mesa PanVK Mali-G57 — kbase JM / Android

Experimental Mesa PanVK Vulkan driver for **ARM Mali-G57 MC2 / Valhall** using the Arm **kbase JM (Job Manager)** interface on Android / Termux.

> [!NOTE]
> This driver enables hardware-accelerated Vulkan on Mali-G57 inside Termux:X11 without requiring Linux mainline DRM/KMS `panfrost.ko`.

---

## Target Hardware & Environment
* **SoC:** MediaTek Dimensity 6300 (MT6835)
* **GPU:** ARM Mali-G57 MC2 (Valhall v9, GPU ID `0x90930010`)
* **Architecture:** Valhall v9 (Job Manager / JM)
* **Kernel Driver:** ARM `kbase` (`/dev/mali0`, uAPI JM 11.38 / 11.46)
* **Environment:** Android / Termux
* **Display Server:** Termux:X11 (via MIT-SHM `userbuf` import)
* **Driver:** Mesa PanVK (`libvulkan_panfrost.so`)

---

## Confirmed Working & Benchmarks

* **Device & Queue:** Mali-G57 MC2 detection, device initialization, queue creation via `kbase` JM uAPI.
* **WSI / Display:** Termux:X11 swapchain presentation via `userbuf` host import and MIT-SHM blit.
* **Benchmarks:**
  * `vkmark 2025.01` (Hardware Mali-G57):
    * `[clear] <default>`: **119 FPS** (8.403 ms)
    * `[cube]  <default>`: **88 FPS** (11.364 ms)
  * **WebGL & Real-World Browser Graphics:**
    * **Broad Sample Compatibility:** Verified loading and running almost all official test demos from [webglsamples.org](https://webglsamples.org/) on MediaTek Dimensity 6300 (Mali-G57 MC2) (dynamic lighting, shaders, textures, reflections, and particle systems).
    * **WebGL Aquarium (500 Fishes at 1024x1024 Canvas):**
      * **PanVK:** **15–25 FPS** (peak ~26 FPS, real-time interactive rendering)
      * **VirGL (`virpipe`):** **~3 FPS** (flat-lined hard bottleneck due to socket IPC serialization)
      * Delivers a **5x–8x real-world speedup** over VirGL.

<p align="center">
  <img src="webgl_aquarium_500fish_screenshot.png" alt="WebGL Aquarium (500 Fishes) on Mali-G57 MC2 via PanVK" width="650" />
  <br>
  <em>Live Capture: WebGL Aquarium running inside Chromium on Termux:X11 with PanVK hardware acceleration on MediaTek Dimensity 6300 (ARM Mali-G57 MC2).</em>
</p>

* **PPSSPP Homebrews (Windows ARM64 via Wine + PanVK):**
  * **Mega Drops (2D Puzzle Game):** Runs at locked **60.0 FPS** (100% full speed).
  * **Suicide Barbie (3D Demoparty Showcase):** Successfully rendered complex multi-pass lighting, alpha blending, and skinned 3D meshes with **zero kernel timeouts (`atom * failed = 0`)** and zero device loss across a continuous 35-minute session.
  * *See [Setup & Configuration Notes](docs/panvk_g57/PPSSPP_AND_CHROMIUM_SETUP.md) and [Full Homebrew Playtest Report](docs/panvk_g57/PPSSPP_HOMEBREW_PLAYTEST.md).*

* **Mesa Zink (OpenGL 3.2 Core over Vulkan):**
  * **glmark2 3D Cat Model (Phong Shading):** Sustained **49.0 FPS** (Score: 48, Avg FrameTime: 20.43 ms) with dynamic per-pixel lighting, specular highlights, and hardware z-buffering.
  * **glmark2 3D Cat Model (Gouraud Shading):** Sustained **59.0 FPS** (Score: 58, Avg FrameTime: 17.16 ms).
  * **glmark2 3D Box & Crate Demos:** **55.0 – 57.0 FPS** (UV texture mapping & dynamic shading).
  * **OpenGL Extension Suite (`glxgears`):** **126.55 FPS** on Termux:X11 display `:0`.
  * *See [Full OpenGL Zink Test Report](docs/panvk_g57/OPENGL_ZINK_TEST_REPORT.md).*

<p align="center">
  <img src="docs/panvk_g57/images/glmark2_zink_cat_phong.png" alt="glmark2 3D Cat Model via Mesa Zink + PanVK on Mali-G57 MC2" width="600" />
  <br>
  <em>Live Capture: glmark2 3D Cat benchmark (per-pixel Phong lighting & specular highlights) running via Mesa Zink on top of PanVK Vulkan at 49 FPS on Mali-G57 MC2 (MediaTek Dimensity 6300).</em>
</p>

* **DirectX Support (Direct3D 9 & 10 via Wine + Zink + PanVK):**
  * **Direct3D 9 (`wined3d`):** **37.60 – 46.91 FPS** sustained (`FPS: 37.6 | Frame: 482`), verified hardware depth buffer (`D3DFMT_D16`) and Euler rotation.
  * **Direct3D 10 (`d3d10.dll` / DXGI):** **26.10 – 28.85 FPS** sustained (`FPS: 26.1 | Cut Corner | Frame: 163`), verified runtime HLSL 4.0 compilation and dynamic lighting with zero driver hangs.
  * *See [Direct3D 9 & 10 Playtest Report](docs/panvk_g57/DIRECTX_TEST_REPORT.md).*

<p align="center">
  <img src="docs/panvk_g57/images/directx9_live_panvk.png" alt="Direct3D 9 via Wine and PanVK on Mali-G57 MC2" width="600" />
  <br>
  <em>Live Capture: Direct3D 9 application executing in Wine via PanVK + Zink at 37.6 FPS on Mali-G57 MC2 (MediaTek Dimensity 6300).</em>
</p>

<p align="center">
  <img src="docs/panvk_g57/images/directx10_live_panvk.png" alt="Direct3D 10 via Wine and PanVK on Mali-G57 MC2" width="600" />
  <br>
  <em>Live Capture: Direct3D 10 scene rendering via Wine DXGI runtime over Mesa Zink + PanVK at 26.1 FPS.</em>
</p>

---

## Key Hardware Patches & Fixes

1. **Termux:X11 WSI Presentation (`wsi_common_x11.c`):**
   * Implements host memory import (`userbuf`) and MIT-SHM blits.
   * Bypasses strict Linux DRI3 explicit sync checks to allow smooth X11 presentation.
2. **Valhall JM Silent Fragment Hang Workaround (`panvk_vX_cmd_meta.c`):**
   * Workaround for a Valhall JM hardware issue where multi-layer instanced blits in `vk_meta` caused the fragment stage to permanently deadlock.
   * Automatically splits multi-layer blits into safe $1 \times 1$ layer passes.
3. **Tilebuffer MSAA Resolve-on-Store (`panvk_vX_cmd_draw.c`):**
   * Resolves multisampled tilebuffers to single-sampled surfaces (`PAN_FB_MSAA_COPY_AVERAGE`).
   * Fixes black screens and corrupted output when resolving multisampled textures.
4. **Kbase Job Dispatch & Synchronization (`panvk_vX_gpu_queue_kbase.c`):**
   * Reworked atom completion loops, timeout handling, and memory barriers.
   * Silenced spammy per-draw memory hex dumps behind `PANVK_VERBOSE` to unlock real-time framerates.
5. **Mesa Zink Support (`nullDescriptor` & `EXT_robustness2`) (`panvk_vX_physical_device.c`):**
   * Lowered extension and feature exposure checks from `PAN_ARCH >= 10` to `PAN_ARCH >= 9`.
   * Mali-G57 (Valhall v9) now advertises `VK_EXT_robustness2` and the `nullDescriptor` feature, unblocking Mesa Zink from rejecting the device and allowing desktop OpenGL 3.2+ and Direct3D translation layers to initialize.
6. **Batch Merging (`panvk_vX_gpu_queue_kbase.c`) *(Update)*:**
   * Collapses the old one-submit-plus-CPU-wait-per-batch pattern into a single job bag per submit, cutting ~160 kernel round trips per frame.
   * Opt-in via `PANVK_MERGE_SUBMIT=1` (always on when async is enabled).
7. **True Async Submission (`panvk_kbase_async.c`, new file) *(Update)*:**
   * Submit hands the job bag to the kernel and returns immediately instead of blocking until the GPU idles; completion is reaped lazily by polling the kbase event fd, with fences/semaphores resolved through `kbase_cpu_sync` armed with bag sequence numbers.
   * One bag in flight per device (per-device serialization preserves kernel execution order); opt-in via `PANVK_ASYNC=1`.
   * Lifts vkmark from ~77 (sync) / ~84 (merge only) to **94** full-suite with zero errors.

---

## Quick Installation (Prebuilt Driver)

Choose whichever installation method is easiest for you:

### Option 1: Fast Install via `curl` (Recommended)
Copy and paste these commands into Termux:

```bash
curl -LO https://github.com/apexspan-svg/mesa-panvk-mali-g57/releases/download/v1.0.0-zink/panvk-mali-g57-v1.0.0-zink.tar.gz
tar -xzvf panvk-mali-g57-v1.0.0-zink.tar.gz
cd panvk-mali-g57-v1.0.0-zink
./install.sh
```

*(Or as a single uninterrupted line: `curl -LO https://github.com/apexspan-svg/mesa-panvk-mali-g57/releases/download/v1.0.0-zink/panvk-mali-g57-v1.0.0-zink.tar.gz && tar -xzvf panvk-mali-g57-v1.0.0-zink.tar.gz && cd panvk-mali-g57-v1.0.0-zink && ./install.sh`)*

---

### Option 2: Direct Download via Browser
If you downloaded `panvk-mali-g57-v1.0.0-zink.tar.gz` through your phone browser from [Releases](https://github.com/apexspan-svg/mesa-panvk-mali-g57/releases/latest) into your `Download` folder:

```bash
cp /sdcard/Download/panvk-mali-g57-v1.0.0-zink.tar.gz ~/
cd ~/
tar -xzvf panvk-mali-g57-v1.0.0-zink.tar.gz
cd panvk-mali-g57-v1.0.0-zink
./install.sh
```

---

### Verify Installation (GPU0)
Because this build includes verbose debug logging (`PANVKDBG`), filter directly for **GPU0**:
```bash
vulkaninfo | grep -A 10 "GPU0"
```
*(Look for `deviceName = Mali-G57 MC2` and `driverName = panvk` under `GPU0:`)*

---

### Test Vulkan Performance with `vkmark`
Verify native Vulkan hardware acceleration and FPS on Termux:X11:
```bash
# 1. Install vkmark
pkg install -y vkmark

# 2. Run benchmark on Termux:X11
DISPLAY=:0 vkmark
```
**Expected Performance on Mali-G57 MC2:**
* `[clear] <default>`: **~119 FPS**
* `[cube]  <default>`: **~88 FPS**

### Async submission build (`v1.0.0-async`, vkmark 94) *(Update)*

A second prebuilt flavor adds batch merging + true async submission (opt-in via `PANVK_ASYNC=1`):

```bash
curl -LO https://github.com/apexspan-svg/mesa-panvk-mali-g57/releases/download/v1.0.0-async/panvk-mali-g57-v1.0.0-async.tar.gz
tar -xzvf panvk-mali-g57-v1.0.0-async.tar.gz
cd panvk-mali-g57-v1.0.0-async
./install.sh
```

Run with async enabled:

```bash
export DISPLAY=:0
export VK_ICD_FILENAMES=$PREFIX/share/vulkan/icd.d/panfrost_icd.aarch64.json
export PANVK_NO_AFBC=1
export PANVK_ASYNC=1
vkmark --winsys xcb -s 640x480
```

**Expected Performance on Mali-G57 MC2:** full-suite **~94** (97 on re-run), zero errors — vs ~81 for the default build on the same device state. Full source: branch [`g57-vkmark-94`](https://github.com/apexspan-svg/mesa-panvk-mali-g57/tree/g57-vkmark-94) (rebuilding it reproduces the release binary byte-for-byte, md5 `d883a28e…`).

---

## Building from Source (Developers)

If you prefer to compile Mesa and PanVK manually from source:

### 1. Install Dependencies
```bash
pkg update
pkg install -y git meson ninja clang python libandroid-shmem-static \
               xorgproto libx11 libxcb libxshmfence vulkan-loader \
               vulkan-tools vkmark
```

### 2. Build the Driver
```bash
./build_panvk.sh
```
Or manually run:
```bash
meson setup build-bionic \
  -Dbuildtype=release \
  -Dpanvk-use-kbase=true \
  -Dvulkan-drivers=panfrost \
  -Dgallium-drivers= \
  -Dplatforms=x11 \
  -Ddebug=false \
  -Dstrip=true \
  -Dbuild-tests=false \
  -Dc_link_args=-landroid-shmem \
  -Dcpp_link_args=-landroid-shmem \
  -Dpanfrost-kmds=kbase,panthor

ninja -C build-bionic src/panfrost/vulkan/libvulkan_panfrost.so
```

### 3. Install Built Driver
```bash
./install_panvk.sh
```

---

## Standalone Tests

Test programs and shaders are provided in `tests/panvk-g57-async/`:

```bash
cd tests/panvk-g57-async

# Test direct kbase kernel ioctls
clang test_gpu_id.c -o test_gpu_id
./test_gpu_id

# Test X11 swapchain presentation (requires Termux:X11 running on DISPLAY=:0)
clang test_swapchain_g57.c -lxcb -lvulkan -o test_swapchain_g57
DISPLAY=:0 ./test_swapchain_g57

# Test full 3D terrain rendering pipeline
clang test_panvk_terrain.c -lxcb -lvulkan -lm -o test_panvk_terrain
DISPLAY=:0 ./test_panvk_terrain
```

---

## Running Benchmarks
Start your Termux:X11 desktop session and run:

### Vulkan Benchmark (`vkmark`)
```bash
DISPLAY=:0 vkmark
```

### OpenGL 3.2 Benchmark via Mesa Zink
```bash
export DISPLAY=:0
export WSI_X11_TERMUX=1
export PANVK_NO_AFBC=1
export PANVK_SPLIT_SUBMIT=1
export GALLIUM_DRIVER=zink
export MESA_LOADER_DRIVER_OVERRIDE=zink

# Check OpenGL acceleration
glxinfo -B | grep "OpenGL"

# Run OpenGL benchmark / glmark2
glmark2
```

---

## Credits & Prior Art

This work builds directly on top of foundational research, forks, and patches from the open-source graphics community:

* **[Mesa 3D Project](https://gitlab.freedesktop.org/mesa/mesa):** The upstream Panfrost / PanVK driver developers.
* **[LukeValen/panvk-mali-g52](https://github.com/LukeValen/panvk-mali-g52):** Key foundational roadmap, architecture investigation, and critical insights into native Termux bringup, kbase non-drm sync signaling, and raw Job Manager (JM) submission without DRM.
* **[funnymdzz/mesa](https://github.com/funnymdzz/mesa):** Pioneered the initial `mali_kbase` kernel module backend and non-DRM device discovery on Android.
* **[leegao/mesa-funnymdzz](https://github.com/leegao/mesa-funnymdzz):** "panvk-over-kbase for Winlator", solving device enumeration and `pan_kmod_dev_create_with_driver` initialization without `/dev/dri`.
* **[mexicanbr0auth/mesa-panvk-g57](https://github.com/mexicanbr0auth/mesa-panvk-g57):** Experimental snapshot and base branch for Mali-G57 kbase/JM bringup.
* **[wonderkast02/panvk-g720-kbase-csf](https://github.com/wonderkast02/panvk-g720-kbase-csf):** Community discussions and reverse-engineering insights on Android Mali kbase interfaces.

---

*Made with AI.*

---

## License
Mesa source files retain their existing upstream licenses (MIT / X11). New modifications follow applicable Mesa licensing requirements.
