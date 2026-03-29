# MK1 USB / HID Protocol Notes

## Device Identification

| Property | Value |
|----------|-------|
| Vendor ID | `0x17CC` (Native Instruments) |
| Product ID | `0x0808` |
| Device name | "Maschine Controller" |
| USB class | HID |

## Interfaces

The device exposes two USB interfaces:
1. **HID interface** — buttons, pads, encoders in; LEDs, displays out
2. **DFU interface** — firmware upgrade, ignore for our purposes

## HID Input Reports (device → host)

### Report ID `0x10` — Buttons and Encoders
Sent when button state or encoder position changes.

| Byte offset | Content |
|-------------|---------|
| 0 | Report ID (`0x10`) |
| 1-N | Button bitmask + encoder values |

TODO: Full byte map (capture from Intel Mac / reference open-maschine)

### Report ID `0x20` — Pad Pressure
Sent continuously (not just on change).

| Byte offset | Content |
|-------------|---------|
| 0 | Report ID (`0x20`) |
| 1-32 | 16 pads × 2 bytes pressure each |

TODO: Confirm byte order and pressure range

## HID Output Reports (host → device)

### LED Control
TODO: Report ID and format for setting button LEDs

### Display Output
Two monochrome LCD displays.
TODO: Report ID, dimensions, pixel format

## References

- fzero/maschine-mk1: https://github.com/fzero/maschine-mk1
- hansfbaier/open-maschine: https://github.com/hansfbaier/open-maschine
- Linux caiaq driver (kernel source): drivers/usb/misc/caiaq/
