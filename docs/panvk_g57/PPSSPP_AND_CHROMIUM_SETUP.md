# PanVK on Mali-G57 (Termux): Wine/PPSSPP and Chromium setup notes

Device: ARM Mali-G57 MC2 (Valhall JM) · Driver: Mesa PanVK fork at
`~/funnymdzz-mesa`, binary
`build-bionic/src/panfrost/vulkan/libvulkan_panfrost.so` · X11 on `:0` (Termux:X11).

---

## 1. PPSSPP (Windows ARM64) on Wine + PanVK

### Layout
- Emulator: `~/ppsspp-wine/PPSSPPWindowsARM64.exe` (native ARM64 Windows binary —
  runs under Wine with no CPU emulation). Adjacent `memstick/` makes it portable;
  games live in `memstick/PSP/GAME/` (e.g. `MegaDrops/EBOOT.PBP`, `SuicideBarbie/EBOOT.PBP`).
- Wine prefix: default `~/.wine`. No special prefix setup needed.

### Launch recipe (Vulkan via PanVK)
```sh
export DISPLAY=:0
export VK_ICD_FILENAMES=~/panfrost_icd.json   # points at the PanVK .so
export PANVK_NO_AFBC=1
wine ~/ppsspp-wine/PPSSPPWindowsARM64.exe [path/to/EBOOT.PBP] [--log=ppsspplog.txt]
```
- With a game path argument, PPSSPP boots straight into the game; without it, it
  opens the menu (Games/Homebrew Store).
- Wine's `winevulkan` forwards guest Vulkan to the host ICD, so PanVK is used
  transparently. Verify with `WINEDEBUG=+vulkan` (traces every thunk call) and
  PPSSPP's `--log=` file.

### The "refusing Vulkan" problem and how it was beaten
Symptom: dialog "PPSSPP crashed while starting … switched VULKAN -> DIRECT3D11",
`memstick/PSP/SYSTEM/FailedGraphicsBackends.txt` contains `VULKAN`, D3D11 fallback
also dead under Wine here.

Diagnosis (with `WINEDEBUG=+vulkan` + PPSSPP source):
1. Trace showed: `vkCreateInstance` → 1 device enumerated → `vkGetPhysicalDeviceProperties`
   → `vkDestroyInstance`, no `vkCreateDevice`. Looked like rejection, but this
   enumerate→properties→destroy sequence is PPSSPP's **normal** availability probe
   (`VulkanMayBeAvailable` in `Common/GPU/Vulkan/VulkanLoader.cpp`) — not a failure.
2. Control experiment with Lavapipe (`VK_ICD_FILENAMES=<lvp json>`) worked identically
   up to the same point, proving the probe flow is driver-independent.
3. Real root cause: **stale `FailedGraphicsBackends.txt`**. Once `VULKAN` lands in that
   file (from any old failure — e.g. from before the driver fixes), PPSSPP's
   `CheckFailedGPUBackends` (`UI/NativeApp.cpp`) skips Vulkan on every later launch
   *without trying*. Self-perpetuating: each failed/skipped run re-writes the file.
4. Fix: **delete the file** and relaunch clean. The next run then proceeds to real
   `vkCreateDevice` + swapchain + tens of thousands of draws (verified 20k+ submits,
   UI + Mega Drops + Suicide Barbie all rendering, zero device loss).

Rule: whenever PPSSPP shows the Graphics Error dialog, delete
`~/ppsspp-wine/memstick/PSP/SYSTEM/FailedGraphicsBackends.txt` first, then decide
whether the failure is real.

### Input without a keyboard
- `xdotool` drives the PPSSPP window:
  `WID=$(xdotool search --name "Mega Drops" | head -n 1)`,
  `xdotool windowactivate "$WID"`, `xdotool key --window "$WID" <key>`.
- Discovered mapping: arrows = D-pad/move, `z` = PSP Cross (confirm, hold ~150 ms),
  `x` = Circle (back), `Down` = soft-drop. (`x` does NOT confirm.)
- If a window-manager menu pops up, press `Escape` and re-focus.
- Screenshots: `DISPLAY=:0 scrot file.png`.
- **Shell trap:** never `pkill -f "PPSSPPWindows"` — the pattern matches your own
  command line and kills your session. Use `pkill -f "PPSSPPWindow[s]"`.

