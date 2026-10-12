# NB36 protocol v1

All fields are unsigned **big endian**, including the 64-bit counters. A connection contains one test. Direction 1 means PC sends, Xbox receives; direction 2 means Xbox sends, PC receives. Single TCP stream, port 36020.

The 32-byte request contains eight uint32 fields: magic `0x4E423336`, version `1`, direction, duration seconds, chunk bytes `65536`, requested socket bytes, warm-up MiB, and pattern seed. Allowed duration 1-60, buffers 16384/32768/65536/131072, warm-up 1-64. Reject unsupported fields before allocating large or unbounded resources.

As of application v0.1.2, requested socket bytes describe the PC setting. The Xbox does not resize an accepted socket: it requests 64 KiB defaults at XNet startup and reports actual send/receive sizes in the response. This preserves protocol v1 compatibility; clients must use actual sizes rather than assuming the Xbox honored the PC request.

The 32-byte response contains eight uint32 fields: magic, version, status (0 accepted, 1 invalid request), chunk bytes, actual Xbox send buffer, actual Xbox receive buffer, integrity bytes `1048576`, warm-up MiB. On rejection, fields after status are zero and the connection closes.

The pattern is xorshift32, initialized to the supplied seed (zero becomes one): `s ^= s << 13; s ^= s >> 17; s ^= s << 5`, with uint32 wrapping; each output byte is the low byte after the three operations. Generate 65536 bytes once and repeat that block. Integrity and warm-up use exact block counts. After each phase, the receiver sends a four-byte zero ACK; a failure closes the connection. Only the integrity phase validates bytes.

The timed sender loops through whole 65536-byte blocks until its timer reaches the requested duration. Then it sends this **32-byte finish record** and shuts down only its send half:

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
| 56 | 8 | Reserved, zero |

The sender validates result magic/version/direction/status, equal payload bytes, nonzero elapsed time no more than 180 seconds, and integrity phase size. Its displayed final average uses the receiver's result. Local live rate while sending is provisional. All partial TCP reads/writes are handled; cancellation is polled at most every 100 ms while waiting. One worker owns the socket throughout its lifetime.

This is a diagnostic protocol for cooperating trusted peers. It is not authenticated, and byte-count agreement or a finish record is not cryptographic proof of valid payload.
