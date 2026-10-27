// SPDX-License-Identifier: MIT
// Minimal passive-mode FTP server for throughput tests. The optional login is
// sent unencrypted, so it must only be armed on a trusted LAN.
#include "ftp.h"
#include "checksum.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace nb {
namespace {
const double ControlIdleSeconds=300, DataWaitSeconds=15, DataIdleSeconds=30;
const char AbortedReply[]="426 Transfer aborted by the client.";
// Longest file name and path accepted from a client: FAT32 names and Windows
// MAX_PATH. FATX itself allows only 42 characters per name.
enum { NameMax=255, PathMax=259, FatxNameMax=42 };
volatile LONG NextDataPort=0,NextTemporary=0;

// Overlaps disk and network: a helper thread writes behind (uploads) or reads
// ahead (downloads) through two buffers of block bytes that alternate with the
// socket side.
struct Pump {
    HANDLE file,thread,filled[2],freed[2];
    unsigned char* buffer[2];
    DWORD block,length[2],error;
    bool writing,uncached;
    volatile LONG stop,failed;
    int next;
    // Uncached uploads: the buffer holding the last, shorter block, which the
    // helper leaves for the caller to write through the cache; -1 for none.
    int tail;
};
DWORD WINAPI PumpThread(void* context) {
    Pump& p=*static_cast<Pump*>(context);
    bool last=false;
    for(int i=0;;i^=1) {
        WaitForSingleObject(p.writing ? p.filled[i] : p.freed[i],INFINITE);
        if(p.stop) break;
        DWORD done=0;
        if(p.writing) {
            if(!p.length[i]) break;
            if(p.uncached && p.length[i]<p.block) { p.tail=i; break; }
            if(!WriteFile(p.file,p.buffer[i],p.length[i],&done,NULL) || done!=p.length[i]) {
                p.error=GetLastError(); InterlockedExchange(&p.failed,1); SetEvent(p.freed[i]); break;
            }
            SetEvent(p.freed[i]);
        } else {
            // A short read is the end of the file. Uncached reads must start on
            // a sector, so none follows it.
            if(!last && !ReadFile(p.file,p.buffer[i],p.block,&done,NULL)) { p.error=GetLastError(); InterlockedExchange(&p.failed,1); done=0; }
            last=done<p.block;
            p.length[i]=done; SetEvent(p.filled[i]);
            if(!done) break;
        }
    }
    return 0;
}
// diskBlock as for ServeFtp; the file must be opened to match.
bool StartPump(Pump& p,HANDLE file,bool writing,int diskBlock) {
    memset(&p,0,sizeof(p)); p.file=file; p.writing=writing; p.tail=-1;
    p.uncached=diskBlock!=0; p.block=p.uncached ? DWORD(diskBlock) : DWORD(FtpBuffer);
    for(int i=0;i<2;++i) {
        // Page-aligned, as uncached I/O needs.
        p.buffer[i]=static_cast<unsigned char*>(VirtualAlloc(NULL,p.block,MEM_COMMIT,PAGE_READWRITE));
        p.filled[i]=CreateEvent(NULL,FALSE,FALSE,NULL);
        p.freed[i]=CreateEvent(NULL,FALSE,TRUE,NULL);
        if(!p.buffer[i] || !p.filled[i] || !p.freed[i]) return false;
    }
    p.thread=CreateThread(NULL,64*1024,PumpThread,&p,0,NULL);
    return p.thread!=NULL;
}
// Stops the helper if it is still running and releases everything.
void EndPump(Pump& p) {
    if(p.thread) {
        InterlockedExchange(&p.stop,1);
        for(int i=0;i<2;++i) { SetEvent(p.filled[i]); SetEvent(p.freed[i]); }
        WaitForSingleObject(p.thread,INFINITE); CloseHandle(p.thread);
    }
    for(int i=0;i<2;++i) {
        if(p.filled[i]) CloseHandle(p.filled[i]);
        if(p.freed[i]) CloseHandle(p.freed[i]);
        if(p.buffer[i]) VirtualFree(p.buffer[i],0,MEM_RELEASE);
    }
    memset(&p,0,sizeof(p));
}

struct Shared {
    const FtpRoot* roots; int rootCount; DWORD address; int socketBuffer,speedLimit,diskBlock; bool downloadsOnly;
    const char* user; const char* password;
    const FtpObserver* observer;
    volatile LONG active,finished;
    // Speed limit bookkeeping: when the data moved so far is paid off.
    CRITICAL_SECTION paceLock; double paceNext;
};

class Session {
    Session(const Session&);
    Session& operator=(const Session&);
    Shared& shared_;
    SOCKET control_,passive_;
    char cwd_[512],input_[1024],renameFrom_[512];
    char user_[64],peer_[24],renamePath_[600];
    int inputCount_;
    // peerAddress_ is the client's IPv4 address and localAddress_ the Xbox
    // address it connected to (network order, 0 if unknown).
    DWORD peerAddress_,localAddress_;
    // discarding_: the rest of an over-long command line is being skipped.
    // aborted_: during a transfer the client sent ABOR or closed the control connection.
    bool loggedIn_,discarding_,aborted_;
    // restart_: where the next RETR or STOR starts (REST). hash_: HASH's algorithm.
    U64 restart_;
    Checksum::Kind hash_;
public:
    Session(Shared& shared,SOCKET control) : shared_(shared),control_(control),passive_(INVALID_SOCKET),inputCount_(0),
        peerAddress_(0),localAddress_(0),loggedIn_(!shared.user),discarding_(false),aborted_(false),
        restart_(0),hash_(Checksum::Sha1) {
        strcpy_s(cwd_,"/"); renameFrom_[0]=0; renamePath_[0]=0; user_[0]=0; strcpy_s(peer_,"?");
        sockaddr_in address; int length=sizeof(address);
        if(!getpeername(control,reinterpret_cast<sockaddr*>(&address),&length)) {
            peerAddress_=address.sin_addr.s_addr;
            const BYTE* ip=reinterpret_cast<const BYTE*>(&address.sin_addr);
            _snprintf_s(peer_,sizeof(peer_),_TRUNCATE,"%u.%u.%u.%u",ip[0],ip[1],ip[2],ip[3]);
        }
        length=sizeof(address);
        if(!getsockname(control,reinterpret_cast<sockaddr*>(&address),&length)) localAddress_=address.sin_addr.s_addr;
    }
    ~Session() { ClosePassive(); }
    void Run();
private:
    bool Cancelled() const { return shared_.observer->cancelled && shared_.observer->cancelled(shared_.observer->context); }
    void Log(const char* format,...) {
        if(!shared_.observer->log) return;
        char line[700]; int used=_snprintf_s(line,sizeof(line),_TRUNCATE,"%s  ",peer_);
        va_list arguments; va_start(arguments,format);
        _vsnprintf_s(line+used,sizeof(line)-used,_TRUNCATE,format,arguments);
        va_end(arguments);
        shared_.observer->log(shared_.observer->context,line);
    }
    void Progress(const char* operation,const char* file,U64 bytes,double seconds,bool done,bool ok) {
        if(shared_.observer->progress) shared_.observer->progress(shared_.observer->context,operation,file,bytes,seconds,done,ok);
    }
    // The data connection is open: log it and let the observer get ready
    // (the Xbox pauses drawing) before the first byte moves.
    void Starting(const char* operation,const char* name,const char* path,U64 offset) {
        const char* kind=operation[0]=='S' ? "Upload" : "Download";
        char from[48]="";
        if(offset) _snprintf_s(from,sizeof(from),_TRUNCATE," at byte %I64u",offset);
        if(Limit(operation)) Log("%s %s started%s, limited to %d MiB/s",kind,path,from,Limit(operation));
        else Log("%s %s started%s",kind,path,from);
        if(shared_.observer->starting) shared_.observer->starting(shared_.observer->context,operation,name);
    }
    // The speed limit for a STOR or RETR; 0 is none.
    int Limit(const char* operation) const { return operation[0]=='S' && shared_.downloadsOnly ? 0 : shared_.speedLimit; }
    // Holds all limited transfers together to the speed limit. Each piece of
    // data books the time it takes at that rate after the bookings before it,
    // and the session waits until its booking ends. A pause of more than 0.1 s
    // earns no credit, so a new transfer cannot burst. False if stopped.
    bool Pace(const char* operation,int count) {
        if(!Limit(operation)) return true;
        EnterCriticalSection(&shared_.paceLock);
        const double now=Now();
        if(shared_.paceNext<now-0.1) shared_.paceNext=now;
        shared_.paceNext+=double(count)/(double(shared_.speedLimit)*1048576.0);
        const double due=shared_.paceNext;
        LeaveCriticalSection(&shared_.paceLock);
        while(Now()<due) { if(Cancelled()) return false; Sleep(1); }
        return true;
    }
    // During a transfer (watchControl) the control connection is watched too,
    // so ABOR or a client that hangs up ends the transfer at once.
    bool Wait(SOCKET s,bool writing,double timeout,bool watchControl=false) {
        double end=Now()+timeout;
        for(;;) {
            if(Cancelled() || Now()>end) return false;
            const bool watching=watchControl && inputCount_<int(sizeof(input_));
            fd_set readable,writable; FD_ZERO(&readable); FD_ZERO(&writable);
            FD_SET(s,writing ? &writable : &readable);
            if(watching) FD_SET(control_,&readable);
            timeval interval={0,100000};
            int rc=select(0,&readable,writing?&writable:NULL,NULL,&interval);
            if(rc==SOCKET_ERROR) return false;
            if(watching && FD_ISSET(control_,&readable) && ControlAborted()) return false;
            if(rc>0 && FD_ISSET(s,writing ? &writable : &readable)) return true;
        }
    }
    // Reads whatever the client sent on the control connection into the
    // command buffer (ReadCommand handles it after the transfer) and reports
    // whether it was ABOR at the start of a line, or the connection closed.
    // FileZilla and others cancel by closing; RFC 959 clients send ABOR.
    bool ControlAborted() {
        if(aborted_) return true;
        int room=int(sizeof(input_))-inputCount_;
        if(room>0) {
            fd_set readable; FD_ZERO(&readable); FD_SET(control_,&readable);
            timeval now={0,0};
            if(select(0,&readable,NULL,NULL,&now)>0) {
                int got=recv(control_,input_+inputCount_,room,0);
                if(got==0 || (got<0 && WSAGetLastError()!=WSAEWOULDBLOCK)) aborted_=true;
                else if(got>0) inputCount_+=got;
            }
        }
        for(int i=0;!aborted_ && i+4<=inputCount_;++i) {
            // Telnet interrupt bytes (0xFF...) may come before an urgent ABOR.
            unsigned char before=i ? static_cast<unsigned char>(input_[i-1]) : '\n';
            if((before<32 || before>=0x7F) && !_strnicmp(input_+i,"ABOR",4) &&
               (i+4==inputCount_ || input_[i+4]=='\r' || input_[i+4]=='\n')) aborted_=true;
        }
        return aborted_;
    }
    // Sends in WriteSize pieces with a yield, as the benchmark does on Xbox.
    bool SendAll(SOCKET s,const char* data,int count,bool watchControl=false) {
        while(count) {
            if(!Wait(s,true,DataIdleSeconds,watchControl)) return false;
            int sent=send(s,data,count>WriteSize ? WriteSize : count,0);
            if(sent>0) { data+=sent; count-=sent; Sleep(0); }
            else if(sent==0 || WSAGetLastError()!=WSAEWOULDBLOCK) return false;
            else Sleep(1);
        }
        return true;
    }
    int Receive(SOCKET s,char* data,int count,double timeout,bool watchControl=false) {
        for(;;) {
            if(!Wait(s,false,timeout,watchControl)) return -1;
            int got=recv(s,data,count,0);
            if(got>=0) return got;
            if(WSAGetLastError()!=WSAEWOULDBLOCK) return -1;
            Sleep(1);
        }
    }
    bool Reply(const char* text) {
        // An over-long reply is cut short rather than dropped, so the client
        // never waits for a reply that will not come.
        char line[600]; int length=_snprintf_s(line,sizeof(line)-2,_TRUNCATE,"%s",text);
        if(length<0) length=int(strlen(line));
        line[length]='\r'; line[length+1]='\n';
        return SendAll(control_,line,length+2);
    }
    int ReadCommand(char* line,size_t capacity);
    void ClosePassive() { if(passive_!=INVALID_SOCKET) { closesocket(passive_); passive_=INVALID_SOCKET; } }
    bool OpenPassive(int& port);
    SOCKET AcceptData();
    void CloseData(SOCKET data) { shutdown(data,SD_SEND); closesocket(data); }
    enum { TopLevel=-1, Invalid=-2, TooLong=-3 };
    int Resolve(const char* argument,char* native,size_t nativeCapacity,char* name,size_t nameCapacity,char* path,size_t pathCapacity);
    bool IsDirectory(int root,const char* native,int depth);
    // How LIST, NLST and MLSD write each entry.
    enum ListStyle { Long, NamesOnly, Facts };
    bool Describe(int root,const char* native,int depth,WIN32_FIND_DATA& entry);
    void List(const char* argument,ListStyle style);
    void ListOne(const char* argument);
    void ModifiedTime(const char* argument);
    void SetModifiedTime(const char* argument);
    void Hash(const char* verb,const char* argument);
    void Retrieve(const char* argument);
    void Store(const char* argument,bool append);
    bool TransferDone(const char* operation,const char* name,const char* path,U64 bytes,double begin,bool ok,const char* failure);
};

// Returns 1 with the next command in line, -1 for a line too long to use,
// or 0 when the connection ends. A line ends at CR or LF; empty lines and
// Telnet bytes before the command (from an urgent ABOR) are skipped.
int Session::ReadCommand(char* line,size_t capacity) {
    for(;;) {
        int end=0;
        while(end<inputCount_ && input_[end]!='\n' && input_[end]!='\r') ++end;
        if(end<inputCount_) {
            int start=0,length=end;
            while(start<length && (static_cast<unsigned char>(input_[start])<=' ' || static_cast<unsigned char>(input_[start])>=0x7F)) ++start;
            while(length>start && input_[length-1]==' ') --length;
            const bool tooLong=discarding_ || size_t(length-start)>=capacity;
            if(!tooLong) { memcpy(line,input_+start,length-start); line[length-start]=0; }
            memmove(input_,input_+end+1,inputCount_-end-1); inputCount_-=end+1;
            discarding_=false;
            if(tooLong) return -1;
            if(line[0]) return 1;
            continue;
        }
        if(inputCount_==int(sizeof(input_))) { inputCount_=0; discarding_=true; } // Skip to the line's end.
        int got=Receive(control_,input_+inputCount_,int(sizeof(input_))-inputCount_,ControlIdleSeconds);
        if(got<=0) return 0;
        inputCount_+=got;
    }
}

bool Session::OpenPassive(int& port) {
    ClosePassive();
    for(int attempt=0;attempt<FtpDataPortCount;++attempt) {
        int candidate=FtpDataPortFirst+int(DWORD(InterlockedIncrement(&NextDataPort))%FtpDataPortCount);
        SOCKET s=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);
        if(s==INVALID_SOCKET) return false;
        char error[100];
        bool ok=(!shared_.socketBuffer || SetSocketBuffers(s,shared_.socketBuffer,error,sizeof(error))) &&
            PrepareListener(s,error,sizeof(error));
        sockaddr_in address={0}; address.sin_family=AF_INET; address.sin_addr.s_addr=INADDR_ANY;
        address.sin_port=htons(u_short(candidate));
        if(ok && !bind(s,reinterpret_cast<sockaddr*>(&address),sizeof(address)) && !listen(s,1)) {
            passive_=s; port=candidate; return true;
        }
        closesocket(s);
    }
    return false;
}

