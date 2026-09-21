# Mesa PanVK Vulkan Driver for ARM Mali-G57 (Valhall JM) on Android/Termux

This repository contains the experimental Mesa PanVK Vulkan driver patched to run hardware-accelerated Vulkan on **ARM Mali-G57 MC2** (Valhall v9 / Job Manager architecture) directly inside **Android Termux** using ARM's proprietary kernel module (`/dev/mali0` via `kbase` uAPI JM 11.38).

---

## Key Hardware & Software Features

1. **Native `kbase` Integration:**
   * Talks directly to Android's kernel graphics driver via `/dev/mali0` using the Job Manager (JM) submission path.
   * Eliminates the need for Linux mainline DRM/KMS `panfrost.ko`.

2. **Termux:X11 WSI Presentation via MIT-SHM (`userbuf`):**
   * Imports X11 MIT-SHM shared memory into Mali GPU virtual address space using `KBASE_IOCTL_MEM_IMPORT` (`userbuf`).
   * Eliminates DRI3 explicit synchronization requirements, allowing direct presentation to Termux:X11.

3. **Valhall JM Hardware Errata Workarounds:**
   * **Multi-Layer Blit Split:** Fixes a silent GPU hardware freeze where $N$-layer instanced blits in `vk_meta` deadlocked the Valhall JM fragment stage.
   * **Tilebuffer MSAA Resolve:** Reverse-engineered tilebuffer store descriptors (`PAN_FB_MSAA_COPY_AVERAGE`) to fix black/corrupted output when resolving multisampled textures.
   * **Diagnostic Silencing:** Gated noisy per-draw memory hex dumps behind `PANVK_VERBOSE`, allowing smooth real-time performance.

---

## Verified Benchmarks & Tests

* **Hardware:** MediaTek / ARM Mali-G57 MC2
* **Kernel uAPI:** `kbase` JM 11.38
* **`vkmark` Performance:**
  * `[clear] <default>`: **119 FPS**
  * `[cube]  <default>`: **88 FPS**
  * Overall vkmark score: **103**
* **3D Pipeline Verification:** Tested with custom procedural 3D terrain rendering pipeline (`test_panvk_terrain`).

---

## How to Build in Termux

### 1. Prerequisites
Install required packages in Termux:
```bash
pkg update
pkg install -y git meson ninja clang python libandroid-shmem-static \
               xorgproto libx11 libxcb libxshmfence vulkan-loader \
               vulkan-tools vkmark
```

### 2. Configure & Compile
Run the included build script:
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

---

## How to Install & Configure

### Register the Vulkan ICD
Run the included installation script:
```bash
./install_panvk.sh
```
Or manually create `$PREFIX/share/vulkan/icd.d/panfrost_icd.aarch64.json`:
```json
{
    "file_format_version": "1.0.1",
    "ICD": {
        "api_version": "1.3.354",
        "library_arch": "64",
        "library_path": "/full/path/to/build-bionic/src/panfrost/vulkan/libvulkan_panfrost.so"
    }
}
```

### Verification
Verify that Vulkan detects the hardware driver:
```bash
vulkaninfo --summary
```
Expected output:
```text
GPU0:
    deviceName         = Mali-G57 MC2
    deviceType         = PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU
    driverID           = DRIVER_ID_MESA_PANVK
    driverName         = panvk
```

---

## Running Tests

Navigate to `tests/panvk-g57/`:
```bash
cd tests/panvk-g57

# Compile direct kernel probe
clang test_gpu_id.c -o test_gpu_id
./test_gpu_id

# Compile swapchain test (requires DISPLAY=:0 with Termux:X11)
clang test_swapchain_g57.c -lxcb -lvulkan -o test_swapchain_g57
DISPLAY=:0 ./test_swapchain_g57

# Compile 3D terrain test
clang test_panvk_terrain.c -lxcb -lvulkan -lm -o test_panvk_terrain
DISPLAY=:0 ./test_panvk_terrain
```
