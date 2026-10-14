# Changes

## 0.5.0 (experimental)

**Experimental; ABadAvatar 1.3 / ABadUpdate compatibility not yet verified.** Built and host-tested; first console runs cover the NetISO protocol source only.

- New **Read test** tab for judging NetISO and ConnectX game loading: sources Internal HDD, USB 0, USB 1, App drive, **Game disc**, **NetISO protocol** and **ConnectX**; workloads sequential 64 KiB / 256 KiB / 1 MiB and random 4 / 16 / 64 KiB (4 KiB aligned, seeded); 10/30/60/120 s after a 3 s warm-up. Reports MiB/s, reads/s, latency mean/p50/p95/p99/max (percentiles approximate to ~3%), failed, short, timed-out reads and integrity failures, first pass versus repeats. Read only.
- **NetISO protocol** client written from tuxuser/netiso-srv's source: exact sends and receives, 10 s idle timeout, B cancels within 100 ms, ambiguous ISO names refused. It bypasses the console's NetISO plugin.
- **ConnectX** is used only if `connectx.xex` is already loaded and `connectx_folder=` is set; otherwise only that source is off.
- Mounted NetISO disc reads are not implemented: the plugin's device name could not be verified.
- **Game disc** reads one of the 64 largest files on the disc in the DVD drive (read-only), the baseline NetISO replaces.
- Known-data fixtures (`FFTPFIX-<seed>.*`) are checked byte by byte; other files only by byte count.
- Results in `FastFTP.log` and the new `FastFTP-reads.csv`; read settings in `FastFTP.ini` (`read_folder`, `connectx_folder`, `netiso_server`, `netiso_port`, `read_seed`). The FTP login lines are unchanged.
- Drawing pauses during reads, as during other transfers.
- Host tests: `tests/read.py` (56 checks) with `tests/netiso_server.py`, a NetISO test server with fault injection; `tests/posix_compat.h` builds the engine on Linux. Found and documented `netiso-srv` defects: unbuffered `try_write` cuts large replies, single-`read` request parsing, suffix-match mounting.
- Network test protocol unchanged (v3); `FastFTP-PC.exe` only shows the new version.


## 0.4.1

The first published release of the 0.4 work. The 0.4.0 tag pointed at an earlier commit, from before the rename to FastFTP and the new README, so 0.4.1 replaces it. Apart from the version number, the code is the same as the 0.4.0 files.

## 0.4.0

- **Your own FTP login**: press **Y** on the FTP server tab to type a user name and password with the Xbox keyboard (1-23 characters, no spaces). The login and the Login on/off setting are saved in `FastFTP.ini` next to the XEX, which can also be edited on a PC. Without the file the login is `xboxftp` / `xboxftp`. Four new checks in `tests/ftp.py`.
- The app opens on the **FTP server** tab, now the first tab.
- Renamed to **FastFTP** everywhere: `FastFTP.xex`, `FastFTP-PC.exe`, `FastFTP.sln`, the FTP greeting, the disk test's `FASTFTP-DISKTEST.TMP`, the screen header and the PC app. The PC app's hint about Xbox buffers points to the Settings tab.
- The running FTP server's folder list wraps instead of running off the card. The header shows a port only on the FTP and Network tabs.
- New logo, and a README rewritten with screenshots of every tab.
- New **Settings** tab for the advanced options: Xbox socket buffers, worker CPU and drawing during transfers. The FTP tab keeps only the login (on/off, user name and password shown for the FTP client); the Network tab shows what to enter in the PC app; the Disk tab keeps drive and block size.
- **Last 5 transfers** on the FTP tab: direction, file name, size and speed (or Failed), newest first.
- **Log file** `FastFTP.log` next to the XEX, one dated line per event: server start (address, login, folders) and stop, each client's connect, login (or wrong login), uploads and downloads with path, bytes, seconds and MiB/s, deletes, renames, new folders and disconnect. A log over 1 MiB moves to `FastFTP.old.log` when the server starts. One new check in `tests/ftp.py`.
- The Xbox screen is drawn with **Dear ImGui 1.86** (the last release that supports the VS2010-era Xbox compiler) and an embedded Roboto font, replacing the 5x7 bitmap font.
- **Tabs** for FTP server, Network and Disk, switched with **LB**/**RB** while stopped. Each tab shows its own live rate, progress and last result. **A** starts the tab's action, **B** stops, **Back** exits.
- A **settings panel** per tab: D-pad Up/Down picks a setting, Left/Right changes it. Settings are locked while something runs.
- New setting **Drawing during transfers** (Network and FTP tabs), **Paused** by default: the screen holds one frame while data moves, as with the PC's "Pause Xbox drawing". The first console run of this prototype crashed about 5 seconds into Xbox sends (FTP and Xbox-to-PC) with live drawing.
- **FTP login**: user `xboxftp`, password `xboxftp` by default. The FTP tab's **Login** setting can switch it off (any user, as before). Wrong passwords get `530` after a one-second pause, and other commands are refused until login. Three new checks in `tests/ftp.py`.
- **FTP top level listed like Aurora's FTP**: `Game`, `Hdd1`, `HddX`, `SysExt`, `System`, `Usb0`-`Usb2`, `USBMU0`-`USBMU2`, `USBMUCache0`-`USBMUCache2` (only drives that can be read), sorted by name. Listings use `rwxrwxrwx root root`, and the drive folders are dated 1 Jan 2000, as in Aurora. Replaces `HDD`, `USB0`, `USB1`, `APP` and `NULL`; the `NULL` test folder is no longer listed on the Xbox.
- The footer shows only the buttons that do something right now, and the layout stays inside the title-safe area.
- `xbox/imgui_impl_xbox360.cpp` renders ImGui with a shader pair, since the stock DX9 backend needs fixed-function D3D that the 360 lacks.
- `tests/ui_preview.cpp` rasterizes the ImGui output in software, so the screen can still be checked without a console.

The Dear ImGui screen, Aurora-style folders and fixed xboxftp login have run on a console. The user-set login is built and host-tested only.

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
