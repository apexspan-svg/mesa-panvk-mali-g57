# Known Issues

## X11 output is black
Current: `RED -> BLACK`; expected: `RED -> BLUE`.

The rendered native image and destination USER_BUFFER path are validated.
The current focus is the sampled texture read / `nir_txf` source path.

## vkmark batch reuse
Independent issue:

    JD event 0x58
    DATA_INVALID_FAULT

## Experimental USER_BUFFER handling
Current external-resource handling is diagnostic and still needs proper
per-job/per-batch resource tracking.

## Temporary diagnostics
The development tree may contain `PANVKDBG` logging, diagnostic environment
variables, memory dumps, forced LINEAR WSI behavior and other temporary
instrumentation.
