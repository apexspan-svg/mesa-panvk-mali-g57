# Setup & Configuration Guide: PPSSPP (Wine) & Chromium WebGL with PanVK

This guide outlines how to configure **PPSSPP (Windows ARM64 via Wine)** and **Chromium (WebGL via ANGLE)** to run on top of the experimental **Mesa PanVK Vulkan driver** on ARM Mali-G57 (Valhall JM) devices.

---

## Prerequisites

1. **Working PanVK Driver:**
   * Built and registered via ICD JSON (e.g., at `$PREFIX/share/vulkan/icd.d/panfrost_icd.aarch64.json` or pointed to by `VK_ICD_FILENAMES`).
   * Verify with:
     ```bash
     vulkaninfo --summary
     ```
     Ensure `Mali-G57 MC2` (driver: `panvk`) is enumerated.
2. **X11 Display Server:**
   * An active X11 server on `DISPLAY=:0` (such as Termux:X11).

---

## 1. PPSSPP (Windows ARM64 on Wine)

Running the Windows ARM64 build of PPSSPP through Wine provides a full desktop-class interface with **zero CPU translation overhead** on 64-bit ARM devices.

### Launch Recipe

```bash
export DISPLAY=:0
export VK_ICD_FILENAMES=/path/to/panfrost_icd.json
export PANVK_NO_AFBC=1

wine /path/to/PPSSPPWindowsARM64.exe [path/to/EBOOT.PBP]
```

* Wine's built-in `winevulkan` library forwards guest Win32 Vulkan calls directly to the host PanVK ICD.
* Specifying a game or homebrew path boots directly into the title; omitting it opens the PPSSPP main menu.

### Troubleshooting: The "Refusing Vulkan" Trap

**Symptom:**  
PPSSPP shows an alert: *"PPSSPP crashed while starting … switched VULKAN -> DIRECT3D11"* or refuses to initialize the Vulkan backend, falling back to OpenGL/D3D11 even when PanVK is properly installed.

**Root Cause:**  
If PPSSPP encounters an error during any previous run (such as an early driver crash during testing), it records `VULKAN` in:
```text
memstick/PSP/SYSTEM/FailedGraphicsBackends.txt
```
On subsequent launches, PPSSPP checks this file and **silently skips Vulkan without even attempting to probe it**.

**Solution:**  
Delete the file before launching:
```bash
rm -f memstick/PSP/SYSTEM/FailedGraphicsBackends.txt
```
Once removed, PPSSPP will cleanly initialize the Vulkan device and swapchain on PanVK.

---

## 2. Chromium + PanVK (Hardware-Accelerated WebGL)

By default, Chromium blocklists mobile ARM Mali GPUs and unknown/development driver versions from using Vulkan and hardware compositing. To unlock native hardware WebGL acceleration, specific launch flags are required.

### Launch Flags

```bash
chromium \
  --no-sandbox \
  --test-type \
  --disable-dev-shm-usage \
  --ignore-gpu-blocklist \
  --ignore-gpu-blacklist \
  --enable-features=Vulkan,DefaultANGLEVulkan,VulkanFromANGLE \
  --use-gl=angle \
  --use-angle=vulkan \
  --use-vulkan=native \
  --disable-gpu-rasterization \
  --disable-gpu-multisampling \
  http://webglsamples.org/aquarium/aquarium.html
```

### Flag Breakdown

| Flag | Purpose |
| :--- | :--- |
| `--ignore-gpu-blocklist` | Bypasses Chromium's internal blacklist for Mali GPUs and development drivers. |
| `--use-gl=angle --use-angle=vulkan` | Directs Chromium to use Google ANGLE to translate OpenGL ES/WebGL directly into Vulkan calls. |
| `--use-vulkan=native` | Forces native Vulkan device creation rather than software emulation (SwiftShader). |
| `--enable-features=Vulkan,DefaultANGLEVulkan,VulkanFromANGLE` | Activates Chromium's Vulkan compositor and presentation pipelines. |
| `--no-sandbox --disable-dev-shm-usage` | Essential for containerized/proot/Termux environments without sandbox namespaces. |

### Verification
Navigate to `chrome://gpu` inside Chromium:
* **Vulkan:** Should report **Hardware accelerated**.
* **Compositing:** Should report **Hardware accelerated**.
* **WebGL / WebGL2:** Should report **Hardware accelerated** powered by ANGLE (Vulkan).

---

## 3. Environment Variables Reference

When running Vulkan applications with PanVK on Valhall JM:

* `VK_ICD_FILENAMES`: Path to the custom PanVK ICD JSON file.
* `PANVK_NO_AFBC=1`: Disables ARM Framebuffer Compression (AFBC) on swapchain blit paths to ensure clean LINEAR image presentation in X11.
* `PANVK_VERBOSE=1`: Optional. Enables verbose Job Manager (JM) atom dispatch and buffer diagnostics in stderr (useful for debugging, but leave disabled for full gaming framerates).
