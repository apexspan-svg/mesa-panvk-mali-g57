# PanVK Mali-G57 MC2 — Development Status

## Current phase
**WSI / Vulkan presentation through Termux:X11**

## Confirmed
- [x] kbase JM uAPI 11.46
- [x] Mali-G57 MC2 detected
- [x] Vulkan offscreen rendering
- [x] Triangle + fragment shader
- [x] Validated offscreen clear
- [x] Termux:X11
- [x] Surface and swapchain
- [x] WSI `sw_device=1`
- [x] MIT-SHM / `alloc_shm` / `sw_host_ptr`
- [x] USER_BUFFER import and GPU mapping
- [x] `BASE_MEM_CACHED_CPU`
- [x] JM external resources
- [x] `BASE_JD_REQ_EXTERNAL_RESOURCES`
- [x] Fragment JD event `0x01`
- [x] Present compute/VTC JD event `0x01`
- [x] `vkQueuePresentKHR() == VK_SUCCESS`
- [x] Compute -> USER_BUFFER
- [x] Meta constant -> USER_BUFFER

## Rendered source image
- 640x480
- LINEAR during current diagnostic
- stride: 2560 / `0xA00`
- size: 1,228,800 / `0x12C000`
- observed pixels: `d9 33 14 ff`

## Current isolation

    Fragment -> native image          WORKING
    Compute -> USER_BUFFER            WORKING
    Meta constant -> USER_BUFFER      WORKING
    Native image -> normal meta-copy  FAILING

`convert_texel()` was bypassed while preserving the real texture fetch.
The output remained zero.

Current primary investigation:

**sampled texture fetch / `nir_txf`**

Next useful isolation: raw/global memory load from the rendered native image,
bypassing the texture unit.

## Visual status

    Current:  RED -> BLACK
    Expected: RED -> BLUE

Complete WSI remains **IN PROGRESS**.

## Separate issue
`vkmark` has an independent batch-reuse failure:

    JD event 0x58
    DATA_INVALID_FAULT
