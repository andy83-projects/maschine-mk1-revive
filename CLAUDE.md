# maschine-mk1-revive — CLAUDE.md

Reverse-engineering project to revive Native Instruments Maschine MK1 on modern macOS
without the original kext. All RE work is on a legitimately-owned device for interoperability.

## Project layout

| Directory | Purpose |
|-----------|---------|
| `mk1-usb/` | Core USB layer (IOKit bulk transfers, caiaq init, display, LEDs) |
| `mk1-ipc/` | CFMessagePort IPC layer (mimics NIHardwareAgent ↔ Maschine app) |
| `mk1-bridge/` | Bridge daemon: registers NIHWMainHandler, owns IPC + USB |
| `mk1-shim/` | DYLIB shim — reference only; has working LED selector dispatch |
| `mk1-ipc-test/` | Standalone IPC handshake smoke-test |
| `mk1-ipc-sniffer/` | Passive sniffer for CFMessagePort traffic |
| `mk1-trace/` | frida/dtrace trace tooling |
| `docs/` | Findings, implementation plan |
| `trace/` | Raw capture sessions |

## Architecture

The bridge impersonates NIHardwareAgent. Kill stock NIHA, run mk1-bridge. Maschine
software connects to our bridge via `NIHWMainHandler` CFMessagePort bootstrap port.

```
[Maschine.app] ←─CFMessagePort─→ [mk1-bridge] ←─IOKit USB─→ [MK1 hardware]
```

The shim (`mk1-shim`) is kept as reference. It intercepts NIHA's IOKit→KEXT calls
and its `mk1_shim_handle_set_leds` / `mk1_shim_remap_led_payload` are authoritative
for the LED IPC→EP1 payload translation.

## Device identity

- **VID**: `0x17CC`
- **PID**: `0x0808`
- **Serial**: `SN-buscwvye` (confirmed from hardware)
- **USB class in IORegistry (Apple Silicon)**: `IOUSBDevice` (child of `IOUSBHostDevice`)
  - Search `IOUSBDevice` first when using `IOCreatePlugInInterfaceForService`
  - `kIOUSBDeviceUserClientTypeID` does not work against `IOUSBHostDevice` directly

## Authoritative USB protocol facts (from usb.pcapng)

### Endpoint map (device 14, bus 20)
- **EP1 (0x01)** — bulk out, caiaq control commands (GET_DEVICE_INFO, AUTO_MSG, DIMM_LEDS, SET_THRESHOLDS, etc.)
- **EP4 (0x04/0x84)** — bulk in/out, high-bandwidth (audio/pad data)
- **EP8 (0x08)** — bulk out, **all display traffic only**

### EP4 pad input format (confirmed from live hardware testing 2026-04-06)
- 64-byte reports at ~700 Hz; no report-ID prefix byte
- Layout: `[pad0_lo, pad0_hi, pad1_lo, pad1_hi, ..., pad15_lo, pad15_hi, <32-byte duplicate channel B>]`
- Channel A = bytes 0–31: 16 × LE uint16 pressure values, one per pad
- Channel B = bytes 32–63: second ADC reading of same 16 pads (values track channel A; use A only)
- **Pad order is sequential**: pair 0 = pad 1 (idx 0), pair 15 = pad 16 (idx 15). No remap needed.
- Pad numbering: pad 1 = bottom-left, pad 16 = top-right (standard Maschine grid order, bottom-to-top)
- Baseline calibration: first report received contains resting ADC values per pad; subtract baseline
  before using as pressure. Resting values observed: pair 0 = 0x0000, pairs 1–15 = increments of 0x1000.
- Pressure range after baseline subtraction: 0–4095 (12-bit), normalize to 0.0–1.0 for IPC

### LED output (EP1 / selector 6)
- Command: `0x0c` (DIMM_LEDS) on **EP1**, never EP8
- Wire format: **offset-based**, two packets per full update:
  - `{ 0x0c, offset, led[0], led[1], ... }` — byte[1] is the start position in the device's LED array
  - **Packet A** (33 bytes): `{ 0x0c, 0x00, led[0..30] }` — device positions 0..30
  - **Packet B** (34 bytes): `{ 0x0c, 0x1e, led[0..32] }` — device positions 30..62
