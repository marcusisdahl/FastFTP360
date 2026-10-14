// SPDX-License-Identifier: MIT
#include "readbench.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace nb {
DWORD WorkloadRequest(int w) {
    static const DWORD sizes[]={65536,262144,1048576,4096,16384,65536};
    return w>=0 && w<ReadWorkloadCount ? sizes[w] : 0;
}
bool WorkloadRandom(int w) { return w>=ReadRand4K && w<ReadWorkloadCount; }
const char* WorkloadName(int w) {
    static const char* names[]={"Sequential 64 KiB","Sequential 256 KiB","Sequential 1 MiB","Random 4 KiB","Random 16 KiB","Random 64 KiB"};
    return w>=0 && w<ReadWorkloadCount ? names[w] : "?";
}

U64 ReadRandom::Next() {
    state+=0x9E3779B97F4A7C15ULL;
    U64 z=state;
    z=(z^(z>>30))*0xBF58476D1CE4E5B9ULL;
    z=(z^(z>>27))*0x94D049BB133111EBULL;
    return z^(z>>31);
}

LatencyHistogram::LatencyHistogram() { Reset(); }
void LatencyHistogram::Reset() { memset(counts_,0,sizeof(counts_)); count_=total_=maximum_=0; }
int LatencyHistogram::Bucket(U64 v) {
    if(v<Linear) return int(v);
    int top=63; while(!(v>>top)) --top; // top >= 6
    int exponent=top-6;
    if(exponent>=Exponents) return Buckets-1;
    int mantissa=int(v>>(top-5)); // 32..63
    return Linear+exponent*SubBuckets+(mantissa-SubBuckets);
}
U64 LatencyHistogram::BucketHigh(int b) {
    if(b<Linear) return U64(b);
    int exponent=(b-Linear)/SubBuckets, mantissa=(b-Linear)%SubBuckets+SubBuckets, shift=exponent+1;
    return ((U64(mantissa)+1)<<shift)-1;
}
void LatencyHistogram::Add(U64 v) {
    ++counts_[Bucket(v)]; ++count_; total_+=v; if(v>maximum_) maximum_=v;
}
U64 LatencyHistogram::Percentile(double fraction) const {
    if(!count_) return 0;
    U64 rank=U64(fraction*double(count_)); if(double(rank)<fraction*double(count_)) ++rank;
    if(rank<1) rank=1;
    if(rank>count_) rank=count_;
    U64 seen=0;
    for(int b=0;b<Buckets;++b) {
        seen+=counts_[b];
        if(seen>=rank) { U64 high=BucketHigh(b); return high<maximum_ ? high : maximum_; }
    }
    return maximum_;
}

DWORD FixtureWord(DWORD seed,U64 word) {
    DWORD x=DWORD(word)*0x9E3779B1u ^ DWORD(word>>32)*0xC2B2AE35u ^ seed*0x85EBCA77u ^ 0x27D4EB2Fu;
    x^=x>>16; x*=0x7FEB352Du; x^=x>>15; x*=0x846CA68Bu; x^=x>>16;
    return x;
}
static unsigned char FixtureByte(DWORD seed,U64 offset) {
    return BYTE(FixtureWord(seed,offset>>2)>>(8*(3-DWORD(offset&3))));
}
void FixtureFill(unsigned char* data,DWORD seed,U64 offset,DWORD length) {
    DWORD i=0;
    for(;i<length && ((offset+i)&3);++i) data[i]=FixtureByte(seed,offset+i);
    for(;i+4<=length;i+=4) Write32(data+i,FixtureWord(seed,(offset+i)>>2));
    for(;i<length;++i) data[i]=FixtureByte(seed,offset+i);
}
long FixtureCheck(const unsigned char* data,DWORD seed,U64 offset,DWORD length) {
    DWORD i=0;
    for(;i<length && ((offset+i)&3);++i) if(data[i]!=FixtureByte(seed,offset+i)) return long(i);
    for(;i+4<=length;i+=4) {
        if(Read32(data+i)!=FixtureWord(seed,(offset+i)>>2)) {
            for(DWORD k=0;k<4;++k) if(data[i+k]!=FixtureByte(seed,offset+i+k)) return long(i+k);
        }
    }
    for(;i<length;++i) if(data[i]!=FixtureByte(seed,offset+i)) return long(i);
    return -1;
}
bool FixtureSeed(const char* name,DWORD& seed) {
    if(_strnicmp(name,"FFTPFIX-",8)) return false;
    DWORD value=0;
    for(int i=0;i<8;++i) {
        char c=name[8+i]; int digit;
        if(c>='0' && c<='9') digit=c-'0'; else if(c>='a' && c<='f') digit=c-'a'+10; else if(c>='A' && c<='F') digit=c-'A'+10; else return false;
        value=(value<<4)|DWORD(digit);
    }
    if(name[16] && name[16]!='.') return false;
    seed=value; return true;
}