SOCKET Session::AcceptData() {
    if(passive_==INVALID_SOCKET) return INVALID_SOCKET;
    SOCKET data=INVALID_SOCKET;
    const double end=Now()+DataWaitSeconds;
    while(data==INVALID_SOCKET && Wait(passive_,false,end-Now())) {
        sockaddr_in address; int length=sizeof(address);
        data=accept(passive_,reinterpret_cast<sockaddr*>(&address),&length);
        // Only the client on this control connection may connect. Passive ports
        // are easy to guess, so anyone else could otherwise take the transfer.
        if(data!=INVALID_SOCKET && peerAddress_ && address.sin_addr.s_addr!=peerAddress_) {
            const BYTE* ip=reinterpret_cast<const BYTE*>(&address.sin_addr);
            Log("Refused a data connection from %u.%u.%u.%u",ip[0],ip[1],ip[2],ip[3]);
            closesocket(data); data=INVALID_SOCKET;
        }
    }
    ClosePassive();
    char error[100];
    if(data!=INVALID_SOCKET && !PrepareSocket(data,error,sizeof(error))) { closesocket(data); data=INVALID_SOCKET; }
    return data;
}

// Appends text at out+used if it fits within limit characters and capacity.
static bool Append(char* out,size_t capacity,size_t limit,size_t& used,const char* text) {
    size_t length=strlen(text);
    if(used+length>limit || used+length>=capacity) return false;
    memcpy(out+used,text,length+1); used+=length;
    return true;
}