- `sendCommand` in the kext rejects payloads `>= 0x40` (64 bytes); both packets are well within limit
- LED stability requires a live EP1 command-reply loop (`commandReplyDispatch` in kext
  continuously re-arms EP1-in reads; without this, repeated writes may stall)
- **Note**: our bridge previously sent only Packet B with byte[1]=0x1e misidentified as a "control
  register". It is an offset. Packet A was missing, which is why pad rubber LEDs and scene-row
  buttons (Mute/Solo/Scene/Pattern/etc.) were never lit.

#### IPC → EP1 LED pipeline (confirmed from bridge logs 2026-04-06)
IPC `NI_CMD_LED` (0x036c7500) message layout:
```
bytes[0..3]  = msg_type (NI_CMD_LED)
bytes[4..7]  = led_len  (LE uint32, observed = 57)
bytes[8..39] = logical[0..31]  — button/group/transport brightness ← remap via hw_by_logical
bytes[40..43]= logical[32..35] — unused (other NI device slots)
bytes[44..59]= logical[36..51] — pad rubber LED brightness for pads 1–16
               logical[N+36] = pad N brightness (N=1..16, i.e. logical[37]=pad1, logical[52]=pad16)
```
Send **two EP1 packets** per LED update:
- **Packet B** (34 bytes, offset=0x1E): Apply `hw_by_logical` remap for logical[0..31] → groups/transport/display buttons
- **Packet A** (33 bytes, offset=0x00): Map logical[37+K] → slot `(3-K%4)+(K/4)*4` for pad rubber LEDs; scene-row button mapping TBD from learn pass

**Note**: The IPC payload offset changed between Maschine versions. Old Maschine (Intel/pcap era)
placed pad rubber data at logical[17..31] (within 32 bytes, so old 33-byte EP1 packets worked).
Current Maschine (Apple Silicon) places rubber data at logical[37..52] — confirmed from led.log
velocity-flash correlation: pressing pad 3 flashes both logical[3] (Group G button) and
logical[39] (pad 3 rubber) to 0x3f simultaneously.

#### Confirmed physical EP1 position → hardware (0-indexed)

**Packet A (offset=0x00) — device positions 0..30** (confirmed 2026-04-12 via offset=0x00 probe):

| pos | Hardware        | pos | Hardware        | pos | Hardware        |
|-----|-----------------|-----|-----------------|-----|-----------------|
|  0  | pad 4 rubber    |  8  | pad 12 rubber   | 16  | Mute            |
|  1  | pad 3 rubber    |  9  | pad 11 rubber   | 17  | Solo            |
|  2  | pad 2 rubber    | 10  | pad 10 rubber   | 18  | Select          |
|  3  | pad 1 rubber    | 11  | pad 9 rubber    | 19  | Duplicate       |
|  4  | pad 8 rubber    | 12  | pad 16 rubber   | 20  | Navigate        |
|  5  | pad 7 rubber    | 13  | pad 15 rubber   | 21  | Pad Mode        |
|  6  | pad 6 rubber    | 14  | pad 14 rubber   | 22  | Pattern         |
|  7  | pad 5 rubber    | 15  | pad 13 rubber   | 23  | Scene           |
|     |                 |     |                 | 24  | Shift           |
|     |                 |     |                 | 25  | Erase           |
|     |                 |     |                 | 26  | Grid            |
|     |                 |     |                 | 27  | Transport Left  |
|     |                 |     |                 | 28  | Record          |
|     |                 |     |                 | 29  | Play            |
|     |                 |     |                 | 30  | (unused)        |

**Packet B (offset=0x1E) — device positions 30..62** (data byte N → device position 30+N):

