# v0.1 validation

Built on Windows using VS 2022 Community with installed RXDK-360 integration, Xbox PowerPC compiler 16.00.11886.00, and .NET Framework C# compiler. Xbox build succeeds with `DEVKIT_ONLY=0`. Deployment is excluded in the project.

Build reports three warnings: MSB8003 for the integration's missing WindowsSDKDir property, and two C4127 constant-condition warnings from SDK `FD_SET` macro expansions. There are no compile or link errors. These warnings do not substitute for a hardware run.

`tests/protocol.py` passes 21 checks:

- Cross-language transfers in both directions with equal sender/receiver payload totals and final receiver rates.
- Cancellation discards an unfinished run; the next connection succeeds.
- Nine invalid request cases rejected (magic, version, direction, durations, chunk, buffer, warm-up bounds).
- Fragmented request accepted; corrupt integrity data rejected.
- Clean disconnect without a finish record rejected; truncated request rejected.
- Forged receiver result rejected by both engines.
- Truncated response, wrong protocol and corrupt integrity data rejected by the Windows client.

The Xbox screen layout is rendered locally using the same layout source, without Xbox graphics calls. The Windows form is visually checked and exercised with the native loopback harness: six 20-second runs completed (three per direction), each transferring more than 4 GiB and exercising 64-bit counters. CSV export produced six rows; recomputed medians matched the screen. Empty IP validation and the Stop button were checked; the cancelled run was discarded while completed rows remained. Loopback rates are PC-only test data, not Xbox or USB performance results.

**Still untested:** launching the XEX on a real Xbox, runtime shader creation and display modes, controller input on hardware, retail homebrew XNet socket/half-close behavior, route selection, CUsbEnet interaction, and actual sustained USB throughput. A successful build and host tests cannot establish hardware stability.

First console test: keep the known-working driver loaded, launch this app manually, run one 10-second test in each direction at 64 KiB, verify both completed results, then press B and Back. Do not configure it as a boot plugin. Only proceed to repeated driver comparisons after this succeeds.
