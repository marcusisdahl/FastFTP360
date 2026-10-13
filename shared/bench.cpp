// SPDX-License-Identifier: MIT
#include "bench.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

namespace nb {
Result::Result() : direction(0), seconds(0), sendBuffer(0), recvBuffer(0), samples(0), rateLimit(0), options(0),
    bytes(0), microseconds(0), verified(0), minimum(0), maximum(0) {}
double Result::Rate() const { return microseconds ? double(bytes) / 1.048576 / double(microseconds) : 0; }
DWORD Read32(const unsigned char* p) { return (DWORD(p[0])<<24)|(DWORD(p[1])<<16)|(DWORD(p[2])<<8)|p[3]; }
U64 Read64(const unsigned char* p) { return (U64(Read32(p))<<32)|Read32(p+4); }
void Write32(unsigned char* p, DWORD v) { p[0]=BYTE(v>>24); p[1]=BYTE(v>>16); p[2]=BYTE(v>>8); p[3]=BYTE(v); }
void Write64(unsigned char* p, U64 v) { Write32(p,DWORD(v>>32)); Write32(p+4,DWORD(v)); }
void Pattern(unsigned char* p, DWORD seed) {
    DWORD state=seed ? seed : 1;
    for (DWORD i=0;i<Chunk;++i) { state^=state<<13; state^=state>>17; state^=state<<5; p[i]=BYTE(state); }
}
double Now() {
    LARGE_INTEGER counter, frequency;
    QueryPerformanceCounter(&counter); QueryPerformanceFrequency(&frequency);
    return double(counter.QuadPart)/double(frequency.QuadPart);
}
const char* PhaseName(Phase p) {
    static const char* names[]={"READY", "WAITING FOR PC", "CONNECTING", "CHECKING DATA",
        "WARMING UP", "MEASURING", "COMPLETE", "ERROR", "STOPPED"};
    return names[p>=Idle && p<=Stopped ? p : Failed];
}
static void Error(char* text, size_t capacity, const char* message) {
    _snprintf_s(text,capacity,_TRUNCATE,"%s",message);
}
bool PrepareSocket(SOCKET s, char* error, size_t capacity) {
    u_long nonblocking=1;
    if (ioctlsocket(s,FIONBIO,&nonblocking)) {
        int code=WSAGetLastError();
        _snprintf_s(error,capacity,_TRUNCATE,"NONBLOCKING SOCKET FAILED (%d)",code); return false;
    }
    return true;
}
bool PrepareListener(SOCKET s, char* error, size_t capacity) {
#if defined(_XBOX) || defined(NB_TEST_LAN_OPTIONS)
    // Configure LAN access before bind/listen. Accepted connections inherit the
    // listener's policy; retail XNet can reject changing these options afterward.
    BOOL enabled=TRUE;
    if (setsockopt(s,SOL_SOCKET,0x5801,reinterpret_cast<const char*>(&enabled),sizeof(enabled))) {
        int code=WSAGetLastError();
        _snprintf_s(error,capacity,_TRUNCATE,"LISTENER LAN 5801 FAILED (%d)",code); return false;
    }
    if (setsockopt(s,SOL_SOCKET,0x5802,reinterpret_cast<const char*>(&enabled),sizeof(enabled))) {
        int code=WSAGetLastError();
        _snprintf_s(error,capacity,_TRUNCATE,"LISTENER LAN 5802 FAILED (%d)",code); return false;
    }
#endif
    BOOL reuse=TRUE;
    if(setsockopt(s,SOL_SOCKET,SO_REUSEADDR,reinterpret_cast<const char*>(&reuse),sizeof(reuse))) {
        int code=WSAGetLastError();
        _snprintf_s(error,capacity,_TRUNCATE,"LISTENER PORT REUSE FAILED (%d)",code); return false;
    }
    return PrepareSocket(s,error,capacity);
}
bool SetSocketBuffers(SOCKET s, int bytes, char* error, size_t capacity) {
    // Set on the listener before bind/listen; accepted sockets inherit it.
    if (setsockopt(s,SOL_SOCKET,SO_SNDBUF,reinterpret_cast<const char*>(&bytes),sizeof(bytes))) {
        int code=WSAGetLastError();
        _snprintf_s(error,capacity,_TRUNCATE,"LISTENER SEND BUFFER %d KIB FAILED (%d)",bytes/1024,code); return false;
    }
    if (setsockopt(s,SOL_SOCKET,SO_RCVBUF,reinterpret_cast<const char*>(&bytes),sizeof(bytes))) {
        int code=WSAGetLastError();
        _snprintf_s(error,capacity,_TRUNCATE,"LISTENER RECEIVE BUFFER %d KIB FAILED (%d)",bytes/1024,code); return false;
    }
    return true;
}

// Compares stream bytes [verified, limit) with the repeating pattern. Stream
// offset `base` is the first byte of `data`; the 32 bytes before it are kept
// in `tail`, because the newest 32 bytes may be the finish record.
static bool VerifyStream(const unsigned char* expected,const unsigned char* tail,U64 base,const unsigned char* data,U64& verified,U64 limit) {
    while(verified<limit) {
        const unsigned char* source=verified<base ? tail+32-DWORD(base-verified) : data+DWORD(verified-base);
        U64 available=(verified<base ? base : limit)-verified;
        if(verified+available>limit) available=limit-verified;
        DWORD offset=DWORD(verified%Chunk);
        U64 count=Chunk-offset; if(count>available) count=available;
        if(memcmp(source,expected+offset,size_t(count))) return false;
        verified+=count;
    }
    return true;
}

// One worker owns the socket. Cancellation is polled without closing a socket
// concurrently with another thread's send/recv or reusing its numeric handle.
class Connection {
    Connection(const Connection&);
    Connection& operator=(const Connection&);
    SOCKET socket_;
    const Observer& observer_;
    char* error_;
    size_t capacity_;
    double lastActivity_, deadline_;
public:
    Connection(SOCKET s,const Observer& o,char* e,size_t c) : socket_(s), observer_(o), error_(e), capacity_(c),
        lastActivity_(Now()), deadline_(Now()+180) {}
    bool Stop(const char* message) { Error(error_,capacity_,message); return false; }
    bool StopSocket(const char* operation) {
        int code=WSAGetLastError();
        _snprintf_s(error_,capacity_,_TRUNCATE,"%s (%d)",operation,code); return false;
    }
    bool Wait(bool writing) {
        for (;;) {
            if (observer_.cancelled && observer_.cancelled(observer_.context)) return Stop("Cancelled");
            double current=Now();
            if (current-lastActivity_>15) return Stop("No data for 15 seconds");
            if (current>deadline_) return Stop("Session time limit reached");
            fd_set set; FD_ZERO(&set); FD_SET(socket_,&set);
            timeval timeout={0,100000};
            int rc=select(0,writing?NULL:&set,writing?&set:NULL,NULL,&timeout);
            if (rc>0) return true;
            if (rc==SOCKET_ERROR) return StopSocket("SOCKET WAIT FAILED");
        }
    }
    int Receive(unsigned char* data,int count) {
        for (;;) {
            if (!Wait(false)) return -1;
            int got=recv(socket_,reinterpret_cast<char*>(data),count,0);
            if (got>=0) { if(got) lastActivity_=Now(); return got; }
            if (WSAGetLastError()!=WSAEWOULDBLOCK) { Stop("Connection interrupted while receiving"); return -1; }
            Sleep(1);
        }
    }
    bool ReceiveAll(unsigned char* data,int count) {
        while(count) { int got=Receive(data,count); if(got<=0) return got==0 ? Stop("Unexpected end of data") : false;
            data+=got; count-=got; }
        return true;
    }
    bool SendAll(const unsigned char* data,int count) {
        while(count) {
            if (!Wait(true)) return false;
            int size=count>WriteSize ? WriteSize : count;
            int sent=send(socket_,reinterpret_cast<const char*>(data),size,0);
            if(sent>0) { data+=sent; count-=sent; lastActivity_=Now(); Sleep(0); }
            else if(sent==0 || WSAGetLastError()!=WSAEWOULDBLOCK) return Stop("Connection interrupted while sending");
            else Sleep(1);
        }
        return true;
    }
    void Progress(Phase phase,DWORD direction,U64 bytes,double seconds,double rate) {
        if(observer_.progress) observer_.progress(observer_.context,phase,direction,bytes,seconds,rate);
    }
    bool Configure(DWORD seconds,DWORD limit,DWORD options) {
        return !observer_.configured || observer_.configured(observer_.context,seconds,limit,options) || Stop("UI PAUSE HANDSHAKE FAILED");
    }
    bool Pace(double until) {
        while(Now()<until) {
            if(observer_.cancelled && observer_.cancelled(observer_.context)) return Stop("Cancelled");
            if(Now()>deadline_) return Stop("Session time limit reached");
            Sleep(1);
        }
        return true;
    }
};
static bool ExchangeFixed(Connection& c,bool sender,unsigned char* data,unsigned char* expected,DWORD bytes,bool verify,DWORD limit) {
    for(DWORD offset=0;offset<bytes;offset+=Chunk) {
        if(sender) {
            double start=Now();
            if(!c.SendAll(expected,Chunk)) return false;
            if(limit && !c.Pace(start+double(Chunk)/(limit*1048576.0))) return false;
        }
        else {
            if(!c.ReceiveAll(data,Chunk)) return false;
            if(verify && memcmp(data,expected,Chunk)) return c.Stop("Integrity check failed");
        }
    }
    unsigned char ack[4]={0,0,0,0};
    if(sender) return c.ReceiveAll(ack,4) && (Read32(ack)==0 || c.Stop("Peer rejected integrity check"));
    return c.SendAll(ack,4);
}
static void EncodeResult(unsigned char* wire,const Result& r) {
    memset(wire,0,ResultSize); Write32(wire,Magic); Write32(wire+4,Version); Write32(wire+8,r.direction);
    Write64(wire+16,r.bytes); Write64(wire+24,r.microseconds);
    Write32(wire+32,DWORD(r.minimum*1024)); Write32(wire+36,DWORD(r.maximum*1024));
    Write32(wire+40,r.sendBuffer); Write32(wire+44,r.recvBuffer); Write32(wire+48,r.samples);
    Write32(wire+52,IntegrityBytes);
    Write32(wire+56,r.rateLimit); Write32(wire+60,r.options); Write64(wire+64,r.verified);
}
static bool Session(SOCKET s,Connection& c,Result& r,unsigned char* data,unsigned char* expected) {
    unsigned char request[RequestSize], response[RequestSize]={0};
    if(!c.ReceiveAll(request,32)) return false;
    Write32(response,Magic); Write32(response+4,Version);
    if(Read32(request)!=Magic || Read32(request+4)!=Version) {
        Write32(response+8,1); c.SendAll(response,32); return c.Stop("UPDATE BOTH APPS TO PROTOCOL V3");
    }
    if(!c.ReceiveAll(request+32,8)) return false;
    DWORD flags=Read32(request+8),direction=flags&0xFF,seconds=Read32(request+12),chunk=Read32(request+16),
        socketBuffer=Read32(request+20),warmupMiB=Read32(request+24),seed=Read32(request+28),
        rateLimit=Read32(request+32),options=Read32(request+36);
    bool verifyAll=(flags&FlagVerifyAll)!=0;
    // Socket bytes 0 means the PC kept its operating system's automatic buffers.
    bool valid=Read32(request)==Magic && Read32(request+4)==Version && (direction==1 || direction==2) &&
        (flags&~DWORD(FlagVerifyAll|0xFF))==0 && seconds>=1 && seconds<=60 && chunk==Chunk &&
        (socketBuffer==0 || socketBuffer==16384 || socketBuffer==32768 || socketBuffer==65536 || socketBuffer==131072) &&
        warmupMiB>=1 && warmupMiB<=64 &&
        (rateLimit==0 || rateLimit==8 || rateLimit==12 || rateLimit==16 || rateLimit==20 || rateLimit==24 || rateLimit==32) &&
        (options&~DWORD(PauseDrawing))==0;
    Write32(response,Magic); Write32(response+4,Version); Write32(response+8,valid ? 0 : 1);
    if(!valid) { c.SendAll(response,RequestSize); return c.Stop("Unsupported test request"); }
    // XNet on the tested retail console rejects SO_RCVBUF on an accepted TCP
    // connection (10022). Keep startup/listener defaults and report real values.
    // The request's buffer setting is a PC setting, not a guarantee for Xbox.
    int actualSend=0,actualRecv=0,length=sizeof(int);
    if(getsockopt(s,SOL_SOCKET,SO_SNDBUF,reinterpret_cast<char*>(&actualSend),&length))
        return c.StopSocket("SEND BUFFER QUERY FAILED");
    length=sizeof(int);
    if(getsockopt(s,SOL_SOCKET,SO_RCVBUF,reinterpret_cast<char*>(&actualRecv),&length)) return c.StopSocket("RECEIVE BUFFER QUERY FAILED");
    r.direction=direction; r.seconds=seconds; r.sendBuffer=actualSend; r.recvBuffer=actualRecv;
    r.rateLimit=direction==2 ? rateLimit : 0; r.options=options;
    c.Progress(Verifying,direction,0,0,0);
    if(!c.Configure(seconds,r.rateLimit,options)) return false;
    Write32(response+12,Chunk); Write32(response+16,actualSend); Write32(response+20,actualRecv);
    Write32(response+24,IntegrityBytes); Write32(response+28,warmupMiB);
    Write32(response+32,r.rateLimit); Write32(response+36,options);
    if(!c.SendAll(response,RequestSize)) return false;
    Pattern(expected,seed);
    bool sender=direction==2;
    c.Progress(Verifying,direction,0,0,0);
    if(!ExchangeFixed(c,sender,data,expected,IntegrityBytes,true,r.rateLimit)) return false;
    c.Progress(Warming,direction,0,0,0);
    if(!ExchangeFixed(c,sender,data,expected,warmupMiB*1048576,false,r.rateLimit)) return false;
    c.Progress(Measuring,direction,0,0,0);
    double begin=Now(),lastSample=begin,lastUpdate=begin,currentRate=0;
    U64 sampleBytes=0;
    unsigned char finish[32]={0};
    for(;;) {
        if(sender) {
            if(Now()-begin>=seconds) break;
            double sentAt=Now();
            if(!c.SendAll(expected,Chunk)) return false;
            r.bytes+=Chunk;
            if(r.rateLimit && !c.Pace(sentAt+double(Chunk)/(r.rateLimit*1048576.0))) return false;
        } else {
            int got=c.Receive(data,Chunk);
            if(got<0) return false;
            if(got==0) break;
            // Retain the stream tail: an explicit finish record distinguishes
            // successful completion from a disconnect/cancel that also sends FIN.
            if(verifyAll) {
                U64 total=r.bytes+DWORD(got);
                if(!VerifyStream(expected,finish,r.bytes,data,r.verified,total>32 ? total-32 : 0))
                    return c.Stop("Integrity check failed during timed transfer");
            }
            if(got>=32) memcpy(finish,data+got-32,32);
            else { memmove(finish,finish+got,32-got); memcpy(finish+32-got,data,got); }
            r.bytes+=got;
            if(Now()-begin>seconds+15) return c.Stop("Sender exceeded test duration");
        }
        double current=Now();
        if(current-lastSample>=1) {
            currentRate=double(r.bytes-sampleBytes)/(current-lastSample)/1048576;
            if(r.samples==0 || currentRate<r.minimum) r.minimum=currentRate;
            if(currentRate>r.maximum) r.maximum=currentRate;
            ++r.samples; sampleBytes=r.bytes; lastSample=current;
        }
        if(current-lastUpdate>=0.25) { c.Progress(Measuring,direction,r.bytes,current-begin,currentRate); lastUpdate=current; }
    }
    r.microseconds=U64((Now()-begin)*1000000);
    if(!r.bytes || !r.microseconds) return c.Stop("Empty measurement");
    unsigned char wire[ResultSize];
    if(sender) {
        Write32(finish,Magic); Write32(finish+4,Version); Write32(finish+8,direction);
        Write64(finish+16,r.bytes); Write64(finish+24,r.microseconds);
        if(!c.SendAll(finish,32)) return false;
        if(shutdown(s,SD_SEND)) return c.Stop("Could not finish send stream");
        if(!c.ReceiveAll(wire,ResultSize)) return false;
        U64 elapsed=Read64(wire+24);
        if(Read32(wire)!=Magic || Read32(wire+4)!=Version || Read32(wire+8)!=direction || Read32(wire+12)!=0 ||
           Read64(wire+16)!=r.bytes || elapsed==0 || elapsed>180000000 || Read32(wire+52)!=IntegrityBytes ||
           Read32(wire+56)!=r.rateLimit || Read32(wire+60)!=r.options)
            return c.Stop("Receiver result did not match transfer");
        r.microseconds=elapsed; r.minimum=double(Read32(wire+32))/1024; r.maximum=double(Read32(wire+36))/1024;
        r.samples=Read32(wire+48); r.verified=Read64(wire+64);
        if(verifyAll && r.verified!=r.bytes) return c.Stop("Receiver did not verify all data");
    } else {
        U64 sentTime=Read64(finish+24);
        if(r.bytes<=32 || Read32(finish)!=Magic || Read32(finish+4)!=Version || Read32(finish+8)!=direction ||
           Read32(finish+12)!=0 || Read64(finish+16)!=r.bytes-32 || (r.bytes-32)%Chunk ||
           sentTime<U64(seconds)*1000000 || sentTime>U64(seconds+15)*1000000)
            return c.Stop("Transfer ended without a valid finish record");
        r.bytes-=32;
        if(verifyAll && r.verified!=r.bytes) return c.Stop("Integrity check did not cover all data");
        if(r.samples==0) r.minimum=r.maximum=r.Rate();
        EncodeResult(wire,r);
        if(!c.SendAll(wire,ResultSize)) return false;
    }
    c.Progress(Complete,direction,r.bytes,double(r.microseconds)/1000000,r.Rate());
    return true;
}
bool Serve(SOCKET s,const Observer& observer,Result& result,char* error,size_t capacity) {
    if(!PrepareSocket(s,error,capacity)) return false;
    unsigned char* memory=static_cast<unsigned char*>(malloc(Chunk*2));
    if(!memory) { Error(error,capacity,"Could not allocate test buffers"); return false; }
    Connection connection(s,observer,error,capacity);
    bool ok=Session(s,connection,result,memory,memory+Chunk);
    free(memory);
    return ok;
}
}