| data[N] | Hardware             | data[N] | Hardware             |
|---------|----------------------|---------|----------------------|
|  0      | (unused/dead)        | 11      | Auto Write           |
|  1      | dead / no LED        | 12      | Snap                 |
|  2      | Group G              | 13      | Modules Right        |
|  3      | Group H              | 14      | Modules Left         |
|  4      | Restart (transport)  | 15      | Sampling             |
|  5      | Left navigation      | 16      | Browse               |
|  6      | Group E              | 17      | Step                 |
|  7      | Group F              | 18      | Control              |
|  8      | Group C              | 19..26  | SA8..SA1 (disp btns) |
|  9      | Group D              | 27      | Note Repeat          |
| 10      | Skip                 | 27      | LCD backlight (0x5c) |

#### Logical IPC index → physical EP1 position
Button/group/transport LEDs via `hw_by_logical[32]` in `mk1-bridge/main.c`:
```
logical[0]  → phys[0]   (0x1e control reg)
logical[1]  → phys[4]   = Restart
logical[2]  → phys[3]   = Group H
logical[3]  → phys[2]   = Group G
logical[4]  → phys[1]   = dead/no LED
logical[5]  → phys[8]   = Group C
logical[6]  → phys[7]   = Group F
logical[7]  → phys[6]   = Group E
logical[8]  → phys[5]   = Left navigation
logical[9]  → phys[12]  = Group A
logical[10] → phys[11]  = Auto Write
logical[11] → phys[10]  = Skip
logical[12] → phys[9]   = Group D
logical[13] → phys[16]  = Modules Left
logical[14] → phys[15]  = Sampling
logical[15] → phys[14]  = Browse
logical[16] → phys[13]  = Group B
logical[17..31] → phys[17..31] (identity; unused — pad rubbers are at logical[37..52])
```
Pad rubber LEDs (direct, no remap table):
```
logical[37+K] → phys[17+K]  for K=0..15  (pad 1..16 rubber LEDs → phys[17..32])
```

#### LED brightness tiers (from Frida trace of real NIHA)
Firmware uses three discrete levels, not a linear 0–255 scale:
- `0x13` (19) — dim
- `0x32` (50) — medium
- `0x5c` (92) — bright

### Display hardware
- **Two displays**: left = index `0x00`, right = index `0x02`
- **Resolution**: 170 × 64 pixels
- **Pixel format**: 8 bits per pixel, grayscale (`0x00` = black, `0xff` = white)
- **Framebuffer size**: 10 880 bytes (170 × 64)
- **Controller family**: ST7529 (Epson/Seiko; NOT SSD1327)

### EP8 wire format
Every EP8 write is `[display_idx, len_hi, len_lo, payload...]`:
```
Byte 0  : display index  (0x00=left, 0x02=right; 0x01/0x03=continuation chunks)
Byte 1-2: payload length (big-endian uint16)
Byte 3+ : payload
```
Max payload per transfer: **508 bytes** (`0x1FC`).

### Display address window (set during init)
```
0x75, 0x00, 0x3f   → set row  address range: 0–63  (64 rows)
0x15, 0x00, 0x54   → set col  address range: 0–84  (85 col-addresses × 2px = 170px)
```

### Full display init sequence (17 commands, both displays identical)
Sent to display 0x00 first, then 0x02. Each command via `mk1_set_display()`:
```c
{ 1, { 0x30 } },              // enter extension set (SEC)
{ 4, { 0xca, 0x04, 0x0f, 0x00 } }, // duty / display lines
{ 2, { 0xbb, 0x00 } },        // COM scan direction
{ 1, { 0xd1 } },              // (power on sequence)
{ 1, { 0x94 } },              // sleep out
{ 3, { 0x81, 0x1e, 0x02 } },  // electronic volume (contrast = 0x1e)
{ 2, { 0x20, 0x08 } },        // power control
{ 2, { 0x20, 0x0b } },        // power control
{ 1, { 0xa6 } },              // normal display (non-inverted)
{ 1, { 0x31 } },              // exit extension set
{ 4, { 0x32, 0x00, 0x00, 0x05 } }, // scroll/scan config
{ 1, { 0x34 } },              // scroll off / display enable
{ 1, { 0x30 } },              // re-enter extension set
{ 4, { 0xbc, 0x00, 0x00, 0x02 } }, // data scan direction
{ 3, { 0x75, 0x00, 0x3f } },  // row address range: 0–63
{ 3, { 0x15, 0x00, 0x54 } },  // col address range: 0–84
{ 3, { 0x81, 0x20, 0x02 } },  // electronic volume final (contrast = 0x20)
```

