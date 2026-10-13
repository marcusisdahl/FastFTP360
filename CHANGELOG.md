# Changes

## 0.3.0

- Built-in **FTP server** on the Xbox for comparing with Aurora's FTP. Press **LB** while stopped; **B** stops it. Port 21, passive mode (PASV/EPSV), any user name and password, up to 4 simultaneous connections (FileZilla's default works).
- Folders: `HDD`, `USB0`, `USB1` (only drives that can be read), `APP` (the app's own drive) and `NULL`. Uploads to `NULL` are discarded and `NULL/<n>MB.bin` downloads stream n MiB of test data, which measures FTP over the network with no disk involved.
- Disk and network overlap: a helper thread writes behind (uploads) or reads ahead (downloads) through two 1 MiB buffers. Data is sent in 16 KiB pieces with a yield, as in the benchmark.
- The Xbox screen shows the live FTP rate and the last transfer's average; the 226 reply also reports bytes, seconds and MiB/s to the client.
- Supported commands: USER PASS SYST FEAT OPTS TYPE MODE STRU NOOP PWD CWD CDUP PASV EPSV LIST NLST RETR STOR SIZE DELE MKD RMD RNFR RNTO REST 0 ABOR QUIT. Active mode (PORT) is refused.
- The same server source runs on Windows loopback in `tests/ftp.py` (21 checks with Python's ftplib, including path escapes and three simultaneous sessions).

Built and host-tested only. The FTP server has not run on a console yet.

## 0.2.1

- Xbox-only **disk test**, no network or PC needed. While stopped: **Y** picks the drive (the app's own drive, internal HDD, USB 0, USB 1), **RB** picks the block size (64 KiB, 256 KiB, 1 MiB), **X** runs it.
- It writes a 256 MiB file with normal cached writes and a timed final flush (like an FTP server), reads it back without the file cache, compares every byte, and deletes it. Write and read MiB/s show on the Xbox screen.
- HDD and USB drives are mounted for the test through a temporary kernel symbolic link (`NBDISK:`) and unlinked afterwards. The app drive uses `game:`.
- Network protocol and the Windows app are unchanged from 0.2.0.

Built and host-tested only. The disk test has not run on a console yet.

## 0.2.0

- Optional full data check: the receiver compares every timed byte with the pattern (PC "Data check: All data").
- Parallel TCP streams: the PC can run 1, 2 or 4 connections at once; the Xbox serves each on its own thread and shows their combined rate. Streams start their timed phase together. The Xbox send limit applies per stream.
- PC "Windows automatic" socket buffers keep receive-window autotuning on.
- Xbox D-pad settings while stopped: listener socket buffers (startup 64 KiB, 128 KiB, 256 KiB) and the hardware thread for stream workers.
- CSV adds `streams` and `verified_bytes`.
- Keeps every 0.1.3 diagnostic control (send limit, paused drawing, 16 KiB sends) and its defaults.

Protocol v3: **both applications must be updated**. The receiver result grows to 72 bytes to carry the checked byte count. Older peers fail promptly.

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