// Maps a client path to a root index (TopLevel for the virtual top level), the
// native path, the final name and the normalized FTP path. Returns Invalid for
// a bad name or unknown drive, and TooLong past NameMax or PathMax; nothing is
// ever copied past a buffer, whatever the client sends.
int Session::Resolve(const char* argument,char* native,size_t nativeCapacity,char* name,size_t nameCapacity,char* path,size_t pathCapacity) {
    path[0]=0; name[0]=0; native[0]=0;
    char combined[1200];
    int written=argument[0]=='/' ? _snprintf_s(combined,sizeof(combined),_TRUNCATE,"%s",argument)
                                 : _snprintf_s(combined,sizeof(combined),_TRUNCATE,"%s/%s",cwd_,argument);
    if(written<0) return TooLong;
    const char* parts[64]; int count=0;
    for(char* token=combined;;) {
        char* slash=strchr(token,'/');
        if(slash) *slash=0;
        if(!strcmp(token,"..")) { if(count) --count; }
        else if(token[0] && strcmp(token,".")) {
            for(const char* c=token;*c;++c) if(*c=='\\' || *c==':' || *c=='*' || *c=='?' || static_cast<unsigned char>(*c)<32) return Invalid;
            if(strlen(token)>size_t(NameMax) || count==64) return TooLong;
            parts[count++]=token;
        }
        if(!slash) break;
        token=slash+1;
    }
    size_t used=0;
    for(int i=0;i<count;++i)
        if(!Append(path,pathCapacity,PathMax,used,"/") || !Append(path,pathCapacity,PathMax,used,parts[i])) return TooLong;
    if(!count) { strcpy_s(path,pathCapacity,"/"); return TopLevel; }
    int root=-1;
    for(int i=0;i<shared_.rootCount;++i) if(!_stricmp(parts[0],shared_.roots[i].name)) root=i;
    if(root<0) return Invalid;
    if(strlen(parts[count-1])>=nameCapacity) return TooLong;
    strcpy_s(name,nameCapacity,parts[count-1]);
    used=0;
    if(shared_.roots[root].path && (!Append(native,nativeCapacity,PathMax,used,shared_.roots[root].path) ||
       !Append(native,nativeCapacity,PathMax,used,"\\"))) return TooLong;
    for(int i=1;i<count;++i)
        if((i>1 && !Append(native,nativeCapacity,PathMax,used,"\\")) || !Append(native,nativeCapacity,PathMax,used,parts[i])) return TooLong;
    return root;
}

bool Session::IsDirectory(int root,const char* native,int depth) {
    if(root==-1) return true;
    if(depth==1) return true; // Drive roots; FATX may not report attributes for them.
    if(!shared_.roots[root].path) return false;
    DWORD attributes=GetFileAttributes(native);
    return attributes!=DWORD(-1) && (attributes&FILE_ATTRIBUTE_DIRECTORY); // -1: no such path
}

static int Depth(const char* path) { int depth=0; for(;*path;++path) if(*path=='/' && path[1]) ++depth; return depth; }

// "<n>MB" or "<n>MB.<anything>" in the null folder streams n MiB (1 to 4096).
static U64 NullSize(const char* name) {
    char* end=NULL; unsigned long mib=strtoul(name,&end,10);
    if(!end || end==name || _strnicmp(end,"MB",2) || (end[2] && end[2]!='.') || mib<1 || mib>4096) return 0;
    return U64(mib)*1048576;
}

// An FTP time (MDTM, MFMT, MLSD's modify fact): YYYYMMDDHHMMSS.
static void FormatTime(const FILETIME& time,char* out,size_t capacity) {
    SYSTEMTIME st; FileTimeToSystemTime(&time,&st);
    _snprintf_s(out,capacity,_TRUNCATE,"%04u%02u%02u%02u%02u%02u",st.wYear,st.wMonth,st.wDay,st.wHour,st.wMinute,st.wSecond);
}
// Reads YYYYMMDDHHMMSS, optionally with fractional seconds, and skips the
// spaces after it. NULL if text does not start with such a time.
static const char* ParseTime(const char* text,FILETIME& time) {
    int digits[14];
    for(int i=0;i<14;++i) { if(text[i]<'0' || text[i]>'9') return NULL; digits[i]=text[i]-'0'; }
    SYSTEMTIME st={0};
    st.wYear=WORD(digits[0]*1000+digits[1]*100+digits[2]*10+digits[3]); st.wMonth=WORD(digits[4]*10+digits[5]);
    st.wDay=WORD(digits[6]*10+digits[7]); st.wHour=WORD(digits[8]*10+digits[9]);
    st.wMinute=WORD(digits[10]*10+digits[11]); st.wSecond=WORD(digits[12]*10+digits[13]);
    text+=14;
    if(*text=='.') { ++text; while(*text>='0' && *text<='9') ++text; }
    if(*text!=' ' || st.wYear<1980 || !SystemTimeToFileTime(&st,&time)) return NULL;
    while(*text==' ') ++text;
    return text;
}

static void FormatEntry(char* line,size_t capacity,const char* name,bool directory,U64 size,const FILETIME* time,int style) {
    enum { Long, NamesOnly, Facts }; // Session::ListStyle
    if(style==NamesOnly) { _snprintf_s(line,capacity,_TRUNCATE,"%s\r\n",name); return; }
    if(style==Facts) {
        char modify[32]="";
        if(time) { strcpy_s(modify,"modify="); FormatTime(*time,modify+7,sizeof(modify)-7); strcat_s(modify,sizeof(modify),";"); }
        if(directory) _snprintf_s(line,capacity,_TRUNCATE,"type=dir;%s %s\r\n",modify,name);
        else _snprintf_s(line,capacity,_TRUNCATE,"type=file;size=%I64u;%s %s\r\n",size,modify,name);
        return;
    }
    static const char* months[]={"Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec"};
    // Same shape as Aurora's listing: everything rwx for root, and 1 Jan 2000
    // where there is no time (the drive folders).
    SYSTEMTIME st={0}; st.wYear=2000; st.wMonth=1; st.wDay=1;
    if(time) FileTimeToSystemTime(time,&st);
    if(st.wMonth<1 || st.wMonth>12) st.wMonth=1;
    _snprintf_s(line,capacity,_TRUNCATE,"%s 1 root root %I64u %s %2u %04u %s\r\n",
        directory ? "drwxrwxrwx" : "-rwxrwxrwx",size,months[st.wMonth-1],st.wDay,st.wYear,name);
}

