# Maschine MK1 Revive Session Notes

## Current status

- `mk1-bridge` successfully impersonates `NIHardwareAgent` well enough for Maschine 2.0 to detect the MK1 in the controller list.
- The IPC handshake currently works through:
  - `GetServiceVersion`
  - `PID Connect`
  - device ACK
  - `DEVICE_ON`
  - `Serial Connect`
  - instance ACK
  - extra numeric events after instance handshake
- Real MK1 serial is confirmed as `SN-buscwvye`.

## Important protocol findings

- The bridge needed both device and instance phases to get Maschine 2.0 to recognize the controller.
- `Serial Connect` is used by Maschine 2.0 for MK1.
- Adding numeric events after instance handshake was the breakthrough that made the controller appear:
  - `0x03444e00`
  - `0x03434e00`
- Current relevant files:
  - `mk1-bridge/mk1_server.c`
  - `mk1-ipc/mk1_ipc.h`
  - `mk1-bridge/main.c`

## Apple Silicon USB findings

- Pure HID probing was the wrong layer for this device on Apple Silicon.
- `IOHIDManager` broad enumeration triggered behavior we do not want again because macOS treated it like keyboard/input inspection.
- HID matching never found the MK1 anyway.
- USB-device discovery via `IOKit` works.

## Working Apple Silicon USB result

- `mk1-usb/mk1_device.c` now finds the MK1 through the USB registry:
  - class `IOUSBHostDevice`
  - VID `0x17cc`
  - PID `0x0808`
  - serial `SN-buscwvye`
- Bridge startup now reports:
  - MK1 found on USB
  - MK1 serial from USB

## Current blocker

- USB interface and endpoint access are not implemented yet.
- Registry walking has not yet found usable interface nodes from the matched MK1 device on Apple Silicon.
- Attempts so far:
  - `kIOServicePlane` child walk: no useful interface children
  - `kIOUSBPlane` child walk: no useful path/children
  - ancestry match against `IOUSBHostInterface`: no results so far

## Intel Mac findings

- The working Intel system confirms:
  - VID/PID `0x17cc / 0x0808`
  - serial `SN-buscwvye`
  - device appears as `AppleUSBDevice`
  - `IOUSBLib.bundle` is present in the stack
- `NIHardwareAgent` is the hardware-side process; `Maschine 2` is separate.
- This supports staying on native `IOKit` instead of pivoting to `libusb`.

## Most useful pending Intel captures

Run later on the Intel Mac:

```bash
lsof -p <NIHardwareAgent_PID> | egrep 'IOUSB|USB|HID|Maschine'
```

```bash
ioreg -p IOService -l -w 0 | sed -n '/Maschine Controller@14200000/,+300p'
```

Goal:

- determine what `NIHardwareAgent` opens
- find interface children/endpoints under the MK1 on the working Intel stack

## Recommended next steps

1. On Intel Mac, capture `lsof` for `NIHardwareAgent`.
2. On Intel Mac, capture the `IOService` subtree under `Maschine Controller@14200000`.
3. Use that data to choose the correct native `IOKit` USB interface path on Apple Silicon.
4. Implement endpoint I/O in `mk1-usb`.

## Files changed during this session

- `mk1-bridge/main.c`
- `mk1-bridge/mk1_server.c`
- `mk1-ipc/mk1_ipc.h`
- `mk1-usb/mk1_device.h`
- `mk1-usb/mk1_device.c`
- `maschine-mk1-revive.xcodeproj/project.pbxproj`
