// SPDX-License-Identifier: MIT
#pragma once
// Read benchmark: one workload engine shared by every read backend (a local or
// ConnectX file, or an ISO read directly from a NetISO server). Nothing here
// writes to the file or the server; it only reads.
#include "bench.h"

// printf format for U64 on both the Xbox/Windows CRT and the POSIX host tests.
#ifdef _MSC_VER
#define NB_U64 "%I64u"
#else
#define NB_U64 "%llu"
#endif

namespace nb {
enum { ReadMaxRequest = 1048576, ReadAlign = 4096, ReadWarmupSeconds = 3, ReadMaxErrors = 16,
       NetIsoPort = 4323, NetIsoSector = 2048, NetIsoHeader = 20, NetIsoNameBytes = 256, NetIsoMaxIsos = 64,
       ReadNameMax = 256, ReadFolderMax = 96, ReadMaxFiles = 64 };

// Workloads: sequential 64 KiB / 256 KiB / 1 MiB, random 4 KiB / 16 KiB / 64 KiB.
enum ReadWorkload { ReadSeq64K, ReadSeq256K, ReadSeq1M, ReadRand4K, ReadRand16K, ReadRand64K, ReadWorkloadCount };
DWORD WorkloadRequest(int workload);
bool WorkloadRandom(int workload);
const char* WorkloadName(int workload);

// Run outcome. Completed runs have valid throughput; the others do not.
enum ReadStatus { ReadCompleted, ReadCancelled, ReadFailed };
enum ReadPhase { ReadIdle, ReadSetup, ReadWarming, ReadMeasuring };
// What the data check could establish.
enum ReadVerify { VerifyOff, VerifyCounts, VerifyKnownData };

// Deterministic 64-bit generator (splitmix64) for random offsets.
struct ReadRandom {
    U64 state;
    explicit ReadRandom(U64 seed) : state(seed) {}
    U64 Next();
};

// Latency histogram in microseconds with bounded memory (about 4 KiB). Values
// below 64 us are exact; above, each bucket spans 1/32 of its power of two,
// so percentiles are approximate to within about 3%. Mean and maximum are exact.
class LatencyHistogram {
public:
    enum { Linear = 64, SubBuckets = 32, Exponents = 32, Buckets = Linear + SubBuckets * Exponents };
    LatencyHistogram();
    void Reset();
    void Add(U64 microseconds);
    U64 Count() const { return count_; }
    double Mean() const { return count_ ? double(total_) / double(count_) : 0; }
    U64 Maximum() const { return maximum_; }
    // Upper edge of the bucket holding the requested rank, capped at the maximum.
    U64 Percentile(double fraction) const;
    static int Bucket(U64 microseconds);
    static U64 BucketHigh(int bucket);
private:
    DWORD counts_[Buckets];
    U64 count_, total_, maximum_;
};

// Known-data fixtures. Every 32-bit big-endian word at byte offset 4*w holds
// FixtureWord(seed, w). The content is defined by this formula, not by any
// copy of the file, so reads can be checked independently of the data path.
// Fixture files are named FFTPFIX-<8 hex digit seed>.<ext>.
DWORD FixtureWord(DWORD seed, U64 word);
void FixtureFill(unsigned char* data, DWORD seed, U64 offset, DWORD length);
// Returns the index of the first wrong byte, or -1 if all match.
long FixtureCheck(const unsigned char* data, DWORD seed, U64 offset, DWORD length);
bool FixtureSeed(const char* name, DWORD& seed);

// One read backend. Read must return the bytes actually read (0..length) or -1.
// A backend that cannot recover after an error says so through Fatal().
class ReadBackend {
public:
    virtual ~ReadBackend() {}
    virtual const char* Name() const = 0;
    virtual U64 Size() const = 0;
    // Reads length bytes at offset. kind receives a reason on failure.
    virtual int Read(U64 offset, DWORD length, unsigned char* data, int& kind) = 0;
    // True when the connection or handle can no longer be used after an error.
    virtual bool Fatal() const = 0;
    virtual const char* Error() const = 0;
};
enum ReadErrorKind { ReadErrorNone, ReadErrorFailed, ReadErrorShort, ReadErrorTimeout, ReadErrorCancelled };

struct ReadConfig {
    int workload;
    DWORD seconds, warmup, seed;
    // verify checks byte counts always and, for a fixture, every byte.
    bool verify, fixture;
    DWORD fixtureSeed;
    ReadConfig();
};
struct ReadResult {
    int status, verify;
    U64 bytes, operations, fileSize, wraps, startOffset;
    double seconds, verifySeconds, setupSeconds;
    DWORD failed, shortReads, timeouts, integrity;
    double latencyMean, latencyP50, latencyP95, latencyP99, latencyMax; // milliseconds
    char error[120];
    ReadResult();
    double Rate() const;       // MiB/s over the measurement, verification excluded
    double Operations() const; // reads per second
};
struct ReadObserver {
    void* context;
    bool (*cancelled)(void*);
    // Phase, bytes and seconds so far in that phase, current MiB/s.
    void (*progress)(void*, int phase, U64 bytes, double seconds, double rate);
};

// Runs warm-up then measurement on an open backend. buffer must hold
// ReadMaxRequest bytes and, for unbuffered local reads, be page aligned.
// Returns the run status; the result holds the details either way.
int RunReadBenchmark(ReadBackend& backend, const ReadConfig& config, const ReadObserver& observer,
                     unsigned char* buffer, ReadResult& result);

// Local or ConnectX file opened read-only. noBuffering bypasses the Xbox file
// cache and needs sector-aligned reads (the engine always aligns to 4 KiB).
class FileReadBackend : public ReadBackend {
public:
    FileReadBackend();
    ~FileReadBackend();
    bool Open(const char* path, bool noBuffering, bool random);
    void Close();
    const char* Name() const { return "file"; }
    U64 Size() const { return size_; }
    int Read(U64 offset, DWORD length, unsigned char* data, int& kind);
    bool Fatal() const { return false; }
    const char* Error() const { return error_; }
private:
    FileReadBackend(const FileReadBackend&);
    FileReadBackend& operator=(const FileReadBackend&);
    HANDLE file_;
    U64 size_;
    char error_[120];
};

// Client for the NetISO server protocol, as implemented by tuxuser/netiso-srv
// (src/main.rs): 20-byte big-endian requests "ISVR", u16 command, u16 ISO
// index, u64 byte offset, u32 length. One request is outstanding at a time.
// It talks to the server directly and does not use the console's NetISO plugin.
class NetIsoClient : public ReadBackend {
public:
    enum Command { Ping = 0, GetIsoSize = 1, HasType1File = 2, ReadData = 3, GetIsoName = 4, MountIso = 5 };
    NetIsoClient(const ReadObserver& observer, double timeoutSeconds);
    ~NetIsoClient();
    // address is IPv4 in network order.
    bool Connect(DWORD address, int port, double connectSeconds);
    bool PingServer();
    // Lists ISO names by index until the first empty name. Names longer than
    // the 256-byte reply are reported as an error rather than cut.
    bool ListIsos(char names[][ReadNameMax], int capacity, int& count);
    // Mounts by name on this connection and reads its size. The reference
    // server matches by filename suffix, so a name that is the end of another
    // listed name is refused as ambiguous.
    bool Mount(const char* name, const char names[][ReadNameMax], int count);
    void Close();
    const char* Name() const { return "NetISO protocol"; }
    U64 Size() const { return size_; }
    int Read(U64 offset, DWORD length, unsigned char* data, int& kind);
    bool Fatal() const { return broken_; }
    const char* Error() const { return error_; }
    // Encodes a request header; exposed for host tests.
    static void Header(unsigned char* out, int command, DWORD index, U64 offset, DWORD length);
private:
    NetIsoClient(const NetIsoClient&);
    NetIsoClient& operator=(const NetIsoClient&);
    bool Wait(bool writing, double deadline, int& kind);
    bool SendAll(const unsigned char* data, int count, double deadline, int& kind);
    bool ReceiveAll(unsigned char* data, DWORD count, double deadline, DWORD& received, int& kind);
    bool Fail(int kind, const char* format, ...);
    const ReadObserver& observer_;
    SOCKET socket_;
    double timeout_;
    U64 size_;
    bool broken_;
    char error_[120];
};

// Settings kept in FastFTP.ini next to the login lines. Unknown lines are
// ignored, so older FastFTP versions still read the login from the same file.
struct ReadSettings {
    char folder[ReadFolderMax];    // read_folder: folder on local drives, relative to the drive root
    char connectx[ReadFolderMax];  // connectx_folder: full path reachable through ConnectX; empty = not set
    DWORD server;                  // netiso_server: IPv4 in network order, 0 = not set
    int port;                      // netiso_port
    DWORD seed;                    // read_seed
    ReadSettings();
};
void ParseReadSettings(const char* text, ReadSettings& settings);
void FormatReadSettings(char* out, size_t capacity, const ReadSettings& settings);
// "a.b.c.d" or "a.b.c.d:port". Returns false if invalid.
bool ParseServer(const char* text, DWORD& address, int& port);
void FormatServer(char* out, size_t capacity, DWORD address, int port);
bool ValidReadFolder(const char* text, bool absolute);

// Describes a finished run for the log file (one line) and the CSV export.
struct ReadReport {
    const char* backend; const char* server; const char* file; const char* cache;
    int workload, pass; DWORD seconds, seed;
};
const char* ReadStatusName(int status);
const char* ReadVerifyName(int verify);
void FormatReadLog(char* out, size_t capacity, const ReadReport& report, const ReadResult& result);
const char* ReadCsvHeader();
void FormatReadCsv(char* out, size_t capacity, const char* date, const ReadReport& report, const ReadResult& result);
}
