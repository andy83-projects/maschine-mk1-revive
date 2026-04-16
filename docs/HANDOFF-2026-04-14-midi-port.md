# MIDI Port Investigation Handoff

Date: 2026-04-14

## Goal

Determine whether the Maschine MK1 exposes a real macOS/CoreMIDI port, or whether
it is only visible as a USB device with no class-compliant MIDI endpoint.

## What was checked

The following checks were run from the repo checkout on the Apple Silicon Mac:

```bash
system_profiler SPUSBDataType
ioreg -l | rg -i "maschine|native instruments|midi"
ioreg -l | rg -i "MIDIDevice|MIDIEntity|MIDIEndpoint|CoreMIDI|MIDIServer|Maschine Controller"
system_profiler SPAudioDataType | rg -i "midi|maschine|native instruments"
system_profiler SPMIDIDataType
lsof -nP -p 6078
ioreg -l -w0 | rg -i -C 8 "Maschine Controller|idVendor|idProduct|bInterfaceClass|bInterfaceSubClass|IOUSBHostInterface"
```

`PID 6078` was `MIDIServer` at the time of capture.

## What was confirmed

The device is definitely present on USB:

- Product: `Maschine Controller`
- Vendor: `Native Instruments`
- Vendor ID: `6092` (`0x17cc`)
- Product ID: `2056` (`0x0808`)

`ioreg` showed:

- `Maschine Controller@02100000`
- `IOUserClientCreator = "pid 6078, MIDIServer"`

That means the system sees the hardware and the MIDI stack is at least probing or
opening something in the device tree.

## What was *not* confirmed

No clearly exposed CoreMIDI port or endpoint was found:

- `system_profiler SPMIDIDataType` returned no visible Maschine MIDI device
- `system_profiler SPAudioDataType | rg -i "midi|maschine|native instruments"` returned nothing useful
- `ioreg` did not show a named `MIDIDevice`, `MIDIEntity`, or `MIDIEndpoint` corresponding to the Maschine

`lsof -nP -p 6078` showed `MIDIServer` had Apple MIDI driver components loaded:

- `AppleMIDIUSBDriver.plugin`
- `AppleMIDINetworkDriver.plugin`
- `AppleMIDIBluetoothDriver.plugin`
- `AppleMIDIIACDriver.plugin`

But that still did **not** prove there is a user-visible MIDI endpoint for the MK1.

## New evidence from live USB interface inspection

The follow-up `ioreg` inspection on the `IOService` plane exposed the actual USB
interface node under the Maschine device.

Device node:

- `Maschine Controller@02100000`
- `bDeviceClass = 255`
- `bDeviceSubClass = 255`
- `bDeviceProtocol = 255`

Interface node:

- `Highspeed@0  <class IOUSBHostInterface>`
- `bInterfaceNumber = 0`
- `bAlternateSetting = 1`
- `bInterfaceClass = 255`
- `bInterfaceSubClass = 255`
- `bInterfaceProtocol = 0`
- `bNumEndpoints = 4`

At the time of capture the device was owned by the bridge:

- `UsbExclusiveOwner = "pid 44165, mk1-bridge"`

This is materially stronger evidence than the earlier CoreMIDI checks, because it
shows the USB descriptors currently visible to macOS are vendor-specific, not USB
Audio / USB MIDI class descriptors.

## Current conclusion

Current best read:

- USB device present: **yes**
- Standard macOS/CoreMIDI port visible: **no evidence**
- Device-level USB class is vendor-specific (`255`)
- Observed interface-level USB class is vendor-specific (`255/255`)

This now strongly suggests the MK1 is **not** exposing a normal class-compliant
USB MIDI port on macOS, even though the device is attached and `MIDIServer` is aware
of it at some level.

## Most likely explanation

The MK1 presents itself as a vendor-specific / NI-specific USB device rather than a
standard USB MIDI class device, so macOS does not create a normal MIDI port for it
in CoreMIDI.

That is consistent with the rest of this project, where the bridge talks directly to
the device over IOKit/USB rather than relying on a standard system MIDI path.

## Remaining ambiguity

One live interface was directly observed:

- interface `0`, alternate setting `1`, class/subclass `255/255`

That is enough to explain the missing CoreMIDI endpoint on this machine.

What is still not fully enumerated from the current capture is whether the device
also exposes an additional interface node elsewhere in the registry, for example a
DFU or secondary vendor-specific function. Existing project notes suggest a second
non-runtime interface may exist, but nothing from the live Apple Silicon capture
indicates a USB Audio/MIDI interface.

## Next step for a fresh chat

The CoreMIDI question is effectively answered. The next investigation should focus on
fully enumerating all USB interfaces/endpoints for documentation completeness, not on
trying to find a hidden macOS MIDI port.

Ask the next chat to determine whether the MK1 advertises any USB MIDI interface at all.

Recommended next commands:

```bash
system_profiler SPUSBDataType
ioreg -p IOUSB -l -w0 | rg -i -C 6 "Maschine Controller|bInterfaceClass|bInterfaceSubClass|bInterfaceProtocol"
system_profiler SPUSBDataType | sed -n '/Maschine Controller/,+80p'
ioreg -l -w0 | rg -n -C 12 'Maschine Controller@02100000|Highspeed@0|bInterfaceNumber|bAlternateSetting|bNumEndpoints|bInterfaceClass|bInterfaceSubClass|kUSBString'
```

If needed, also inspect raw descriptors more deeply with:

```bash
ioreg -p IOUSB -l -w0
```

## What to look for next

In the next chat, verify whether any Maschine interface advertises:

- Audio class / MIDI class interface descriptors
- USB MIDI subclass values
- or whether all relevant interfaces are HID / vendor-specific

The current evidence already points to that answer:

- observed device class: vendor-specific
- observed interface class: vendor-specific
- no USB Audio / MIDI class interface seen

That explains why no normal MIDI port appears in macOS.