ReadConfig::ReadConfig() : workload(ReadSeq1M), seconds(30), warmup(ReadWarmupSeconds), seed(1), verify(true), fixture(false), fixtureSeed(0) {}
ReadResult::ReadResult() : status(ReadFailed), verify(VerifyOff), bytes(0), operations(0), fileSize(0), wraps(0), startOffset(0),
    seconds(0), verifySeconds(0), setupSeconds(0), failed(0), shortReads(0), timeouts(0), integrity(0),
    latencyMean(0), latencyP50(0), latencyP95(0), latencyP99(0), latencyMax(0) { error[0]=0; }
double ReadResult::Rate() const {
    double t=seconds-verifySeconds;
    return t>0 ? double(bytes)/1048576.0/t : 0;
}
double ReadResult::Operations() const { double t=seconds-verifySeconds; return t>0 ? double(operations)/t : 0; }

static int Finish(ReadResult& r,int status,const char* format,...) {
    r.status=status;
    if(format) { va_list a; va_start(a,format); _vsnprintf_s(r.error,sizeof(r.error),_TRUNCATE,format,a); va_end(a); }
    return status;
}

int RunReadBenchmark(ReadBackend& backend,const ReadConfig& c,const ReadObserver& o,unsigned char* buffer,ReadResult& r) {
    const DWORD request=WorkloadRequest(c.workload);
    const bool random=WorkloadRandom(c.workload);
    const U64 size=backend.Size();
    r.fileSize=size;
    r.verify=!c.verify ? VerifyOff : c.fixture ? VerifyKnownData : VerifyCounts;
    if(!request || request>ReadMaxRequest || request%ReadAlign) return Finish(r,ReadFailed,"Unsupported workload");
    if(!c.seconds || c.seconds>3600) return Finish(r,ReadFailed,"Unsupported duration");
    if(size<request) return Finish(r,ReadFailed,"The file is " NB_U64 " bytes, smaller than one %u KiB read. Pick a bigger file.",size,request/1024);
    // Every read is whole and inside the file: offsets are multiples of 4 KiB
    // and offset+request never passes the end.
    const U64 positions=(size-request)/ReadAlign+1;
    ReadRandom rng(U64(c.seed)*0x100000001B3ULL+U64(c.workload));
    U64 next=0;
    LatencyHistogram histogram; // About 4 KiB, whatever the run length.
    DWORD errors=0;
    int status=ReadCompleted;
    for(int phase=ReadWarming;phase<=ReadMeasuring && status==ReadCompleted;++phase) {
        const double length=phase==ReadWarming ? double(c.warmup) : double(c.seconds);
        if(length<=0) continue;
        U64 bytes=0,sampleBytes=0;
        double verifyTime=0;
        const double begin=Now(); double update=begin,sample=begin,rate=0;
        if(phase==ReadMeasuring) r.startOffset=random ? 0 : next;
        if(o.progress) o.progress(o.context,phase,0,0,0);
        for(;;) {
            if(o.cancelled && o.cancelled(o.context)) { status=Finish(r,ReadCancelled,"Cancelled"); break; }
            double now=Now();
            if(now-begin>=length) break;
            U64 offset;
            if(random) offset=(rng.Next()%positions)*ReadAlign;
            else {
                if(next>size-request) { next=0; if(phase==ReadMeasuring) ++r.wraps; }
                offset=next; next+=request;
            }
            int kind=ReadErrorNone;
            const double start=Now();
            int got=backend.Read(offset,request,buffer,kind);
            const double end=Now();
            if(got==int(request) && kind==ReadErrorNone) {
                if(phase==ReadMeasuring) {
                    histogram.Add(U64((end-start)*1000000.0+0.5)); ++r.operations; r.bytes+=request;
                }
                bytes+=request;
                if(c.verify && c.fixture) {
                    long wrong=FixtureCheck(buffer,c.fixtureSeed,offset,request);
                    verifyTime+=Now()-end;
                    if(wrong>=0) {
                        ++r.integrity;
                        status=Finish(r,ReadFailed,"Wrong data at byte " NB_U64 " of the fixture. Stopped.",offset+U64(wrong));
                        break;
                    }
                }
            } else {
                if(got>0 && phase==ReadMeasuring) r.bytes+=DWORD(got);
                if(kind==ReadErrorCancelled) { status=Finish(r,ReadCancelled,"Cancelled"); break; }
                if(kind==ReadErrorTimeout) ++r.timeouts;
                else if(kind==ReadErrorShort || (got>=0 && got<int(request))) ++r.shortReads;
                else ++r.failed;
                ++errors;
                if(backend.Fatal()) {
                    status=Finish(r,ReadFailed,"%s%s at offset " NB_U64 ": %s",kind==ReadErrorTimeout ? "Timeout" : kind==ReadErrorShort ? "Short read" : "Read failed",
                        phase==ReadWarming ? " during warm-up" : "",offset,backend.Error()[0] ? backend.Error() : "no detail");
                    break;
                }
                if(errors>=ReadMaxErrors) {
                    status=Finish(r,ReadFailed,"%u read errors; last at offset " NB_U64 ": %s",errors,offset,backend.Error()[0] ? backend.Error() : "no detail");
                    break;
                }
            }
            now=Now();
            if(now-sample>=1) { rate=double(bytes-sampleBytes)/1048576.0/(now-sample); sampleBytes=bytes; sample=now; }
            if(o.progress && now-update>=0.25) { update=now; o.progress(o.context,phase,bytes,now-begin,rate); }
        }
        if(phase==ReadMeasuring) { r.seconds=Now()-begin; r.verifySeconds=verifyTime; }
    }
    r.latencyMean=histogram.Mean()/1000.0;
    r.latencyP50=double(histogram.Percentile(0.50))/1000.0;
    r.latencyP95=double(histogram.Percentile(0.95))/1000.0;
    r.latencyP99=double(histogram.Percentile(0.99))/1000.0;
    r.latencyMax=double(histogram.Maximum())/1000.0;
    if(status==ReadCompleted && !r.operations) return Finish(r,ReadFailed,"No read completed during the measurement");
    r.status=status;
    if(o.progress && status==ReadCompleted) o.progress(o.context,ReadIdle,r.bytes,r.seconds,r.Rate());
    return status;
}

