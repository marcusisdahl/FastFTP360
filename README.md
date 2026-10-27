<p align="center">
  <img src="docs/images/logo.png" alt="FastFTP360 for homebrew Xbox 360" width="500">
</p>

<p align="center">
  <b>A fast FTP server for homebrew Xbox 360 consoles, with built-in network and disk tests.</b>
</p>

<p align="center">
  <img src="docs/images/screen-ftp-upload.png" alt="FastFTP360 receiving an upload at 26 MiB/s, with the last five transfers listed" width="800">
</p>

FastFTP360 moves files between your PC and the Xbox's internal drive or USB storage using any regular FTP client, such as FileZilla. It reads and writes the disk **while** the network transfer is running, instead of one after the other. With a USB Ethernet adapter, uploads to the Xbox have reached about **27.5 MiB/s**, where Aurora's built-in FTP managed about 19.5 MiB/s on the same console.

- **Works like Aurora's FTP:** the same folder names (`Hdd1`, `Usb0`, `Game`...), so your paths and bookmarks keep working.
- **Your own login:** `xboxftp` / `xboxftp` by default. Change it with the Xbox keyboard or in `FastFTP360.ini`.
- **Last 5 transfers** on screen with file name, size and speed. Everything is also written to `FastFTP360.log`.
- **Network test and disk test** built in, to find out whether the network, the drive or the FTP client is the slow part.

