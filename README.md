# NetBench360

A standalone **RAM-to-RAM network benchmark** for homebrew-capable Xbox 360 consoles, with a small Xbox screen and a Windows companion. Version 0.1.3 is a **diagnostic build**, prepared after a console freeze/restart during an unlimited Xbox-to-PC run with v0.1.2 and TxScan. The cause remains unknown. It adds yielding sends, a selectable Xbox send limit and optional paused rendering; these are diagnostic controls, not a proven crash fix.

**Update both applications to v0.1.3.** Protocol v2 explicitly negotiates the send limit and rendering option; older versions are rejected. Stop and exit the Xbox app before replacing its application XEX. Neither application needs to be run automatically at boot; the Windows app needs no administrator privileges. See [crash investigation](docs/CRASH-INVESTIGATION.md).

It measures one TCP stream in either direction without reading or writing benchmark data to the Xbox drive. It is useful for comparing CUsbEnet builds and separating network limits from FTP, dashboard, or disk overhead. It measures the application, TCP stack, driver and link together, not raw USB bandwidth. It is a custom protocol, not an iperf server.

## Run a first test

1. For investigation of the freeze, restore the previously working **Baseline CUsbEnet driver** using your existing plugin/recovery method, preserving the current XEX and launch.ini backup. Reboot between driver builds. Copy `NetBench360.xex` into its **own application folder**, for example `Hdd1:\Apps\NetBench360\`. Launch it from Aurora or your usual homebrew launcher. **Do not add this XEX to launch.ini as a plugin.**
2. Press **A** on the Xbox to listen. Note the IP address on screen.
3. Run `NetBench360-PC.exe` on a Windows PC on the same trusted LAN. Enter that IP. The PC app uses .NET Framework 4.5 or newer and requires no installer.
4. For the first diagnostic check, choose **Xbox to PC, 10 seconds, 1 repeat, 64 KiB PC buffers, 16 MiB/s Xbox send limit**, with **Pause Xbox drawing** checked. Set the driver label to the build actually loaded, such as `Baseline`. Click **Start benchmark**. The cap applies to Xbox sending during the integrity check, warm-up and timed transfer.
5. Save the CSV after completion. On Xbox, **B** stops the listener/test; once stopped, **Back** returns to the dashboard. PC **Stop** cancels the current batch and keeps earlier completed results.

The PC label is only a note: it does not detect, install, or switch a driver. The Xbox screen keeps the latest completed rate in each direction. The Windows summary shows medians for matching IP, label, duration and buffer settings in the current session.

With drawing paused, the Xbox deliberately holds a screen saying to use the PC for live progress. Controller B is still polled. Normal rendering resumes when the test finishes or stops. A static Xbox picture in this mode is expected; the PC's live progress and the response to B distinguish it from a frozen console.

NetBench360 uses the console's existing network route. To measure a USB Ethernet adapter, ensure the console is actually using that adapter rather than native Ethernet or Wi-Fi. It does not select the adapter for you. Stop FTP transfers and other traffic while measuring.

## Compare driver builds

First establish that the diagnostic run finishes on the baseline. A **capped result does not measure maximum throughput**. The cap and drawing setting appear in the UI and CSV, and medians do not mix those settings. Do not repeat the unlimited v0.1.2 test while investigating the freeze. If another freeze occurs, stop the experiment and retain the last working driver; host tests cannot make console testing safe.

Once a working control is established, change one variable at a time. Keep duration, repeats, PC buffers, Xbox cap and drawing setting identical when comparing driver builds. Both directions run sequentially if selected. Save separate CSV files and repeat the baseline to check ordinary variation. A few tenths of a MiB/s alone is not convincing evidence of a driver improvement. The Unlimited setting is available for later testing, but was the condition under which the reported freeze occurred.

- If RAM throughput is substantially above FTP throughput, the FTP application and/or storage path likely contributes to the FTP limit. This test alone cannot distinguish those two causes.
- If RAM throughput stays close to FTP, investigate the network path and benchmark/stack overhead before attributing the limit to the drive.
- The buffer selector changes **PC socket buffers**, before connecting. The Xbox requests **64 KiB send/receive defaults at XNet startup** and keeps the accepted socket's actual buffers. Both ends' actual values are recorded; the Xbox's actual size may differ from the request. Keep settings fixed for driver comparisons. A separate PC buffer-size sweep can be useful later.

`MiB/s` uses 1,048,576 bytes per second; multiply by 8.388608 for decimal Mbit/s. Low/high values are receiver samples covering at least one second; the final partial interval is excluded. Overall average includes the whole measured payload transfer and drain.

## What a run does

1. Negotiate direction, time, Xbox send limit and rendering mode over TCP port **36020**, and report actual buffers at both ends.
2. Transfer and validate **1 MiB** of known data outside the timed measurement.
3. Transfer a **32 MiB warm-up**, also outside the measurement.
4. Reuse 64 KiB memory blocks for the requested duration, with individual sends limited to 16 KiB and explicit yields. There is no growing RAM file. The C++ session uses two 64 KiB buffers, plus socket and UI memory. The Xbox sender waits between blocks when capped; software scheduling may produce a rate below the selected limit.
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