FileReadBackend::FileReadBackend() : file_(INVALID_HANDLE_VALUE), size_(0) { error_[0]=0; }
FileReadBackend::~FileReadBackend() { Close(); }
void FileReadBackend::Close() { if(file_!=INVALID_HANDLE_VALUE) { CloseHandle(file_); file_=INVALID_HANDLE_VALUE; } }
bool FileReadBackend::Open(const char* path,bool noBuffering,bool random) {
    Close();
    DWORD flags=FILE_ATTRIBUTE_NORMAL|(random ? FILE_FLAG_RANDOM_ACCESS : FILE_FLAG_SEQUENTIAL_SCAN);
    if(noBuffering) flags|=FILE_FLAG_NO_BUFFERING;
    // Read-only, and others may keep reading: the benchmark never writes.
    file_=CreateFile(path,GENERIC_READ,FILE_SHARE_READ,NULL,OPEN_EXISTING,flags,NULL);
    if(file_==INVALID_HANDLE_VALUE) { _snprintf_s(error_,sizeof(error_),_TRUNCATE,"Cannot open the file (%u)",GetLastError()); return false; }
    DWORD high=0, low=GetFileSize(file_,&high);
    if(low==INVALID_FILE_SIZE && GetLastError()!=NO_ERROR) {
        _snprintf_s(error_,sizeof(error_),_TRUNCATE,"Cannot read the file size (%u)",GetLastError()); Close(); return false;
    }
    size_=(U64(high)<<32)|low;
    return true;
}
int FileReadBackend::Read(U64 offset,DWORD length,unsigned char* data,int& kind) {
    kind=ReadErrorNone;
    if(file_==INVALID_HANDLE_VALUE || length>size_ || offset>size_-length) { kind=ReadErrorFailed; strcpy_s(error_,"Read outside the file"); return -1; }
    LONG high=LONG(offset>>32);
    DWORD low=SetFilePointer(file_,LONG(DWORD(offset)),&high,FILE_BEGIN);
    if(low==INVALID_SET_FILE_POINTER && GetLastError()!=NO_ERROR) {
        kind=ReadErrorFailed; _snprintf_s(error_,sizeof(error_),_TRUNCATE,"Seek failed (%u)",GetLastError()); return -1;
    }
    DWORD got=0;
    if(!ReadFile(file_,data,length,&got,NULL)) {
        kind=ReadErrorFailed; _snprintf_s(error_,sizeof(error_),_TRUNCATE,"Read failed (%u)",GetLastError()); return -1;
    }
    if(got!=length) { kind=ReadErrorShort; _snprintf_s(error_,sizeof(error_),_TRUNCATE,"Read returned %u of %u bytes",got,length); }
    return int(got);
}

