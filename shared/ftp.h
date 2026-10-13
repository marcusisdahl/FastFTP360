// SPDX-License-Identifier: MIT
#pragma once
#include "bench.h"

namespace nb {
enum { FtpPort = 21, FtpDataPortFirst = 36100, FtpDataPortCount = 100, FtpBuffer = 1048576, FtpMaxSessions = 4 };
// A top-level FTP folder. A NULL path is the null device: uploads are discarded
// and a download named "<n>MB" streams n MiB of test pattern without a disk.
struct FtpRoot { const char* name; const char* path; };
struct FtpObserver {
    void* context;
    bool (*cancelled)(void*);
    // operation is "STOR" or "RETR". Called during a transfer and once with done set.
    void (*progress)(void*, const char* operation, const char* file, U64 bytes, double seconds, bool done, bool ok);
};
// Serves passive-mode FTP on an already listening socket until cancelled, or
// until maxSessions control connections have ended when maxSessions is nonzero.
// address is the IPv4 address (network order) announced for passive data.
// With a NULL user any user name and password log in; otherwise both must match.
bool ServeFtp(SOCKET listener, const FtpRoot* roots, int rootCount, DWORD address, int socketBuffer,
              int maxSessions, const char* user, const char* password, const FtpObserver& observer,
              char* error, size_t capacity);
}