### Framebuffer write
After init, set address window again, then send `0x5c` (RAMWR) + 10 880 pixel bytes, chunked:
```
First chunk : display_idx=0x00, len=508, payload=[0x5c, <507 pixels>]
Continuation: display_idx=0x01, len=508, payload=[<508 pixels>]
Last chunk  : display_idx=0x01, len=<remainder>
```
`0x5c` is the ST7529 "Write Display Data" (RAMWR) command — always the first byte of a
framebuffer transfer, never a pixel value.

## IOKit user-client selectors (confirmed from kext RE + Frida trace)

| Selector | Name | Notes |
|----------|------|-------|
| 3  | GET_DEVICE_INFO | returns 0x6e bytes; called twice at startup (before and after GET_DEVICE_SPEC) |
| 5  | SET_AUTO_MSG | enables spontaneous reports for buttons/pads/knobs |
| 6  | SET_LEDS | struct=32-byte brightness array (indices 0–31); EP1 cmd 0x0c |
| 7  | DISPLAY_COMMAND | scalars=[display_idx, len]; struct=payload ≤508 bytes; EP8 |
| 17 | DISPLAY_COMMAND_LONG | scalars=[display_idx, ?, total_len]; full framebuffer; auto-chunked. Scalar values observed during pad interaction: 0x3 and 0x141 (purpose TBD) |
| 18 | GET_DEVICE_SPEC | returns 0x0e bytes |
| 19 | READ_USER_DATA | returns 0x21 bytes |
| 20 | WRITE_USER_DATA | |
| 21 | DIGITAL_INPUT_ARM | arms async input reads |

### IOKit startup selector sequence (from Frida trace of real NIHA)
```
IOServiceOpen()
IOConnectCallMethod() selector 3  (GET_DEVICE_INFO)
IOConnectCallMethod() selector 18 (GET_DEVICE_SPEC)
IOConnectCallMethod() selector 3  (GET_DEVICE_INFO again)
```

### Per-pad-press IOKit cluster (from Frida trace)
Each pad press generates exactly:
- 2× selector 6 calls (LED update: first sparse, then full state)
- 3× selector 17 calls (scalar args 0x3 and 0x141; purpose TBD)

## IPC protocol (CFMessagePort)

### Full handshake sequence (confirmed from sniffer + session work)
```
TX NIHWMainHandler  msgid=0x03536756  GetServiceVersion (4 bytes)
RX                  msgid=0x00020802  version reply (8 bytes)
TX NIHWMainHandler  msgid=0x03447500  PID_CONNECT (20 bytes)
   payload: 0x00000808 + "2MiNymrp"
RX                  msgid=0x74727565  "true" + port names (63 bytes)
TX NIHWS08080051Request  msgid=0x03404300  ACK_NOTIF_PORT (42 bytes)
RX                  msgid=0x74727565  "true" (4 bytes)
── device phase complete; software sees device ──
TX NIHWS08080051Request  SERIAL_CONNECT
RX                  "true" + instance port names
TX inst_request     ACK (same format as ACK_NOTIF_PORT)
RX                  "true" → bridge pushes DEVSTATE_BOOL
TX inst_request     GETSERIAL → RX: [serial_len, padded_serial]
TX inst_request     START
TX inst_request     INSTANCE_NAME
── post-instance numeric events needed for controller to appear ──
push 0x03444e00
push 0x03434e00
```
Port name pattern: `NIHWS<VendorID><ProductID><HWIndex>` e.g. `NIHWS08080051`.

### IPC event message format (confirmed from Frida trace)

All input events share a common header:
```c
uint32_t msg_type;    // event type constant (see mk1_ipc.h)
uint32_t timestamp_hi; // upper 32 bits of 64-bit monotonic nanosecond clock
uint32_t timestamp_lo; // lower 32 bits
uint32_t count;       // number of records following
// records follow...
```

