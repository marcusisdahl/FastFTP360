// SPDX-License-Identifier: MIT
// Minimal passive-mode FTP server for throughput tests. The optional login is
// sent unencrypted, so it must only be armed on a trusted LAN.
#include "ftp.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace nb {
namespace {
const double ControlIdleSeconds=300, DataWaitSeconds=15, DataIdleSeconds=30;
volatile LONG NextDataPort=0;

// Overlaps disk and network: a helper thread writes behind (uploads) or reads
// ahead (downloads) through two buffers that alternate with the socket side.
struct Pump {
    HANDLE file,thread,filled[2],freed[2];
    unsigned char* buffer[2];
    DWORD length[2],error;
    bool writing;
    volatile LONG stop,failed;
    int next;
};
DWORD WINAPI PumpThread(void* context) {
    Pump& p=*static_cast<Pump*>(context);
    for(int i=0;;i^=1) {
        WaitForSingleObject(p.writing ? p.filled[i] : p.freed[i],INFINITE);
        if(p.stop) break;
        DWORD done=0;
        if(p.writing) {
            if(!p.length[i]) break;
            if(!WriteFile(p.file,p.buffer[i],p.length[i],&done,NULL) || done!=p.length[i]) {
                p.error=GetLastError(); InterlockedExchange(&p.failed,1); SetEvent(p.freed[i]); break;
            }
            SetEvent(p.freed[i]);
        } else {
            if(!ReadFile(p.file,p.buffer[i],FtpBuffer,&done,NULL)) { p.error=GetLastError(); InterlockedExchange(&p.failed,1); done=0; }
            p.length[i]=done; SetEvent(p.filled[i]);
            if(!done) break;
        }
    }
    return 0;
}
bool StartPump(Pump& p,HANDLE file,bool writing) {
    memset(&p,0,sizeof(p)); p.file=file; p.writing=writing;
    for(int i=0;i<2;++i) {
        p.buffer[i]=static_cast<unsigned char*>(malloc(FtpBuffer));
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
        free(p.buffer[i]);
    }
    memset(&p,0,sizeof(p));
}

struct Shared {
    const FtpRoot* roots; int rootCount; DWORD address; int socketBuffer;
    const char* user; const char* password;
    const FtpObserver* observer;
    volatile LONG active,finished;
};

class Session {
    Session(const Session&);
    Session& operator=(const Session&);
    Shared& shared_;
    SOCKET control_,passive_;
    char cwd_[512],input_[1024],renameFrom_[512];
    char user_[64],peer_[24],renamePath_[600];
    int inputCount_;
    bool loggedIn_;
public:
    Session(Shared& shared,SOCKET control) : shared_(shared),control_(control),passive_(INVALID_SOCKET),inputCount_(0),loggedIn_(!shared.user) {
        strcpy_s(cwd_,"/"); renameFrom_[0]=0; renamePath_[0]=0; user_[0]=0; strcpy_s(peer_,"?");
        sockaddr_in address; int length=sizeof(address);
        if(!getpeername(control,reinterpret_cast<sockaddr*>(&address),&length)) {
            const BYTE* ip=reinterpret_cast<const BYTE*>(&address.sin_addr);
            _snprintf_s(peer_,sizeof(peer_),_TRUNCATE,"%u.%u.%u.%u",ip[0],ip[1],ip[2],ip[3]);
        }
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
    bool Wait(SOCKET s,bool writing,double timeout) {
        double end=Now()+timeout;
        for(;;) {
            if(Cancelled() || Now()>end) return false;
            fd_set set; FD_ZERO(&set); FD_SET(s,&set);
            timeval interval={0,100000};
            int rc=select(0,writing?NULL:&set,writing?&set:NULL,NULL,&interval);
            if(rc>0) return true;
            if(rc==SOCKET_ERROR) return false;
        }
    }
    // Sends in WriteSize pieces with a yield, as the benchmark does on Xbox.
    bool SendAll(SOCKET s,const char* data,int count) {
        while(count) {
            if(!Wait(s,true,DataIdleSeconds)) return false;
            int sent=send(s,data,count>WriteSize ? WriteSize : count,0);
            if(sent>0) { data+=sent; count-=sent; Sleep(0); }
            else if(sent==0 || WSAGetLastError()!=WSAEWOULDBLOCK) return false;
            else Sleep(1);
        }
        return true;
    }
    int Receive(SOCKET s,char* data,int count,double timeout) {
        for(;;) {
            if(!Wait(s,false,timeout)) return -1;
            int got=recv(s,data,count,0);
            if(got>=0) return got;
            if(WSAGetLastError()!=WSAEWOULDBLOCK) return -1;
            Sleep(1);
        }
    }
    bool Reply(const char* text) {
        char line[600]; int length=_snprintf_s(line,sizeof(line),_TRUNCATE,"%s\r\n",text);
        return length>0 && SendAll(control_,line,length);
    }
    bool ReadCommand(char* line,size_t capacity);
    void ClosePassive() { if(passive_!=INVALID_SOCKET) { closesocket(passive_); passive_=INVALID_SOCKET; } }
    bool OpenPassive(int& port);
    SOCKET AcceptData();
    void CloseData(SOCKET data) { shutdown(data,SD_SEND); closesocket(data); }
    int Resolve(const char* argument,char* native,size_t nativeCapacity,char* name,size_t nameCapacity,char* path,size_t pathCapacity);
    bool IsDirectory(int root,const char* native,int depth);
    void List(const char* argument,bool namesOnly);
    void Retrieve(const char* argument);
    void Store(const char* argument);
    bool TransferDone(const char* operation,const char* name,const char* path,U64 bytes,double begin,bool ok,const char* failure);
};

bool Session::ReadCommand(char* line,size_t capacity) {
    for(;;) {
        for(int i=0;i<inputCount_;++i) {
            if(input_[i]!='\n') continue;
            int length=i;
            while(length>0 && (input_[length-1]=='\r' || input_[length-1]==' ')) --length;
            if(size_t(length)>=capacity) length=int(capacity-1);
            memcpy(line,input_,length); line[length]=0;
            memmove(input_,input_+i+1,inputCount_-i-1); inputCount_-=i+1;
            return true;
        }
        if(inputCount_==sizeof(input_)) inputCount_=0; // Over-long line: discard it.
        int got=Receive(control_,input_+inputCount_,int(sizeof(input_))-inputCount_,ControlIdleSeconds);
        if(got<=0) return false;
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
    if(Wait(passive_,false,DataWaitSeconds)) data=accept(passive_,NULL,NULL);
    ClosePassive();
    char error[100];
    if(data!=INVALID_SOCKET && !PrepareSocket(data,error,sizeof(error))) { closesocket(data); data=INVALID_SOCKET; }
    return data;
}

// Maps a client path to a root index (-1 for the virtual top level), the native
// path, the final name and the normalized FTP path. Returns -2 if invalid.
int Session::Resolve(const char* argument,char* native,size_t nativeCapacity,char* name,size_t nameCapacity,char* path,size_t pathCapacity) {
    char combined[1100];
    if(argument[0]=='/') strcpy_s(combined,argument);
    else _snprintf_s(combined,sizeof(combined),_TRUNCATE,"%s/%s",cwd_,argument);
    const char* parts[64]; int count=0;
    for(char* token=combined;;) {
        char* slash=strchr(token,'/');
        if(slash) *slash=0;
        if(!strcmp(token,"..")) { if(count) --count; }
        else if(token[0] && strcmp(token,".")) {
            for(const char* c=token;*c;++c) if(*c=='\\' || *c==':' || *c=='*' || *c=='?' || static_cast<unsigned char>(*c)<32) return -2;
            if(count==64) return -2;
            parts[count++]=token;
        }
        if(!slash) break;
        token=slash+1;
    }
    path[0]=0; name[0]=0; native[0]=0;
    for(int i=0;i<count;++i) { strcat_s(path,pathCapacity,"/"); strcat_s(path,pathCapacity,parts[i]); }
    if(!count) { strcpy_s(path,pathCapacity,"/"); return -1; }
    int root=-1;
    for(int i=0;i<shared_.rootCount;++i) if(!_stricmp(parts[0],shared_.roots[i].name)) root=i;
    if(root<0) return -2;
    strcpy_s(name,nameCapacity,parts[count-1]);
    if(shared_.roots[root].path) _snprintf_s(native,nativeCapacity,_TRUNCATE,"%s\\",shared_.roots[root].path);
    for(int i=1;i<count;++i) { if(i>1) strcat_s(native,nativeCapacity,"\\"); strcat_s(native,nativeCapacity,parts[i]); }
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

static void FormatEntry(char* line,size_t capacity,const char* name,bool directory,U64 size,const FILETIME* time,bool namesOnly) {
    if(namesOnly) { _snprintf_s(line,capacity,_TRUNCATE,"%s\r\n",name); return; }
    static const char* months[]={"Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec"};
    // Same shape as Aurora's listing: everything rwx for root, and 1 Jan 2000
    // where there is no time (the drive folders).
    SYSTEMTIME st={0}; st.wYear=2000; st.wMonth=1; st.wDay=1;
    if(time) FileTimeToSystemTime(time,&st);
    if(st.wMonth<1 || st.wMonth>12) st.wMonth=1;
    _snprintf_s(line,capacity,_TRUNCATE,"%s 1 root root %I64u %s %2u %04u %s\r\n",
        directory ? "drwxrwxrwx" : "-rwxrwxrwx",size,months[st.wMonth-1],st.wDay,st.wYear,name);
}

void Session::List(const char* argument,bool namesOnly) {
    while(argument[0]=='-') { const char* space=strchr(argument,' '); argument=space ? space+1 : ""; }
    char native[600],name[300],path[600];
    int root=Resolve(argument,native,sizeof(native),name,sizeof(name),path,sizeof(path));
    if(root==-2) { Reply("550 No such file or folder."); return; }
    int depth=Depth(path);
    if(passive_==INVALID_SOCKET) { Reply("425 Use PASV or EPSV first."); return; }
    // Build the whole listing before opening the data connection.
    size_t capacity=65536,used=0; char* text=static_cast<char*>(malloc(capacity));
    if(!text) { Reply("451 Out of memory."); return; }
    char line[700]; bool found=true;
    if(root==-1) {
        for(int i=0;i<shared_.rootCount;++i) {
            FormatEntry(line,sizeof(line),shared_.roots[i].name,true,0,NULL,namesOnly);
            size_t n=strlen(line); if(used+n<capacity) { memcpy(text+used,line,n); used+=n; }
        }
    } else if(!shared_.roots[root].path) {
        static const char* samples[]={"64MB.bin","256MB.bin","1024MB.bin"};
        for(int i=0;i<3;++i) {
            if(depth==2 && _stricmp(name,samples[i])) continue;
            FormatEntry(line,sizeof(line),samples[i],false,NullSize(samples[i]),NULL,namesOnly);
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
                    (U64(entry.nFileSizeHigh)<<32)|entry.nFileSizeLow,&entry.ftLastWriteTime,namesOnly);
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
    HANDLE file=INVALID_HANDLE_VALUE;
    if(null && (!size || Depth(path)!=2)) { Reply("550 Use a name like 256MB.bin in this folder."); return; }
    if(!null) {
        file=CreateFile(native,GENERIC_READ,FILE_SHARE_READ,NULL,OPEN_EXISTING,FILE_FLAG_SEQUENTIAL_SCAN,NULL);
        if(file==INVALID_HANDLE_VALUE) { Reply("550 Cannot open the file."); return; }
    }
    Reply("150 Opening binary data connection.");
    SOCKET data=AcceptData();
    if(data==INVALID_SOCKET) { if(file!=INVALID_HANDLE_VALUE) CloseHandle(file); Reply("425 Data connection was not opened."); return; }
    double begin=Now(),update=begin; U64 bytes=0; bool ok=true; const char* failure="426 Transfer interrupted.";
    if(null) {
        unsigned char* block=static_cast<unsigned char*>(malloc(Chunk));
        if(!block) ok=false;
        else {
            Pattern(block,0x46545021);
            while(ok && bytes<size) {
                int count=int(size-bytes<U64(Chunk) ? size-bytes : Chunk);
                ok=SendAll(data,reinterpret_cast<const char*>(block),count);
                if(ok) bytes+=count;
                if(Now()-update>=0.25) { update=Now(); Progress("RETR",name,bytes,update-begin,false,true); }
            }
            free(block);
        }
    } else {
        Pump pump;
        if(!StartPump(pump,file,false)) { ok=false; failure="451 Out of memory."; }
        while(ok) {
            int i=pump.next;
            while(WaitForSingleObject(pump.filled[i],100)==WAIT_TIMEOUT) if(Cancelled()) { ok=false; break; }
            if(!ok) break;
            if(pump.failed) { ok=false; failure="451 Read error on the Xbox drive."; break; }
            if(!pump.length[i]) break;
            ok=SendAll(data,reinterpret_cast<const char*>(pump.buffer[i]),int(pump.length[i]));
            if(ok) bytes+=pump.length[i];
            SetEvent(pump.freed[i]); pump.next^=1;
            if(Now()-update>=0.25) { update=Now(); Progress("RETR",name,bytes,update-begin,false,true); }
        }
        EndPump(pump); CloseHandle(file);
    }
    CloseData(data);
    TransferDone("RETR",name,path,bytes,begin,ok,failure);
}

void Session::Store(const char* argument) {
    char native[600],name[300],path[600];
    int root=Resolve(argument,native,sizeof(native),name,sizeof(name),path,sizeof(path));
    if(root<0 || Depth(path)<2) { Reply("553 Choose a file inside a drive folder."); return; }
    if(passive_==INVALID_SOCKET) { Reply("425 Use PASV or EPSV first."); return; }
    bool null=!shared_.roots[root].path;
    if(null && Depth(path)!=2) { Reply("553 The null folder has no subfolders."); return; }
    HANDLE file=INVALID_HANDLE_VALUE;
    if(!null) {
        file=CreateFile(native,GENERIC_WRITE,0,NULL,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,NULL);
        if(file==INVALID_HANDLE_VALUE) { Reply("553 Cannot create the file."); return; }
    }
    Reply("150 Ready to receive.");
    SOCKET data=AcceptData();
    if(data==INVALID_SOCKET) {
        if(file!=INVALID_HANDLE_VALUE) { CloseHandle(file); DeleteFile(native); }
        Reply("425 Data connection was not opened."); return;
    }
    double begin=Now(),update=begin; U64 bytes=0; bool ok=true; const char* failure="426 Transfer interrupted.";
    if(null) {
        char* sink=static_cast<char*>(malloc(Chunk*4));
        if(!sink) ok=false;
        for(;ok;) {
            int got=Receive(data,sink,Chunk*4,DataIdleSeconds);
            if(got<0) ok=false; else if(!got) break; else bytes+=got;
            if(Now()-update>=0.25) { update=Now(); Progress("STOR",name,bytes,update-begin,false,true); }
        }
        free(sink);
    } else {
        Pump pump; bool eof=false;
        if(!StartPump(pump,file,true)) { ok=false; failure="451 Out of memory."; }
        while(ok && !eof) {
            int i=pump.next;
            while(WaitForSingleObject(pump.freed[i],100)==WAIT_TIMEOUT) if(Cancelled() || pump.failed) { ok=false; break; }
            if(!ok || pump.failed) { ok=false; break; }
            DWORD filled=0;
            while(filled<FtpBuffer) {
                int got=Receive(data,reinterpret_cast<char*>(pump.buffer[i])+filled,int(FtpBuffer-filled),DataIdleSeconds);
                if(got<0) { ok=false; break; }
                if(!got) { eof=true; break; }
                filled+=got; bytes+=got;
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
        if(pump.failed) { ok=false; failure="452 Write error on the Xbox drive. Is it full?"; }
        EndPump(pump);
        if(ok && !FlushFileBuffers(file)) { ok=false; failure="452 Write error on the Xbox drive. Is it full?"; }
        CloseHandle(file);
        if(!ok) DeleteFile(native);
    }
    CloseData(data);
    TransferDone("STOR",name,path,bytes,begin,ok,failure);
}

void Session::Run() {
    Log("Connected");
    Reply(shared_.user ? "220 FastFTP ready." : "220 FastFTP ready. Any user name and password.");
    char line[600];
    while(!Cancelled() && ReadCommand(line,sizeof(line))) {
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
        else if(!strcmp(verb,"FEAT")) Reply("211-Features:\r\n SIZE\r\n PASV\r\n EPSV\r\n211 End");
        else if(!strcmp(verb,"OPTS") || !strcmp(verb,"CLNT") || !strcmp(verb,"MODE") || !strcmp(verb,"STRU")) Reply("200 OK.");
        else if(!strcmp(verb,"TYPE")) Reply("200 Binary transfers are used for every type.");
        else if(!strcmp(verb,"NOOP")) Reply("200 OK.");
        else if(!strcmp(verb,"ALLO")) Reply("202 No storage allocation needed.");
        else if(!strcmp(verb,"PWD") || !strcmp(verb,"XPWD")) { _snprintf_s(text,sizeof(text),_TRUNCATE,"257 \"%s\" is the current folder.",cwd_); Reply(text); }
        else if(!strcmp(verb,"CWD") || !strcmp(verb,"XCWD") || !strcmp(verb,"CDUP") || !strcmp(verb,"XCUP")) {
            const char* target=(verb[0]=='C' && verb[1]=='D') || !strcmp(verb,"XCUP") ? ".." : argument;
            int root=Resolve(target,native,sizeof(native),name,sizeof(name),path,sizeof(path));
            if(root>-2 && IsDirectory(root,native,Depth(path))) { strcpy_s(cwd_,path); Reply("250 Folder changed."); }
            else Reply("550 No such folder.");
        }
        else if(!strcmp(verb,"PASV") || !strcmp(verb,"EPSV")) {
            int port=0;
            if(!OpenPassive(port)) { Reply("425 Cannot open a passive port."); continue; }
            const BYTE* ip=reinterpret_cast<const BYTE*>(&shared_.address);
            if(verb[0]=='P') _snprintf_s(text,sizeof(text),_TRUNCATE,"227 Entering Passive Mode (%u,%u,%u,%u,%d,%d).",ip[0],ip[1],ip[2],ip[3],port>>8,port&255);
            else _snprintf_s(text,sizeof(text),_TRUNCATE,"229 Entering Extended Passive Mode (|||%d|)",port);
            Reply(text);
        }
        else if(!strcmp(verb,"PORT") || !strcmp(verb,"EPRT")) Reply("502 Active mode is not supported. Use passive mode.");
        else if(!strcmp(verb,"LIST") || !strcmp(verb,"NLST")) List(argument,verb[0]=='N');
        else if(!strcmp(verb,"RETR")) Retrieve(argument);
        else if(!strcmp(verb,"STOR")) Store(argument);
        else if(!strcmp(verb,"REST")) Reply(strcmp(argument,"0") ? "502 Resuming is not supported." : "350 Starting at 0.");
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
        else if(!strcmp(verb,"ABOR")) Reply("226 No transfer to abort.");
        else Reply("502 Command not implemented.");
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
              int maxSessions,const char* user,const char* password,const FtpObserver& observer,char* error,size_t capacity) {
    Shared shared={roots,rootCount,address,socketBuffer,user,password ? password : "",&observer,0,0};
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
        HANDLE thread=CreateThread(NULL,128*1024,SessionThread,client,0,NULL);
        if(!thread) { InterlockedDecrement(&shared.active); free(client); closesocket(s); continue; }
        threads[started++]=thread;
    }
    // Sessions poll the same cancel flag; wait so no socket outlives the server.
    for(int i=0;i<started;++i) { WaitForSingleObject(threads[i],INFINITE); CloseHandle(threads[i]); }
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
}
