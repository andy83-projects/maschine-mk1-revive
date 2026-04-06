# MK1 Revive -- Claude Handoff

Last updated: 2026-04-04

---

## What Was Done (2026-04-03 to 2026-04-04)

### Sniffer: Passive IPC Capture (DONE)
- Built `mk1-ipc-sniffer` -- DYLD interpose sniffer using `__DATA,__interpose`
- Captured full Maschine 2 <-> NIHA traffic on Intel Mac with real MK1 hardware
- Decoded all event formats: BTN_DATA, PAD_DATA, KNOB_ROTATE, DEVICE_ON
- Decoded full 12-step handshake including SERIAL_CONNECT and instance-level flow
- Results documented in `memory/protocol.md`

### Server Protocol Fixes (DONE -- builds clean)

Updated mk1-bridge server to match sniffer-confirmed formats:

| Fix | File | What Changed |
|-----|------|-------------|
| ACK client send | mk1_ipc.c:446 | `[type, 0, 0, len, name\0]` not `[type, true, 0, len, name]` |
| ACK server reply | mk1_server.c:536 | `[true]` (4 bytes) not `[type, true]` (8 bytes) |
| DEVICE_ON | mk1_server.c:837 | `[type, 0x03774720, devID, 17, paddedSerial]` (33 bytes) |
| GETSERIAL reply | mk1_server.c:567 | `[17, paddedSerial\0]` (21 bytes) -- no type/true prefix |
| Instance name | mk1_server.c:580 | New handler for 0x0349734e, parses project name |
| SETFOCUS timing | mk1_server.c:607 | Moved to after instance name (was after ACK) |
| pad_serial() | mk1_server.c:83 | 16-char space-padded + null = 17 bytes |
| New constants | mk1_ipc.h | NI_MSG_INSTANCE_NAME, NI_DEVICE_ON_FIELD1, NI_SERIAL_PADDED_LEN |

### USB Forwarding Wired Up (DONE -- builds clean)

Wired both directions of the USB bridge in `mk1-bridge/main.c`:

#### Output: IPC -> USB (Maschine software -> MK1 hardware)

| Function | What It Does |
|----------|-------------|
| `forward_led_to_usb()` | Parses IPC LED command (65 bytes), sends two DIMM_LEDS EP1 banks (0x0c cmd, bank 0 @ index 0, bank 1 @ index 0x1e) |
| `forward_display_to_usb()` | Parses IPC DISPLAY command (10900 bytes), chunks pixel data (max 0x1fc per chunk) through `mk1_set_display()` on EP8 |
| `on_software_cmd()` | Updated switch to call forwarding for NI_CMD_LED and NI_CMD_DISPLAY |

#### Input: USB -> IPC (MK1 hardware -> Maschine software)

| Function | Format | What Changed |
|----------|--------|-------------|
| `on_pad_event()` | PAD_DATA (28 bytes) | Rewritten. `[type, seq, timestamp, flags, padIndex, phase, pressure(float)]`. Phase: 1=touch, 4=aftertouch, 3=release. Pressure normalized to 0.0-1.0 float. |
| `on_button_event()` | BTN_DATA (24 bytes) | Rewritten. `[type, seq, timestamp, flags, btnIndex, state]`. Iterates changed bits in USB bitmask, sends one event per changed button. btnIndex = bit position in HID report (matches sniffer: 0x20-0x27 for top row). |

**Old format removed**: `send_button_event_records()` used MsgControllerChanged with 28-byte header + 8-byte records -- this is NOT what the sniffer shows. Now uses direct BTN_DATA events.

**Key fix**: Old code blocked all button events through `mk1_map_button_bit_to_control()` which suppressed events unless `unsafe_raw_input_forwarding` was set. New code sends events unconditionally (after instance handshake).

---

## Current Status

- **Handshake: CONFIRMED WORKING** -- Maschine 2 detects MK1 when mk1-bridge is running
- **Input forwarding: REWRITTEN** -- pad/button events use sniffer-confirmed formats (code complete, untested with real USB)
- **Output forwarding: WIRED** -- LED/display commands routed to USB functions (code complete, untested with real USB)
- **USB layer: COMPILES but may not be linked** -- `MK1_USB_ENABLED=1` because `__has_include` finds the header, but `libmk1-usb.a` may not be linked to mk1-bridge target
- **Knob events: NOT YET DONE** -- no knob callback exists in `mk1_device.h` (see step 2)

---

## Dead Code to Clean Up

The following are now unused after the button event rewrite:

| Symbol | File | Why Dead |
|--------|------|----------|
| `send_button_event_records()` | main.c:552 | Replaced by inline BTN_DATA in `on_button_event()` |
| `mk1_map_button_bit_to_control()` | main.c:517 | Was the gate that blocked all button events |
| `log_suppressed_button_event()` | main.c:489 | No longer called |
| `mk1_ipc_button_record_t` | main.c:77 | Was used by old record format |
| `unsafe_raw_input_forwarding` field | main.c:65 (bridge_t) | Was the bypass flag for the old gate |
| `suppressed_unmapped_button_bits` field | main.c:70 (bridge_t) | Counter for the old gate |
| `suppressed_button_events` field | main.c:69 (bridge_t) | Counter for old suppression |
| Forward declarations at main.c:102-111 | main.c | For the above dead functions |