// The directory entry for a file or folder below a drive folder.
bool Session::Describe(int root,const char* native,int depth,WIN32_FIND_DATA& entry) {
    if(root<0 || depth<2 || !shared_.roots[root].path) return false;
    HANDLE find=FindFirstFile(native,&entry);
    if(find==INVALID_HANDLE_VALUE) return false;
    FindClose(find);
    return true;
}

void Session::List(const char* argument,ListStyle style) {
    // MLSD has no options; LIST and NLST skip ls-style flags such as -la.
    while(style!=Facts && argument[0]=='-') { const char* space=strchr(argument,' '); argument=space ? space+1 : ""; }
    char native[600],name[300],path[600];
    int root=Resolve(argument,native,sizeof(native),name,sizeof(name),path,sizeof(path));
    if(root<TopLevel) { Reply("550 No such file or folder."); return; }
    int depth=Depth(path);
    if(style==Facts && !IsDirectory(root,native,depth)) { Reply("501 Not a folder. Use MLST for a file."); return; }
    if(passive_==INVALID_SOCKET) { Reply("425 Use PASV or EPSV first."); return; }
    // Build the whole listing before opening the data connection.
    size_t capacity=65536,used=0; char* text=static_cast<char*>(malloc(capacity));
    if(!text) { Reply("451 Out of memory."); return; }
    char line[700]; bool found=true;
    if(root==-1) {
        for(int i=0;i<shared_.rootCount;++i) {
            FormatEntry(line,sizeof(line),shared_.roots[i].name,true,0,NULL,style);
            size_t n=strlen(line); if(used+n<capacity) { memcpy(text+used,line,n); used+=n; }
        }
    } else if(!shared_.roots[root].path) {
        static const char* samples[]={"64MB.bin","256MB.bin","1024MB.bin"};
        for(int i=0;i<3;++i) {
            if(depth==2 && _stricmp(name,samples[i])) continue;
            FormatEntry(line,sizeof(line),samples[i],false,NullSize(samples[i]),NULL,style);
            size_t n=strlen(line); if(used+n<capacity) { memcpy(text+used,line,n); used+=n; }
        }
        found=depth<=2;
    } else {
        char pattern[620];
        if(IsDirectory(root,native,depth)) _snprintf_s(pattern,sizeof(pattern),_TRUNCATE,"%s%s*",native,depth>1 ? "\\" : "");
        else strcpy_s(pattern,native);
        WIN32_FIND_DATA entry;
        HANDLE find=FindFirstFile(pattern,&entry);
        found=find!=INVALID_HANDLE_VALUE || IsDirectory(root,native,depth);
        if(find!=INVALID_HANDLE_VALUE) {
            do {
                if(!strcmp(entry.cFileName,".") || !strcmp(entry.cFileName,"..")) continue;
                FormatEntry(line,sizeof(line),entry.cFileName,(entry.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)!=0,
                    (U64(entry.nFileSizeHigh)<<32)|entry.nFileSizeLow,&entry.ftLastWriteTime,style);
                size_t n=strlen(line);
                if(used+n>=capacity) {
                    char* larger=static_cast<char*>(realloc(text,capacity*2));
                    if(!larger) break;
                    text=larger; capacity*=2;
                }
                memcpy(text+used,line,n); used+=n;
            } while(FindNextFile(find,&entry));
            FindClose(find);
        }
    }
    if(!found) { free(text); Reply("550 No such file or folder."); return; }
    Reply("150 Here comes the listing.");
    SOCKET data=AcceptData();
    if(data==INVALID_SOCKET) { free(text); Reply("425 Data connection was not opened."); return; }
    bool ok=SendAll(data,text,int(used));
    free(text); CloseData(data);
    Reply(ok ? "226 Listing sent." : "426 Listing interrupted.");
}

// MLST: one entry's facts on the control connection.
void Session::ListOne(const char* argument) {
    char native[600],name[300],path[600],line[700],text[1400];
    int root=Resolve(argument,native,sizeof(native),name,sizeof(name),path,sizeof(path));
    const int depth=Depth(path);
    WIN32_FIND_DATA entry;
    if(root==TopLevel || (root>=0 && depth==1)) FormatEntry(line,sizeof(line),path,true,0,NULL,Facts);
    else if(root>=0 && !shared_.roots[root].path && depth==2 && NullSize(name)) FormatEntry(line,sizeof(line),path,false,NullSize(name),NULL,Facts);
    else if(Describe(root,native,depth,entry))
        FormatEntry(line,sizeof(line),path,(entry.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)!=0,
            (U64(entry.nFileSizeHigh)<<32)|entry.nFileSizeLow,&entry.ftLastWriteTime,Facts);
    else { Reply("550 No such file or folder."); return; }
    line[strlen(line)-2]=0; // FormatEntry's CRLF; Reply adds the last one.
    _snprintf_s(text,sizeof(text),_TRUNCATE,"250-Facts follow\r\n %s\r\n250 End",line);
    Reply(text);
}

// MDTM: a file's modification time.
void Session::ModifiedTime(const char* argument) {
    char native[600],name[300],path[600],text[64];
    int root=Resolve(argument,native,sizeof(native),name,sizeof(name),path,sizeof(path));
    WIN32_FIND_DATA entry;
    if(!Describe(root,native,Depth(path),entry) || (entry.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)) { Reply("550 Not a file."); return; }
    strcpy_s(text,"213 "); FormatTime(entry.ftLastWriteTime,text+4,sizeof(text)-4);
    Reply(text);
}

// MFMT <time> <path>: sets a file's modification time, so a client can keep
// the PC's dates on uploaded files.
void Session::SetModifiedTime(const char* argument) {
    FILETIME time; const char* target=ParseTime(argument,time);
    if(!target || !*target) { Reply("501 Use MFMT YYYYMMDDHHMMSS path."); return; }
    char native[600],name[300],path[600],text[700];
    int root=Resolve(target,native,sizeof(native),name,sizeof(name),path,sizeof(path));
    WIN32_FIND_DATA entry;
    if(!Describe(root,native,Depth(path),entry) || (entry.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)) { Reply("550 Not a file."); return; }
    HANDLE file=CreateFile(native,GENERIC_WRITE,FILE_SHARE_READ,NULL,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,NULL);
    bool done=file!=INVALID_HANDLE_VALUE && SetFileTime(file,NULL,NULL,&time);
    if(file!=INVALID_HANDLE_VALUE) CloseHandle(file);
    if(!done) { Reply("550 Cannot set the time."); return; }
    char stamp[16]; FormatTime(time,stamp,sizeof(stamp));
    _snprintf_s(text,sizeof(text),_TRUNCATE,"213 Modify=%s; %s",stamp,path);
    Reply(text);
}