NetIsoClient::NetIsoClient(const ReadObserver& o,double timeout) : observer_(o), socket_(INVALID_SOCKET), timeout_(timeout), size_(0), broken_(false) { error_[0]=0; }
NetIsoClient::~NetIsoClient() { Close(); }
void NetIsoClient::Close() {
    if(socket_!=INVALID_SOCKET) { shutdown(socket_,SD_SEND); closesocket(socket_); socket_=INVALID_SOCKET; }
}
void NetIsoClient::Header(unsigned char* p,int command,DWORD index,U64 offset,DWORD length) {
    p[0]='I'; p[1]='S'; p[2]='V'; p[3]='R';
    p[4]=BYTE(command>>8); p[5]=BYTE(command); p[6]=BYTE(index>>8); p[7]=BYTE(index);
    Write64(p+8,offset); Write32(p+16,length);
}
bool NetIsoClient::Fail(int kind,const char* format,...) {
    (void)kind;
    va_list a; va_start(a,format); _vsnprintf_s(error_,sizeof(error_),_TRUNCATE,format,a); va_end(a);
    return false;
}
bool NetIsoClient::Wait(bool writing,double deadline,int& kind) {
    for(;;) {
        if(observer_.cancelled && observer_.cancelled(observer_.context)) { kind=ReadErrorCancelled; return Fail(kind,"Cancelled"); }
        if(Now()>deadline) { kind=ReadErrorTimeout; return Fail(kind,"No reply from the server for %.0f s",timeout_); }
        fd_set set; FD_ZERO(&set); FD_SET(socket_,&set);
        timeval interval={0,100000};
        int rc=select(0,writing ? NULL : &set,writing ? &set : NULL,NULL,&interval);
        if(rc>0) return true;
        if(rc==SOCKET_ERROR) { kind=ReadErrorFailed; return Fail(kind,"Socket wait failed (%d)",WSAGetLastError()); }
    }
}
bool NetIsoClient::SendAll(const unsigned char* data,int count,double deadline,int& kind) {
    while(count) {
        if(!Wait(true,deadline,kind)) return false;
        int sent=send(socket_,reinterpret_cast<const char*>(data),count>WriteSize ? WriteSize : count,0);
        if(sent>0) { data+=sent; count-=sent; Sleep(0); }
        else if(sent==0 || WSAGetLastError()!=WSAEWOULDBLOCK) { kind=ReadErrorFailed; return Fail(kind,"Connection lost while sending (%d)",WSAGetLastError()); }
        else Sleep(1);
    }
    return true;
}
// Receives exactly count bytes. The idle timeout restarts whenever data
// arrives; a reply may take at most six idle timeouts in total.
bool NetIsoClient::ReceiveAll(unsigned char* data,DWORD count,double deadline,DWORD& received,int& kind) {
    received=0;
    const double limit=Now()+timeout_*6;
    while(received<count) {
        if(!Wait(false,deadline<limit ? deadline : limit,kind)) return false;
        int got=recv(socket_,reinterpret_cast<char*>(data+received),int(count-received),0);
        if(got>0) { received+=DWORD(got); deadline=Now()+timeout_; continue; }
        if(got==0) { kind=ReadErrorShort; return Fail(kind,"Server closed the connection after %u of %u bytes",received,count); }
        if(WSAGetLastError()!=WSAEWOULDBLOCK) { kind=ReadErrorFailed; return Fail(kind,"Connection lost while receiving (%d)",WSAGetLastError()); }
        Sleep(1);
    }
    return true;
}
bool NetIsoClient::Connect(DWORD address,int port,double connectSeconds) {
    Close(); broken_=false; size_=0;
    socket_=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);
    if(socket_==INVALID_SOCKET) return Fail(ReadErrorFailed,"Socket create failed (%d)",WSAGetLastError());
    char setup[100];
    if(!PrepareOutbound(socket_,setup,sizeof(setup))) { Close(); return Fail(ReadErrorFailed,"%s",setup); }
    sockaddr_in to={0}; to.sin_family=AF_INET; to.sin_addr.s_addr=address; to.sin_port=htons(u_short(port));
    if(connect(socket_,reinterpret_cast<sockaddr*>(&to),sizeof(to))) {
        int code=WSAGetLastError();
        if(code!=WSAEWOULDBLOCK) { Close(); return Fail(ReadErrorFailed,"Connect failed (%d)",code); }
        const double deadline=Now()+connectSeconds;
        for(;;) {
            if(observer_.cancelled && observer_.cancelled(observer_.context)) { Close(); return Fail(ReadErrorCancelled,"Cancelled"); }
            if(Now()>deadline) { Close(); return Fail(ReadErrorTimeout,"No answer from %u.%u.%u.%u port %d",
                BYTE(address),BYTE(address>>8),BYTE(address>>16),BYTE(address>>24),port); }
            fd_set writable,failed; FD_ZERO(&writable); FD_ZERO(&failed); FD_SET(socket_,&writable); FD_SET(socket_,&failed);
            timeval interval={0,100000};
            int rc=select(0,NULL,&writable,&failed,&interval);
            if(rc==SOCKET_ERROR) { code=WSAGetLastError(); Close(); return Fail(ReadErrorFailed,"Connect wait failed (%d)",code); }
            if(rc==0) continue;
            int status=0;
#ifdef SO_ERROR
            int length=sizeof(status);
            if(getsockopt(socket_,SOL_SOCKET,SO_ERROR,reinterpret_cast<char*>(&status),&length)) status=0;
#endif
            // The Xbox SDK has no SO_ERROR; Winsock reports a refused connect through the except set there.
            if(FD_ISSET(socket_,&failed) || status) {
                Close(); return Fail(ReadErrorFailed,"The server refused the connection (%d). Is it running on port %d?",status,port);
            }
            break;
        }
    }
    return true;
}
bool NetIsoClient::PingServer() {
    unsigned char header[NetIsoHeader],reply[8]; int kind=ReadErrorNone; DWORD got=0;
    Header(header,Ping,0,0,0);
    if(!SendAll(header,NetIsoHeader,Now()+timeout_,kind) || !ReceiveAll(reply,8,Now()+timeout_,got,kind)) { broken_=true; return false; }
    if(memcmp(reply,"ISVRokOK",8)) { broken_=true; return Fail(ReadErrorFailed,"Not a NetISO server: unexpected ping reply"); }
    return true;
}
bool NetIsoClient::ListIsos(char names[][ReadNameMax],int capacity,int& count) {
    count=0;
    unsigned char header[NetIsoHeader],reply[NetIsoNameBytes];
    if(capacity>NetIsoMaxIsos) capacity=NetIsoMaxIsos;
    for(int i=0;i<capacity;++i) {
        int kind=ReadErrorNone; DWORD got=0;
        Header(header,GetIsoName,DWORD(i),0,NetIsoNameBytes);
        if(!SendAll(header,NetIsoHeader,Now()+timeout_,kind) || !ReceiveAll(reply,NetIsoNameBytes,Now()+timeout_,got,kind)) { broken_=true; return false; }
        const unsigned char* end=static_cast<const unsigned char*>(memchr(reply,0,NetIsoNameBytes));
        if(!end) return Fail(ReadErrorFailed,"ISO %d has a name of 256 or more bytes, which this test cannot use",i);
        size_t length=size_t(end-reply);
        if(!length) break;
        for(size_t k=0;k<length;++k) if(reply[k]<0x20 || reply[k]>0x7E) { broken_=true; return Fail(ReadErrorFailed,"Malformed ISO name from the server (index %d)",i); }
        memcpy(names[count],reply,length); names[count][length]=0; ++count;
    }
    return true;
}
bool NetIsoClient::Mount(const char* name,const char names[][ReadNameMax],int count) {
    size_t length=strlen(name);
    if(!length || length>=ReadNameMax || strchr(name,'\\')) return Fail(ReadErrorFailed,"This ISO name cannot be sent to the server");
    // netiso-srv mounts the first listed file whose name ends with the request.
    for(int i=0;i<count;++i) {
        size_t other=strlen(names[i]);
        if(other>length && !strcmp(names[i]+other-length,name))
            return Fail(ReadErrorFailed,"Ambiguous: \"%s\" also ends with this name. Rename one of them.",names[i]);
    }
    // Header and name go out in one send: the reference server reads the name
    // with a single read and gives up if it arrives in pieces.
    unsigned char message[NetIsoHeader+ReadNameMax],reply[8]; int kind=ReadErrorNone; DWORD got=0;
    Header(message,MountIso,0,0,DWORD(length)); memcpy(message+NetIsoHeader,name,length);
    if(!SendAll(message,int(NetIsoHeader+length),Now()+timeout_,kind) || !ReceiveAll(reply,4,Now()+timeout_,got,kind)) { broken_=true; return false; }
    DWORD code=Read32(reply);
    if(code==0) return Fail(ReadErrorFailed,"The server could not mount %s",name);
    if(code!=1) { broken_=true; return Fail(ReadErrorFailed,"Malformed mount reply (%u)",code); }
    unsigned char header[NetIsoHeader];
    Header(header,GetIsoSize,0,0,0);
    if(!SendAll(header,NetIsoHeader,Now()+timeout_,kind) || !ReceiveAll(reply,8,Now()+timeout_,got,kind)) { broken_=true; return false; }
    DWORD sectors=Read32(reply),sectorSize=Read32(reply+4);
    if(sectorSize!=NetIsoSector) { broken_=true; return Fail(ReadErrorFailed,"Malformed size reply (sector size %u)",sectorSize); }
    if(!sectors) return Fail(ReadErrorFailed,"The server reports no mounted ISO");
    size_=U64(sectors)*NetIsoSector;
    return true;
}
int NetIsoClient::Read(U64 offset,DWORD length,unsigned char* data,int& kind) {
    kind=ReadErrorNone;
    if(broken_ || socket_==INVALID_SOCKET) { kind=ReadErrorFailed; return -1; }
    if(!length || length>ReadMaxRequest || length>size_ || offset>size_-length) { kind=ReadErrorFailed; Fail(kind,"Read outside the ISO"); return -1; }
    unsigned char header[NetIsoHeader]; DWORD got=0;
    Header(header,ReadData,0,offset,length);
    // There is no way to resynchronize a stream after a short or late reply,
    // so any failure ends this connection.
    if(!SendAll(header,NetIsoHeader,Now()+timeout_,kind)) { broken_=true; return -1; }
    if(!ReceiveAll(data,length,Now()+timeout_,got,kind)) { broken_=true; return int(got); }
    return int(got);
}