These compile harmlessly but should be removed for clarity.

---

## Priority Order (Next Steps)

### Step 1: Verify USB Linking [CONFIDENCE: MEDIUM]

**What**: Confirm `libmk1-usb.a` is actually linked into the mk1-bridge executable.

**Why medium confidence**: `MK1_USB_ENABLED` is 1 (header found via `__has_include`), and the project builds clean. But this only means the USB *calling* code compiles -- the USB *implementation* symbols resolve at link time only if `libmk1-usb.a` is in the link phase. The build may succeed because all USB calls are inside `#if MK1_USB_ENABLED` blocks that the compiler compiles but the linker might not resolve yet.

**How to verify**:
```bash
# Check if USB symbols are in the binary:
nm Products/mk1-bridge | grep mk1_device_open
# If "U mk1_device_open" (undefined) -> NOT linked
# If "T _mk1_device_open" (text) -> linked
```

**If NOT linked**: In Xcode project, add `libmk1-usb.a` to mk1-bridge target's "Link Binary With Libraries" build phase. Also need to link IOKit.framework and CoreFoundation.framework (mk1_device.c uses IOKit USB).

**Existing USB flow in main.c** (already written, just needs USB linked):
- `wait_for_usb_device()` (line 276): polls `mk1_device_open()` in a loop
- `mk1_device_init_hardware()` (line 299): sends caiaq init sequence
- `start_usb_if_available()` (line 260): calls `mk1_device_start(dev, on_pad_event, on_button_event, ctx)`
- All called from `main()` -- the flow already exists

### Step 2: Add Knob/Encoder Input [CONFIDENCE: LOW]

**What**: Wire knob rotation events to sniffer-confirmed KNOB_ROTATE format.

**Why low confidence**:
1. `mk1_device.h` has NO knob callback type -- `mk1_device_start()` only accepts `mk1_pad_callback_t` and `mk1_button_callback_t`
2. Knobs may arrive via the button report (packed in same HID report) or via a separate encoder async read (`MK1_UC_ASYNC_ENCODER_INPUT_READ` in mk1_device.c:1725)
3. We don't know which bytes in the HID report contain encoder/knob data
4. The sniffer captured KNOB_ROTATE events but we don't have the corresponding USB HID bytes to see the mapping

**Sniffer-confirmed IPC format** (this part IS known):
```
KNOB_ROTATE (0x03654e00) -- 24 bytes:
  [type(4), seq(4), timestamp(4), flags(4), knobIndex(4), delta(float4)]
  knobIndex: 0-7 for knobs, 0x0A for main encoder
  delta: IEEE 754 float (+CW, -CCW)
```

**What needs to happen**:
1. Add a knob callback type to `mk1_device.h`
2. In `mk1_device.c`, figure out which HID report bytes carry knob/encoder data (may need USB captures or trial-and-error)
3. Parse HID -> call knob callback
4. In `main.c`, add `on_knob_event()` using the KNOB_ROTATE format above

**Alternative**: Knobs might already be arriving as part of button reports. Check if `on_button_event()` is called with extra bytes when knobs turn. If so, the encoder bits are in the button bitmask and would already be sent as BTN_DATA -- but Maschine expects them as KNOB_ROTATE, not BTN_DATA.

### Step 3: Test Full Loop [CONFIDENCE: HIGH -- if USB links]

**What**: Run mk1-bridge with MK1 plugged in, launch Maschine 2, verify LEDs/displays/buttons/pads.

**How**:
```bash
sudo kill $(pgrep NIHardwareAgent)   # kill stock NIHA
./Products/mk1-bridge                 # run our bridge
# Then launch Maschine 2
```

**Expected if working**: LEDs light up, displays show content, pads/buttons respond in Maschine.

**If LEDs don't light up**: Check bridge stderr for `[bridge] <- LED command` messages. If present, the IPC->USB forwarding path needs debugging. If absent, the handshake may not be completing fully.

**If buttons don't respond**: Check for `[bridge] sending BTN_DATA` on stderr. The btnIndex mapping (bit position = NIHA index) is based on sniffer showing 0x20-0x27 for top row, which matches bit positions. If some buttons are wrong, a remap table is needed.

### Step 4: Button Index Mapping Verification [CONFIDENCE: MEDIUM-HIGH]

**What**: Verify that USB HID bit positions match NIHA button indices for ALL buttons.

**Why medium-high**: Top row (0x20-0x27) is confirmed by sniffer. Bit 0x1D also observed. The pattern (bit position = NIHA index) is consistent but we only have ~9 of ~40+ buttons confirmed.

