# mk1-trace

`mk1-trace` is a small macOS command-line capture tool intended to run on an Intel Mac with a working original Native Instruments stack.

It connects to the real `NIHardwareAgent` bootstrap port (`NIHWMainHandler`), performs the MK1-style handshake, and logs:

- bootstrap request/reply traffic
- request-port ACK traffic
- notification-port events from NIHA
- raw bytes, uint32 words, ASCII previews, and timestamps

It is meant to collect the kind of evidence Sam Lerner used in `NIProtocol`, but for the original Maschine MK1.
It is also the primary tool for capturing the remaining missing button mapping data; Frida is no longer needed for that step.

## Files

- `main.c`: self-contained trace client
- `messages.bin`: binary session capture format written at runtime
- `session.log`: human-readable transcript written at runtime

## Add To Xcode

Add a new macOS `Command Line Tool` target named `mk1-trace` and point it at `mk1-trace/main.c`.

Recommended target settings:

- Language: `C`
- Deployment Target: `macOS 12.0`
- Architectures: `x86_64`
- Header Search Paths: repo root so `../mk1-ipc/mk1_ipc.h` resolves cleanly
- Enable Modules: `Yes`
- Other C Flags: `-fblocks`

Link frameworks:

- `CoreFoundation.framework`
- `Foundation.framework`

## Usage

Run on the Intel Mac while the real `NIHardwareAgent` is running:

```bash
./mk1-trace
```

Useful options:

```bash
./mk1-trace --query
./mk1-trace --duration 20
./mk1-trace --no-second-stage
./mk1-trace --serial SN-EXAMPLE
```

Recommended command for button mapping:

```bash
./mk1-trace --duration 120
```

If the device was already powered on before `mk1-trace` attached, a fresh
`DEVICE_ON` notification may never arrive. In that case, provide the known
device serial so the tracer can force the second-stage `SERIAL_CONNECT`
without waiting for `DEVICE_ON`:

```bash
./mk1-trace --duration 120 --serial SN-EXAMPLE
```

## Output

Each run creates a directory like:

```text
mk1-trace-session-YYYYMMDD-HHMMSS/
```

Inside:

- `session.log`: human-readable transcript
- `messages.bin`: compact binary record stream

`session.log` is the file you want for button mapping. It logs every notification as:

- message direction and port
- raw hex bytes
- decoded 32-bit little-endian words
- message type constant from `mk1-ipc/mk1_ipc.h`

There is also a helper to extract button records directly:

```bash
python3 mk1-trace/summarize_session.py path/to/session.log
python3 mk1-trace/summarize_session.py --press-only path/to/session.log
```

The helper prints one summarized line per `BTN_DATA` event plus the decoded
`control_index` / `is_pressed` pairs from that event.

## What To Capture

Use the tool on Intel Monterey and then, while it is running:

1. Launch Maschine software.
2. Let the device handshake complete.
3. Press a few known buttons one at a time.
4. Hit pads one at a time at different velocities.
5. Turn encoders and press transport controls.

That should give you the real MK1 NIHA event payloads needed to compare against your Apple Silicon bridge.

For the immediate bridge task, use a stricter button-only workflow:

1. Start `mk1-trace` with `--duration 120`.
2. Launch Maschine and wait for the handshake to settle.
3. Press one physical button at a time in a fixed order.
4. Release before pressing the next button.
5. Write down the exact physical order you used.
6. Save the resulting `session.log`.

If the first run only contains handshake traffic, retry with `--serial` and
watch stderr. The tracer now prints explicit diagnostics when:

- the local notification port could not be created
- the device notification ACK completed
- no notifications arrive within 3 seconds of the ACK

That should distinguish "the tracer never registered a callback port" from
"NIHardwareAgent accepted the handshake but stayed silent".

The target output for each button press is the `BTN_DATA` event payload, from which we need:

- `control_index`
- `is_pressed`
- enough context to map the event back to the physical button you pressed

Until that table exists, the Apple Silicon bridge should not send guessed button `control_index` values.

## Consolidated Learnings

These points combine the repo's Ghidra notes and the verified Frida findings:

- `mk1-trace` is the correct tool for button mapping because it captures real outbound `NIHardwareAgent` notification traffic with no Frida dependency.
- `PAD_DATA` is byte-level confirmed by Frida as:

```c
uint32_t msg_type;       // 0x03504E00
uint32_t timestamp_hi;
uint32_t timestamp_lo;
uint32_t count;          // usually 1 in captures so far
uint32_t pad_index;      // 0-based
uint32_t event_type;     // observed: 1=hit_on, 3=hit_off, 4=pressure
float    value;          // normalized 0.0-1.0
```

- `KNOB_ROTATE` is a confirmed 24-byte payload with an analog-style signed float delta.
- The April 5, 2026 IPC sniffer capture emitted repeated `0x03654e00` notifications during
  handshake/UI initialization with no known user control movement, so treat the event as
  encoder-style traffic that may include idle noise rather than as confirmed user knob turns.
- The timestamp fields observed in Frida traffic are consistent with a 64-bit monotonic nanosecond clock split high word first.
- `BTN_DATA` framing is currently understood as:

```c
uint32_t word0;          // 0x0000000c
uint32_t word1;          // 0x00000001
uint32_t word2;          // 0x00000000
uint32_t msg_type;       // 0x03734e00
uint32_t timestamp_hi;
uint32_t timestamp_lo;
uint32_t payload_size;   // 0x18 + (N * 8)
// N records of:
uint32_t control_index;
uint32_t is_pressed;
```

- What remains unknown is the MK1-specific mapping from raw hardware button bits to NIHA logical `control_index` values.
- Frida remains useful later for remote port names and reply payloads, but that is a lower-priority follow-up task and not required for the button mapping capture.

## Notes

- The tool is passive apart from performing the handshake and optional post-handshake queries.
- It does not try to replace `NIHardwareAgent`.
- Serial extraction from `DEVICE_ON` is heuristic and logged; verify it against the raw transcript.
- If a capture contains only 6 short records ending at `ACK_NOTIF_PORT`, the
  tracer did not receive any notification-port traffic. `messages.bin` will
  not contain hidden button events in that case; it is just the binary form of
  the same short handshake.
