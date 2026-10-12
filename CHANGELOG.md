# Changes

## 0.1.3 diagnostic

- Respond to a reported whole-console freeze during unlimited Xbox-to-PC testing; the cause remains unknown.
- Split sends into at most 16 KiB calls; yield successful C++ sends and sleep before would-block retries.
- Negotiate an Xbox send cap covering integrity, warm-up and timed data; default to 16 MiB/s. Mark caps in the UI/CSV and keep medians separate.
- Optionally pause Xbox drawing after GPU-idle acknowledgement while still polling the controller. PC progress remains live.
- Default to one 10-second Xbox-to-PC run. Add cap, diagnostic metadata, slow-receiver and forced would-block tests.
- Use protocol v2: **both applications must be updated**. Old v1 peers fail promptly.

This is an unvalidated diagnostic revision, not a proven crash fix. Use the known-working Baseline driver for the first control. See the crash investigation notes.

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
