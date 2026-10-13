# NetBench360

A standalone **RAM-to-RAM network benchmark** for homebrew-capable Xbox 360 consoles, with a small Xbox screen and a Windows companion. Version 0.2.0 adds a full data check, parallel TCP streams, automatic PC socket buffers and Xbox-side buffer and CPU settings for finding where throughput stops. It keeps the 0.1.3 diagnostic controls (yielding sends, a selectable Xbox send limit and optional paused rendering), added after a console freeze/restart during an unlimited Xbox-to-PC run with v0.1.2 and TxScan. The cause of that freeze remains unknown, and the 0.2.0 features still need console validation.

**Update both applications to v0.2.0.** Protocol v3 explicitly negotiates the send limit and rendering option; older versions are rejected. Stop and exit the Xbox app before replacing its application XEX. Neither application needs to be run automatically at boot; the Windows app needs no administrator privileges. See [crash investigation](docs/CRASH-INVESTIGATION.md).

It measures one to four TCP streams in either direction without reading or writing benchmark data to the Xbox drive. A separate Xbox-only disk test (below) measures drive write and read speed without the network. It is useful for comparing CUsbEnet builds and separating network limits from FTP, dashboard, or disk overhead. It measures the application, TCP stack, driver and link together, not raw USB bandwidth. It is a custom protocol, not an iperf server.

## Run a first test

