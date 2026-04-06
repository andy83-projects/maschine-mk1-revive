# NIHardwareAgent IPC Input Findings

Source: `/tmp/ghidra-out/niha_decompiled.c`

This note records the current reverse-engineering results for the input event payloads sent from `NIHardwareAgent` to clients over the notification port.

## High-confidence findings

- NIHA does not forward raw USB reports for buttons or pads.
- Input is converted into `Message` objects and then sent through `CFMessagePortSendRequest`.
- The send path is:
  - `NI::NHL2::SERVER::IPCConnection::sendMessage`
  - `NI::NHL2::MessageSendPort::sendMessageToConnectionPort`
  - `NI::NHL2::IPCPort::OSImpl::sendMessage`
  - `_CFMessagePortSendRequest`
- `IPCConnection::sendMessage` sends bytes starting at `Message + 0x0c`, not the raw in-memory object header before that.

## Button event format

Relevant functions:

- `NI::NHL2::SERVER::ControllerBase::processButtons`
- `NI::NHL2::MsgControllerChanged<NI::NHL2::SwitchEvent,128u>::MsgControllerChanged`

Observed behavior:

- Event type is `0x03734e00` (`BTN_DATA`).
- NIHA keeps internal button state and only emits changed controls.
- Each changed button becomes one compact record, not a 64-byte raw report.

Inferred payload layout:

```c
struct ButtonEventRecord {
    uint32_t control_index;
    uint32_t is_pressed;
};
```

Inferred message shape:

```c
struct ButtonEventMessage {
    uint32_t msg_type;       // 0x03734e00
    uint32_t timestamp_hi;
    uint32_t timestamp_lo;
    uint32_t count;
    struct ButtonEventRecord records[count];
};
```

Evidence:

- `processButtons` toggles internal bitfields and appends changed buttons only.
- It writes `control_index` and a boolean pressed state into 8-byte slots.
- It updates the message length incrementally as records are appended.

## Pad event format

Relevant functions:

- `NI::NHL2::SERVER::PadControlImplementation::processPads`
- `NI::NHL2::SERVER::PadControl::processPadData<2u>`
- `NI::NHL2::SERVER::PadControl::processPadData<3u>`
- `NI::NHL2::MsgControllerChanged<NI::NHL2::PadEvent,32u>::MsgControllerChanged`

Observed behavior:

- Event type is `0x03504e00` (`PAD_DATA`).
- NIHA converts raw pad scans into normalized pad events.
- It emits typed semantic events, not one pressure value per pad.
- Each emitted record is 12 bytes.

Inferred payload layout:

```c
struct PadEventRecord {
    uint32_t pad_index;
    uint32_t event_type;
    float value;
};
```

Inferred message shape:

```c
struct PadEventMessage {
    uint32_t msg_type;       // 0x03504e00
    uint32_t timestamp_hi;
    uint32_t timestamp_lo;
    uint32_t count;
    struct PadEventRecord records[count];
};
```

Observed `event_type` values:

- `0`: pad switch on
- `1`: pad hit on
- `2`: pad switch off
- `3`: pad hit off
- `4`: pressure update

Observed `value` semantics:

- For `0` and `1`, `value` is a normalized strike/activation strength.
- For `2` and `3`, `value` is zero.
- For `4`, `value` is normalized pressure.

## Important implication for this repo

The old bridge logic in `mk1-bridge/main.c` that did:

- `PAD_DATA = [msg_type][raw mk1_pad_event_t array]`
- `BTN_DATA = [msg_type][raw 64-byte button report]`

is structurally wrong.

To match NIHA, the bridge needs encoders that:

- track previous button state and emit only changed buttons
- decode the MK1 pad report stream into semantic pad events
- build compact `records[count]` payloads
- include timestamp words in the message header

## Confidence / caveats

- High confidence:
  - buttons are `count + array of {index, pressed}`
  - pads are `count + array of {index, event_type, float}`
  - raw USB memcpy payloads are wrong
- Medium confidence:
  - the exact placement and meaning of the two timestamp words
  - whether `count` is explicitly read by the receiver or only implied by message length
- Low confidence:
  - exact timestamp source expected by Maschine for MK1 compatibility

## Next implementation target

Bridge work should start with buttons first, because the format is simpler:

- decode MK1 button reports into per-control pressed bits
- emit `BTN_DATA` records only for changes

Pads should follow after that:

- derive `pad_index`, `event_type`, and normalized `value`
- emit a compact `PAD_DATA` record array instead of raw per-pad pressure dumps
