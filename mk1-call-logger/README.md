# mk1-call-logger

`mk1-call-logger` is a separate DYLD interposition logger for the
`NIHardwareAgent -> kext/user-client` boundary.

It is intentionally log-only. It does not fake services, emulate USB, or
modify return values. The goal is to capture the real IOKit behavior of the
original Intel/Monterey stack.

## What It Hooks

- `IOServiceMatching`
- `IOServiceGetMatchingServices`
- `IOIteratorNext`
- `IOObjectRelease`
- `IORegistryEntryCreateCFProperties`
- `IOServiceOpen`
- `IOServiceClose`
- `IOConnectMapMemory64`
- `IOConnectCallMethod`
- `IOConnectCallScalarMethod`
- `IOConnectCallStructMethod`
- `IOConnectCallAsyncMethod`
- `IOConnectCallAsyncScalarMethod`
- `IOConnectCallAsyncStructMethod`
- `io_connect_method_scalarI_scalarO`
- `io_connect_method_scalarI_structureO`
- `io_connect_method_scalarI_structureI`
- `io_connect_method_structureI_structureO`
- `io_async_method_scalarI_scalarO`
- `io_async_method_scalarI_structureO`
- `io_async_method_scalarI_structureI`
- `io_async_method_structureI_structureO`
- `IOHIDManagerCreate`
- `IOHIDManagerOpen`
- `IOHIDManagerClose`
- `IOHIDManagerSetDeviceMatching`
- `IOHIDManagerSetDeviceMatchingMultiple`
- `IOHIDManagerCopyDevices`
- `IOHIDManagerRegisterDeviceMatchingCallback`
- `IOHIDManagerRegisterInputValueCallback`
- `IOHIDManagerRegisterInputReportCallback`
- `IOHIDDeviceCreate`
- `IOHIDDeviceOpen`
- `IOHIDDeviceClose`
- `IOHIDDeviceGetProperty`
- `IOHIDDeviceRegisterInputValueCallback`
- `IOHIDDeviceRegisterInputReportCallback`
- `IOHIDDeviceSetReport`
- `IOHIDDeviceGetReport`

## Log Output

Set `MK1_CALL_LOGGER_PATH` to control the output file. Default:

```text
/tmp/mk1-call-logger.log
```

## Usage

Run `NIHardwareAgent` with the logger injected:

```bash
export MK1_CALL_LOGGER_PATH=/tmp/mk1-call-logger.log
export DYLD_INSERT_LIBRARIES=/path/to/libmk1-call-logger.dylib
export DYLD_FORCE_FLAT_NAMESPACE=1
/path/to/NIHardwareAgent
```

Or use the included launcher:

```bash
zsh /path/to/mk1-call-logger/run-niha-with-logger.sh /path/to/NIHardwareAgent /tmp/mk1-call-logger.log
```

If you omit the agent path, the launcher will try a couple of common install
locations automatically.

Then launch Maschine and exercise:

1. app startup
2. device discovery
3. handshake
4. LED/display output
5. pad/button/encoder activity

## Notes

- This is the right layer for sniffing `NIHardwareAgent -> kext` behavior.
- `mk1-trace` remains the right layer for `Maschine.app -> NIHardwareAgent` IPC.