ReadSettings::ReadSettings() : server(0), port(NetIsoPort), seed(1) { strcpy_s(folder,"FastFTP-Bench"); connectx[0]=0; }
bool ValidReadFolder(const char* text,bool absolute) {
    size_t length=strlen(text);
    if(!length || length>=ReadFolderMax || strstr(text,"..")) return false;
    for(size_t i=0;i<length;++i) {
        char c=text[i];
        if(c<0x20 || c>0x7E || c=='*' || c=='?' || c=='"' || c=='<' || c=='>' || c=='|' || c=='/') return false;
    }
    const char* colon=strchr(text,':');
    if(absolute) return colon && colon>text && (colon[1]==0 || colon[1]=='\\');
    return !colon && text[0]!='\\';
}
bool ParseServer(const char* text,DWORD& address,int& port) {
    DWORD octets[4]; const char* p=text;
    for(int i=0;i<4;++i) {
        if(*p<'0' || *p>'9') return false;
        DWORD value=0; int digits=0;
        while(*p>='0' && *p<='9') { value=value*10+DWORD(*p-'0'); if(++digits>3) return false; ++p; }
        if(value>255) return false;
        octets[i]=value;
        if(i<3) { if(*p!='.') return false; ++p; }
    }
    int parsedPort=port;
    if(*p==':') {
        ++p; long value=0; int digits=0;
        while(*p>='0' && *p<='9') { value=value*10+(*p-'0'); if(++digits>5) return false; ++p; }
        if(!digits || value<1 || value>65535) return false;
        parsedPort=int(value);
    }
    if(*p) return false;
    DWORD host=(octets[0]<<24)|(octets[1]<<16)|(octets[2]<<8)|octets[3];
    if(host==0 || host==0xFFFFFFFF) return false;
    address=htonl(host); port=parsedPort;
    return true;
}
void FormatServer(char* out,size_t capacity,DWORD address,int port) {
    DWORD host=ntohl(address);
    _snprintf_s(out,capacity,_TRUNCATE,"%u.%u.%u.%u:%d",host>>24,(host>>16)&255,(host>>8)&255,host&255,port);
}
void ParseReadSettings(const char* text,ReadSettings& s) {
    while(*text) {
        const char* end=text; while(*end && *end!='\n' && *end!='\r') ++end;
        char line[160]; size_t length=size_t(end-text);
        if(length<sizeof(line)) {
            memcpy(line,text,length); line[length]=0;
            char* value=strchr(line,'=');
            if(value) {
                *value++=0;
                if(!_stricmp(line,"read_folder") && ValidReadFolder(value,false)) strcpy_s(s.folder,value);
                else if(!_stricmp(line,"connectx_folder")) { if(!value[0]) s.connectx[0]=0; else if(ValidReadFolder(value,true)) strcpy_s(s.connectx,value); }
                else if(!_stricmp(line,"netiso_server")) { DWORD a=0; int p=s.port; if(!value[0]) s.server=0; else if(ParseServer(value,a,p)) { s.server=a; s.port=p; } }
                else if(!_stricmp(line,"netiso_port")) { long p=strtol(value,NULL,10); if(p>=1 && p<=65535) s.port=int(p); }
                else if(!_stricmp(line,"read_seed")) { char* stop=NULL; unsigned long v=strtoul(value,&stop,0); if(stop && stop!=value && !*stop) s.seed=DWORD(v); }
            }
        }
        text=end; while(*text=='\n' || *text=='\r') ++text;
    }
}
void FormatReadSettings(char* out,size_t capacity,const ReadSettings& s) {
    char server[32]="";
    if(s.server) { DWORD host=ntohl(s.server); _snprintf_s(server,sizeof(server),_TRUNCATE,"%u.%u.%u.%u",host>>24,(host>>16)&255,(host>>8)&255,host&255); }
    _snprintf_s(out,capacity,_TRUNCATE,"read_folder=%s\r\nconnectx_folder=%s\r\nnetiso_server=%s\r\nnetiso_port=%d\r\nread_seed=%u\r\n",
        s.folder,s.connectx,server,s.port,s.seed);
}

