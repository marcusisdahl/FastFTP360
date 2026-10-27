// SPDX-License-Identifier: MIT
// CRC-32, MD5 and SHA-1 for the FTP server's checksum commands (HASH, XCRC,
// XMD5, XSHA1), so a client can check an upload without downloading it back.
// Byte-order neutral: the same code runs on the big-endian Xbox and on PCs.
#pragma once
#include <stddef.h>

namespace nb {
class Checksum {
public:
    enum Kind { Crc32, Md5, Sha1 };
    explicit Checksum(Kind kind);
    void Add(const void* data, size_t count);
    // Lowercase hex digest (8, 32 or 40 characters); out needs 41 bytes.
    void Finish(char* out);
    // "CRC32", "MD5" or "SHA-1", as the HASH command names them.
    static const char* Name(Kind kind);
    // Accepts the names above, case-insensitively, also "SHA1" and "CRC-32".
    static bool Parse(const char* name, Kind& kind);
private:
    void Block(const unsigned char* block);
    Kind kind_;
    unsigned int state_[5], crc_;
    unsigned long long length_;
    unsigned char buffer_[64];
    size_t used_;
};
}