// HASH (draft-bryan-ftp-hash, algorithm from OPTS HASH), XCRC, XMD5 and XSHA1:
// a checksum of a whole file, read on the Xbox, so an upload can be checked
// without a download. The reply comes when the file has been read.
void Session::Hash(const char* verb,const char* argument) {
    Checksum::Kind kind=hash_;
    if(!strcmp(verb,"XCRC")) kind=Checksum::Crc32;
    else if(!strcmp(verb,"XMD5")) kind=Checksum::Md5;
    else if(!strcmp(verb,"XSHA1")) kind=Checksum::Sha1;
    // Some clients quote the name, and some X commands may add a start and end.
    char target[600]; strncpy_s(target,argument[0]=='"' ? argument+1 : argument,_TRUNCATE);
    if(argument[0]=='"') { char* quote=strrchr(target,'"'); if(quote) *quote=0; }
    char native[600],name[300],path[600],text[800];
    int root=Resolve(target,native,sizeof(native),name,sizeof(name),path,sizeof(path));
    WIN32_FIND_DATA entry;
    if(!Describe(root,native,Depth(path),entry) || (entry.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)) { Reply("550 Not a file."); return; }
    HANDLE file=CreateFile(native,GENERIC_READ,FILE_SHARE_READ,NULL,OPEN_EXISTING,
        FILE_FLAG_SEQUENTIAL_SCAN|(shared_.diskBlock ? FILE_FLAG_NO_BUFFERING : 0),NULL);
    if(file==INVALID_HANDLE_VALUE) { Reply("550 Cannot open the file."); return; }
    Checksum sum(kind);
    const double begin=Now(); U64 bytes=0; bool ok=true; const char* failure="451 Read error on the Xbox drive.";
    aborted_=false;
    Pump pump;
    if(!StartPump(pump,file,false,shared_.diskBlock)) { ok=false; failure="451 Out of memory."; }
    while(ok) {
        int i=pump.next;
        while(WaitForSingleObject(pump.filled[i],100)==WAIT_TIMEOUT) if(Cancelled() || ControlAborted()) { ok=false; failure=AbortedReply; break; }
        if(!ok) break;
        if(pump.failed) { ok=false; break; }
        if(!pump.length[i]) break;
        sum.Add(pump.buffer[i],pump.length[i]); bytes+=pump.length[i];
        SetEvent(pump.freed[i]); pump.next^=1;
    }
    EndPump(pump); CloseHandle(file);
    if(!ok) { Log("Checksum of %s FAILED after %I64u bytes",path,bytes); Reply(failure); return; }
    char digest[41]; sum.Finish(digest);
    const double seconds=Now()-begin;
    Log("%s %s %s, %I64u bytes in %.2f s",Checksum::Name(kind),path,digest,bytes,seconds);
    if(!strcmp(verb,"HASH")) _snprintf_s(text,sizeof(text),_TRUNCATE,"213 %s 0-%I64u %s %s",Checksum::Name(kind),bytes,digest,path);
    else _snprintf_s(text,sizeof(text),_TRUNCATE,"250 %s",digest);
    Reply(text);
}

bool Session::TransferDone(const char* operation,const char* name,const char* path,U64 bytes,double begin,bool ok,const char* failure) {
    double seconds=Now()-begin;
    Progress(operation,name,bytes,seconds,true,ok);
    const char* kind=operation[0]=='S' ? "Upload" : "Download";
    if(ok) Log("%s %s, %I64u bytes in %.2f s (%.2f MiB/s)",kind,path,bytes,seconds,seconds>0 ? double(bytes)/1048576/seconds : 0);
    else Log("%s %s FAILED after %I64u bytes: %s",kind,path,bytes,failure);
    char text[200];
    if(ok) _snprintf_s(text,sizeof(text),_TRUNCATE,"226 Transfer complete: %I64u bytes in %.2f s (%.2f MiB/s).",
        bytes,seconds,seconds>0 ? double(bytes)/1048576/seconds : 0);
    else strcpy_s(text,failure);
    return Reply(text);
}

void Session::Retrieve(const char* argument) {
    char native[600],name[300],path[600];
    int root=Resolve(argument,native,sizeof(native),name,sizeof(name),path,sizeof(path));
    if(root<0 || IsDirectory(root,native,Depth(path))) { Reply("550 Not a file."); return; }
    if(passive_==INVALID_SOCKET) { Reply("425 Use PASV or EPSV first."); return; }
    bool null=!shared_.roots[root].path;
    U64 size=null ? NullSize(name) : 0;
    // A resumed download (REST) starts at offset. Uncached reads must start on
    // a whole block, so they start before it and skip the first skip bytes.
    const U64 offset=restart_; DWORD skip=0;
    HANDLE file=INVALID_HANDLE_VALUE;
    if(null && (!size || Depth(path)!=2)) { Reply("550 Use a name like 256MB.bin in this folder."); return; }
    if(!null) {
        file=CreateFile(native,GENERIC_READ,FILE_SHARE_READ,NULL,OPEN_EXISTING,
            FILE_FLAG_SEQUENTIAL_SCAN|(shared_.diskBlock ? FILE_FLAG_NO_BUFFERING : 0),NULL);
        if(file==INVALID_HANDLE_VALUE) { Reply("550 Cannot open the file."); return; }
        DWORD high=0; const DWORD low=GetFileSize(file,&high);
        size=(U64(high)<<32)|low;
    }
    if(offset>size) { if(file!=INVALID_HANDLE_VALUE) CloseHandle(file); Reply("551 The restart point is past the end of the file."); return; }
    if(offset && !null) {
        const U64 start=shared_.diskBlock ? offset-offset%U64(shared_.diskBlock) : offset;
        skip=DWORD(offset-start);
        LONG high=LONG(start>>32);
        if(SetFilePointer(file,LONG(DWORD(start)),&high,FILE_BEGIN)==INVALID_SET_FILE_POINTER && GetLastError()!=NO_ERROR) {
            CloseHandle(file); Reply("551 Cannot start at the restart point."); return;
        }
    }
    Reply("150 Opening binary data connection.");
    SOCKET data=AcceptData();
    if(data==INVALID_SOCKET) { if(file!=INVALID_HANDLE_VALUE) CloseHandle(file); Reply("425 Data connection was not opened."); return; }
    Starting("RETR",name,path,offset);
    double begin=Now(),update=begin; U64 bytes=0; bool ok=true; const char* failure="426 Transfer interrupted.";
    aborted_=false;
    if(null) {
        unsigned char* block=static_cast<unsigned char*>(malloc(Chunk));
        if(!block) ok=false;
        else {
            Pattern(block,0x46545021);
            for(U64 at=offset;ok && at<size;at+=U64(Chunk)-at%U64(Chunk)) {
                const int start=int(at%U64(Chunk));
                int count=int(size-at<U64(Chunk-start) ? size-at : Chunk-start);
                ok=SendAll(data,reinterpret_cast<const char*>(block)+start,count,true);
                if(ok) { bytes+=count; ok=Pace("RETR",count); }
                if(Now()-update>=0.25) { update=Now(); Progress("RETR",name,bytes,update-begin,false,true); }
            }
            free(block);
        }
    } else {
        Pump pump;
        if(!StartPump(pump,file,false,shared_.diskBlock)) { ok=false; failure="451 Out of memory."; }
        while(ok) {
            int i=pump.next;
            while(WaitForSingleObject(pump.filled[i],100)==WAIT_TIMEOUT) if(Cancelled()) { ok=false; break; }
            if(!ok) break;
            if(pump.failed) { ok=false; failure="451 Read error on the Xbox drive."; break; }
            if(!pump.length[i]) break;
            // Sent a Chunk at a time, so a speed limit spreads the buffer out.
            const DWORD skipped=skip<pump.length[i] ? skip : pump.length[i]; skip-=skipped;
            const char* piece=reinterpret_cast<const char*>(pump.buffer[i])+skipped;
            for(DWORD left=pump.length[i]-skipped;ok && left;) {
                const int count=int(left>DWORD(Chunk) ? DWORD(Chunk) : left);
                ok=SendAll(data,piece,count,true);
                if(ok) { bytes+=count; piece+=count; left-=count; ok=Pace("RETR",count); }
            }
            SetEvent(pump.freed[i]); pump.next^=1;
            if(Now()-update>=0.25) { update=Now(); Progress("RETR",name,bytes,update-begin,false,true); }
        }
        EndPump(pump); CloseHandle(file);
    }
    CloseData(data);
    if(aborted_) failure=AbortedReply;
    TransferDone("RETR",name,path,bytes,begin,ok,failure);
}

