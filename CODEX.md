# maschine-mk1-revive — CODEX.md

This file is the Codex view of project state as of 2026-04-05.

It is intentionally narrower than `CLAUDE.md`. It separates:
- verified facts from captures, code, or repeated runtime evidence
- strong but still provisional conclusions
- current risks and preferred next steps

## Bottom line

Do not restart the project yet.

The repo already contains real value:
- working or near-working NIHA/Maschine handshake work in `mk1-bridge/`
- real Apple Silicon USB interface and endpoint enumeration in `mk1-usb/`
- a second viable integration path in `mk1-shim/`

The immediate problem is not lack of progress. It is lack of scope control.

## What I agree with from `CLAUDE.md`

- VID/PID and serial are established: `0x17cc` / `0x0808`, serial `SN-buscwvye`.
- EP1 is the control path for LED and related command traffic.
- EP8 is the display path.
- The display/controller work points to an ST7529-family device, not SSD1327.
- The IPC handshake implemented in `mk1-bridge/mk1_server.c` is meaningful progress and should be preserved.
- Raw USB reports must not be forwarded as final IPC button/pad payloads.
- `mk1-bridge/main.c` currently has a real regression: `forward_led()` is stubbed.

## What I do not fully agree with

### 1. The bridge is not yet proven to be the single correct architecture

`CLAUDE.md` treats `mk1-bridge` as the primary architecture and `mk1-shim` as reference-only.
I do not think that is settled.

Current repo evidence says:
- `mk1-bridge` is stronger on handshake and protocol control
- `mk1-shim` is closer to the original NIHA output model because it reuses NIHA and fakes the missing user client

For output bring-up, the shim path may still be lower risk. For long-term ownership, the bridge may be better. Those are different questions.

### 2. Some “authoritative” USB/display claims are still stronger than the evidence we have in-tree

The following look plausible, but I would not label them all authoritative without the supporting capture or decompile attached directly to the claim:
- exact `DISPLAY_COMMAND_LONG` scalar semantics
- exact IPC display header layout consumed by `forward_display`
- exact meaning of all selector-17 call clusters

### 3. The shim is not merely reference if it is the cleanest recovery path

If the goal is to get LEDs and LCDs working again fast, a temporary shim-first output path is a valid engineering decision. It should not be dismissed for ideological reasons.

## Verified current code facts

### Bridge

- `mk1-bridge/main.c` currently drops LED commands because `forward_led()` is a TODO.
- `mk1-bridge/main.c` contains speculative pad/button forwarding formats and should not be treated as final protocol truth.
- `mk1-bridge/mk1_server.c` contains the strongest current handshake implementation in the repo.

### USB

- `mk1-usb/mk1_device.c` now opens a real USB device/interface path on Apple Silicon and enumerates endpoint pipes.
- `mk1-usb/mk1_device.c` contains EP1 write support, EP8 write support, init sequencing, and EP1 reply draining logic.
- `mk1-usb/mk1_device.c` also contains a fake NIUSB user-client surface. That broadens the module beyond pure transport.

### Shim

- `mk1-shim/mk1_shim.c` is no longer just a logger. It now implements a meaningful fake service/connect/method surface.
- The shim duplicates selector behavior that also exists in `mk1-usb/mk1_device.c`. That duplication is a maintenance risk.

## Current risks

- Output logic is split across bridge, shim, and USB layers.
- LED regression is easy to explain, but LCD correctness is still partly assumption-driven.
- Input forwarding remains unsafe until button mapping and event formatting are fully confirmed.
- The repo is carrying two architectures without a clean ownership boundary.

## Recommended working position

Use this project. Do not start over.

But treat the next phase as a stabilization pass:
1. Recover the last known-good output path.
2. Pick one primary architecture for active work.
3. Demote the other path to reference/experimental until output is stable.
4. Do not resume button/pad feature work until LED and LCD output are repeatable.

## Preferred next-step priorities

1. Restore LED forwarding in `mk1-bridge/main.c` or explicitly pause bridge output work and use shim output as the active path.
2. Verify the LCD command/header assumptions against a known-good capture before more display code changes.
3. Consolidate selector/output translation logic so it exists in one canonical implementation.
4. Keep handshake code, USB transport code, and selector emulation conceptually separate.

## Files to trust most

- `mk1-bridge/mk1_server.c`
- `mk1-usb/mk1_device.c`
- `HANDOFF.md`
- `docs/session-notes.md`
- `docs/PLAN.md`

## Files to read with caution

- `mk1-bridge/main.c`
  Current output/input forwarding logic mixes verified facts with provisional assumptions.
- `CLAUDE.md`
  Good high-signal summary, but some sections overstate certainty and architecture choice.