---

## 2. Chromium + PanVK (WebGL)

### How the default is wired
- The `chromium` wrapper sources `/usr/etc/chromium/*.conf`. The active file is
  `flags.conf` (PanVK); the virgl config is parked as `flags.conf.virgl_bak`,
  which the `*.conf` glob does **not** match — so plain `chromium`, the desktop
  icon, and `start-desktop` sessions all default to PanVK. Swap the filenames to
  switch back to virgl.
- Active `flags.conf`:
  - `CHROMIUM_FLAGS="--no-sandbox --test-type --disable-dev-shm-usage
    --ignore-gpu-blocklist --ignore-gpu-blacklist --use-gl=angle --use-angle=vulkan
    --enable-features=Vulkan,DefaultANGLEVulkan,VulkanFromANGLE
    --disable-gpu-rasterization --disable-gpu-multisampling --enable-webgl ..."`
  - Env: `WSI_X11_TERMUX=1`, `PANVK_NO_AFBC=1`, `PANVK_SPLIT_SUBMIT=1`.
- Driver selection: no `VK_ICD_FILENAMES` override here; Chromium uses the system
  ICD registry, where `panfrost_icd.aarch64.json` points at the fixed
  `libvulkan_panfrost.so` (Lavapipe is also registered but ANGLE prefers the real
  GPU). Verified with `vulkaninfo`: `Mali-G57 MC2 / panvk` listed first.

### Why the blocklist flags are needed
Chromium blocklists Mali GPUs and unknown/old drivers for Vulkan/Graphite paths
(e.g. Mali-G57 + driver-version gates in its GPU blocklist). PanVK reports as Mesa
26.x-dev on Mali-G57, which trips those gates, so:
- `--ignore-gpu-blocklist --ignore-gpu-blacklist` bypass the software-fallback list.
- `--use-gl=angle --use-angle=vulkan --use-vulkan=native` forces the
  ANGLE-on-native-Vulkan path instead of ANGLE-on-GL (virgl) or SwiftShader.
- `--enable-features=Vulkan,DefaultANGLEVulkan,VulkanFromANGLE` turns on
  Chromium's Vulkan backend + ANGLE-as-default-GL + Vulkan-from-ANGLE compositing.
- `--disable-gpu-rasterization --disable-gpu-multisampling` keeps risky GPU
  raster paths off (carried over from the virgl config; harmless with PanVK).
- `--test-type --no-sandbox --disable-dev-shm-usage` are Termux necessities
  (no sandbox namespaces, tiny /dev/shm).

### Test/automation recipe (what was used for all verification)
```sh
export DISPLAY=:0 VK_ICD_FILENAMES=~/panfrost_icd.json PANVK_NO_AFBC=1
chromium --no-sandbox --test-type --disable-dev-shm-usage \
  --ignore-gpu-blocklist --enable-features=Vulkan,DefaultANGLEVulkan,VulkanFromANGLE \
  --use-vulkan=native --use-angle=vulkan --use-gl=angle \
  --remote-debugging-port=92XX --no-first-run --user-data-dir=<fresh-profile> <url> &
```
- Fresh `--user-data-dir` per run (stale GPU-cache/profile state causes flakes).
- `--remote-debugging-port` + CDP (`/json`, screenshot/eval) for headless checks.
- **Shell trap:** plain `pkill -x chrome` matches nothing (comm is the full
  `/usr/lib/chromium/chrome` path) — old instances pile up and share the GPU,
  which contaminates results. Use `pkill -f "chromium/chrom[e]"` and confirm
  with `ps`.
- `vulkaninfo --summary` is the 10-second check that the driver is visible
  (expect `Mali-G57 MC2 / panvk` first, `llvmpipe` second).

### `start-desktop` relationship
`start-desktop` sets up X11 (:0), PulseAudio, `virgl_test_server_android`, and XFCE
with GL env (`GALLIUM_DRIVER=virpipe`, GL version overrides) — that covers **desktop
OpenGL apps only**. It sets no Vulkan/PanVK variables and does not touch Chromium's
config, so Chromium under the desktop still follows section 2 above. The two stacks
coexist (virgl for GL apps, PanVK for Vulkan/WebGL).
