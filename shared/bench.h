// SPDX-License-Identifier: MIT
#pragma once
#ifdef _XBOX
#include <xtl.h>
#elif defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <windows.h>
#endif

namespace nb {
typedef unsigned __int64 U64;
enum { Port = 36020, Magic = 0x4E423336, Version = 3, Chunk = 65536, WriteSize = 16384,
       IntegrityBytes = 1048576, RequestSize = 40, ResultSize = 72, PauseDrawing = 1,
       // Request direction field: low byte is the direction, this bit asks the
       // receiver to compare every timed payload byte with the pattern.
       FlagVerifyAll = 0x100, MaxStreams = 4 };
enum Phase { Idle, Listening, Connecting, Verifying, Warming, Measuring, Complete, Failed, Stopped };
struct Result {
    DWORD direction, seconds, sendBuffer, recvBuffer, samples, rateLimit, options;
    U64 bytes, microseconds, verified;
    double minimum, maximum;
    Result();
    double Rate() const;
};
struct Observer {
    void* context;
    bool (*cancelled)(void*);
    void (*progress)(void*, Phase, DWORD, U64, double, double);
    bool (*configured)(void*, DWORD, DWORD, DWORD);
};
DWORD Read32(const unsigned char* p);
U64 Read64(const unsigned char* p);
void Write32(unsigned char* p, DWORD value);
void Write64(unsigned char* p, U64 value);
void Pattern(unsigned char* p, DWORD seed);
double Now();
bool PrepareSocket(SOCKET s, char* error, size_t capacity);
bool PrepareListener(SOCKET s, char* error, size_t capacity);
// For a connection the Xbox opens itself: the same LAN options as a listener
// (set before connect), then nonblocking.
bool PrepareOutbound(SOCKET s, char* error, size_t capacity);
bool SetSocketBuffers(SOCKET s, int bytes, char* error, size_t capacity);
bool Serve(SOCKET s, const Observer& observer, Result& result, char* error, size_t capacity);
const char* PhaseName(Phase phase);
}
