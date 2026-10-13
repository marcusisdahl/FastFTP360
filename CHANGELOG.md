# Changes

## 0.2.0

- Optional full data check: the receiver compares every timed byte with the pattern (PC "Data check: All data").
- Parallel TCP streams: the PC can run 1, 2 or 4 connections at once; the Xbox serves each on its own thread and shows their combined rate. Streams start their timed phase together.
- PC "Windows automatic" socket buffers keep receive-window autotuning on.
- Xbox D-pad settings while stopped: listener socket buffers (startup 64 KiB, 128 KiB, 256 KiB) and the hardware thread for stream workers.
- CSV adds `streams` and `verified_bytes`.

Protocol v1 stays compatible for existing settings. The new data check, automatic PC buffers and parallel streams need the v0.2.0 Xbox XEX.

## 0.1.2

- Avoid receive-buffer setup error 10022 by keeping accepted sockets' buffers instead of resizing them; report their actual sizes.
- Set PC buffer preferences before connecting, and clarify that the PC selector does not resize Xbox buffers. Xbox requests 64 KiB defaults at startup.
- Apply port reuse before binding the listener; report numeric socket creation/bind/listen/wait errors instead of the generic port failure.
- Test both transfer directions with buffer changes rejected, all PC buffer sizes and metadata, and immediate listener restart on the same host port.

The previous Windows clients remain compatible with the unchanged wire protocol. Console retesting is required.

## 0.1.1

- Fix `LAN socket options failed` after the Xbox accepts a connection: configure LAN options on the listener before bind/listen and avoid resetting them on accepted sockets.
- Include the failing operation and numeric code in Xbox socket setup errors.
- Identify the transfer phase and received byte count when the PC sees an early close.
- Display the version in both interfaces.
- Add regression tests for both directions with accepted-socket LAN-option changes rejected.

Console retesting is still required. The wire protocol and CUsbEnet driver are unchanged.

## 0.1

Initial Xbox and Windows interfaces, RAM transfers, integrity smoke test, warm-up, repeated tests, receiver results and CSV export.
