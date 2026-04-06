# MK1 Kext Reverse-Engineering Notes

This file is a checkpoint for continuing the MK1 revive work in a later session.

## Current State

- The recreated user-space compatibility daemon in [`mk1-bridge/main.c`](./maschine-mk1-revive/mk1-bridge/main.c) is working well enough to:
  - start without the device attached
  - wait for the MK1 to appear
  - detect unplug/replug
  - serve `GET_DEVICE_INFO` through the local `CFMessagePort`
- The recreated user-client path in [`mk1-usb/mk1_device.c`](./maschine-mk1-revive/mk1-usb/mk1_device.c) responds to selector `3` (`getDeviceInfo`) correctly.
- LED probing is partially working, but repeated runtime LED updates are still not stable.

## Confirmed Working

- `user-client-daemon` starts and listens on `com.dragco.mk1-user-client`
- `user-client-probe` returns:
  - vendor `0x17cc`
  - product `0x0808`
  - serial `SN-buscwvye`
- The original driver-facing bottom-row mapping observed through the earlier EP1-based LED probe was:
  - index `1` -> button `4`
  - index `2` -> button `3`
  - index `3` -> button `2`
  - index `4` -> button `1`

## Important Corrections Already Made

### User-space daemon / bridge

- Added:
  - `user-client-daemon`
  - `user-client-probe`
  - `user-client-probe-led-index`
  - `user-client-probe-led-shell`
- Added persistent compat state:
  - cached device info
  - reconnect monitoring
  - persistent compat client
  - serialized LED write mutex

### USB / user-client layer

- `mk1_user_client_open()` no longer performs eager full hardware init.
- `displayCommand` path was corrected earlier to use MK1 EP8 framing.
- `setLEDs` payload size was corrected from:
  - old buggy path: 33-byte payload + `0x0c` prefix = 34 bytes total
  - corrected generic size: 32-byte payload semantics

## What The Decompiled Kext Tells Us

The decompiled file used during this work was:

- `/tmp/ghidra-out/NIUSBMaschineController.decompiled.c`

### Generic command path

- `NIUSBAudioDevice::sendCommand(cmd, payload, len)` is the generic EP1 command path.
- It writes:
  - command byte to one buffer slot
  - payload to another buffer slot
  - then submits the transfer on the EP1 out pipe

### `setLEDs`

- `NIUSBUserClient::setLEDs(...)` forwards to `NIUSBAudioDevice::setLEDs(...)`
- That path is serialized with an `IOLock`
- In the decompile, `NIUSBAudioDevice::setLEDs(...)` has:
  - a generic branch: `sendCommand(this, '\f', payload, len)`
  - a special endpoint-8 branch for a specific product check
- The product check in `setLEDs` does not appear to match the same product branch used by the MK1 display path

### `displayCommand`

- `NIUSBAudioDevice::displayCommand(...)` definitely special-cases MK1 (`0x0808`)
- It uses endpoint `0x08`
- It prepends a 3-byte header in an EP8 buffer before submitting the transfer

### Init path

- `initHardware()`:
  - allocates multiple `IOLock`s
  - calls `initUSB()`
  - calls `updateFirmware()`
  - allocates EP1 and EP8 buffers
- `initUSB()`:
  - opens the device for configuration
  - sets configuration
  - switches alternate setting to `1`
  - resolves/open pipes including endpoint `0x08`

## What We Tested

### EP1-wrapped LED path

- We initially implemented selector `6` as:
  - EP1 command `0x0c` + payload
- This path could light some LEDs, but it was unstable:
  - first write often worked
  - later writes stalled or stopped changing visible LED state

### Raw EP8 LED path

- We then tested selector `6` as a raw endpoint `0x08` payload write.
- Result:
  - repeated writes were transport-stable
  - no visible LEDs lit
- This suggests raw EP8 payload alone is not sufficient for MK1 LED output in the current device state.

## Current Best Interpretation

- MK1 display definitely belongs on EP8.
- MK1 LED transport is still unresolved.
- The decompile strongly suggests that `setLEDs` may still use the generic `sendCommand('\f', ...)` path for MK1, even though a separate special endpoint-8 branch exists for some other hardware case.
- Our old EP1 `0x0c` translation was probably not faithful to the original `sendCommand('\f', ...)` implementation.

## Open Questions

1. What exact USB bytes does `sendCommand('\f', payload, len)` place on the wire for selector `6`?
2. Does MK1 require some extra `updateFirmware()` or post-init state before repeated LED updates are honored?
3. Is the selector-6 payload itself already in final hardware LED layout, or does the original driver transform it first?

## Next Recommended Steps

1. Keep mining the decompiled kext around:
   - `sendCommand`
   - `NIUSBAudioDevice::setLEDs`
   - `updateFirmware`
   - any callsites that invoke `setLEDs`
2. Rework selector `6` to mimic the original generic `sendCommand('\f', ...)` path more faithfully instead of:
   - EP1 `0x0c` wrapping
   - or raw EP8 payload writes
3. Compare any kext buffer layout used by `sendCommand` against the current `mk1_device_write_endpoint` behavior.