**Best with [CUsbEnet](https://github.com/marcusisdahl/CUsbEnet).** FastFTP360 was made alongside CUsbEnet 1.1, a USB Ethernet driver plugin for the Xbox 360. Together they're the fastest way we know of to copy files to an Xbox 360 over FTP. All the speeds on this page were measured with a TP-Link UE306 adapter on CUsbEnet. FastFTP360 also works over the console's built-in network port.

FastFTP360 is experimental homebrew. Read [Safety](#safety) before using it on files you care about.

## Contents

- [Quick start](#quick-start)
- [The screens](#the-screens)
- [FTP server](#ftp-server)
- [Network test](#network-test)
- [Disk test](#disk-test)
- [Settings](#settings)
- [Why it is faster](#why-it-is-faster)
- [Safety](#safety)
- [Build from source](#build-from-source)
- [Project history](#project-history)

## Quick start

You need a console that runs homebrew (XEX) applications, a working network connection, and a PC on the same home network. If you use a USB Ethernet adapter, keep its driver loaded as usual. For the speeds shown here, use [CUsbEnet](https://github.com/marcusisdahl/CUsbEnet) 1.1.

1. Download `FastFTP360-<version>.zip` from the [Releases page](https://github.com/marcusisdahl/Xbox360_FastFTP/releases).
2. Copy `FastFTP360.xex` to its own folder on the Xbox, for example `Hdd1:\Apps\FastFTP360\`.
3. Start it from Aurora, XeXMenu or your usual launcher. **Do not** add it to `launch.ini`: it is an app, not a plugin.
4. The app opens on the **FTP server** tab. Press **A** to start the server.
5. In your FTP client, connect with:

   | Setting | Value |
   |---|---|
   | Host | The IP address shown at the top right of the Xbox screen |
   | Port | `21` |
   | Protocol | Plain FTP (no TLS) |
   | Transfer mode | Passive |
   | User name / password | `xboxftp` / `xboxftp` |

6. When you are done, press **B** to stop the server, then **Back** to exit to the dashboard.

`FastFTP360-PC.exe` in the zip is only for the network test. You don't need it for FTP.

## The screens

Switch tabs with **LB** and **RB** while nothing is running. The footer always shows the buttons that do something right now.

| | |
|---|---|
| <img src="docs/images/screen-ftp-ready.png" alt="FTP server tab, ready" width="400"><br>**FTP server**, ready. The right card shows the login to type into your FTP client. | <img src="docs/images/screen-ftp-running.png" alt="FTP server tab, server running" width="400"><br>**FTP server** running, with the address, login and the folders it found. |
| <img src="docs/images/screen-network.png" alt="Network tab" width="400"><br>**Network** test, waiting for the PC app. | <img src="docs/images/screen-network-test.png" alt="Network test measuring" width="400"><br>**Network** test measuring PC to Xbox. |
| <img src="docs/images/screen-disk.png" alt="Disk tab" width="400"><br>**Disk** test with the drive and block size. | <img src="docs/images/screen-settings.png" alt="Settings tab" width="400"><br>**Settings** for the advanced options. |

These images are drawn by the same screen code the console runs (`tests/ui_preview.cpp`), with example values.

## FTP server

### Login

The default user name and password are both `xboxftp`. To choose your own, press **Y** on the FTP server tab while the server is stopped. The Xbox keyboard asks for the user name and then the password: 1 to 23 letters, digits or symbols, no spaces. Cancelling either one leaves the login unchanged.

On the FTP tab, **D-pad Left/Right** switches the login off, so any user name and password work. The login is saved in `FastFTP360.ini` next to the XEX, together with the [settings](#settings). You can also edit the file on a PC:

```ini
login=on
user=xboxftp
password=xboxftp
buffers=0
cpu=auto
drawing=paused
speed=0
disk=cached
```

Missing or invalid lines keep the defaults.

### Folders

The top level lists the drives the way Aurora's FTP does, sorted by name, so paths such as `/Hdd1/Content/...` are the same in both servers. Only drives that are present and readable are listed.

| FTP path | Contents |
|---|---|
| `/Game/` | The drive and folder FastFTP360 was started from |
| `/Hdd1/` | The internal hard drive |
| `/HddX/`, `/SysExt/`, `/System/` | Hard drive system partitions |
| `/Usb0/` to `/Usb2/` | USB storage devices |
| `/USBMU0/` to `/USBMU2/`, `/USBMUCache0/` to `/USBMUCache2/` | USB sticks formatted by the Xbox |

### Transfer list and log file

The FTP tab lists the last five finished transfers with their file name, size and speed, newest first. Every event is also written to `FastFTP360.log` next to the XEX:

```text
2026-10-13 18:40:12  FTP server started on 192.168.1.50 port 21, login xboxftp, speed limit Off, worker CPU Automatic. Folders: Game Hdd1 Usb0
2026-10-13 18:40:31  192.168.1.20  Logged in as xboxftp
2026-10-13 18:40:33  192.168.1.20  Upload /Hdd1/Games/game.iso started
2026-10-13 18:44:52  192.168.1.20  Upload /Hdd1/Games/game.iso, 7466336256 bytes in 259.10 s (27.48 MiB/s)
```

Each transfer is logged when it starts and when it ends, so after a crash the log shows what was running. Wrong logins, downloads, deletes, renames, new folders and disconnects are logged too. When the log grows past 1 MiB, it is moved to `FastFTP360.old.log` the next time the server starts.

### What the server supports

- Uploads, downloads, folder listings, file sizes, new folders, deleting, and renaming.
- Up to **four** client connections at once. One is usually enough. More connections don't add speed, because the connection itself is the limit.
- Control port **21**. Passive data ports **36100 to 36199**.
- **Resuming** uploads and downloads (`REST`, `APPE`). If an upload of a new file breaks off, the part that arrived stays on the Xbox, and FileZilla offers to resume it the next time (choose **Resume** when it asks what to do with the existing file).
- **Checksums on the Xbox** (`HASH` with SHA-1, MD5 or CRC32, and `XSHA1`, `XMD5`, `XCRC`): the Xbox reads the file back from its own drive and sends only the checksum, so an upload can be checked without downloading it. WinSCP can use this (Files > Calculate checksum); `tests/ftp_console.py` does too. A 4 GiB file takes about a minute on the internal drive.
- **Exact file dates** (`MLSD`, `MLST`, `MDTM`, `MFMT`): listings carry the time to the second, so FileZilla's directory comparison and "overwrite if newer" work, and with *Preserve timestamps of transferred files* turned on, uploads keep the PC's dates.
- **Not supported:** active mode (`PORT`/`EPRT`), FTPS/TLS and SFTP.
- Uploading to an existing name replaces the file once the upload has finished. Until then the data goes to a temporary `FastFTP360-<number>.tmp` file in the same folder, so a failed or cancelled upload leaves the old file as it was. Replacing a file needs free space for both copies for a moment.
- Cancelling in the FTP client stops the transfer straight away, whether the client sends `ABOR` or closes the connection. A new file's unfinished upload is kept so it can be resumed, and shows as failed in the transfer list and log; an unfinished replacement is deleted and the old file stays.
- Names of up to 255 characters and paths of up to 259 are accepted; longer ones get an error. The Xbox's own drives (FATX) allow at most 42 characters per name.
- Data connections are accepted only from the computer that is logged in.
- Each finished transfer's `226` reply in the FTP client log shows bytes, seconds and MiB/s.

## Network test

The network test measures raw TCP speed between the PC and the Xbox's memory, with no disk involved. Use it to check what your network adapter and driver can do before blaming the disk or the FTP client.

1. On the **Network** tab, press **A**. The Xbox listens on port **36020**.
2. On the PC, run `FastFTP360-PC.exe` and enter the Xbox IP.
3. Pick a direction, duration and number of streams, then start the test.

<p align="center"><img src="docs/images/screen-pc-app.png" alt="FastFTP360-PC.exe, the network test's PC app" width="640"></p>

The PC app can run 1, 2 or 4 TCP streams, check every byte (**All data**), limit the Xbox's send rate, and save results to CSV. Each stream first checks 1 MiB of known data and warms up with 32 MiB, then runs the timed transfer. The receiver reports the final rate. Use the PC app from the same release as the XEX. Versions with a different protocol are rejected.

The Xbox send limit applies **per stream**. While data moves, the Xbox holds a still screen by default and the PC shows live progress. B still stops the test.

## Disk test

On the **Disk** tab, use the D-pad to pick the drive (app drive, HDD, USB 0 or USB 1) and the block size (64 KiB, 256 KiB or 1 MiB), then press **A**.

The test writes a **256 MiB** file named `FASTFTP360-DISKTEST.TMP`, including the final flush. It then reads the file back without the cache, checks every byte, and deletes it. You need more than 256 MiB free. **B** cancels.

USB storage and a USB Ethernet adapter share the Xbox's USB bandwidth. So FTP to a USB drive is much slower than FTP to the internal drive, even though both disks are fast on their own.

## Settings

The **Settings** tab holds options most people never need. While stopped, **D-pad Up/Down** picks one and **Left/Right** changes it. The left card explains the selected option. Changes are saved in `FastFTP360.ini` right away, so they stay set the next time you start FastFTP360.

| Setting | Choices | Used by |
|---|---|---|
| Xbox socket buffers | Startup default (64 KiB), 128 KiB, 256 KiB | FTP and network test. The Xbox may refuse bigger buffers, and bigger is not always faster. |
| Worker CPU | Automatic, hardware thread 0 to 5 | Network test streams and FTP sessions. USB network work runs on hardware thread 2. |
| Drawing during transfers | Paused (default), Live | FTP and network test. Keep **Paused**: live drawing during Xbox sends has crashed the console in testing. With Paused, an FTP transfer waits until the screen has stopped drawing before any data moves. |
| FTP speed limit | Off (default), 12, 16, 20 MiB/s, or Downloads 12 or 16 MiB/s | All FTP uploads and downloads together, or only downloads (Xbox to PC). Try 16 if the console freezes during fast transfers. One console with the stock 60 GB drive froze at 20 MiB/s and more, but moved 4 GiB at 16 without a freeze. Its full-speed uploads went through; only its full-speed downloads froze. |
| FTP disk I/O | Cached, 1 MiB (default); Uncached, 1 MiB; Uncached, 256 KiB | How FTP reads and writes Xbox drives. Uncached skips the Xbox file cache and gives the drive requests of a fixed size. On one console, Uncached, 256 KiB raised uploads from about 22 to 26.5 MiB/s. It did not stop that console's freezes. |

In `FastFTP360.ini` these are `buffers` (bytes, 0 for the startup default), `cpu` (`auto` or a hardware thread from 0 to 5), `drawing` (`paused` or `live`), `speed` (MiB/s, 0 for off, or `downloads-12` and `downloads-16`) and `disk` (`cached`, `uncached-1m` or `uncached-256k`). A missing or unknown value keeps the default.

## Why it is faster

A simple FTP server reads or writes a chunk of the file, then sends or receives it over the network, then repeats, so the disk and the network take turns waiting. FastFTP360 gives each transfer a helper thread and **two 1 MiB buffers**:

- **Upload:** the network fills one buffer while the helper writes the other to disk. The file is flushed before the transfer is reported as complete.
- **Download:** the helper reads ahead into one buffer while the network sends the other.

Network sends go out in 16 KiB pieces with a short yield in between, which keeps the console responsive during long transfers.

Measured on one console with a TP-Link UE306 USB Ethernet adapter and the [CUsbEnet](https://github.com/marcusisdahl/CUsbEnet) 1.1 acksmall driver:

| | PC to Xbox |
|---|---|
| Network test (RAM to RAM) | about 27.7 MiB/s |
| FastFTP360 upload to the internal drive | about 27.5 MiB/s |
| Aurora's built-in FTP | about 19.5 MiB/s |
| Internal drive on its own (disk test) | about 57 MiB/s |

At this point the Xbox's USB 2 controller is the limit, not the disk or the FTP server. Your numbers depend on the adapter, driver, drive, FTP client and network.

## Safety

- **Nothing is encrypted**, including the login. Use FastFTP360 only on a trusted home network, never port-forward it to the internet, and stop it with **B** when you are done.
- Anyone who logs in can overwrite and delete files on every listed drive, including the system partitions. Keep backups of anything important.
- FastFTP360 is an app you start yourself. It does not install itself, change `launch.ini`, patch the kernel, write NAND, or change your network driver. It writes only the files your FTP client sends, its own `FastFTP360.ini` and `FastFTP360.log`, and the disk test's temporary file.
- Homebrew can still crash or hang the console. Keep a known-good driver and your usual recovery method.

The host tests run the same FTP server code on Windows with Python's `ftplib`. They cover file operations, exact byte counts, path handling, the login, `FastFTP360.ini`, the log, and simultaneous sessions. They are not a substitute for testing on a console. See the [changelog](CHANGELOG.md) for what has and hasn't been tested on hardware, and the [crash investigation](docs/CRASH-INVESTIGATION.md) for an earlier console freeze during network tests.

## Build from source

You need Visual Studio 2022 C++ tools, an Xbox 360 SDK with the RXDK-360 Visual Studio integration (toolset `2010-01`), and .NET Framework 4.5 or later. The SDK is not included.

From PowerShell in the repository:

```powershell
.\build.ps1
```

This produces `build\xbox\FastFTP360.xex` (the Xbox app) and `build\windows\FastFTP360-PC.exe` (the network test's PC app). The `.exe` next to the XEX is an intermediate PowerPC file, not the PC app. You can also open `FastFTP360.sln` and build **Release | Xbox 360**. The project uses `DEVKIT_ONLY=0` for retail consoles and never deploys to a console automatically.

To build the PC app and run the host tests without the Xbox SDK (needs Python 3):

```powershell
.\build.ps1 -HostOnly -Tests
```

`build\host\ui-preview.exe out.bmp <scene>` renders the Xbox screen without a console, and is how the images in this README are made. The network test's wire format is described in [docs/PROTOCOL.md](docs/PROTOCOL.md).

## Project history

FastFTP360 grew out of **NetBench360**, a network benchmark for testing USB Ethernet drivers on the Xbox 360. Its FTP server turned out to be faster than Aurora's, so it became its own project. Version 0.4.0 renamed the XEX, the PC app and the Visual Studio solution to FastFTP, and they were renamed again to FastFTP360 after 0.4.1 so the name is easier to tell apart from other FTP tools.

## License

MIT, see [LICENSE](LICENSE). Xbox 360 SDK components are not included and keep their own terms. This project is not affiliated with Microsoft.
