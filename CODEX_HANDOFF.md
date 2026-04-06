# Maschine MK1 Bridge Handoff

This file captures the current state of the MK1 reverse-engineering work so a new Codex/Claude session can resume without replaying the entire investigation.

## Current Status

- `mk1-bridge` completes the NIHA bootstrap/device/instance handshake.
- Normal notification events succeed:
  - `DEVICE_ON`
  - `DEVSTATE_BOOL`
  - `SETFOCUS`
- The first live `BTN_DATA` event still causes Maschine to tear down both notification ports.
- `PAD_DATA` is still disabled.

## Files Involved

- `mk1-bridge/main.c`
- `mk1-bridge/mk1_server.c`
- `mk1-usb/mk1_device.c`
- `mk1-shim/mk1_shim.c`
- Decompiled NIHA reference used during this work:
  - `/tmp/ghidra-out/niha_decompiled.c`

## Important Code Changes Already Made

### 1. `BTN_DATA` packet framing in `mk1-bridge/main.c`

The bridge now emits `BTN_DATA` with the NIHA-style 0x1c-byte header:

- word 0: `0x0000000c`
- word 1: `0x00000001`
- word 2: `0x00000000`
- word 3: `NI_EVT_BTN_DATA` / `0x03734e00`
- word 4: timestamp hi
- word 5: timestamp lo
- word 6: trailing payload byte size

The last header word was fixed from raw record count to:

```c
header[6] = 0x18 + (record_count * sizeof(records[0]));
```

This matches the NIHA decompile:

```c
local_470[0] = local_458 * 8 + 0x18;
```

### 2. `mk1_ipc_button_record_t` layout

The record layout now matches NIHA's 8-byte switch record:

```c
typedef struct {
    uint32_t control_index;
    uint8_t  is_pressed;
    uint8_t  reserved0;
    uint8_t  reserved1;
    uint8_t  reserved2;
} mk1_ipc_button_record_t;
```

### 3. `mk1-bridge/mk1_server.c` push/reconnect logic

`push_to_port()` was patched to:

- derive the message id more defensively
- log target label on success/failure
- reconnect invalid device/instance notification ports by name
- retry once after reconnect

This is important because it proved the remote notification ports are valid before `BTN_DATA` and only become invalid after a live switch payload.

## Runtime Evidence

The key proving run was:

- `build/Debug/mk1-bridge-20260401-124440.log`

From that run:

- handshake succeeds
- logs show:
  - `push_event ok (device msgid=...)`
  - `push_event ok (instance msgid=...)`
- after first `BTN_DATA`, logs show:
  - `failed to reconnect instance port 'NIHWS08080002Notification'`
  - `instance target is unavailable`
  - `failed to reconnect device port 'NIHWS08080001Notification'`
  - `device target is unavailable`

Conclusion:

- the transport/message-port layer is no longer the main failure
- the `BTN_DATA` payload content is still malformed enough that Maschine abandons both ports

## Decompiled NIHA Findings

### 1. Generic switch builder

In `ControllerBase::processButtons`:

```c
*(int *)(auStack_450 + uVar3 * 8 + -4) = (int)uVar6;
auStack_450[uVar3 * 8] = (uVar5 & uVar7) != 0;
local_470[0] = local_458 * 8 + 0x18;
```

This confirms:

- each switch record is 8 bytes
- first 4 bytes are a logical switch/control index
- 5th byte is pressed state
- size word is `0x18 + N * 8`

### 2. Generic FX2 digital input path

`FX2Controller::processDigitalInputs(...)` simply calls:

```c
ControllerBase::processButtons(param_1, param_2);
```

So raw digital input reports are converted into NIHA switch events through device-managed logical state, not directly exposed as raw USB bit positions.

### 3. Device-specific mapping exists in NIHA

`RigControl2::processDigitalInputs(...)` uses a translation table:

```c
s_arr2to3
```

before writing the switch record's control id. This is strong evidence that at least some NI devices require a raw-bit-to-logical-control mapping layer.

### 4. Maschine MK1 consumes logical switch ids

`MaschineMK1::onSwitchEvents(...)` processes logical ids from the `MsgSwitchesChanged` payload at `0x1c + n * 8` and special-cases several values, including:

- `0x0b`
- `0x18`
- transport-related ids in the `<= 0x29` range

This confirms Maschine expects meaningful logical control ids, not arbitrary raw USB bit numbers.

## Most Important Diagnosis

`mk1-bridge/main.c` currently still does this:

```c
records[record_count].control_index = (uint32_t)bit;
```

That is almost certainly the remaining bug.

The bridge is still sending raw USB bit positions as NIHA logical switch ids.

That explains the behavior:

- packet shell is accepted
- normal non-switch events are accepted
- first `BTN_DATA` causes Maschine to tear down both notification ports

## What The Logs Also Show

The current raw USB button reports often produce ids like:

- `0x0c`
- `0x0d`
- `0x0e`
- `0x0f`
- `0x1c`
- `0x1d`
- `0x1e`
- `0x1f`
- `0x2c`
- `0x2d`
- `0x2e`
- `0x2f`
- `0x3c`
- `0x3d`
- `0x3e`
- `0x3f`
- etc.

These are consistent with raw bit positions from a matrix-style input report. They are not yet proven to be valid MK1 NIHA control ids.

## Startup LED / Screen Note

The missing startup animation is expected in the current bridge mode.

There is still a startup replay hook in `mk1-bridge/main.c`:

- `replay_startup_init_if_requested()`

It only runs if:

- `MK1_ENABLE_CAPTURE_REPLAY` is enabled

So the absence of the Intel-Mac-style startup LED/screen sequence during current bridge runs does not contradict the present diagnosis.

## Best Next Step

Do not spend more time on `mk1_server.c` first.

The next useful implementation step is in `mk1-bridge/main.c`:

1. Introduce an MK1-specific helper like:

```c
static bool mk1_map_button_bit_to_control(size_t bit, uint32_t *control_id);
```

2. Stop sending raw `bit` as `control_index`.

3. Suppress unknown mappings instead of forwarding them.

4. Log unmapped bits compactly so a future run can correlate:

- raw report bytes
- physical button pressed
- candidate logical id

## Conservative Strategy For Next Session

The safest plan is:

- keep the current `BTN_DATA` framing exactly as-is
- add a mapping/filter layer
- forward only well-justified controls
- suppress everything else

Even partial progress is useful if it avoids sending ids that make Maschine drop the session.

## Suggested Prompt For A New Session

Use this exact context:

> Read `CODEX_HANDOFF.md` first. Then inspect `mk1-bridge/main.c`, `/tmp/ghidra-out/niha_decompiled.c`, and the latest `BTN_DATA` path. The current problem is no longer packet framing; it is likely raw USB bit positions being sent as NIHA logical switch ids. Implement the safest MK1 button mapping/filter you can justify from the decompile and existing code, suppress unknown ids, and preserve the current server/reconnect logic.
