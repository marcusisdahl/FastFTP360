# NetBench360

A standalone **RAM-to-RAM network benchmark** for homebrew-capable Xbox 360 consoles, with a small Xbox screen and a Windows companion. Version 0.1.2 addresses the console-reported LAN-option and receive-buffer setup failures, adds listener port reuse, and reports precise listener errors. The Xbox UI and listener have been exercised on hardware; timed console transfers still need validation.

**Updating from v0.1 or v0.1.1:** stop and exit the Xbox app, replace its application XEX with the v0.1.2 XEX, and relaunch it. The previous PC clients remain protocol-compatible; the updated companion clarifies the buffer setting. Changing only the PC executable will not fix the Xbox setup errors. Neither application needs to be run automatically at boot; the Windows app needs no administrator privileges.

It measures one TCP stream in either direction without reading or writing benchmark data to the Xbox drive. It is useful for comparing CUsbEnet builds and separating network limits from FTP, dashboard, or disk overhead. It measures the application, TCP stack, driver and link together, not raw USB bandwidth. It is a custom protocol, not an iperf server.

## Run a first test

1. Leave the currently working network driver installed. Copy `NetBench360.xex` into its **own application folder**, for example `Hdd1:\Apps\NetBench360\`. Launch it from Aurora or your usual homebrew launcher. **Do not add this XEX to launch.ini as a plugin.**
2. Press **A** on the Xbox to listen. Note the IP address on screen.
3. Run `NetBench360-PC.exe` on a Windows PC on the same trusted LAN. Enter that IP. The PC app uses .NET Framework 4.5 or newer and requires no installer.
4. For the first check, choose **Both directions, 10 seconds, 1 per direction, 64 KiB**. Set the driver label to the build actually loaded, such as `TxScan`. Click **Start benchmark**.
5. Save the CSV after completion. On Xbox, **B** stops the listener/test; once stopped, **Back** returns to the dashboard. PC **Stop** cancels the current batch and keeps earlier completed results.

The PC label is only a note: it does not detect, install, or switch a driver. The Xbox screen keeps the latest completed rate in each direction. The Windows summary shows medians for matching IP, label, duration and buffer settings in the current session.

NetBench360 uses the console's existing network route. To measure a USB Ethernet adapter, ensure the console is actually using that adapter rather than native Ethernet or Wi-Fi. It does not select the adapter for you. Stop FTP transfers and other traffic while measuring.

## Compare driver builds

After the first test works, use **20 seconds, 3 repeats per direction, 64 KiB** for every build. Both directions run sequentially, alternating within each repeat. Save a separate CSV for each driver. Compare the medians and the spread of individual runs, then repeat the baseline to check for ordinary variation. A few tenths of a MiB/s alone is not convincing evidence of a driver improvement.

- If RAM throughput is substantially above FTP throughput, the FTP application and/or storage path likely contributes to the FTP limit. This test alone cannot distinguish those two causes.
- If RAM throughput stays close to FTP, investigate the network path and benchmark/stack overhead before attributing the limit to the drive.
- The buffer selector changes **PC socket buffers**, before connecting. The Xbox requests **64 KiB send/receive defaults at XNet startup** and keeps the accepted socket's actual buffers. Both ends' actual values are recorded; the Xbox's actual size may differ from the request. Keep settings fixed for driver comparisons. A separate PC buffer-size sweep can be useful later.

`MiB/s` uses 1,048,576 bytes per second; multiply by 8.388608 for decimal Mbit/s. Low/high values are receiver samples covering at least one second; the final partial interval is excluded. Overall average includes the whole measured payload transfer and drain.

## What a run does

1. Negotiate direction and time over TCP port **36020**, and report the actual buffers at both ends.
2. Transfer and validate **1 MiB** of known data outside the timed measurement.
3. Transfer a **32 MiB warm-up**, also outside the measurement.
4. Reuse 64 KiB memory blocks for the requested duration. There is no growing RAM file. The C++ session uses two 64 KiB buffers, plus socket and UI memory.
5. Send a completion record, half-close the sending stream, and drain it at the receiver. The receiver reports payload bytes and elapsed time; the sender checks that byte totals agree. An interrupted stream without the completion record fails.

The initial integrity check is a smoke test. **Timed payload is not checked byte by byte**, so a passed run is not proof that a driver can never corrupt data. The 32-byte completion record is excluded from final payload bytes; its transmission and TCP drain are included in receiver elapsed time. Current-rate samples can include those final 32 bytes, a negligible difference; the final average excludes them.

Tests stop on errors, 15 seconds without socket activity, or a 180-second session limit. Requests are bounded to 60 seconds, 64 KiB transfer blocks, 16/32/64/128 KiB socket buffers and 1-64 MiB warm-up. The UI offers a narrower set of normal test durations. Actual buffers at both ends are recorded in CSV.

## Scope and recovery

This is an application, not a kernel plugin. It does not patch hooks, modify USB queues, enable AX88179 aggregation, write NAND, edit launch.ini, install itself, or write test data on Xbox. The project explicitly excludes deployment from builds. Nothing is automatically copied to a console.

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