1. For investigation of the freeze, restore the previously working **Baseline CUsbEnet driver** using your existing plugin/recovery method, preserving the current XEX and launch.ini backup. Reboot between driver builds. Copy `NetBench360.xex` into its **own application folder**, for example `Hdd1:\Apps\NetBench360\`. Launch it from Aurora or your usual homebrew launcher. **Do not add this XEX to launch.ini as a plugin.**
2. Press **A** on the Xbox to listen. Note the IP address on screen.
3. Run `NetBench360-PC.exe` on a Windows PC on the same trusted LAN. Enter that IP. The PC app uses .NET Framework 4.5 or newer and requires no installer.
4. For the first diagnostic check, choose **Xbox to PC, 10 seconds, 1 repeat, 64 KiB PC buffers, 16 MiB/s Xbox send limit**, with **Pause Xbox drawing** checked. Set the driver label to the build actually loaded, such as `Baseline`. Click **Start benchmark**. The cap applies to Xbox sending during the integrity check, warm-up and timed transfer.
5. Save the CSV after completion. On Xbox, **B** stops the listener/test; once stopped, **Back** returns to the dashboard. PC **Stop** cancels the current batch and keeps earlier completed results.

The PC label is only a note: it does not detect, install, or switch a driver. The Xbox screen keeps the latest completed rate in each direction. The Windows summary shows medians for matching IP, label, duration and buffer settings in the current session.

With drawing paused, the Xbox deliberately holds a screen saying to use the PC for live progress. Controller B is still polled. Normal rendering resumes when the test finishes or stops. A static Xbox picture in this mode is expected; the PC's live progress and the response to B distinguish it from a frozen console.

NetBench360 uses the console's existing network route. To measure a USB Ethernet adapter, ensure the console is actually using that adapter rather than native Ethernet or Wi-Fi. It does not select the adapter for you. Stop FTP transfers and other traffic while measuring.

## FTP server (Xbox)

A small built-in FTP server for comparing with Aurora's FTP using the same driver, drive and PC. With the Xbox app stopped, press **LB**. The screen shows the folders and the address; connect an FTP client to that IP on port **21** in **passive mode**, with any user name and password. **B** stops the server.

- `HDD`, `USB0`, `USB1`: the internal drive and USB drives that are present. `APP`: the drive NetBench360 was launched from.
- `NULL`: uploads are discarded, and downloading `NULL/64MB.bin`, `NULL/256MB.bin` or any `NULL/<n>MB.bin` (1-4096) streams test data. This measures FTP over the network without the drive.
- The Xbox shows the live rate and the last transfer's average. The client log shows the same in the `226` reply.

Suggested comparison, same file and PC each time: upload to `NULL`, then to `HDD`, then to the USB drive; download `NULL/1024MB.bin`, then a file from `HDD`. Compare with the same transfers through Aurora. `NULL` close to the RAM benchmark means the FTP code is not the limit; `HDD` well below `NULL` points at the drive.

There is **no authentication or encryption**, as with the benchmark listener. Use it only on a trusted LAN and stop it with B when finished. It can delete and overwrite files on the drives it lists.

## Disk test (Xbox only)

Use it to see whether FTP is limited by the drive rather than the network. With the Xbox app stopped (not listening):

1. Press **Y** to choose the drive: `APP DRIVE` (wherever NetBench360.xex was launched from), `HDD` (internal hard drive), `USB 0` or `USB 1`.
2. Press **RB** to choose the block size: 64 KiB, 256 KiB or 1 MiB. Small blocks are closer to how many FTP servers write.
3. Press **X**. The app writes a 256 MiB file `NETBENCH360-DISKTEST.TMP` and flushes it, reads it back without the file cache, compares every byte, then deletes the file. **B** stops it and also deletes the file.

The result line shows write and read MiB/s. The drive needs 256 MiB free. Compare: if disk write is well above the network RAM rate, the drive is not the FTP limit; if FTP to that drive is close to its disk write rate, the drive is the limit. FTP to a USB drive also shares the Xbox's USB with a USB Ethernet adapter, so it can be slower than either test alone.

`HDD` and `USB` are mounted through a temporary kernel symbolic link that is removed afterwards. If a drive is absent or not a FATX device, the screen shows `NOT FOUND OR NOT MOUNTABLE` with a status code. The test writes only its own temporary file.

## Compare driver builds

First establish that the diagnostic run finishes on the baseline. A **capped result does not measure maximum throughput**. The cap and drawing setting appear in the UI and CSV, and medians do not mix those settings. Do not repeat the unlimited v0.1.2 test while investigating the freeze. If another freeze occurs, stop the experiment and retain the last working driver; host tests cannot make console testing safe.

Once a working control is established, change one variable at a time. Keep duration, repeats, PC buffers, Xbox cap and drawing setting identical when comparing driver builds. Both directions run sequentially if selected. Save separate CSV files and repeat the baseline to check ordinary variation. A few tenths of a MiB/s alone is not convincing evidence of a driver improvement. The Unlimited setting is available for later testing, but was the condition under which the reported freeze occurred.

- If RAM throughput is substantially above FTP throughput, the FTP application and/or storage path likely contributes to the FTP limit. This test alone cannot distinguish those two causes.
- If RAM throughput stays close to FTP, investigate the network path and benchmark/stack overhead before attributing the limit to the drive.
- The buffer selector changes **PC socket buffers**, before connecting. The Xbox requests **64 KiB send/receive defaults at XNet startup** and keeps the accepted socket's actual buffers. Both ends' actual values are recorded; the Xbox's actual size may differ from the request. Keep settings fixed for driver comparisons. A separate PC buffer-size sweep can be useful later.

`MiB/s` uses 1,048,576 bytes per second; multiply by 8.388608 for decimal Mbit/s. Low/high values are receiver samples covering at least one second; the final partial interval is excluded. Overall average includes the whole measured payload transfer and drain.

## Find the bottleneck

- **Full data check.** Set **Data check** to **All data** to compare every timed byte, not only the first 1 MiB. Use it whenever you test an experimental driver build, such as CUsbEnet with `USBENET_AX88179_TX_AGGREGATION=1`. It costs some CPU on the receiver, so compare rates with the same setting.
- **Parallel streams.** Run the same test with 1, 2 and 4 streams. If 2 or 4 streams are clearly faster than 1, one TCP connection's window is the limit, not the driver or USB. If the total stays flat, look at the driver and USB path. The Xbox send limit applies to each stream, so set it to Unlimited (once the Baseline control run has passed) or account for streams × limit.
- **PC socket buffers.** **Windows automatic** leaves receive-window autotuning on. Any fixed size turns it off for that connection.
- **Xbox settings (D-pad, while stopped).** Up/Down cycles the listener's socket buffers: the 64 KiB startup default, 128 KiB or 256 KiB. Accepted connections inherit them; the actual sizes are shown in the CSV. Left/Right picks the hardware thread for stream workers: automatic or thread 0-5. USB completions run on hardware thread 2, so compare 2 with another thread. An error naming the buffer size means XNet refused it.

## What a run does

1. Negotiate direction, time, Xbox send limit and rendering mode over TCP port **36020**, and report actual buffers at both ends.
2. Transfer and validate **1 MiB** of known data outside the timed measurement.
3. Transfer a **32 MiB warm-up**, also outside the measurement.
4. Reuse 64 KiB memory blocks for the requested duration, with individual sends limited to 16 KiB and explicit yields. There is no growing RAM file. Each C++ stream uses two 64 KiB buffers, plus socket and UI memory. The Xbox sender waits between blocks when capped; software scheduling may produce a rate below the selected limit.
5. Send a completion record, half-close the sending stream, and drain it at the receiver. The receiver reports payload bytes and elapsed time; the sender checks that byte totals agree. An interrupted stream without the completion record fails.

The initial integrity check is a smoke test. **Timed payload is not checked byte by byte unless you choose the All data check**, and even then a passed run is not proof that a driver can never corrupt data. The 32-byte completion record is excluded from final payload bytes; its transmission and TCP drain are included in receiver elapsed time. Current-rate samples can include those final 32 bytes, a negligible difference; the final average excludes them.

Tests stop on errors, 15 seconds without socket activity, or a 180-second session limit. Requests are bounded to 60 seconds, 64 KiB transfer blocks, automatic or 16/32/64/128 KiB PC socket buffers, 1-64 MiB warm-up and four parallel streams. The UI offers a narrower set of normal test durations. Actual buffers at both ends are recorded in CSV.

## Scope and recovery

This is an application, not a kernel plugin. It does not patch hooks, modify USB queues, enable AX88179 aggregation, write NAND, edit launch.ini, install itself, or write network test data on Xbox. The disk test writes only its own temporary file, which it deletes. The FTP server writes what an FTP client sends it. The project explicitly excludes deployment from builds. Nothing is automatically copied to a console.

Keep your known-good CUsbEnet XEX and launch.ini backup. Homebrew code and the loaded kernel driver can still crash or hang; zero risk cannot be guaranteed. If the app hangs, stop the PC test and restart the console using your existing recovery procedure. Since this app is launched manually, it is not configured to restart at boot. Stop testing a driver if an integrity failure occurs.

The listener is armed only after A and has **no authentication or encryption**. Use a trusted LAN, do not forward its port to the internet, and press B when finished. NetBench360 does not change firewall settings. A PC firewall prompt is not normally needed for its outbound client connection; any network permissions remain yours to manage.

## Build

Requirements: Visual Studio 2022 C++ tools, RXDK-360 Xbox 360 platform integration/toolset `2010-01`, your own installed compatible SDK, and .NET Framework compiler. The SDK is not included. A normal modern Windows C++ toolchain cannot produce the Xbox binary on its own.

From PowerShell in this repository:

```powershell
.\build.ps1
```

Outputs: `build\xbox\NetBench360.xex` and `build\windows\NetBench360-PC.exe`. The `.exe` beside the Xbox XEX is the intermediate PowerPC image, not the Windows companion. Open `NetBench360.sln` in Visual Studio to build the Xbox project using `Release | Xbox 360`; use **Build**, not a launch/debug command. `DEVKIT_ONLY=0` is set for homebrew retail targeting.

To build only the Windows companion and the native host test harness, without an Xbox SDK:

```powershell
.\build.ps1 -HostOnly -Tests
```

Tests additionally require Python 3 and the VS C++ tools. They run the **same C++ session source** on Windows loopback against the **same C# engine** used by the GUI. See [validation notes](docs/VALIDATION.md) and [wire protocol](docs/PROTOCOL.md).

The Xbox UI uses original bitmap lettering and basic rectangles; there are no SDK sample assets. Source is MIT licensed. SDK components remain subject to their own terms and are not included in the source repository.
