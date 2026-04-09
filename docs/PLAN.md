# MK1 Revive — Implementation Plan

Last updated: 2026-04-05

## Consensus (settled 2026-04-05)

1. `mk1-bridge` is the primary path. `mk1-shim` is reference only — this is closed.
2. Immediate next task: run `mk1-ipc-sniffer` against real NIHA, capture a live
   `NI_CMD_DISPLAY` packet, verify display header offsets.
3. Fix `forward_display()` against the capture.
4. Only then do a minimal split (LED/display packet construction into shared helpers)
   if duplicated code is still causing regressions.

Do not restart. Do not broad-refactor. Do not touch architecture until step 3 is done.

---

## Architecture Decision: Bridge (not Shim)

The bridge (`mk1-bridge`) kills NIHardwareAgent, registers `NIHWMainHandler`, and owns
both the IPC side (Maschine software) and USB side (hardware). This is the right long-term
architecture.

The shim (`mk1-shim`) is kept as reference only. Its `mk1_shim_handle_set_leds` and
`mk1_shim_remap_led_payload` functions are authoritative for the LED IPC→EP1 translation.

**Confidence: 85%** — confirmed: NIHardwareAgent has a native arm64 binary; NIHIA (the
launchd agent, PID persists) launches NIHA via `open`. When the bridge registers
`NIHWMainHandler` first, NIHA cannot claim it. Maschine software successfully connects to
the bridge and sees the MK1 controller. Remaining 15%: NIHIA respawn behaviour under
prolonged use; launchd ordering for production deploy.

---

## Implementation Order

| Step | Area | Status | Confidence | Notes |
|------|------|--------|-----------|-------|
| 1 | LED forwarding (`forward_led`) | **completed** | 90% | Button, group, transport, and pad LEDs now light; remaining visual issue is display backlight flicker |
| 2 | USB hot-plug | not started | 72% | None |
| 3 | Capture IPC DISPLAY message format | not started | 45% | `IPC_DISPLAY_HDR_LEN = 16` unverified |
| 4 | LCD display forwarding (`forward_display`) | partial | 45% | Display init lights up; pixel forwarding unverified |
| 5 | Input report decode (buttons, pads, knobs) | not started | 38% | Codex/MLX: parse ep01.txt |
| 6 | Pad/button/knob IPC event emission | not started | 38% | Step 5 prerequisite |
| 7 | Shift+button combos | not started | 25% | Step 6 prerequisite |

---

## Step 1: LED Forwarding

Root causes of broken LEDs (resolved and outstanding):

1. `mk1_set_led` writing to EP8 instead of EP1 — **fixed**.
2. Wrong packet format — **fixed** (`{ 0x0c, brightness_bytes... }` on EP1, no start_index).
3. `forward_led()` stub — **implemented**. Remap table ported from `mk1_shim_remap_led_payload`.
4. IPC header offset wrong (skipped 4 bytes instead of 8) — **fixed**. Format confirmed:
   `[msg_type(4), led_len(4 LE), logical_brightness_array(led_len)]`, led_len observed = 57.
5. LED array cap: hardware expects 32 bytes max; we now cap at 32 — **fixed**.
6. Button LEDs confirmed working (play/record visible after hitting play in Maschine).
7. Pad LEDs: **unconfirmed**. Need EP1 log from new build to verify remap correctness.
   Pad brightness in IPC: 0x13 (dim) at rest; may be too faint to see. Watch for 0x3f/0x5c.
8. EP1 command-reply loop: **not implemented**. Kext re-armed EP1-in reads continuously;
   without this, repeated writes may stall under sustained LED traffic.

#### Confirmed IPC LED wire format
```
bytes[0..3]  = NI_CMD_LED (0x036c7500)
bytes[4..7]  = led_len (LE uint32, observed 57)
bytes[8..]   = logical brightness array (led_len bytes, indices 0–56)
```
Only first 32 bytes (indices 0–31) map to physical LEDs via `hw_by_logical` remap table.
Bytes 32–56 are ignored (cap at 32 before sending to EP1).

#### EP1 DIMM_LEDS wire format (confirmed from usb.pcapng)
```
[0x0c, phys[0], phys[1], ..., phys[31]]   — 33 bytes total, NO start_index byte
```
- `0x0c` = DIMM_LEDS command on EP1
- `phys[i]` = brightness of physical LED i after logical→physical remap
- Physical LED 0: real NIHA sends `0x1e` on full-state packets and `0x00` on sparse.
  Our bridge sends `0x13`. No visible effect confirmed, but may be a mode/sequence flag.

#### Pad LED visibility
- Pad rubber backlights are NOT visible at 0x13 (dim) under room lighting.
  Minimum visible brightness through rubber surface appears to be 0x32.
- Maschine sends 0x13 as idle baseline; pads only go to 0x32/0x3f/0x5c when
  a note is on that step and the sequencer is playing over it.
- **Test**: add a note to a pad in the step sequencer, hit play → that pad
  should stay at 0x32+ and be visible. Confirms LED forwarding is correct.
- Transport/mode button LEDs (phys[17-31]) ARE visible at 0x13 (surface mounted).
- "Group" buttons A-H adjacent to pads also visible at 0x13 (surface mounted).

## Step 2: USB Hot-Plug

- On device arrival: `IOServiceAddMatchingNotification` with `kIOFirstMatchNotification`
  → `mk1_device_open()` + caiaq init + send `DEVICE_ON` if client connected.
- On device removal: `kIOTerminatedNotification` → `mk1_device_stop()` +
  `mk1_device_close()` + send `DEVICE_OFF`.
- Notifications must share the bridge's main CFRunLoop.
- Mutex required around `g_bridge.usb` — USB writes happen on IPC callback thread.

## Step 3 + 4: Display

Run `mk1-ipc-sniffer` against real NIHA to capture a live `NI_CMD_DISPLAY` packet and
confirm the header layout (`display_index` at byte 0, `pixel_len` at bytes 12-15).
Current assumption in `forward_display` (`IPC_DISPLAY_HDR_LEN = 16`) is unverified.

## Step 5 + 6: Input Events

Bridge must emit semantically correct IPC event payloads — not raw USB bytes.
See `CLAUDE.md` for confirmed wire formats.

Key points:
- Buttons: track previous state, emit only changed controls.
- Pads: decode pressure values from EP4, emit `PadEventRecord` with float value.
- Knobs: emit signed float delta per encoder tick.
- All messages include 64-bit monotonic timestamp split as `{ts_hi, ts_lo}`.

## Step 7: Shift Combos

Unknown whether Maschine handles shift internally (receiving raw button state) or
expects NIHA to translate. Investigate once Step 6 is working.

---

## What MLX / Codex Can Do

These tasks are pure research and can run without session context:

1. **Parse `ep01.txt`** — extract exact byte layout of button, pad, and knob USB input
   reports from EP1. Output: offset table mapping bytes to field names/values.

2. **Decode `mk1_shim_remap_led_payload`** — document exactly what transformation it
   applies to the selector-6 IOKit struct to produce the EP1 DIMM_LEDS byte array.

3. **Validate display IPC header** — run `mk1-ipc-sniffer`, capture a DISPLAY packet,
   confirm or correct the 16-byte header layout assumed in `forward_display`.

---

## Confidence Key

Percentages reflect current knowledge state. Do not implement steps rated below 50%
without first completing the prerequisite capture/analysis work.
