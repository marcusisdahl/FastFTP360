# Xbox360-FastFTP

**A standalone FTP server for homebrew Xbox 360 consoles, with built-in tools for measuring network and storage performance.**

FastFTP transfers files between a PC and the Xbox's internal or USB storage using a regular FTP client. Its transfer engine overlaps disk reads/writes with network traffic through two alternating 1 MiB buffers, and sends data in smaller pieces with explicit thread yields. The aim is to reduce time spent waiting between disk and network operations.

The application also includes a RAM network benchmark, a local disk test, and a virtual `NULL` folder for testing FTP without a disk. Together, these help investigate whether a transfer is limited by the FTP application, storage, network stack, driver, or connection.

The project grew from **NetBench360**. The current source still uses that name in executable filenames, the Visual Studio solution, and parts of the interface. The Xbox UI identifies the FTP additions as **v0.3.0**; the Windows benchmark companion still displays **v0.2.0** and uses the matching benchmark protocol v3.

## What is included

| Component | Purpose | What runs on the PC |
|---|---|---|
| Xbox FTP server | Upload, download, browse, rename, and manage files on exposed drives | A regular FTP client, such as FileZilla |
| FTP `NULL` folder | Measure FTP transfer speed without reading or writing a drive | The same FTP client |
| RAM network benchmark | Test TCP throughput in either direction, with optional data verification and parallel streams | `NetBench360-PC.exe` |
| Local disk test | Measure and verify a temporary file on the selected Xbox drive | No PC application needed |

**The Windows companion is for the RAM benchmark. Ordinary FTP transfers do not require it.** Only one top-level mode—FTP, RAM benchmark, or disk test—runs at a time. The FTP server itself accepts up to four simultaneous client sessions.

## Start transferring files

You need a console that can run homebrew XEX applications, a working network connection, and a PC on the same trusted LAN. Keep your existing working network driver installed. If using USB Ethernet, that adapter must already have a compatible driver loaded.

