# FTP server fixes (unreleased)

`tests/ftp.py` has 45 checks, 16 of them new: long names and paths, over-long command lines, `ABOR` during uploads and downloads, a failed overwrite keeping the old file, an upload cancelled by hanging up, a data connection from a second address (127.0.0.2) being refused, and the `PASV` address coming from the control connection. They passed on Linux (server built with `tests/ftp_posix.h`, three runs in a row). The same tests against the previous `ftp.cpp` stop at the first long name: the server process ends, as the Xbox app would. The Windows host build, the Xbox build and console runs are **not yet verified**.

# v0.2.0 validation

`tests/protocol.py` now has 53 checks. On top of the v0.1.3 checks, new ones cover the full data check in both directions (every timed byte counted), a single flipped timed byte being rejected, two parallel streams on one listener, the automatic PC buffer setting in both directions, rejection of unknown direction-field flags, and prompt rejection of protocol v2 (app 0.1.3) peers.

These were run on a Linux loopback stand-in (POSIX socket shim for the C++ session, Mono for the C# engine) because no Windows host was available: all 53 passed. A four-stream run of the Windows app's batch logic, with the full data check, passed in both directions; with an 8 MiB/s Xbox send limit the four streams together stayed under 32 MiB/s. The Windows and Xbox builds, the Xbox multi-stream screen, D-pad settings, worker pinning, paused drawing with several streams, and XNet's response to larger listener buffers are **not yet verified**.

# v0.1.3 diagnostic validation

Built on Windows using VS 2022 Community with installed RXDK-360 integration, Xbox PowerPC compiler 16.00.11886.00, and .NET Framework C# compiler. Xbox build succeeds with `DEVKIT_ONLY=0`. Deployment is excluded in the project.

Build reports three warnings: MSB8003 for the integration's missing WindowsSDKDir property, and two C4127 constant-condition warnings from SDK `FD_SET` macro expansions. There are no compile or link errors. These warnings do not substitute for a hardware run.

`tests/protocol.py` passes 44 checks:

- Cross-language transfers in both directions with equal sender/receiver payload totals and final receiver rates.
- Both directions also pass with a socket shim that rejects LAN and buffer changes on accepted connections, reproducing the operations that failed on the user's console. This is a host regression test, not a replacement for retail XNet validation.
- All four PC buffer settings work in both directions with fixed server buffers; actual buffer metadata is checked.
- A listener can finish a transfer, stop, and immediately restart on the same port under the host socket implementation.
- Xbox send caps 8/16 MiB/s and both drawing flag values are acknowledged and reported; rate bounds are checked. Xbox caps do not apply to PC upload.
- Cancellation while pacing does not hang.
- Old protocol v1 headers and invalid diagnostic options/limits are rejected promptly.
- A slow receiver completes with matching byte totals, including under a shim that forces would-block sends even when select reports writable. These tests exercise explicit retry sleeps; they cannot prove that CPU starvation caused the hardware freeze.
- Cancellation discards an unfinished run; the next connection succeeds.
- Nine invalid request cases rejected (magic, version, direction, durations, chunk, buffer, warm-up bounds).
- Fragmented request accepted; corrupt integrity data rejected.
- Clean disconnect without a finish record rejected; truncated request rejected.
- Forged receiver result rejected by both engines.
- Truncated response, wrong protocol and corrupt integrity data rejected by the Windows client; truncated negotiation identifies the phase and received byte count.

The Xbox screen layout is rendered locally using the same layout source, without Xbox graphics calls. The Windows form is visually checked and exercised with the native loopback harness: six 20-second runs completed (three per direction), each transferring more than 4 GiB and exercising 64-bit counters. CSV export produced six rows; recomputed medians matched the screen. Empty IP validation and the Stop button were checked; the cancelled run was discarded while completed rows remained. Loopback rates are PC-only test data, not Xbox or USB performance results.

The v0.1.3 Windows GUI also completed one 10-second local capped run. Its result was marked CAP 16 and its CSV contained the acknowledged 16 MiB/s cap and drawing flag. The host's coarse sleep timing produced a rate below that limit, as allowed; it is not an Xbox performance result. This checks UI/CSV metadata and cannot validate actual Xbox GPU pausing.

The first hardware attempt with v0.1 launched the Xbox UI and accepted a PC connection, then displayed `LAN socket options failed`. A PC-side handshake probe received zero response bytes before the console closed the connection. The original code reapplied the listener's LAN options on the accepted socket. Version 0.1.1 applies them only before binding/listening and sets only nonblocking mode on accepted sockets. Xbox socket setup errors now name the operation and include the numeric error code; PC disconnect errors identify the phase.

The next v0.1.1 hardware attempt progressed past LAN setup and request parsing, then reported `RECEIVE BUFFER SETUP FAILED (10022)`. A later attempt reported the generic listen failure. Version 0.1.2 removes accepted-socket send/receive buffer resizing, retaining and querying the actual startup buffers. It applies SO_REUSEADDR before bind and distinguishes socket creation, bind, listen and wait errors with their numeric codes. The exact cause of the earlier generic listen failure is still unknown; no numeric bind/listen error was available in that build.

The v0.1.2 hardware run then completed PC-to-Xbox at 20.68 MiB/s but the console froze/restarted during Xbox-to-PC. The PC's last update showed 120.2 MiB at 5.8 seconds, followed by its 15-second idle timeout. The cause remains unknown. See [crash investigation](CRASH-INVESTIGATION.md) for the reviewed paths, diagnostic controls and next comparison.

**Still untested:** v0.1.3 transfers and paused-rendering synchronization on the actual console, listener restart, retail XNet half-close behavior, display modes beyond the initial launch, all controller/exit paths, route selection, CUsbEnet interaction, and sustained USB throughput. A successful build and host tests cannot establish hardware stability. This revision is not a proven crash fix.

Next console control: use the previously working Baseline driver, launch this app manually, and run Xbox-to-PC only for 10 seconds, one repeat, 64 KiB PC buffers, 16 MiB/s cap and paused drawing. Install both v0.1.3 applications. Do not configure the benchmark as a boot plugin. A capped result is not a maximum-throughput measurement.
