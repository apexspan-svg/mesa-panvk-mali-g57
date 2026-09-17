# Mesa PanVK Mali-G57 — kbase JM / Android

Experimental Mesa PanVK work for **Mali-G57 / Valhall** using the Arm **kbase JM (Job Manager)** interface on Android/Termux.

> [!WARNING]
> Experimental / WIP. Not currently a conformant or production-ready Vulkan implementation.

## Target
- GPU: Mali-G57 MC2
- Architecture: Valhall
- Backend: kbase JM
- Tested uAPI: 11.46
- Environment: Android / Termux
- Window system: Termux:X11
- Driver: Mesa PanVK

## Confirmed working
- Mali-G57 MC2 detection and kbase JM initialization
- Vulkan device/queue creation
- Validated offscreen triangle, fragment shader and clear
- Termux:X11 surface and swapchain
- WSI software path and MIT-SHM
- kbase USER_BUFFER import/GPU mapping
- JM external resources
- Fragment and compute/VTC jobs returning JD event `0x01`
- `vkQueuePresentKHR()` returning `VK_SUCCESS`
- Compute writes to WSI USER_BUFFER

## Current WSI status
Current visual result:

    RED -> BLACK

Expected:

    RED -> BLUE

The native swapchain image is rendered correctly (`d9 33 14 ff` observed).
The destination side of the WSI meta-copy is validated, but the normal sampled
source read currently produces zero.

Current investigation:

    native image -> sampled descriptor -> nir_txf -> conversion
                 -> nir_store_global -> USER_BUFFER -> MIT-SHM -> X11

The current primary suspect is the sampled texture read / `nir_txf` path.

See [STATUS.md](STATUS.md), [BUILDING.md](BUILDING.md) and
[KNOWN_ISSUES.md](KNOWN_ISSUES.md).

## Contributions
Testing, traces, debugging, documentation and patches are welcome.

## License
Mesa source files retain their existing upstream licenses. New modifications
should follow the applicable Mesa licensing requirements.