1. Obtain a build of `NetBench360.xex`, or [build it from this source](#build-from-source). Copy it into its own application folder, for example `Hdd1:\Apps\FastFTP\`.
2. Launch the XEX through Aurora, XeXMenu, or your usual homebrew launcher. Keep it running while transferring files. **Do not add this application to launch.ini as a plugin.**
3. With other tests stopped, press **LB** to start the FTP server. Read the IP address shown on the Xbox screen.
4. Connect your FTP client with these settings:

   | Setting | Value |
   |---|---|
   | Protocol | FTP |
   | Host | The Xbox IP address shown on screen |
   | Port | `21` |
   | Encryption | Plain FTP; TLS/FTPS is not implemented |
   | Transfer mode | Passive (`PASV` or `EPSV`) |
   | Username / password | Any values, for example `xbox` / `xbox`; these are not access protection |

5. Start with one simultaneous transfer and a disposable file in a new folder. Choose the appropriate drive folder from the table below.
6. Press **B** to stop the FTP server. Once stopped, **Back** exits to the dashboard.

**Access:** the server accepts any credentials and has no encryption. Connected clients can overwrite and delete real files. Use a trusted LAN, keep backups of important files, and do not expose it to the internet.

### Folders exposed over FTP

| FTP path | Contents |
|---|---|
| `/HDD/` | The internal HDD content partition, when readable |
| `/USB0/`, `/USB1/` | USB storage devices that the Xbox runtime can mount and read |
| `/APP/` | The application's `game:` location—the directory tree exposed for the running XEX |
| `/NULL/` | A virtual destination/source for network-only FTP tests |

HDD and USB folders appear only when accessible. File naming, size, and filesystem limits still depend on the selected storage device and Xbox runtime. The application creates temporary drive links while serving FTP and removes them when the server stops.

### FTP support and limitations

- Passive FTP, binary uploads/downloads, directory listings, file sizes, folder creation/removal, file deletion, and renaming.
- Up to **four simultaneous FTP control sessions**. Start with one; additional connections do not guarantee more total throughput.
- Control port **21**; passive data ports **36100–36199**. Directory listings and transfers need the passive data connection as well as the control connection.
- Active mode (`PORT`/`EPRT`), FTPS/TLS, SFTP, and nonzero-offset resume are not implemented. `REST 0` is accepted, but partial-file resume is not supported. Restart interrupted uploads from the beginning.
- Uploading to an existing path replaces its contents. Failed disk uploads are removed, so an interrupted overwrite does not preserve the original file.
- **B** is the application-wide stop control. The FTP `ABOR` handler replies when the command loop is idle; it is not a separate asynchronous transfer-cancellation mechanism.

## How the transfer engine works

Each client session handles its own FTP control/data sockets. For a real-file transfer, a helper thread and **two 1 MiB buffers** overlap storage work with socket work:

- **Upload:** the network side fills a buffer while the helper writes the previous buffer to disk. The final file flush completes before a successful transfer reply.
- **Download:** the helper reads ahead into one buffer while the network side sends the other.

Individual network sends are limited to **16 KiB** and yield between successful sends; would-block retries sleep briefly. These application choices aim to keep transfers moving without an uninterrupted retry loop. They do not change the network driver or USB queue settings.

The Xbox screen shows live transfer progress and the last completed transfer's average. The FTP client's successful `226` reply includes bytes, elapsed seconds, and MiB/s. With several sessions, the screen reflects session progress callbacks rather than a guaranteed aggregate rate; use each completed transfer's totals for comparisons.

**The Windows benchmark's send cap and paused-drawing controls apply to RAM benchmark mode, not FTP transfers.** FTP has its own transfer engine. File transfers are not automatically checked byte-for-byte by the RAM benchmark's verification setting; use file hashes or a round-trip comparison when checking transferred files.

## Measure where time is spent

### FTP without storage: `/NULL/`

- Upload a file to `/NULL/`: the server receives and discards its bytes. No file is stored.
- Download `/NULL/256MB.bin` or `/NULL/1024MB.bin`: the server generates a repeating test pattern in memory.
- Generated names follow `<n>MB.bin`, where `n` is **1–4096**. Despite the `MB` name, each unit represents **1 MiB** (1,048,576 bytes).

Compare the same direction and transfer size using `/NULL/`, then a real drive, and then another FTP server such as Aurora. A lower real-file rate suggests storage or the file-I/O path contributes to the difference. A similar rate suggests looking further along the network/application path. These comparisons narrow possibilities; they do not identify a single cause by themselves. Downloads can benefit from file caching, so repeat tests consistently.

### RAM network benchmark

Press **A** while stopped to listen on TCP **36020**, then run `NetBench360-PC.exe` on the PC. This uses a custom benchmark protocol, separate from FTP; the Windows app and Xbox benchmark must both support **protocol v3**.

The companion offers direction, duration, repeat count, **1/2/4 TCP streams**, PC socket buffers, Xbox send limit, and data-check settings. Each stream checks 1 MiB of known data, warms up with 32 MiB, then runs the timed transfer. Final rates are receiver-reported. **All data** additionally verifies timed payload bytes and costs receiver CPU; the default check covers the initial 1 MiB only. CSV preserves the settings and completed results.

The Xbox send cap applies **per stream**. Four streams at a 16 MiB/s cap have a combined configured ceiling of 64 MiB/s; capped results do not measure maximum bandwidth. With drawing paused, the Xbox deliberately holds a static status screen while the PC shows live progress and the controller remains polled.

While stopped, **D-pad Up/Down** selects Xbox listener buffers (startup default, 128 KiB, or 256 KiB); **Left/Right** selects the RAM stream workers' hardware thread (automatic or 0–5). Buffer choices also feed FTP listener/data-socket setup; worker-thread selection is used by the RAM benchmark. Larger buffers may be refused by XNet. These settings are experiments, not promised speed improvements.

If more streams improve throughput, connection windowing or per-stream scheduling may contribute. A flat result may indicate a shared limit, but neither outcome alone proves that the driver or USB bus is the bottleneck. Keep every setting consistent between driver comparisons, and remember that the driver label is metadata—it does not select or install a driver.

### Local disk test

With all modes stopped, **Y** selects the app location, HDD, USB0, or USB1; **RB** selects a 64 KiB, 256 KiB, or 1 MiB block size; **X** starts the test.

It writes a **256 MiB** file named `NETBENCH360-DISKTEST.TMP`, includes the final flush in write timing, reads it back without the file cache, compares the data, and attempts to delete it afterward. **B** requests cancellation. Use a writable target with more than 256 MiB free, and do not keep an existing file with that test filename in the selected root: the test recreates it. A crash or interrupted cleanup can leave the temporary file behind.

The disk test measures a separate workload from FTP. Similar results can suggest a storage constraint, but different caching, block sizes, and simultaneous network work affect the comparison. USB storage and USB Ethernet can also contend for the console's USB resources.

## Performance and development status

FastFTP is an experimental application. The current source includes the **v0.3.0 FTP additions**; this repository currently has no packaged GitHub release. The changelog records 21 host FTP checks covering file operations, exact upload/download bytes, path handling, virtual transfers, and three simultaneous sessions. That is host-test evidence, not console certification. The latest recorded status says the FTP and disk additions still need console validation; see [validation notes](docs/VALIDATION.md) and [change history](CHANGELOG.md).

Throughput depends on direction, file sizes, the FTP client, console storage, network stack, loaded driver, connection, and settings. The name does not guarantee a particular speed or improvement over another server. A RAM benchmark result is not a real-file transfer result.

Earlier NetBench360 testing recorded a console freeze/restart during an unlimited Xbox-to-PC run with the experimental TxScan driver; its cause was not established. Preserve a known-working driver and your normal recovery method when comparing builds. See [crash investigation](docs/CRASH-INVESTIGATION.md).

The application uses the console's existing network route. It does not automatically install or choose CUsbEnet, alter USB queues, enable adapter aggregation, patch kernel hooks, write NAND, or edit boot configuration. FTP writes the files requested by clients; the disk test writes its named temporary file.

## Build from source

Requirements: Visual Studio C++ tools, compatible **RXDK-360 / Xbox 360 SDK** integration with toolset `2010-01`, and the .NET Framework compiler. The Windows companion requires .NET Framework 4.5 or later. SDK files and libraries are not included in this repository.

From PowerShell in the repository:

```powershell
.\build.ps1
```

Outputs:

- `build\xbox\NetBench360.xex` — the Xbox application, including FTP and the diagnostic modes.
- `build\windows\NetBench360-PC.exe` — the Windows RAM benchmark companion.

The `.exe` in the Xbox output directory is the intermediate PowerPC image, not the Windows app. For the Xbox project, open `NetBench360.sln` and build **Release | Xbox 360**. The project sets `DEVKIT_ONLY=0` and excludes automatic console deployment.

To build the Windows companion and run host tests without the Xbox SDK:

```powershell
.\build.ps1 -HostOnly -Tests
```

Tests additionally require Python 3 and the Visual Studio C++ compiler. They use loopback host versions of the shared engines, including Python's `ftplib` for FTP. See [benchmark wire protocol](docs/PROTOCOL.md) for implementation details.

## License

Project source is licensed under [MIT](LICENSE). SDK components retain their own terms. This project is not affiliated with Microsoft.
