# NB36 protocol v3 (application v0.2.0)

All fields are unsigned **big endian**, including the 64-bit counters. A connection contains one test. Direction 1 means PC sends, Xbox receives; direction 2 means Xbox sends, PC receives. One TCP stream per connection, port 36020.

The 40-byte request contains ten uint32 fields: magic `0x4E423336`, version `3`, direction, duration seconds, chunk bytes `65536`, requested PC socket bytes, warm-up MiB, pattern seed, Xbox send limit in MiB/s, and diagnostic options. Allowed duration 1-60, buffers 0/16384/32768/65536/131072 (0 means the PC kept its automatic buffers), warm-up 1-64. Limits allowed: 0 (unlimited), 8, 12, 16, 20, 24, 32. Option bit 0 means pause Xbox rendering; other bits are rejected. The direction field's low byte is the direction; bit `0x100` asks the receiver to check every timed payload byte against the pattern. Other direction bits must be zero. Reject unsupported fields before allocating large or unbounded resources.

Requested socket bytes describe the PC setting. The Xbox does not resize an accepted socket: it requests 64 KiB defaults at XNet startup and reports actual send/receive sizes. Clients must use actual sizes rather than assuming the Xbox honored the PC request.

The 40-byte response contains ten uint32 fields: magic, version, status (0 accepted, 1 invalid request), chunk bytes, actual Xbox send buffer, actual Xbox receive buffer, integrity bytes `1048576`, warm-up MiB, effective Xbox send limit, and acknowledged options. Effective limit is zero for PC-to-Xbox because that sender is the PC. On rejection, fields after status are zero and the connection closes. An unsupported magic/version is rejected after reading the first 32 bytes, with a 32-byte error response; clients validate that base response before reading the eight-byte settings extension. This fails promptly for older clients rather than waiting for fields they never send.

When pausing is requested, the Xbox main thread renders one static status screen and waits for the GPU to become idle. The worker waits for that acknowledgement before sending its acceptance response or benchmark data. The main thread continues polling controllers but draws no more frames during that session. The settings are diagnostic metadata, not a security boundary.

The pattern is xorshift32, initialized to the supplied seed (zero becomes one): `s ^= s << 13; s ^= s >> 17; s ^= s << 5`, with uint32 wrapping; each output byte is the low byte after the three operations. Generate 65536 bytes once and repeat that block. Integrity and warm-up use exact block counts. After each phase, the receiver sends a four-byte zero ACK; a failure closes the connection. Only the integrity phase validates bytes.

The timed sender loops through whole 65536-byte blocks until its timer reaches the requested duration. Individual send calls use at most 16384 bytes, handle partial writes, yield after success in the C++ engine, and sleep briefly before retrying would-block. When capped, Xbox sending is paced after each block in all three data phases; cap waiting is included in the timed measurement. Then the sender sends this **32-byte finish record** and shuts down only its send half:

| Offset | Width | Field |
|---|---|---|
| 0 | 4 | Magic |
| 4 | 4 | Version |
| 8 | 4 | Direction |
| 12 | 4 | Zero |
| 16 | 8 | Payload bytes sent, excluding record |
| 24 | 8 | Sender elapsed microseconds before record |

The receiver retains the final 32 stream bytes until EOF. It validates the record, payload count and full-block alignment, and sender elapsed time between requested duration and duration+15 seconds. No record means cancellation/interruption, even if TCP ends cleanly. The receiver removes the record from its payload count and replies on the still-open reverse half with this 64-byte result:

| Offset | Width | Field |
|---|---|---|
| 0 | 4 | Magic |
| 4 | 4 | Version |
| 8 | 4 | Direction |
| 12 | 4 | Status, zero |
| 16 | 8 | Receiver payload bytes |
| 24 | 8 | Receiver elapsed microseconds |
| 32 | 4 | Minimum sample rate, integer KiB/s |
| 36 | 4 | Maximum sample rate, integer KiB/s |
| 40 | 4 | Xbox actual send buffer |
| 44 | 4 | Xbox actual receive buffer |
| 48 | 4 | Full interval sample count |
| 52 | 4 | Integrity bytes, 1048576 |
| 56 | 4 | Effective Xbox send limit, MiB/s |
| 60 | 4 | Acknowledged diagnostic options |
| 64 | 8 | Timed payload bytes checked against the pattern; zero unless the full data check was requested |

The sender validates result magic/version/direction/status, equal payload bytes, nonzero elapsed time no more than 180 seconds, integrity phase size, send limit and options. Its displayed final average uses the receiver's result. Local live rate while sending is provisional. All partial TCP reads/writes are handled; cancellation is polled at most every 100 ms while waiting, and during pacing. One worker owns the socket throughout its lifetime. When the full data check was requested, the sender also requires the checked byte count to equal the payload count.

A client may open up to four connections at once; each is an independent session. The Xbox app serves them in parallel and shows their summed receiver rates. The Windows app starts every stream's timed phase together by holding its warm-up ACK (as receiver) or its first timed block (as sender) until all streams are ready. The Xbox send limit applies to each connection, so four capped streams may send up to four times the limit in total.

This is a diagnostic protocol for cooperating trusted peers. It is not authenticated, and byte-count agreement or a finish record is not cryptographic proof of valid payload.
