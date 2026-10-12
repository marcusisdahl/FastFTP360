// SPDX-License-Identifier: MIT
#pragma once
#ifdef _XBOX
#include <xtl.h>
#else
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <windows.h>
#endif

namespace nb {
typedef unsigned __int64 U64;
enum { Port = 36020, Magic = 0x4E423336, Version = 1, Chunk = 65536,
       IntegrityBytes = 1048576, RequestSize = 32, ResultSize = 64 };
enum Phase { Idle, Listening, Connecting, Verifying, Warming, Measuring, Complete, Failed, Stopped };
struct Result {
    DWORD direction, seconds, sendBuffer, recvBuffer, samples;
    U64 bytes, microseconds;
    double minimum, maximum;
    Result();
    double Rate() const;
};
struct Observer {
    void* context;
    bool (*cancelled)(void*);
    void (*progress)(void*, Phase, DWORD, U64, double, double);
    void (*configured)(void*, DWORD);
};
DWORD Read32(const unsigned char* p);
U64 Read64(const unsigned char* p);
void Write32(unsigned char* p, DWORD value);
void Write64(unsigned char* p, U64 value);
void Pattern(unsigned char* p, DWORD seed);
double Now();
bool PrepareSocket(SOCKET s, char* error, size_t capacity);
bool Serve(SOCKET s, const Observer& observer, Result& result, char* error, size_t capacity);
const char* PhaseName(Phase phase);
}
