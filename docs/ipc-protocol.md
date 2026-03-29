# NI IPC Protocol Notes

## Overview

NIHardwareAgent (NIHA) communicates with the Maschine software via `CFMessagePort`
— a Mach-based IPC mechanism in CoreFoundation.

## Port Names

| Port | Name | Owner | Purpose |
|------|------|-------|---------|
| NIHA listen port | `NIHWMainHandler` | NIHardwareAgent | We connect to this |
| Client receive port | `SIHWMainHandler` | Our bridge | NIHA sends events here |

Both names confirmed by biappi/Macchina (MK1, 2012) and SamL98/NIProtocol (MK2, 2019).

## Message Format

Messages are sequences of **big-endian uint32** values packed into CFData:

```
[message_type: uint32] [field_0: uint32] [field_1: uint32] ... [field_N: uint32]
```

Hardware event messages (pad pressure, button state) follow the same structure
with raw HID report data packed into the fields.

Per terminar/rebellion: some USB HID report data is forwarded over IPC
with the same byte layout — the HID report IS the IPC payload in some cases.

## Handshake Sequence

Based on SamL98/NIProtocol reverse engineering of MK2 NIHardwareAgent.
MK1 (Macchina) uses same port names so protocol is likely identical or very similar.
The shim capture with MK1 hardware will confirm exact message IDs.

### Exchange 1 — Initial contact
```
Client → NIHA
  msgid:   0x3536756
  payload: [empty]
```

### Exchange 2 — Session registration
```
Client → NIHA
  msgid:   0x3447500
  payload: [session_id, "NiM2", "prmy", 0]
```
- `session_id`: per-connection uint32 identifier
- `"NiM2"`: device type tag (4-char packed as uint32) — may be `"NiM1"` for MK1
- `"prmy"`: role tag — "primary" instance

### Exchange 3 — Register notification port
```
Client → NIHA
  msgid:   0x3404300
  payload: [0x100, 0, "SIHW"]
```
- `0x100`: boolean "true" — we want event notifications
- `"SIHW"`: abbreviated prefix of our local port name

### Exchange 4 — Device pairing
```
Client → NIHA
  msgid:   0x3447143
  payload: [0]  (serial = 0 on first attempt)
```

### Exchanges 5-7 — NIHA responds
```
NIHA → Client (async, arrives on SIHWMainHandler)
```
- Exchange 5: NIHA sends session info + device serial number
- Exchange 6: NIHA sends port ID 0x1350 — rest of exchange skipped (known quirk)
- Exchange 7: continuation

### Exchange 8 — Client echoes serial
```
Client → NIHA
  msgid:   0x3447143
  payload: [device_serial]  (serial received from NIHA in exchange 5)
```

## Post-Handshake

After handshake completes, NIHA begins forwarding hardware events to our
`SIHWMainHandler` port as CFMessagePort messages.

Output commands (set LEDs, write display) are sent by the Maschine software
to NIHA, which we observe and forward to the USB device.

## Open Questions

1. Does the device type tag change from `"NiM2"` to `"NiM1"` for MK1?
   → Will be answered by shim capture with MK1 plugged in

2. Do the msgid values change between MK1-era and current NIHA?
   → Current NIHA (shipping with Maschine 2.x) may have updated protocol
   → Shim capture will reveal actual msgids

3. Does NIHA check the kext service before accepting connections?
   → If yes, we may need to fake the IORegistry entry
   → Shim will show if IOServiceMatching calls happen before/during handshake

4. What is the exact format of hardware event messages for MK1 pads/buttons?
   → Likely same as USB HID report (per rebellion observation)
   → Capture with device plugged in will confirm

## References

- biappi/Macchina: https://github.com/biappi/Macchina
- SamL98/NIProtocol: https://github.com/SamL98/NIProtocol
- SamL98 article: https://lerner98.medium.com/rage-against-the-maschine-3357be1abc48
- terminar/rebellion: https://github.com/terminar/rebellion
