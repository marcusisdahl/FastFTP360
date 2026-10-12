// SPDX-License-Identifier: MIT
#include "bench.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

namespace nb {
Result::Result() : direction(0), seconds(0), sendBuffer(0), recvBuffer(0), samples(0),
    bytes(0), microseconds(0), minimum(0), maximum(0) {}
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
#ifdef _XBOX
    // Per-socket LAN access used by Xbox homebrew. No kernel patching.
    BOOL enabled=TRUE;
    if (setsockopt(s,SOL_SOCKET,0x5801,reinterpret_cast<const char*>(&enabled),sizeof(enabled)) ||
        setsockopt(s,SOL_SOCKET,0x5802,reinterpret_cast<const char*>(&enabled),sizeof(enabled))) {
        Error(error,capacity,"LAN socket options failed"); return false;
    }
#endif
    if (ioctlsocket(s,FIONBIO,&nonblocking)) { Error(error,capacity,"Socket mode failed"); return false; }
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
            if (rc==SOCKET_ERROR) return Stop("Socket wait failed");
        }
    }
    int Receive(unsigned char* data,int count) {
        for (;;) {
            if (!Wait(false)) return -1;
            int got=recv(socket_,reinterpret_cast<char*>(data),count,0);
            if (got>=0) { if(got) lastActivity_=Now(); return got; }
            if (WSAGetLastError()!=WSAEWOULDBLOCK) { Stop("Connection interrupted while receiving"); return -1; }
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
            int sent=send(socket_,reinterpret_cast<const char*>(data),count,0);
            if(sent>0) { data+=sent; count-=sent; lastActivity_=Now(); }
            else if(sent==0 || WSAGetLastError()!=WSAEWOULDBLOCK) return Stop("Connection interrupted while sending");
        }
        return true;
    }
    void Progress(Phase phase,DWORD direction,U64 bytes,double seconds,double rate) {
        if(observer_.progress) observer_.progress(observer_.context,phase,direction,bytes,seconds,rate);
    }
    void Configure(DWORD seconds) { if(observer_.configured) observer_.configured(observer_.context,seconds); }
};
static bool ExchangeFixed(Connection& c,bool sender,unsigned char* data,unsigned char* expected,DWORD bytes,bool verify) {
    for(DWORD offset=0;offset<bytes;offset+=Chunk) {
        if(sender) { if(!c.SendAll(expected,Chunk)) return false; }
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
}
static bool Session(SOCKET s,Connection& c,Result& r,unsigned char* data,unsigned char* expected) {
    unsigned char request[RequestSize], response[RequestSize]={0};
    if(!c.ReceiveAll(request,RequestSize)) return false;
    DWORD direction=Read32(request+8),seconds=Read32(request+12),chunk=Read32(request+16),
        socketBuffer=Read32(request+20),warmupMiB=Read32(request+24),seed=Read32(request+28);
    bool valid=Read32(request)==Magic && Read32(request+4)==Version && (direction==1 || direction==2) &&
        seconds>=1 && seconds<=60 && chunk==Chunk && (socketBuffer==16384 || socketBuffer==32768 ||
        socketBuffer==65536 || socketBuffer==131072) && warmupMiB>=1 && warmupMiB<=64;
    Write32(response,Magic); Write32(response+4,Version); Write32(response+8,valid ? 0 : 1);
    if(!valid) { c.SendAll(response,RequestSize); return c.Stop("Unsupported test request"); }
    int buffer=int(socketBuffer), actualSend=0,actualRecv=0,length=sizeof(int);
    if(setsockopt(s,SOL_SOCKET,SO_SNDBUF,reinterpret_cast<const char*>(&buffer),sizeof(buffer)) ||
       setsockopt(s,SOL_SOCKET,SO_RCVBUF,reinterpret_cast<const char*>(&buffer),sizeof(buffer)) ||
       getsockopt(s,SOL_SOCKET,SO_SNDBUF,reinterpret_cast<char*>(&actualSend),&length))
        return c.Stop("Socket buffer setup failed");
    length=sizeof(int);
    if(getsockopt(s,SOL_SOCKET,SO_RCVBUF,reinterpret_cast<char*>(&actualRecv),&length)) return c.Stop("Socket buffer query failed");
    r.direction=direction; r.seconds=seconds; r.sendBuffer=actualSend; r.recvBuffer=actualRecv;
    c.Configure(seconds);
    Write32(response+12,Chunk); Write32(response+16,actualSend); Write32(response+20,actualRecv);
    Write32(response+24,IntegrityBytes); Write32(response+28,warmupMiB);
    if(!c.SendAll(response,RequestSize)) return false;
    Pattern(expected,seed);
    bool sender=direction==2;
    c.Progress(Verifying,direction,0,0,0);
    if(!ExchangeFixed(c,sender,data,expected,IntegrityBytes,true)) return false;
    c.Progress(Warming,direction,0,0,0);
    if(!ExchangeFixed(c,sender,data,expected,warmupMiB*1048576,false)) return false;
    c.Progress(Measuring,direction,0,0,0);
    double begin=Now(),lastSample=begin,lastUpdate=begin,currentRate=0;
    U64 sampleBytes=0;
    unsigned char finish[32]={0};
    for(;;) {
        if(sender) {
            if(Now()-begin>=seconds) break;
            if(!c.SendAll(expected,Chunk)) return false;
            r.bytes+=Chunk;
        } else {
            int got=c.Receive(data,Chunk);
            if(got<0) return false;
            if(got==0) break;
            // Retain the stream tail: an explicit finish record distinguishes
            // successful completion from a disconnect/cancel that also sends FIN.
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
           Read64(wire+16)!=r.bytes || elapsed==0 || elapsed>180000000 || Read32(wire+52)!=IntegrityBytes)
            return c.Stop("Receiver result did not match transfer");
        r.microseconds=elapsed; r.minimum=double(Read32(wire+32))/1024; r.maximum=double(Read32(wire+36))/1024;
        r.samples=Read32(wire+48);
    } else {
        U64 sentTime=Read64(finish+24);
        if(r.bytes<=32 || Read32(finish)!=Magic || Read32(finish+4)!=Version || Read32(finish+8)!=direction ||
           Read32(finish+12)!=0 || Read64(finish+16)!=r.bytes-32 || (r.bytes-32)%Chunk ||
           sentTime<U64(seconds)*1000000 || sentTime>U64(seconds+15)*1000000)
            return c.Stop("Transfer ended without a valid finish record");
        r.bytes-=32;
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
