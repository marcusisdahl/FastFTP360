# Read benchmarks: NetISO, ConnectX and local files

FastFTP 0.5.0 adds a **Read test** tab. It answers one question: is playing Xbox 360 games from a LAN server (NetISO or ConnectX) practical on this console, and how do its throughput, read latency and stability compare with the internal drive and USB?

> **Status: Experimental; ABadAvatar 1.3 / ABadUpdate compatibility not yet verified.**
> Everything below was built and host-tested. None of it has run on a console yet. See [Console test checklist](CONSOLE-TEST-CHECKLIST.md).

## Contents

- [What each test measures](#what-each-test-measures)
- [What the numbers mean](#what-the-numbers-mean)
- [Prerequisites](#prerequisites)
- [Using the Read test tab](#using-the-read-test-tab)
- [Known-data fixtures](#known-data-fixtures)
- [The NetISO wire protocol](#the-netiso-wire-protocol)
- [Defects in the reference server](#defects-in-the-reference-server)
- [ConnectX](#connectx)
- [Mounted NetISO disc: not implemented](#mounted-netiso-disc-not-implemented)
- [ABadAvatar / BadUpdate compatibility](#abadavatar--badupdate-compatibility)
- [Recovery](#recovery)
- [Comparisons worth making](#comparisons-worth-making)
- [Limitations](#limitations)
- [Host tests](#host-tests)

## What each test measures

| Source | Reads through | Measures | Does **not** measure |
|---|---|---|---|
| **Internal HDD**, **USB 0**, **USB 1**, **App drive** | The Xbox file system, read-only, from `<drive>:\FastFTP-Bench\` (changeable with `read_folder=`) | The drive and file system as a local baseline | Game code, DVD emulation |
| **Game disc** | Files on the disc in the DVD drive (`\Device\Cdrom0`), read-only. **X** walks the disc and lists its 64 largest files, biggest first | The DVD drive and disc file system, the baseline NetISO replaces | Where on the disc the game's other files sit (the drive is faster on the outer edge), and the game's own read pattern. Burned discs on a stock drive will not read. |
| **NetISO protocol** | FastFTP's own TCP client talking the NetISO server protocol | The network path (adapter, CUsbEnet or stock Ethernet, switch, server, server disk) for the request sizes a game might use | The console's NetISO plugin, its DVD emulation, its caching and its request pattern. This is **not** a full gameplay-path benchmark. |
| **ConnectX** | Ordinary file reads on a path served by the **already loaded** ConnectX module | ConnectX's SMB1 client and the server | Game behaviour; only works if FastFTP can see the ConnectX path at all (see [ConnectX](#connectx)) |

This is separate from the **Disk** tab, which writes and reads back its own 256 MiB file. The Read test never writes: files are opened with `GENERIC_READ` and `FILE_SHARE_READ`, and NetISO requests are only ping, list, mount (per connection, on the server) and read.

### Workloads

| Workload | Request | Offsets |
|---|---|---|
| Sequential 64 KiB / 256 KiB / 1 MiB | 64 KiB, 256 KiB, 1 MiB | From the start of the file, back-to-back. At the end of the file it wraps to 0 and counts a wrap (repeated data, so caches may help). |
| Random 4 KiB / 16 KiB / 64 KiB | 4 KiB, 16 KiB, 64 KiB | Uniform over the file, every offset a multiple of 4 KiB (so also of the 2048-byte ISO sector and the 512-byte disk sector), and never past the end. |

Random offsets come from splitmix64 seeded with `read_seed` (default 1) and the workload, so the same seed, file size and workload give the same sequence on every source. One request is outstanding at a time.

Each run has three phases:

1. **Setup**: open the file, or connect, ping, list and mount the ISO. Timed separately and never part of throughput.
2. **Warm-up**: 3 seconds of the same workload. Not measured.
3. **Measurement**: the selected duration (10, 30 default, 60 or 120 s).

## What the numbers mean

| Field | Meaning |
|---|---|
| Bytes | Bytes actually received or read during the measurement, including any partial reply from a failed read |
| Seconds | Measurement wall time, minus the time spent checking fixture data |
| MiB/s | Bytes / seconds |
| Reads/s | Completed reads / seconds |
| Latency | Duration of one complete read. For NetISO it starts before the request is sent and ends when the last byte arrives. Mean and maximum are exact. **Median, p95 and p99 are approximate**: a fixed 4 KiB histogram; values below 64 µs are exact, above that each bucket spans 1/32 of its power of two, and the reported value is the bucket's upper edge, so it is at most about 3% high. |
| Failed, short, timeouts | Read errors, replies or reads shorter than requested, and reads with no data for 10 s (NetISO) |
| Integrity | Fixture bytes that did not match the formula (see below). The run stops at the first one. |
| Check | `known data checked`, `byte counts only, content not independently verified`, or `off` |
| Pass | 1 the first time this source and file is read in this FastFTP session; 2, 3... are repeats |

Runs end as **completed**, **cancelled** (B) or **failed**, and all three are written to `FastFTP.log` and appended to `FastFTP-reads.csv` next to the XEX (fetch it through the FTP server's `/Game/` folder). Only completed runs show a speed on screen.

A NetISO connection cannot be resynchronized after a short or late reply, so the first such error fails the run. Local and ConnectX reads continue past an error and fail the run after 16.

### Caches: first pass and repeats

- **Xbox file cache**: for local and ConnectX sources, *Bypassed* (default) opens the file with `FILE_FLAG_NO_BUFFERING`; *Normal* uses the cache. Whether ConnectX honours the flag is unknown.
- **Server cache**: FastFTP never flushes it. A file the server has read recently is probably in its RAM. Pass 1 means only "first time in this FastFTP session", **not a cold cache**. To get closer to cold, use a file the server has not read since it booted, or one larger than the server's RAM.
- Sequential runs that wrap re-read the same data.

## Prerequisites

### Console

- Your usual setup: ABadAvatar / BadUpdate, your payload (for example XeUnshackle), Aurora, and CUsbEnet or the built-in Ethernet port. Nothing about it changes.
- FastFTP 0.5.0 copied to **its own folder**, separate from your working FastFTP. For example `Hdd1:\Apps\FastFTP-0.5.0\FastFTP.xex`. Start it manually from Aurora. Do not put it in `launch.ini`.
- For NetISO protocol tests: nothing else on the console. The console's NetISO plugin is not used.
- For ConnectX tests: ConnectX already loaded the way you normally load it. FastFTP does not load, enable or configure it.

### Server (PC, NAS or Raspberry Pi)

- **NetISO protocol**: a NetISO-compatible server on TCP port 4323 with `.iso` files.
  - [tuxuser/netiso-srv](https://github.com/tuxuser/netiso-srv) (Rust, Linux/Docker) is the reference this client was written against, at commit `40e0a77`. Read its [defects](#defects-in-the-reference-server) first: it can fail 256 KiB and 1 MiB requests.
  - `tests/netiso_server.py` in this repository speaks the same protocol with complete reads and writes: `python tests\netiso_server.py D:\ISOs`. Use it to separate server problems from network problems.
  - The Windows `server.exe` from the consolemods NetISO package has no published source; it was not examined.
- **ConnectX**: an SMB1 share, configured in ConnectX/Aurora as you normally would. SMB1 is insecure; keep it on a trusted LAN.
- Use a wired connection for the server.

## Using the Read test tab

1. **LB/RB** to **Read test**. **D-pad Up/Down** picks a row, **Left/Right** changes it.
2. **Source**: Internal HDD, USB 0, USB 1, App drive, Game disc, NetISO protocol or ConnectX.
3. For NetISO, press **Y** and type the server's IP address (`192.168.1.20` or `192.168.1.20:4323`). It is saved in `FastFTP.ini`.
4. Press **X** to list the files in the folder (or the server's ISOs, or the 64 largest files on the disc), then choose one in **File**.
5. Choose **Workload**, **Duration**, **Data check** and **Xbox file cache**, then press **A**.
6. **B** stops a run. The screen holds still while data moves (the *Drawing during transfers* setting applies here too).

`FastFTP.ini` gains these lines. Missing or invalid ones keep the defaults, and older FastFTP versions ignore them:

```ini
read_folder=FastFTP-Bench
connectx_folder=
netiso_server=192.168.1.20
netiso_port=4323
read_seed=1
```

## Known-data fixtures

A file named `FFTPFIX-<8 hex digits>.<anything>` is a **fixture**: every 32-bit big-endian word at byte offset 4·w equals `FixtureWord(seed, w)` (see `shared/readbench.cpp`; an independent Python copy is in `tests/read.py`). With **Data check** on, every byte of every read is compared with that formula, so corruption anywhere on the path (server, network, adapter, driver, file system) is detected. The check's time is excluded from throughput and reported separately.

Make one on a PC with the host tool that `.\build.ps1 -Tests` (or `-HostOnly -Tests`) builds as `build\host\read-host.exe`:

```powershell
build\host\read-host.exe fixture D:\ISOs\FFTPFIX-0000002A.iso 4293918720 0x2A   # 4095 MiB, seed 0x2A
```

Copy the same file to `Hdd1:\FastFTP-Bench\` (and `Usb0:\FastFTP-Bench\`) with FastFTP's FTP server for the local baseline. Keep fixtures below 4 GiB: the Xbox HDD (FATX) and FAT32 USB drives cannot hold a file of 4 GiB or more. Any other file, such as a game ISO, is only **byte-count checked**: the right number of bytes arrived, but nothing independent says they are the right bytes. FastFTP never calls a game ISO verified, and hashing what arrived would not establish correctness either.

## The NetISO wire protocol

Verified from `tuxuser/netiso-srv` `src/main.rs` at `40e0a77`. The console plugin itself is closed source, so this is the server's side of the protocol only.

Request, 20 bytes, all big-endian:

| Offset | Size | Field |
|---|---|---|
| 0 | 4 | Magic `ISVR` |
| 4 | 2 | Command: 0 Ping, 1 GetIsoSize, 2 HasType1File, 3 ReadData, 4 GetIsoName, 5 MountIso |
| 6 | 2 | ISO index (GetIsoName, HasType1File) |
| 8 | 8 | Byte offset in the ISO file (ReadData) |
| 16 | 4 | Length |

| Command | Reply |
|---|---|
| Ping | 8 bytes `ISVRokOK` |
| GetIsoName | Exactly *length* bytes: the file name, NUL padded or cut. An empty name ends the list. |
| MountIso | Followed by *length* bytes of name. Reply u32: 1 mounted, 0 not found. The server strips `\Mount`, backslashes and NULs, then mounts the first listed file whose name **ends with** the result. `[Disable Current ISO]` unmounts. |
| GetIsoSize | u32 sector count (`file size / 2048`, rounded down), u32 sector size 2048. 0 sectors if nothing is mounted. |
| ReadData | Exactly *length* bytes from the raw file offset. No reply at all if nothing is mounted; the connection closes if the read passes the end of the file. |

The mounted ISO belongs to the connection, so FastFTP's mount does not change what the console plugin has mounted, at least with this server. FastFTP's client sends each request in one buffer (MountIso's header and name together), receives exactly the expected bytes with a 10-second idle timeout (5 s during setup), refuses ISO names that are ambiguous under the suffix match, checks the 2048 sector size, and never reads past `sectors × 2048`.

## Defects in the reference server

Found by reading `netiso-srv` `40e0a77` and confirmed against a build of it on Linux:

1. **Replies can be cut short.** Every reply is sent with tokio's `try_write`, which writes only what fits in the socket's free send buffer and silently drops the rest (or fails with `WouldBlock`). Confirmed: with the Linux send buffer capped at 128 KiB (`net.ipv4.tcp_wmem=4096 16384 131072`), a 256 KiB ReadData delivered 106,496 bytes and then nothing; with default buffers, an 8 MiB reply stopped at 2,820,096 bytes. 64 KiB replies always arrived whole in these tests. On a real LAN the risk grows with request size and depends on the server OS's send buffer. FastFTP sees this as a **timeout** and fails the run; the bytes never become data for the next request, so it cannot cause silent corruption here.
2. **Requests are parsed from a single `read()`.** A request split across TCP segments is zero-padded and misinterpreted (a ReadData could use a wrong offset or length), and the rest is then rejected as bad magic, which closes the connection. MountIso reads the name with one `read()` and an `assert`, so a split name panics that connection's task. Unlikely for 20-byte requests sent in one call on a LAN, but not guaranteed by TCP.
3. **Mount matches by suffix.** `a.iso` mounts `data.iso` if that is listed first. FastFTP refuses such names.
4. ReadData with nothing mounted gets no reply at all.

Suggested fix for the server (not submitted upstream): replace `socket.try_write(x)?` with `socket.write_all(x).await?` and `socket.read(&mut buffer)` with `socket.read_exact(&mut buffer)` (and the same for the mount name). `tests/netiso_server.py` behaves that way. **For 256 KiB and 1 MiB results, use `tests/netiso_server.py` or a patched server**, and treat timeouts from unpatched `netiso-srv` as this defect until shown otherwise.

## ConnectX

What is verified from public documentation ([ConsoleMods](https://consolemods.org/wiki/Xbox_360:ConnectX), [RetroNAS](https://github.com/retronas/retronas/wiki/XBox360)): ConnectX is a leaked Microsoft development-kit plugin (`connectx.xex` plus `connectx_patch.xexp`) that Aurora or FreeStyle Dash loads at boot. It mounts an SMB1 share; Aurora shows it as `ConnectX:` and FSD as `ConX:`. It supports extracted (folder) games only, not ISOs.

What is **not** verified: the kernel device it creates, whether the `ConnectX:` name exists outside Aurora (it may be Aurora's own link), and whether it works under XeUnshackle's DashLaunch (`connectx_patch.xexp` is a patch file; how it is applied under BadUpdate is undocumented). There is no source to check.

So FastFTP:

- Detects the module with `GetModuleHandle("connectx.xex")`, a read-only lookup. If it is not loaded, the ConnectX source shows why and stays off. Nothing else is affected.
- Uses **no default path**. You set `connectx_folder=` in `FastFTP.ini` to a path you have confirmed, for example from Aurora's file manager. FastFTP then lists and reads files there with ordinary read-only file calls, through whatever the module provides.
- Reports unreachable paths, authentication problems or disconnects as the error the file system returns. FastFTP cannot tell those apart, because ConnectX handles authentication itself.

**Cancellation caveat:** a ConnectX read is a synchronous file read inside the module. If the server disappears, B takes effect only when that read returns, and how long that takes is up to ConnectX. FastFTP never kills a thread that owns a handle.

## Mounted NetISO disc: not implemented

Reading through the plugin's virtual DVD would measure the real gameplay path, but:

- The console NetISO plugin (`NetISO.xex` from the consolemods package) is closed source and undocumented. Its device namespace is unknown. Aurora's file browser shows a "NetISO" entry, which does not tell what FastFTP would see.
- `Game:` in FastFTP is the folder FastFTP was launched from, not a mounted disc.
- Probing `\Device\Cdrom0` or similar would be a guess, and opening the DVD device while a plugin hooks it is untested kernel behaviour.

It is left out until someone records the actual device path on a console with the plugin loaded (for example from Aurora's file manager, or a kernel object listing). Then it can be added as its own source using the same engine. The NetISO protocol test plus the in-game comparisons below are the substitute for now.

## ABadAvatar / BadUpdate compatibility

The name is ambiguous, so here is what the sources say:

- **BadUpdate** (grimdoomer/Xbox360BadUpdate): software-only hypervisor exploit for dashboard **17559**. Release **v1.3** added stage 3 improvements, new hypervisor-overwrite detection, a boot animation recovery mode, and dropped Tony Hawk's American Wasteland for Rock Band Blitz.
- **ABadAvatar** (shutterbug2000/ABadAvatar): a fork of BadUpdate triggered through Avatar data at boot. Its only GitHub release is *Public Beta 1.0*. [ConsoleMods](https://consolemods.org/wiki/BadUpdate) lists an "ABadAvatar 1.3"; no matching release was found on GitHub. Treat "ABadAvatar 1.3" as unconfirmed and record the exact files you use.
- Both run one unsigned XEX (`BadUpdatePayload\default.xex`), usually **XeUnshackle** or **FreeMyXe**. XeUnshackle applies temporary hypervisor and kernel patches, loads DashLaunch from memory and loads `launch.ini` plugins. All of it is gone after a reboot or power-off.

What FastFTP 0.5.0 relies on, and why it should be the same as 0.4.1 on that setup:

- It is the same kind of XEX as 0.4.1: started manually, no persistent component, no kernel patch, no `launch.ini` change, no flash write. New calls are ordinary title APIs (`socket`/`connect`/`select`/`send`/`recv`, `CreateFile`/`ReadFile`/`FindFirstFile`, `VirtualAlloc`, `GetModuleHandle`) plus the same `ObCreateSymbolicLink`/`ObDeleteSymbolicLink` the Disk and FTP tabs already use.
- Outbound TCP from the Xbox is new for FastFTP (it only listened before). It uses the same XNet startup and LAN socket options as the listener. Whether retail XNet under XeUnshackle allows a title to connect out to a LAN PC this way is **unverified** until the console test.

These are three separate questions, and none is answered yet:

1. Does FastFTP 0.5.0 run under your ABadAvatar / BadUpdate setup? **Unverified.**
2. Does your NetISO or ConnectX build work under that setup at all? **Unverified, and outside FastFTP.** Public guides describe both on RGH/JTAG/XeUnshackle with DashLaunch; none mention BadUpdate specifically. Working under RGH/JTAG says nothing about BadUpdate.
3. Does either stay stable alongside CUsbEnet, during tests and after returning to Aurora? **Unverified.**

## Recovery

From the BadUpdate, ABadAvatar and XeUnshackle documentation:

- The exploit and everything it patches are **not persistent**. Powering the console off and on returns it to the stock 17559 system; running the exploit again restores your setup.
- If the exploit stalls, power off and retry. Its documentation describes ring-of-light progress and a 20-minute limit.
- Do not replace files in flash (such as the boot animation) and avoid homebrew or plugins that modify flash. FastFTP does neither. BadUpdate v1.3's boot animation recovery mode is for consoles where that was done; see its README.
- Keep a NAND backup, as those projects recommend.

What to do if FastFTP 0.5.0 misbehaves:

1. **Freeze or hang:** hold the power button until the console turns off, wait a few seconds, power on, and run your exploit as usual. This document does not invent a button combination; use the procedure from your exploit's documentation.
2. **Crash back to the dashboard:** start your known-good FastFTP from its own folder and carry on.
3. Delete or rename the 0.5.0 folder through your known-good FastFTP or Aurora's FTP if you want it gone. It installs nothing else. Its `FastFTP.ini`, `FastFTP.log` and `FastFTP-reads.csv` sit next to its own XEX, so your working copy's files are untouched.
4. A failed ConnectX or NetISO plugin is handled the way you normally handle a bad plugin: remove it from `launch.ini` through FTP or Aurora, then power-cycle. FastFTP never changes `launch.ini`.

## Comparisons worth making

Record the source, server, adapter and driver for each row. Change one thing at a time.

- **Stock Ethernet versus UE306 + CUsbEnet**: the built-in port is 100 Mbit/s (at most about 11 MiB/s); the UE306 on CUsbEnet measured about 27.7 MiB/s PC to Xbox in the network test. NetISO protocol sequential 1 MiB should sit near those limits; random 4 KiB shows per-request latency, where the adapter's USB round trips may matter more than bandwidth.
- **Server SSD versus NAS (HDD)**: random 4 KiB and 16 KiB on a fixture larger than the server's RAM show the disk; repeats of a smaller file show the cache.
- **First pass versus repeats**: run the same file and workload several times. Pass 1 versus later passes shows how much caches help. Neither is a controlled cold-cache result.
- **Game disc versus network**: run the same six workloads on the largest file of an original disc, ideally of the same game as the ISO. The disc is what NetISO replaces, so this is the most direct comparison. A file's position on the disc changes its sequential speed, so note which file you used.
- **Local baselines**: the same fixture on Internal HDD and USB with the Xbox cache bypassed. USB shares the controller with a USB adapter, so USB results with CUsbEnet in use are lower than either alone.
- **Real games** (by hand, the only full gameplay-path test): pick one game you have both on the internal drive (extracted or GOD) and on the server through NetISO (and/or ConnectX). With a stopwatch or a phone video, time: boot to title screen, the same level load, the same cutscene start, and count visible stalls during the same 5 minutes of play. Repeat each three times, alternating local and network, and note first-boot versus repeat. Compare with the Read test's random 16/64 KiB latency, not just sequential MiB/s.

## Limitations

- No console testing yet. Builds and host tests cannot show that the console is safe, and experimental homebrew can crash or hang the console.
- The NetISO protocol test bypasses the console plugin. Its request sizes and pattern are FastFTP's, not a game's.
- Mounted NetISO disc reads are not implemented (above).
- ConnectX needs a manually configured path and is only as cancellable as the module's own reads.
- One outstanding request at a time. Real NetISO/ConnectX implementations may queue or prefetch differently.
- Percentiles are approximate to about 3% (mean and maximum exact).
- Server and Xbox caches are not controlled.
- IPv4 addresses only, no host names.
- The PC app (`FastFTP-PC.exe`) and the network test protocol are unchanged (still protocol v3). Read results are exported as `FastFTP-reads.csv` on the Xbox instead.

## Host tests

`tests/read.py` runs the Xbox read engine (`shared/readbench.cpp`, built as `build/host/read-host`) against local files and `tests/netiso_server.py` with injected faults: 56 checks, including all six workloads on both backends, partial TCP writes (7-byte pieces), truncated replies, stalls, disconnects, corrupt bytes, malformed size, mount and name replies, ambiguous names, timeouts, cancellation during a stalled read, repeated runs with socket cleanup, files too small for a workload, settings parsing, and percentile accuracy. With `NETISO_SRV` set to a built `netiso-srv`, it also reads a fixture through the reference server. On Linux they build with `tests/posix_compat.h`; on Windows `.\build.ps1 -HostOnly -Tests` builds and runs them with the existing FTP and protocol tests.