const char* ReadStatusName(int s) { return s==ReadCompleted ? "completed" : s==ReadCancelled ? "cancelled" : "failed"; }
const char* ReadVerifyName(int v) {
    return v==VerifyKnownData ? "known data checked" : v==VerifyCounts ? "byte counts only, content not independently verified" : "off";
}
void FormatReadLog(char* out,size_t capacity,const ReadReport& p,const ReadResult& r) {
    int used=_snprintf_s(out,capacity,_TRUNCATE,
        "Read test %s: %s%s%s, %s (" NB_U64 " bytes), %s, seed %u, %u s, cache %s, check %s, pass %d: " NB_U64 " bytes in %.2f s, %.2f MiB/s, %.1f reads/s, "
        "latency ms mean %.2f p50 %.2f p95 %.2f p99 %.2f max %.2f, failed %u, short %u, timeouts %u, integrity %u",
        ReadStatusName(r.status),p.backend,p.server[0] ? " " : "",p.server,p.file,r.fileSize,WorkloadName(p.workload),p.seed,p.seconds,p.cache,
        ReadVerifyName(r.verify),p.pass,r.bytes,r.seconds,r.Rate(),r.Operations(),r.latencyMean,r.latencyP50,r.latencyP95,r.latencyP99,r.latencyMax,
        r.failed,r.shortReads,r.timeouts,r.integrity);
    if(used>0 && r.status!=ReadCompleted && r.error[0]) _snprintf_s(out+used,capacity-size_t(used),_TRUNCATE,". %s",r.error);
}
const char* ReadCsvHeader() {
    return "date,status,backend,server,file,file_bytes,workload,request_bytes,seed,duration_s,cache,check,pass,bytes,seconds,mib_s,reads_s,"
           "verify_seconds,lat_mean_ms,lat_p50_ms,lat_p95_ms,lat_p99_ms,lat_max_ms,failed,short,timeouts,integrity,error\r\n";
}
// CSV text field: quoted, with quotes doubled.
static void Quote(char* out,size_t capacity,const char* text) {
    size_t used=0;
    if(capacity<3) { if(capacity) out[0]=0; return; }
    out[used++]='"';
    for(;*text && used+3<capacity;++text) { if(*text=='"') out[used++]='"'; out[used++]=*text; }
    out[used++]='"'; out[used]=0;
}
void FormatReadCsv(char* out,size_t capacity,const char* date,const ReadReport& p,const ReadResult& r) {
    char backend[48],server[48],file[2*ReadNameMax+4],cache[48],error[2*sizeof(r.error)+4];
    Quote(backend,sizeof(backend),p.backend); Quote(server,sizeof(server),p.server); Quote(file,sizeof(file),p.file);
    Quote(cache,sizeof(cache),p.cache); Quote(error,sizeof(error),r.status==ReadCompleted ? "" : r.error);
    _snprintf_s(out,capacity,_TRUNCATE,"%s,%s,%s,%s,%s," NB_U64 ",%s,%u,%u,%u,%s,%s,%d," NB_U64 ",%.3f,%.3f,%.1f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%u,%u,%u,%u,%s\r\n",
        date,ReadStatusName(r.status),backend,server,file,r.fileSize,WorkloadName(p.workload),WorkloadRequest(p.workload),p.seed,p.seconds,cache,
        r.verify==VerifyKnownData ? "known-data" : r.verify==VerifyCounts ? "counts-only" : "off",p.pass,r.bytes,r.seconds,r.Rate(),r.Operations(),
        r.verifySeconds,r.latencyMean,r.latencyP50,r.latencyP95,r.latencyP99,r.latencyMax,r.failed,r.shortReads,r.timeouts,r.integrity,error);
}
}
