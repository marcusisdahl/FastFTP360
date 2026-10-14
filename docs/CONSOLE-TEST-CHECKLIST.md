# Console test checklist for FastFTP 0.5.0 (Read test)

Until every item below has passed on ABadAvatar / BadUpdate, the build is **Experimental; ABadAvatar 1.3 / ABadUpdate compatibility not yet verified**.

Read [Recovery](READ-BENCHMARKS.md#recovery) first. Write down for every session: exploit and payload files (names and versions), CUsbEnet build (or built-in port), Aurora version, NetISO/ConnectX files and how they are loaded, server software and OS, and the FastFTP commit.

## Before you start

- [ ] Your working FastFTP folder and its `FastFTP.ini` are left as they are.
- [ ] FastFTP 0.5.0 is in its own folder, e.g. `Hdd1:\Apps\FastFTP-0.5.0\`. Not in `launch.ini`.
- [ ] A fixture is ready: `read-host.exe fixture FFTPFIX-0000002A.iso 4293918720 0x2A` on the PC (under 4 GiB, so it fits on FATX and FAT32); a copy in the server's ISO folder and one in `Hdd1:\FastFTP-Bench\` (upload with the working FastFTP).
- [ ] Server: `python tests\netiso_server.py <ISO folder>` (complete replies). Optionally also `netiso-srv` for comparison.

## 1. Without NetISO or ConnectX loaded

- [ ] FastFTP 0.5.0 starts from Aurora, shows *v0.5.0 experimental*, all five tabs switch with LB/RB.
- [ ] **Back** returns to Aurora. Start it again: still fine.
- [ ] FTP: upload and download a large file; speed similar to 0.4.1 (~27.5 MiB/s upload on UE306 + CUsbEnet).
- [ ] Network test with `FastFTP-PC.exe` 0.5.0: PC to Xbox and Xbox to PC (keep the send cap you normally use for Xbox to PC).
- [ ] Disk test on HDD: completes, file deleted.
- [ ] Read test, ConnectX source: says the module is not loaded; nothing else changes.
- [ ] Read test, Internal HDD: X finds the fixture; Sequential 1 MiB and Random 4 KiB complete with *known data checked* and 0 errors. Note MiB/s and p99.
- [ ] Same with USB 0 if you use a USB drive.
- [ ] Read test, Game disc, with an original disc in the drive: X lists its largest files; Sequential 1 MiB and Random 4 KiB complete with 0 errors. Then with no disc: X says no readable disc and nothing else changes.
- [ ] Read test, NetISO protocol (console plugin **not** loaded): Y sets the server, X lists ISOs, all six workloads complete on the fixture with *known data checked*.
- [ ] Repeat one NetISO workload 3 times: passes 1, 2, 3; no errors.
- [ ] Press B during Measuring for each source: stops within about a second, says *Read test stopped*, a new run works.
- [ ] `FastFTP.log` and `FastFTP-reads.csv` (via FTP `/Game/`) contain every run, including the cancelled ones.

## 2. Failure handling (NetISO protocol)

- [ ] Server not running: X fails within about 5 s with a clear message.
- [ ] Stop the server during a 60 s run: the run fails (short read or timeout) within about 10 s; B and the next run still work.
- [ ] Pull the server's network cable during a run: timeout within about 10 s, then plug back in and run again.
- [ ] Optional: `python tests\netiso_server.py <folder> --fault stall --nth 50`: timeout after ~10 s; `--fault corrupt --nth 50`: integrity failure.

## 3. Each module separately, with CUsbEnet

Load **one** module at a time, the way you normally would. Power-cycle and rerun the exploit between module changes.

NetISO plugin loaded:
- [ ] Aurora starts and a NetISO game still mounts and boots as before (without FastFTP).
- [ ] FastFTP 0.5.0 starts; FTP, network test and the Read test NetISO protocol source work as in section 1.
- [ ] Back to Aurora; a NetISO game still mounts and launches.

ConnectX loaded:
- [ ] Aurora shows the ConnectX share and a game launches from it (without FastFTP).
- [ ] In FastFTP, the ConnectX source says the module is loaded.
- [ ] Set `connectx_folder=` to the path you confirmed, X lists files, a run completes. If the path cannot be opened, write down the exact error. That is the answer to "can a title see ConnectX".
- [ ] Back to Aurora; the ConnectX share still works.

## 4. After testing

- [ ] Return to Aurora and launch a known-good **local** game.
- [ ] Power off, power on, run the exploit as usual: normal boot to Aurora, CUsbEnet works, the working FastFTP works.

## Comparisons (once the above pass)

See [Comparisons worth making](READ-BENCHMARKS.md#comparisons-worth-making): stock Ethernet versus UE306, server SSD versus NAS, first pass versus repeats, and stopwatch timings of the same game local versus network.

Report back with `FastFTP-reads.csv`, `FastFTP.log`, and anything that froze, with the step it happened at.
