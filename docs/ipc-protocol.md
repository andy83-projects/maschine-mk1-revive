# NI IPC Protocol Notes

## Overview

NIHardwareAgent communicates with the Maschine software (and our bridge)
via `CFMessagePort` — a Mach-based IPC mechanism.

Port name: `NIHWMainHandler`

## Known References

### biappi/Macchina (MK1, 2012)
- Earliest known RE of this protocol, specifically with MK1 hardware
- Implemented MacchinaClient: connects to NIHA, waits for controller focus, reads messages
- Implemented MacchinaServer: impersonates NIHA, accepts connections from Maschine software
- Source: https://github.com/biappi/Macchina

### SamL98/NIProtocol (MK2, 2019)
- CFMessagePort with name "NIHWMainHandler" confirmed
- Handshake sequence partially documented
- nimessenger module handles message construction
- niparser module handles incoming packet parsing
- Source: https://github.com/SamL98/NIProtocol
- Article: https://lerner98.medium.com/rage-against-the-maschine-3357be1abc48

### terminar/rebellion (MK3/KK, ~2021)
- Most complete implementation
- Notes that USB data and IPC data share identical format in some cases
- Source: https://github.com/terminar/rebellion

## Handshake Sequence

TODO: Fill in after studying Macchina source and running capture on Intel Mac.

## Message Format

TODO: Fill in from NIProtocol / Macchina source analysis.

## Open Questions

- Has the IPC protocol changed between MK1-era NIHardwareAgent and current versions?
- Does current NIHardwareAgent (shipping with Maschine 2.x) still speak the same protocol?
- Does NIHardwareAgent even attempt to enumerate MK1 devices if the kext service is absent?
  (If it filters by device type at the IOKit layer we may need the shim regardless)