// A name for a temporary file in native's folder, unique while the app runs.
static void TemporaryName(const char* native,const char* suffix,char* out,size_t capacity) {
    const char* slash=strrchr(native,'\\');
    int folder=slash ? int(slash-native)+1 : 0;
    _snprintf_s(out,capacity,_TRUNCATE,"%.*sFastFTP360-%u.%s",folder,native,unsigned(InterlockedIncrement(&NextTemporary)),suffix);
}
// Appends data through the file cache: an uncached upload's last, shorter
// block, since uncached writes take whole sectors. Reopens file for this.
static bool AppendCached(HANDLE& file,const char* path,const void* data,DWORD length) {
    CloseHandle(file);
    file=CreateFile(path,GENERIC_WRITE,0,NULL,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,NULL);
    if(file==INVALID_HANDLE_VALUE) return false;
    LONG high=0; DWORD written=0;
    if(SetFilePointer(file,0,&high,FILE_END)==INVALID_SET_FILE_POINTER && GetLastError()!=NO_ERROR) return false;
    return WriteFile(file,data,length,&written,NULL) && written==length;
}
// Puts the finished upload in place of the old file. The old file is moved
// aside first and moved back if that fails, so one of them always survives.
static bool ReplaceWithUpload(const char* temporary,const char* native) {
    char aside[620]; TemporaryName(native,"old",aside,sizeof(aside));
    if(!MoveFile(native,aside)) return false;
    if(!MoveFile(temporary,native)) { MoveFile(aside,native); return false; }
    DeleteFile(aside);
    return true;
}

// STOR, or APPE with append. After REST n, or for APPE to an existing file,
// the upload continues the file at byte n in place; otherwise a new file is
// written, or a temporary one that replaces the old file at the end.
void Session::Store(const char* argument,bool append) {
    char native[600],name[300],path[600],temporary[620]="";
    int root=Resolve(argument,native,sizeof(native),name,sizeof(name),path,sizeof(path));
    if(root==TooLong) { Reply("553 The file name or path is too long."); return; }
    if(root<0 || Depth(path)<2) { Reply("553 Choose a file inside a drive folder."); return; }
    if(passive_==INVALID_SOCKET) { Reply("425 Use PASV or EPSV first."); return; }
    bool null=!shared_.roots[root].path;
    if(null && Depth(path)!=2) { Reply("553 The null folder has no subfolders."); return; }
    if(null && (append || restart_)) { Reply("553 The null folder cannot resume."); return; }
    HANDLE file=INVALID_HANDLE_VALUE;
    U64 offset=0; bool resumed=false;
    // Uncached for this upload; a resumed one goes through the cache when it
    // starts off a whole block.
    bool uncached=shared_.diskBlock!=0;
    if(!null) {
        DWORD attributes=GetFileAttributes(native);
        if(attributes!=DWORD(-1) && (attributes&FILE_ATTRIBUTE_DIRECTORY)) { Reply("553 A folder already has that name."); return; }
        if(attributes!=DWORD(-1) && (append || restart_)) {
            WIN32_FIND_DATA entry; HANDLE find=FindFirstFile(native,&entry);
            if(find==INVALID_HANDLE_VALUE) { Reply("550 Cannot open the file."); return; }
            FindClose(find);
            const U64 size=(U64(entry.nFileSizeHigh)<<32)|entry.nFileSizeLow;
            offset=append ? size : restart_;
            if(offset>size) { Reply("551 The restart point is past the end of the file."); return; }
            if(uncached && offset%U64(shared_.diskBlock)) uncached=false;
            file=CreateFile(native,GENERIC_WRITE,0,NULL,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL|(uncached ? FILE_FLAG_NO_BUFFERING : 0),NULL);
            if(file==INVALID_HANDLE_VALUE) { Reply("550 Cannot open the file."); return; }
            resumed=true;
            // Anything past the restart point is replaced by what comes now.
            LONG high=LONG(offset>>32);
            if((SetFilePointer(file,LONG(DWORD(offset)),&high,FILE_BEGIN)==INVALID_SET_FILE_POINTER && GetLastError()!=NO_ERROR) || !SetEndOfFile(file)) {
                CloseHandle(file); Reply("551 Cannot continue the file at the restart point."); return;
            }
        } else if(restart_) { Reply("551 There is no file to resume."); return; }
        else {
            // Replacing a file: receive into a temporary file next to it, so a
            // failed or cancelled upload leaves the old file as it was.
            if(attributes!=DWORD(-1)) TemporaryName(native,"tmp",temporary,sizeof(temporary));
            file=CreateFile(temporary[0] ? temporary : native,GENERIC_WRITE,0,NULL,CREATE_ALWAYS,
                FILE_ATTRIBUTE_NORMAL|(uncached ? FILE_FLAG_NO_BUFFERING : 0),NULL);
            if(file==INVALID_HANDLE_VALUE) {
                Reply(strlen(name)>size_t(FatxNameMax) ? "553 Cannot create the file. Xbox drives allow names of up to 42 characters." : "553 Cannot create the file.");
                return;
            }
        }
    }
    const char* written=temporary[0] ? temporary : native;
    Reply("150 Ready to receive.");
    SOCKET data=AcceptData();
    if(data==INVALID_SOCKET) {
        if(file!=INVALID_HANDLE_VALUE) { CloseHandle(file); if(!resumed) DeleteFile(written); }
        Reply("425 Data connection was not opened."); return;
    }
    Starting("STOR",name,path,offset);
    double begin=Now(),update=begin; U64 bytes=0; bool ok=true; const char* failure="426 Transfer interrupted.";
    aborted_=false;
    if(null) {
        char* sink=static_cast<char*>(malloc(Chunk*4));
        if(!sink) ok=false;
        for(;ok;) {
            int got=Receive(data,sink,Chunk*4,DataIdleSeconds,true);
            if(got<0) ok=false; else if(!got) break; else { bytes+=got; ok=Pace("STOR",got); }
            if(Now()-update>=0.25) { update=Now(); Progress("STOR",name,bytes,update-begin,false,true); }
        }
        free(sink);
        // A client that cancels may close the data connection first: the end
        // of the data is only a finished upload if the client did not abort.
        if(ok && ControlAborted()) ok=false;
    } else {
        // diskFailed: the drive, not the connection, ended the upload.
        Pump pump; bool eof=false,diskFailed=false;
        if(!StartPump(pump,file,true,uncached ? shared_.diskBlock : 0)) { ok=false; diskFailed=true; failure="451 Out of memory."; }
        while(ok && !eof) {
            int i=pump.next;
            while(WaitForSingleObject(pump.freed[i],100)==WAIT_TIMEOUT) if(Cancelled() || pump.failed) { ok=false; break; }
            if(!ok || pump.failed) { ok=false; break; }
            DWORD filled=0;
            while(filled<pump.block) {
                int got=Receive(data,reinterpret_cast<char*>(pump.buffer[i])+filled,int(pump.block-filled),DataIdleSeconds,true);
                if(got<0) { ok=false; break; }
                if(!got) { eof=true; break; }
                filled+=got; bytes+=got;
                if(!Pace("STOR",got)) { ok=false; break; }
                if(Now()-update>=0.25) { update=Now(); Progress("STOR",name,bytes,update-begin,false,true); }
            }
            if(!ok) break;
            pump.length[i]=filled; SetEvent(pump.filled[i]); pump.next^=1;
            if(eof && filled) {
                // A zero-length buffer tells the writer the upload has ended.
                int j=pump.next;
                while(WaitForSingleObject(pump.freed[j],100)==WAIT_TIMEOUT) if(pump.failed) break;
                pump.length[j]=0; SetEvent(pump.filled[j]);
            }
        }
        if(ok) WaitForSingleObject(pump.thread,INFINITE);
        if(pump.failed) { ok=false; diskFailed=true; failure="452 Write error on the Xbox drive. Is it full?"; }
        if(ok && pump.tail>=0 && !AppendCached(file,written,pump.buffer[pump.tail],pump.length[pump.tail])) {
            ok=false; diskFailed=true; failure="452 Write error on the Xbox drive. Is it full?";
        }
        EndPump(pump);
        if(ok && !FlushFileBuffers(file)) { ok=false; diskFailed=true; failure="452 Write error on the Xbox drive. Is it full?"; }
        if(file!=INVALID_HANDLE_VALUE) CloseHandle(file);
        // A client that cancels may close the data connection just before it
        // hangs up or sends ABOR; by the time the file is flushed, that is in.
        if(ok && ControlAborted()) ok=false;
        if(ok && temporary[0] && !ReplaceWithUpload(temporary,native)) { ok=false; failure="451 Cannot replace the old file. It was left as it was."; }
        // A new or resumed file that the connection cut short stays, so the
        // client can resume it. A temporary file goes: the old file is kept.
        if(!ok) {
            const bool keep=!temporary[0] && !diskFailed && (resumed || bytes);
            if(keep) Log("Kept the unfinished %s for resuming",path);
            else if(!resumed) DeleteFile(written);
        }
    }
    CloseData(data);
    if(aborted_) failure=AbortedReply;
    TransferDone("STOR",name,path,bytes,begin,ok,failure);
}

