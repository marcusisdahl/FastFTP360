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
    // Optional. One line per event for the log file: connections, logins,
    // transfers, deletes, renames and new folders. Called from session threads.
    void (*log)(void*, const char* line);
    // Optional. Called when a transfer's data connection is open, before any
    // data moves; it may wait (the Xbox holds drawing first). Every call is
    // followed by one progress call with done set.
    void (*starting)(void*, const char* operation, const char* file);
};
// Serves passive-mode FTP on an already listening socket until cancelled, or
// until maxSessions control connections have ended when maxSessions is nonzero.
// address is the IPv4 address (network order) announced for passive data.
// speedLimit caps all uploads and downloads together in MiB/s (0: no limit), or
// with downloadsOnly only the downloads. processor is the Xbox hardware thread
// for session threads (-1: the kernel chooses).
// diskBlock 0 reads and writes files through the file cache in FtpBuffer
// blocks; otherwise the cache is bypassed and the drive gets requests of
// diskBlock bytes, a multiple of 64 KiB up to FtpBuffer.
// With a NULL user any user name and password log in; otherwise both must match.
bool ServeFtp(SOCKET listener, const FtpRoot* roots, int rootCount, DWORD address, int socketBuffer,
              int speedLimit, bool downloadsOnly, int processor, int diskBlock, int maxSessions, const char* user, const char* password,
              const FtpObserver& observer, char* error, size_t capacity);

// The settings file next to the XEX holds "key=value" lines: the FTP login as
// "login=on|off", "user=..." and "password=...", and the Xbox app's settings.
enum { FtpLoginMax = 23 };
// 1 to FtpLoginMax printable ASCII characters, no spaces.
bool ValidFtpLogin(const char* text);
// Applies the recognized lines of text; missing or invalid values keep the
// current ones. user and password each hold FtpLoginMax+1 characters.
void ParseFtpLogin(const char* text, bool& required, char* user, char* password);
void FormatFtpLogin(char* out, size_t capacity, bool required, const char* user, const char* password);
// Copies the value of the last "key=value" line for key (any case) that fits
// in capacity; false when there is none.
bool FindSetting(const char* text, const char* key, char* value, size_t capacity);
}
