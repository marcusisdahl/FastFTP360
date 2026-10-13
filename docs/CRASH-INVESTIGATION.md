# Console freeze investigation

## Observation

With NetBench360 v0.1.2 and the user-labeled TxScan driver, a PC-to-Xbox run completed at 20.68 MiB/s with the 1 MiB pre-test check passed. During the following Xbox-to-PC run, the PC's last progress update showed 120.2 MiB at 5.8 seconds. It then reported 15 seconds without data. The user confirmed that the console froze or restarted. The interrupted run has no valid final throughput result.

There is no console stack trace, kernel debug log or baseline-driver RAM comparison. The evidence does not identify whether the benchmark, driver, networking stack, rendering or a timing interaction caused the freeze. The generic PC timeout is the consequence of data stopping.

## Source review and v0.1.3 controls

- Test memory is fixed at two 64 KiB blocks and freed after the session. The sender's pattern is immutable while sending. Socket access belongs to one worker; cancellation does not close it from another thread. No accumulating payload allocation or obvious buffer overrun was found in the reviewed paths. This is not proof that the code is crash-free.
- A real hardening opportunity was found: a writable `select` followed by a would-block `send` could retry without sleeping. v0.1.3 yields on successful C++ sends, sleeps on would-block retries, and sends in at most 16 KiB calls. These changes reduce uninterrupted retry work; they are not an established explanation for the freeze.
- The installed SDK documents that DrawPrimitiveUP copies its input before returning, so the reused CPU vertex array is not itself an identified lifetime defect. Paused rendering is added to isolate concurrent GPU work. The worker waits for the main thread's GPU-idle acknowledgement before transferring data; real Xbox coordination still needs hardware validation.
- A selectable Xbox send cap defaults to 16 MiB/s and covers the integrity phase, warm-up and timed phase. It is explicitly acknowledged and reported in both interfaces/CSV. Capped results cannot establish maximum bandwidth.
- The reviewed driver scheduler retains copying into DMA buffers, ordering barriers, the existing NIC lock, submission priority and queue limits. No confirmed driver defect explaining this freeze was found. The scheduling equivalence test does not cover real USB/DMA concurrency. TxScan is still an experiment.

CUsbEnet, kernel hooks, USB queue parameters and AX88179 aggregation were not modified for v0.1.3. No new console test or deployment was performed by the agent.

## Next controlled hardware check

Use the previously working Baseline driver first, using the same existing recovery/plugin procedure and rebooting between driver builds. Preserve driver/XEX and launch.ini backups. Install both v0.1.3 benchmark applications. Choose Xbox-to-PC only, 10 seconds, one repeat, 64 KiB PC buffers, 16 MiB/s limit, and paused drawing. Label the actual driver Baseline.

If it completes, save the CSV and record whether B/exit still work. That establishes a control for this short, capped workload only. It does not prove the freeze was fixed. Change one variable at a time afterward; do not treat a cap-limited rate as a speed improvement. If the console freezes again, stop further runs and retain the last working driver.
