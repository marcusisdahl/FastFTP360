// SPDX-License-Identifier: MIT
#include "checksum.h"
#include <stdio.h>
#include <string.h>

namespace nb {
namespace {
typedef unsigned int U32;
U32 CrcTable[256];
bool CrcReady=false;
inline U32 Rotate(U32 value,int bits) { return (value<<bits)|(value>>(32-bits)); }
const U32 Md5Sine[64]={
    0xd76aa478,0xe8c7b756,0x242070db,0xc1bdceee,0xf57c0faf,0x4787c62a,0xa8304613,0xfd469501,
    0x698098d8,0x8b44f7af,0xffff5bb1,0x895cd7be,0x6b901122,0xfd987193,0xa679438e,0x49b40821,
    0xf61e2562,0xc040b340,0x265e5a51,0xe9b6c7aa,0xd62f105d,0x02441453,0xd8a1e681,0xe7d3fbc8,
    0x21e1cde6,0xc33707d6,0xf4d50d87,0x455a14ed,0xa9e3e905,0xfcefa3f8,0x676f02d9,0x8d2a4c8a,
    0xfffa3942,0x8771f681,0x6d9d6122,0xfde5380c,0xa4beea44,0x4bdecfa9,0xf6bb4b60,0xbebfbc70,
    0x289b7ec6,0xeaa127fa,0xd4ef3085,0x04881d05,0xd9d4d039,0xe6db99e5,0x1fa27cf8,0xc4ac5665,
    0xf4292244,0x432aff97,0xab9423a7,0xfc93a039,0x655b59c3,0x8f0ccc92,0xffeff47d,0x85845dd1,
    0x6fa87e4f,0xfe2ce6e0,0xa3014314,0x4e0811a1,0xf7537e82,0xbd3af235,0x2ad7d2bb,0xeb86d391};
const int Md5Shift[16]={7,12,17,22,5,9,14,20,4,11,16,23,6,10,15,21};
}

Checksum::Checksum(Kind kind) : kind_(kind),crc_(0xFFFFFFFFu),length_(0),used_(0) {
    if(kind==Crc32 && !CrcReady) {
        for(U32 i=0;i<256;++i) { U32 c=i; for(int k=0;k<8;++k) c=c&1 ? 0xEDB88320u^(c>>1) : c>>1; CrcTable[i]=c; }
        CrcReady=true;
    }
    static const U32 start[5]={0x67452301,0xEFCDAB89,0x98BADCFE,0x10325476,0xC3D2E1F0};
    memcpy(state_,start,sizeof(start));
}

void Checksum::Block(const unsigned char* p) {
    U32 w[80];
    if(kind_==Md5) {
        for(int i=0;i<16;++i) w[i]=U32(p[i*4])|(U32(p[i*4+1])<<8)|(U32(p[i*4+2])<<16)|(U32(p[i*4+3])<<24);
        U32 a=state_[0],b=state_[1],c=state_[2],d=state_[3];
        for(int i=0;i<64;++i) {
            U32 f; int g;
            if(i<16) { f=(b&c)|(~b&d); g=i; }
            else if(i<32) { f=(d&b)|(~d&c); g=(5*i+1)&15; }
            else if(i<48) { f=b^c^d; g=(3*i+5)&15; }
            else { f=c^(b|~d); g=(7*i)&15; }
            const U32 next=d; d=c; c=b;
            b+=Rotate(a+f+Md5Sine[i]+w[g],Md5Shift[(i>>4)*4+(i&3)]);
            a=next;
        }
        state_[0]+=a; state_[1]+=b; state_[2]+=c; state_[3]+=d;
        return;
    }
    for(int i=0;i<16;++i) w[i]=(U32(p[i*4])<<24)|(U32(p[i*4+1])<<16)|(U32(p[i*4+2])<<8)|U32(p[i*4+3]);
    for(int i=16;i<80;++i) w[i]=Rotate(w[i-3]^w[i-8]^w[i-14]^w[i-16],1);
    U32 a=state_[0],b=state_[1],c=state_[2],d=state_[3],e=state_[4];
    for(int i=0;i<80;++i) {
        U32 f,k;
        if(i<20) { f=(b&c)|(~b&d); k=0x5A827999; }
        else if(i<40) { f=b^c^d; k=0x6ED9EBA1; }
        else if(i<60) { f=(b&c)|(b&d)|(c&d); k=0x8F1BBCDC; }
        else { f=b^c^d; k=0xCA62C1D6; }
        const U32 t=Rotate(a,5)+f+e+k+w[i];
        e=d; d=c; c=Rotate(b,30); b=a; a=t;
    }
    state_[0]+=a; state_[1]+=b; state_[2]+=c; state_[3]+=d; state_[4]+=e;
}

void Checksum::Add(const void* data,size_t count) {
    const unsigned char* p=static_cast<const unsigned char*>(data);
    if(kind_==Crc32) {
        U32 c=crc_;
        while(count--) c=CrcTable[(c^*p++)&255]^(c>>8);
        crc_=c; return;
    }
    length_+=count;
    if(used_) {
        size_t take=64-used_<count ? 64-used_ : count;
        memcpy(buffer_+used_,p,take); used_+=take; p+=take; count-=take;
        if(used_<64) return;
        Block(buffer_); used_=0;
    }
    for(;count>=64;p+=64,count-=64) Block(p);
    memcpy(buffer_,p,count); used_=count;
}

void Checksum::Finish(char* out) {
    if(kind_==Crc32) { _snprintf_s(out,41,_TRUNCATE,"%08x",crc_^0xFFFFFFFFu); return; }
    const unsigned long long bits=length_*8;
    unsigned char tail[72]={0x80}; size_t pad=(used_<56 ? 56 : 120)-used_;
    for(int i=0;i<8;++i) tail[pad+i]=static_cast<unsigned char>(kind_==Md5 ? bits>>(8*i) : bits>>(56-8*i));
    Add(tail,pad+8);
    const int words=kind_==Md5 ? 4 : 5;
    for(int i=0;i<words;++i) {
        const U32 v=state_[i];
        if(kind_==Md5) _snprintf_s(out+i*8,9,_TRUNCATE,"%02x%02x%02x%02x",v&255,(v>>8)&255,(v>>16)&255,v>>24);
        else _snprintf_s(out+i*8,9,_TRUNCATE,"%08x",v);
    }
}

const char* Checksum::Name(Kind kind) { return kind==Crc32 ? "CRC32" : kind==Md5 ? "MD5" : "SHA-1"; }

bool Checksum::Parse(const char* name,Kind& kind) {
    static const struct { const char* name; Kind kind; } names[]={{"CRC32",Crc32},{"CRC-32",Crc32},{"MD5",Md5},{"SHA-1",Sha1},{"SHA1",Sha1}};
    for(size_t i=0;i<sizeof(names)/sizeof(names[0]);++i) {
        const char* a=names[i].name; const char* b=name;
        while(*a && (*a==*b || (*b>='a' && *b<='z' && *a==*b-32))) { ++a; ++b; }
        if(!*a && !*b) { kind=names[i].kind; return true; }
    }
    return false;
}
}