**If mapping is wrong**: Add a lookup table in `on_button_event()` to remap bit positions to NIHA indices. The sniffer on Intel Mac can capture all button presses to build the complete map.

### Step 5: LED Index Mapping [CONFIDENCE: MEDIUM]

**What**: Verify the 57-byte IPC LED payload maps correctly to DIMM_LEDS banks.

**Current approach**: `forward_led_to_usb()` sends bytes 0-30 to bank 0 (start_index=0x00) and bytes 30-56 to bank 1 (start_index=0x1e). This is a direct passthrough.

**Risk**: `mk1_remap_led_payload()` in `mk1_device.c` has an existing 32-entry remap table that maps logical LED indices to hardware indices. The IPC LED payload may use logical ordering (matching what NIHA sends) while the hardware expects remapped ordering. May need to apply `mk1_remap_led_payload()` before sending to USB.

### Step 6: Phase 2 -- Proxy Mode [CONFIDENCE: N/A -- future]

Forward non-MK1 devices to stock NIHA so user keeps other NI hardware working.

---

## Key Files

| File | Purpose |
|------|---------|
| `mk1-bridge/mk1_server.c` | NIHA impersonation server (main protocol logic) |
| `mk1-bridge/main.c` | Bridge daemon, USB callbacks, command dispatch, LED/display forwarding |
| `mk1-ipc/mk1_ipc.h` | Protocol constants (all msg types, tags) |
| `mk1-ipc/mk1_ipc.c` | IPC client library |
| `mk1-ipc-sniffer/mk1_ipc_sniffer.c` | Passive DYLD interpose sniffer |
| `memory/protocol.md` | **THE** protocol reference -- all formats confirmed by sniffer |
| `mk1-usb/mk1_device.c` | IOKit USB layer (~2000 lines, device open, HID read, LED/display write) |
| `mk1-usb/mk1_device.h` | USB types (mk1_pad_event_t, mk1_button_event_t, callbacks) |
| `mk1-shim/mk1_shim.c` | IOKit shim for Apple Silicon |

## Cross-Reference

| Topic | Source |
|-------|--------|
| Full protocol decode (sniffer-confirmed) | `memory/protocol.md` |
| Protocol constants (C #defines) | `mk1-ipc/mk1_ipc.h` |
| Sniffer methodology | `mk1-ipc-sniffer/mk1_ipc_sniffer.c` |
| Previous BTN_DATA analysis (pre-sniffer) | `docs/niha-ipc-input-findings.md` |
| Previous PAD_DATA analysis (pre-sniffer) | `docs/frida-trace-findings.md` |
| LED map | `HANDOFF.md` |

## Technical Notes

### What Is Sniffer-Confirmed vs Speculative

| Item | Status |
|------|--------|
| Full 12-step handshake | **CONFIRMED** by sniffer capture |
| BTN_DATA format (24 bytes) | **CONFIRMED** by sniffer |
| PAD_DATA format (28 bytes) | **CONFIRMED** by sniffer |
| KNOB_ROTATE format (24 bytes) | **CONFIRMED** by sniffer |
| LED command format (65 bytes) | **CONFIRMED** by sniffer |
| DISPLAY command format (10900 bytes) | **CONFIRMED** by sniffer |
| Button indices 0x20-0x27 = top row | **CONFIRMED** by sniffer |
| Button index 0x1D | **CONFIRMED** by sniffer (which button TBD) |
| btnIndex = USB HID bit position | **SPECULATIVE** -- inferred from top row matching |
| IPC LED payload -> DIMM_LEDS direct copy | **SPECULATIVE** -- may need remap |
| Display chunk size 0x1fc | **SPECULATIVE** -- based on mk1_device.c existing code |
| Knob HID report byte layout | **UNKNOWN** -- need USB captures or trial-and-error |

### USB Layer Details (mk1_device.c)
- Uses IOKit USB directly (IOUSBDeviceInterface, IOUSBInterfaceInterface)
- EP1 (endpoint 1): command/control channel -- DIMM_LEDS, GET_DEVICE_INFO, AUTO_MSG
- EP8 (endpoint 8): display/audio channel -- pixel data, LED individual writes
- DIMM_LEDS format: `[0x0c, start_index, 31_brightness_bytes]` (33 bytes per bank)
- Bank 0: start_index=0x00 (LEDs 0-30), Bank 1: start_index=0x1e (LEDs 30-60)
- Display format: `[display_index, len_hi, len_lo, pixel_data...]` (max 0x1fc per chunk)
- `mk1_device_init_hardware()` sends caiaq init sequence required before device responds
- `mk1_device_start()` spawns background thread, calls pad_cb and button_cb
- **NO knob callback** -- only pad and button callbacks exist

### Event Sequence Counter
- `g_event_seq` (global uint32_t in main.c): monotonic counter shared by pad and button events
- Incremented per individual event (not per USB report)
- Matches sniffer observation of incrementing sequence numbers in BTN_DATA/PAD_DATA