#### Pad event (NI_EVT_PAD_DATA = 0x03504E00) — 28 bytes for 1 record
```c
struct PadEventRecord {
    uint32_t pad_index;   // 0-indexed; 0x0c = pad 13
    uint32_t event_type;  // 1=hit_on, 3=hit_off, 4=pressure_update (0=switch_on, 2=switch_off expected)
    float    value;       // normalized 0.0–1.0; 0.0 on release
};
```
Float value examples: 0.424 (medium strike), 0.759 (hard strike), 0.292 (pressure).
NIHA emits separate hit_on + hit_off + pressure_update records, not raw USB pressure bytes.

#### Analog encoder-style event (NI_EVT_KNOB_ROTATE = 0x03654E00) — 24 bytes for 1 record
```c
struct KnobRecord {
    uint32_t encoder_index; // likely encoder/control index; observed stable per source
    float    delta;         // analog-style signed delta; may include idle jitter/noise
};
```
The 24-byte payload shape is confirmed, but the April 5, 2026 IPC sniffer capture
showed repeated 0x03654E00 notifications during idle startup/UI activity. Treat
this as an analog encoder-style event that may include noise, not as confirmed
user knob movement from that capture alone.

#### Button event (NI_EVT_BTN_DATA = 0x03734E00) — variable
```c
struct ButtonRecord {
    uint32_t control_index; // button index
    uint32_t is_pressed;    // 1=down, 0=up
};
```
NIHA tracks previous state and only emits changed buttons (not full state each time).

**Important**: do NOT forward raw 64-byte USB reports as IPC payloads. The above
semantically correct formats are required for Maschine software to process events.

## Build

```sh
xcodebuild -project maschine-mk1-revive.xcodeproj -scheme mk1-bridge -configuration Debug build
```

## Key source locations

- `mk1-usb/mk1_device.c` — USB logic; `mk1_device_init_hardware()` ~line 1371; `mk1_set_display()` ~line 1654; `mk1_set_led()` ~line 1642; `find_matching_usb_service()` ~line 891
- `mk1-usb/mk1_device.h` — display constants, selector enum
- `mk1-ipc/mk1_ipc.h` — all IPC message type constants
- `mk1-ipc/mk1_ipc.c` — CFMessagePort server/client
- `mk1-bridge/main.c` — bridge entry point; `forward_display` and `forward_led` implemented
- `mk1-bridge/mk1_server.c` — NIHA impersonation server (full handshake)
- `mk1-shim/mk1_shim.c` — reference: `mk1_shim_remap_led_payload` at ~line 400
- `docs/PLAN.md` — implementation roadmap with confidence levels

## Known open issue: display backlight toggles on button presses

Both displays simultaneously go dark for ~1 frame when screen-area or group buttons are
pressed (view transitions). Pixel content is preserved — only the physical backlight LED
toggles. Confirmed NOT caused by: `0xaf` re-assertion after RAMWR, all-black frame skipping,
phys[1] forcing in DIMM_LEDS, `0x30` before address window. See `docs/HANDOFF-2026-04-08.md`
for full diagnosis and leading hypotheses.

### EP8 steady-state frame write (confirmed from pcap)
Do NOT add `0x30` before the address window in `forward_display`. The real NIHA sends:
`0x75 0x00 0x3f`, `0x15 0x00 0x54`, then RAMWR — no `0x30` prefix for steady-state frames.
`0xaf` is sent after the FIRST RAMWR only (one-time), not on every frame.

## Do not

- `0xaf` (display ON) IS required in display init — without it output stays disabled. Send it at the end of the 18-command init sequence.
- Do not send `0x30` before address window in `forward_display` — display stays in extension mode between frames; adding `0x30` interferes (tested; reverted)
- Do not assume SSD1327 register layout — this is ST7529
- Do not write LED commands to EP8 — LEDs go on EP1 only
- Do not forward raw USB bytes as IPC pad/button payloads — use the semantic formats above
- Do not use `IOUSBHostDevice` as the service for `IOCreatePlugInInterfaceForService` — use `IOUSBDevice`