void Session::Run() {
    Log("Connected");
    Reply(shared_.user ? "220 FastFTP360 ready." : "220 FastFTP360 ready. Any user name and password.");
    char line[600];
    for(int status;!Cancelled() && (status=ReadCommand(line,sizeof(line)))!=0;) {
        if(status<0) { Reply("500 Command line too long."); continue; }
        char verb[8]={0}; const char* argument=line;
        for(int i=0;i<7 && *argument && *argument!=' ';++i,++argument) verb[i]=char(toupper(static_cast<unsigned char>(*argument)));
        while(*argument && *argument!=' ') ++argument;
        while(*argument==' ') ++argument;
        char native[600],name[300],path[600],text[700];
        if(!strcmp(verb,"USER")) {
            if(!shared_.user) { Reply("331 Any password will do."); continue; }
            strncpy_s(user_,argument,_TRUNCATE); loggedIn_=false; Reply("331 Password required.");
        }
        else if(!strcmp(verb,"PASS")) {
            if(!shared_.user) { Log("Logged in (no login required)"); Reply("230 Logged in."); continue; }
            loggedIn_=!strcmp(user_,shared_.user) && !strcmp(argument,shared_.password);
            // A short pause makes guessing passwords slow.
            if(!loggedIn_) { Log("Wrong user name or password (user %s)",user_); Sleep(1000); Reply("530 Wrong user name or password."); }
            else { Log("Logged in as %s",user_); Reply("230 Logged in."); }
        }
        else if(!strcmp(verb,"QUIT")) { Reply("221 Goodbye."); break; }
        else if(!loggedIn_ && strcmp(verb,"SYST") && strcmp(verb,"FEAT") && strcmp(verb,"OPTS") && strcmp(verb,"NOOP"))
            Reply("530 Log in with USER and PASS first.");
        else if(!strcmp(verb,"SYST")) Reply("215 UNIX Type: L8");
        else if(!strcmp(verb,"FEAT")) {
            _snprintf_s(text,sizeof(text),_TRUNCATE,"211-Features:\r\n SIZE\r\n PASV\r\n EPSV\r\n REST STREAM\r\n MDTM\r\n MFMT\r\n"
                " MLST type*;size*;modify*;\r\n HASH CRC32%s;MD5%s;SHA-1%s\r\n XCRC\r\n XMD5\r\n XSHA1\r\n211 End",
                hash_==Checksum::Crc32 ? "*" : "",hash_==Checksum::Md5 ? "*" : "",hash_==Checksum::Sha1 ? "*" : "");
            Reply(text);
        }
        else if(!strcmp(verb,"OPTS") && !_strnicmp(argument,"HASH",4) && (!argument[4] || argument[4]==' ')) {
            const char* choice=argument+4; while(*choice==' ') ++choice;
            Checksum::Kind kind;
            if(!*choice) kind=hash_;
            else if(!Checksum::Parse(choice,kind)) { Reply("501 Unknown algorithm. Use CRC32, MD5 or SHA-1."); continue; }
            hash_=kind; _snprintf_s(text,sizeof(text),_TRUNCATE,"200 %s",Checksum::Name(kind)); Reply(text);
        }
        else if(!strcmp(verb,"OPTS") || !strcmp(verb,"CLNT") || !strcmp(verb,"MODE") || !strcmp(verb,"STRU")) Reply("200 OK.");
        else if(!strcmp(verb,"TYPE")) Reply("200 Binary transfers are used for every type.");
        else if(!strcmp(verb,"NOOP")) Reply("200 OK.");
        else if(!strcmp(verb,"ALLO")) Reply("202 No storage allocation needed.");
        else if(!strcmp(verb,"PWD") || !strcmp(verb,"XPWD")) { _snprintf_s(text,sizeof(text),_TRUNCATE,"257 \"%s\" is the current folder.",cwd_); Reply(text); }
        else if(!strcmp(verb,"CWD") || !strcmp(verb,"XCWD") || !strcmp(verb,"CDUP") || !strcmp(verb,"XCUP")) {
            const char* target=(verb[0]=='C' && verb[1]=='D') || !strcmp(verb,"XCUP") ? ".." : argument;
            int root=Resolve(target,native,sizeof(native),name,sizeof(name),path,sizeof(path));
            if(root>=TopLevel && IsDirectory(root,native,Depth(path))) { strcpy_s(cwd_,path); Reply("250 Folder changed."); }
            else Reply("550 No such folder.");
        }
        else if(!strcmp(verb,"PASV") || !strcmp(verb,"EPSV")) {
            int port=0;
            if(!OpenPassive(port)) { Reply("425 Cannot open a passive port."); continue; }
            // The address the client reached us on; the one found at server
            // start is only a fallback, as it is 0.0.0.0 before DHCP finishes.
            const DWORD address=localAddress_ ? localAddress_ : shared_.address;
            const BYTE* ip=reinterpret_cast<const BYTE*>(&address);
            if(verb[0]=='P') _snprintf_s(text,sizeof(text),_TRUNCATE,"227 Entering Passive Mode (%u,%u,%u,%u,%d,%d).",ip[0],ip[1],ip[2],ip[3],port>>8,port&255);
            else _snprintf_s(text,sizeof(text),_TRUNCATE,"229 Entering Extended Passive Mode (|||%d|)",port);
            Reply(text);
        }
        else if(!strcmp(verb,"PORT") || !strcmp(verb,"EPRT")) Reply("502 Active mode is not supported. Use passive mode.");
        else if(!strcmp(verb,"LIST") || !strcmp(verb,"NLST") || !strcmp(verb,"MLSD")) List(argument,verb[0]=='L' ? Long : verb[0]=='N' ? NamesOnly : Facts);
        else if(!strcmp(verb,"MLST")) ListOne(argument);
        else if(!strcmp(verb,"MDTM")) ModifiedTime(argument);
        else if(!strcmp(verb,"MFMT")) SetModifiedTime(argument);
        else if(!strcmp(verb,"HASH") || !strcmp(verb,"XCRC") || !strcmp(verb,"XMD5") || !strcmp(verb,"XSHA1")) Hash(verb,argument);
        else if(!strcmp(verb,"RETR")) Retrieve(argument);
        else if(!strcmp(verb,"STOR") || !strcmp(verb,"APPE")) Store(argument,verb[0]=='A');
        else if(!strcmp(verb,"REST")) {
            U64 offset=0; const char* digit=argument;
            for(;*digit>='0' && *digit<='9' && offset<(U64(1)<<60);++digit) offset=offset*10+U64(*digit-'0');
            if(digit==argument || *digit) { Reply("501 Use REST with a byte count."); continue; }
            restart_=offset;
            _snprintf_s(text,sizeof(text),_TRUNCATE,"350 Restarting at %I64u.",offset); Reply(text);
            continue;
        }
        else if(!strcmp(verb,"SIZE")) {
            int root=Resolve(argument,native,sizeof(native),name,sizeof(name),path,sizeof(path));
            U64 size=0; bool known=false;
            if(root>=0 && !shared_.roots[root].path) { size=NullSize(name); known=size && Depth(path)==2; }
            else if(root>=0 && !IsDirectory(root,native,Depth(path))) {
                WIN32_FIND_DATA entry; HANDLE find=FindFirstFile(native,&entry);
                if(find!=INVALID_HANDLE_VALUE) { size=(U64(entry.nFileSizeHigh)<<32)|entry.nFileSizeLow; known=true; FindClose(find); }
            }
            if(known) { _snprintf_s(text,sizeof(text),_TRUNCATE,"213 %I64u",size); Reply(text); } else Reply("550 Not a file.");
        }
        else if(!strcmp(verb,"DELE") || !strcmp(verb,"RMD") || !strcmp(verb,"XRMD") || !strcmp(verb,"MKD") || !strcmp(verb,"XMKD") || !strcmp(verb,"RNFR") || !strcmp(verb,"RNTO")) {
            int root=Resolve(argument,native,sizeof(native),name,sizeof(name),path,sizeof(path));
            if(root<0 || !shared_.roots[root].path || Depth(path)<2) { Reply("550 Not allowed here."); renameFrom_[0]=0; continue; }
            if(!strcmp(verb,"DELE")) {
                bool done=DeleteFile(native)!=0; Log(done ? "Deleted %s" : "Could not delete %s",path);
                Reply(done ? "250 Deleted." : "550 Cannot delete.");
            }
            else if((verb[0]=='R' && verb[1]=='M') || !strcmp(verb,"XRMD")) {
                bool done=RemoveDirectory(native)!=0; Log(done ? "Removed folder %s" : "Could not remove folder %s",path);
                Reply(done ? "250 Folder removed." : "550 Cannot remove the folder.");
            }
            else if(!strcmp(verb,"MKD") || !strcmp(verb,"XMKD")) {
                if(CreateDirectory(native,NULL)) { Log("Created folder %s",path); _snprintf_s(text,sizeof(text),_TRUNCATE,"257 \"%s\" created.",path); Reply(text); }
                else Reply("550 Cannot create the folder.");
            }
            else if(!strcmp(verb,"RNFR")) { strcpy_s(renameFrom_,native); strcpy_s(renamePath_,path); Reply("350 Ready for RNTO."); }
            else {
                bool done=renameFrom_[0] && MoveFile(renameFrom_,native);
                if(done) Log("Renamed %s to %s",renamePath_,path);
                Reply(done ? "250 Renamed." : "550 Cannot rename."); renameFrom_[0]=0;
            }
        }
        else if(!strcmp(verb,"ABOR")) Reply("226 Abort complete.");
        else Reply("502 Command not implemented.");
        // REST applies to the next transfer; clients may set the type or
        // passive mode in between.
        if(strcmp(verb,"PASV") && strcmp(verb,"EPSV") && strcmp(verb,"TYPE") && strcmp(verb,"NOOP")) restart_=0;
    }
    Log("Disconnected");
}

struct Client { Shared* shared; SOCKET socket; };
DWORD WINAPI SessionThread(void* context) {
    Client client=*static_cast<Client*>(context); free(context);
    {
        Session session(*client.shared,client.socket);
        session.Run();
    }
    shutdown(client.socket,SD_SEND); closesocket(client.socket);
    InterlockedIncrement(&client.shared->finished);
    InterlockedDecrement(&client.shared->active);
    return 0;
}
}

bool ServeFtp(SOCKET listener,const FtpRoot* roots,int rootCount,DWORD address,int socketBuffer,
              int speedLimit,bool downloadsOnly,int processor,int diskBlock,int maxSessions,const char* user,const char* password,
              const FtpObserver& observer,char* error,size_t capacity) {
    Shared shared={roots,rootCount,address,socketBuffer,speedLimit,diskBlock,downloadsOnly,user,password ? password : "",&observer,0,0};
    InitializeCriticalSection(&shared.paceLock); shared.paceNext=0;
    HANDLE threads[64]={0}; int started=0; bool ok=true;
    while(!(observer.cancelled && observer.cancelled(observer.context))) {
        if(maxSessions && shared.finished>=maxSessions) break;
        fd_set set; FD_ZERO(&set); FD_SET(listener,&set); timeval interval={0,100000};
        int rc=select(0,&set,NULL,NULL,&interval);
        if(rc==SOCKET_ERROR) { _snprintf_s(error,capacity,_TRUNCATE,"FTP LISTENER WAIT FAILED (%d)",WSAGetLastError()); ok=false; break; }
        // Reap finished session threads so the handle table stays small.
        for(int i=0;i<started;) {
            if(WaitForSingleObject(threads[i],0)==WAIT_OBJECT_0) { CloseHandle(threads[i]); threads[i]=threads[--started]; }
            else ++i;
        }
        if(!rc) continue;
        SOCKET s=accept(listener,NULL,NULL);
        if(s==INVALID_SOCKET) continue;
        char setup[100];
        if(!PrepareSocket(s,setup,sizeof(setup))) { closesocket(s); continue; }
        if(shared.active>=FtpMaxSessions || started==64) {
            const char* busy="421 Too many connections.\r\n"; send(s,busy,int(strlen(busy)),0); closesocket(s); continue;
        }
        Client* client=static_cast<Client*>(malloc(sizeof(Client)));
        if(!client) { closesocket(s); continue; }
        client->shared=&shared; client->socket=s;
        InterlockedIncrement(&shared.active);
        HANDLE thread=CreateThread(NULL,128*1024,SessionThread,client,CREATE_SUSPENDED,NULL);
        if(!thread) { InterlockedDecrement(&shared.active); free(client); closesocket(s); continue; }
#ifdef _XBOX
        if(processor>=0) XSetThreadProcessor(thread,processor);
#else
        (void)processor;
#endif
        ResumeThread(thread);
        threads[started++]=thread;
    }
    // Sessions poll the same cancel flag; wait so no socket outlives the server.
    for(int i=0;i<started;++i) { WaitForSingleObject(threads[i],INFINITE); CloseHandle(threads[i]); }
    DeleteCriticalSection(&shared.paceLock);
    return ok;
}

bool ValidFtpLogin(const char* text) {
    size_t length=strlen(text);
    if(!length || length>FtpLoginMax) return false;
    for(size_t i=0;i<length;++i) if(text[i]<0x21 || text[i]>0x7E) return false;
    return true;
}
void ParseFtpLogin(const char* text, bool& required, char* user, char* password) {
    while(*text) {
        const char* end=text; while(*end && *end!='\n' && *end!='\r') ++end;
        char line[96]; size_t length=size_t(end-text);
        if(length<sizeof(line)) {
            memcpy(line,text,length); line[length]=0;
            char* value=strchr(line,'=');
            if(value) {
                *value++=0;
                if(!_stricmp(line,"login")) { if(!_stricmp(value,"on")) required=true; else if(!_stricmp(value,"off")) required=false; }
                else if(!_stricmp(line,"user") && ValidFtpLogin(value)) strcpy_s(user,FtpLoginMax+1,value);
                else if(!_stricmp(line,"password") && ValidFtpLogin(value)) strcpy_s(password,FtpLoginMax+1,value);
            }
        }
        text=end; while(*text=='\n' || *text=='\r') ++text;
    }
}
void FormatFtpLogin(char* out, size_t capacity, bool required, const char* user, const char* password) {
    _snprintf_s(out,capacity,_TRUNCATE,"login=%s\r\nuser=%s\r\npassword=%s\r\n",required ? "on" : "off",user,password);
}
bool FindSetting(const char* text, const char* key, char* value, size_t capacity) {
    const size_t keyLength=strlen(key); bool found=false;
    while(*text) {
        const char* end=text; while(*end && *end!='\n' && *end!='\r') ++end;
        if(size_t(end-text)>keyLength && text[keyLength]=='=' && !_strnicmp(text,key,keyLength)) {
            const size_t length=size_t(end-text)-keyLength-1;
            if(length<capacity) { memcpy(value,text+keyLength+1,length); value[length]=0; found=true; }
        }
        text=end; while(*text=='\n' || *text=='\r') ++text;
    }
    return found;
}
}
