// SPDX-License-Identifier: MIT
#include <xtl.h>
#include <d3dx9.h>
#include <stdio.h>
#include "../shared/bench.h"
#include "../shared/ui.h"
#include "../shared/ftp.h"

static CRITICAL_SECTION StateLock;
static nb::UiState State;
static volatile LONG Cancel=0;
static HANDLE Worker=NULL;
static HANDLE GraphicsIdle=NULL;
static bool NetworkReady=false;
static IDirect3DDevice9* Device=NULL;
static IDirect3DVertexShader9* VertexShader=NULL;
static IDirect3DPixelShader9* PixelShader=NULL;
static IDirect3DVertexDeclaration9* Declaration=NULL;
struct Vertex { float x,y; DWORD color; };
static Vertex Vertices[65532];
static unsigned int VertexCount=0;

// Xbox socket buffer choices for the listener; accepted sockets inherit them.
// Zero keeps the 64 KiB XNet startup default.
static const int BufferChoices[]={0,131072,262144};
static int BufferChoice=0;
// Hardware thread for stream workers: 0 lets the kernel choose, otherwise 1-6
// selects hardware thread 0-5. USB completion DPCs run on hardware thread 2.
static int CpuChoice=0;

// Disk test targets. The app's own drive needs no mount; the others are linked
// to their kernel device for the test and unlinked afterwards.
struct DiskTarget { const char* name; const char* device; };
static const DiskTarget DiskTargets[]={{"APP DRIVE",NULL},{"HDD","\\Device\\Harddisk0\\Partition1"},
    {"USB 0","\\Device\\Mass0"},{"USB 1","\\Device\\Mass1"}};
static int DiskChoice=0;
static const DWORD DiskBlocks[]={65536,262144,1048576};
static int DiskBlockChoice=2;
enum { DiskTestBytes=256*1048576 };
struct KernelString { USHORT length,capacity; char* text; };
extern "C" LONG __stdcall ObCreateSymbolicLink(KernelString* link,KernelString* device);
extern "C" LONG __stdcall ObDeleteSymbolicLink(KernelString* link);

// Links a DOS-style name such as \??\NBHDD: to a kernel device path.
static LONG LinkDrive(const char* name,const char* device) {
    char nameText[32],deviceText[64]; strcpy_s(nameText,name); strcpy_s(deviceText,device);
    KernelString link={USHORT(strlen(nameText)),USHORT(sizeof(nameText)),nameText};
    KernelString target={USHORT(strlen(deviceText)),USHORT(sizeof(deviceText)),deviceText};
    ObDeleteSymbolicLink(&link); // Left over from an interrupted run.
    return ObCreateSymbolicLink(&link,&target);
}
static void UnlinkDrive(const char* name) {
    char nameText[32]; strcpy_s(nameText,name);
    KernelString link={USHORT(strlen(nameText)),USHORT(sizeof(nameText)),nameText};
    ObDeleteSymbolicLink(&link);
}
// FTP folders backed by drives. A drive is listed only if its root can be read.
struct FtpDrive { const char* name; const char* link; const char* path; const char* device; };
static const FtpDrive FtpDrives[]={{"HDD","\\??\\NBHDD:","NBHDD:","\\Device\\Harddisk0\\Partition1"},
    {"USB0","\\??\\NBUSB0:","NBUSB0:","\\Device\\Mass0"},{"USB1","\\??\\NBUSB1:","NBUSB1:","\\Device\\Mass1"}};

struct Stream {
    HANDLE thread;
    SOCKET socket;
    nb::Result result;
    char error[100];
    nb::Phase phase;
    nb::U64 bytes;
    double elapsed,rate;
};
static Stream Streams[nb::MaxStreams];
static int ActiveStreams=0;
static double BatchRate[3];
static int BatchCount=0;
// Each stream paces itself, so a capped batch sends at most this many MiB/s in total.
static DWORD BatchLimit=0;
static bool BatchFailed=false;

static void DescribeSettings() {
    char buffers[24],cpu[16];
    if(BufferChoices[BufferChoice]) _snprintf_s(buffers,sizeof(buffers),_TRUNCATE,"%d KIB",BufferChoices[BufferChoice]/1024);
    else strcpy_s(buffers,"STARTUP 64 KIB");
    if(CpuChoice) _snprintf_s(cpu,sizeof(cpu),_TRUNCATE,"THREAD %d",CpuChoice-1); else strcpy_s(cpu,"AUTO");
    _snprintf_s(State.settings,sizeof(State.settings),_TRUNCATE,"XBOX BUFFERS: %s / WORKER CPU: %s / DISK: %s %u KIB",
        buffers,cpu,DiskTargets[DiskChoice].name,DiskBlocks[DiskBlockChoice]/1024);
}
static bool IsCancelled(void*) { return InterlockedCompareExchange(&Cancel,0,0)!=0; }
// Called with StateLock held: combine every running stream into the screen state.
static void Summarize() {
    nb::U64 bytes=0; double rate=0,elapsed=0;
    for(int i=0;i<nb::MaxStreams;++i) {
        if(!Streams[i].thread) continue;
        bytes+=Streams[i].bytes;
        if(Streams[i].phase==nb::Measuring) rate+=Streams[i].rate;
        if(Streams[i].elapsed>elapsed) elapsed=Streams[i].elapsed;
    }
    State.mebibytes=double(bytes)/1048576; State.rate=rate; State.elapsed=elapsed; State.streams=ActiveStreams;
}
static void Progress(void* context,nb::Phase phase,DWORD direction,nb::U64 bytes,double elapsed,double rate) {
    EnterCriticalSection(&StateLock);
    Stream* stream=static_cast<Stream*>(context);
    State.phase=phase; State.direction=direction; State.message[0]=0;
    if(stream) {
        stream->phase=phase; stream->bytes=bytes; stream->elapsed=elapsed; stream->rate=rate;
        Summarize();
    } else { State.mebibytes=double(bytes)/1048576; State.elapsed=elapsed; State.rate=rate; }
    LeaveCriticalSection(&StateLock);
}
static bool Configure(void*,DWORD seconds,DWORD limit,DWORD options) {
    if(options&nb::PauseDrawing) {
        if(!GraphicsIdle || !ResetEvent(GraphicsIdle)) return false;
    }
    EnterCriticalSection(&StateLock);
    State.duration=seconds; State.rateLimit=limit; State.options=options;
    LeaveCriticalSection(&StateLock);
    if(!(options&nb::PauseDrawing)) return true;
    double deadline=nb::Now()+5;
    while(!IsCancelled(NULL) && nb::Now()<deadline) {
        if(WaitForSingleObject(GraphicsIdle,100)==WAIT_OBJECT_0) return true;
    }
    return false;
}
static void Failure(const char* message) {
    EnterCriticalSection(&StateLock); State.phase=nb::Failed;
    _snprintf_s(State.message,sizeof(State.message),_TRUNCATE,"%s",message); LeaveCriticalSection(&StateLock);
}
// One thread per accepted connection. The PC may open up to MaxStreams
// connections at once; their receiver rates add up to the batch result.
static DWORD WINAPI StreamThread(void* context) {
    Stream* stream=static_cast<Stream*>(context);
    nb::Observer observer={stream,IsCancelled,Progress,Configure};
    bool ok=nb::Serve(stream->socket,observer,stream->result,stream->error,sizeof(stream->error));
    closesocket(stream->socket); stream->socket=INVALID_SOCKET;
    EnterCriticalSection(&StateLock);
    if(ok) {
        BatchRate[stream->result.direction]+=stream->result.Rate(); ++BatchCount;
        if(stream->result.direction==2) BatchLimit+=stream->result.rateLimit;
    }
    else if(!IsCancelled(NULL)) { BatchFailed=true; State.phase=nb::Failed; _snprintf_s(State.message,sizeof(State.message),_TRUNCATE,"%s",stream->error); }
    stream->phase=nb::Idle; stream->bytes=0; stream->rate=0;
    if(--ActiveStreams==0) {
        State.options=0;
        // A batch with any failed stream reports no rate.
        if(BatchCount && !BatchFailed) {
            if(BatchRate[1]>0) State.upload=BatchRate[1];
            if(BatchRate[2]>0) { State.download=BatchRate[2]; State.downloadLimit=BatchLimit; }
            ++State.completed;
            if(BatchCount>1) _snprintf_s(State.message,sizeof(State.message),_TRUNCATE,"%d STREAMS PASSED THEIR DATA CHECKS. READY FOR THE NEXT PC TEST.",BatchCount);
            else strcpy_s(State.message,stream->result.verified ? "ALL DATA CHECKED. READY FOR THE NEXT PC TEST." : "DATA CHECK PASSED. READY FOR THE NEXT PC TEST.");
        }
        BatchRate[1]=BatchRate[2]=0; BatchCount=0; BatchLimit=0; BatchFailed=false;
    }
    State.streams=ActiveStreams;
    LeaveCriticalSection(&StateLock);
    return 0;
}
static bool StartStream(SOCKET client) {
    Stream* slot=NULL;
    for(int i=0;i<nb::MaxStreams && !slot;++i) {
        if(Streams[i].thread && WaitForSingleObject(Streams[i].thread,0)==WAIT_OBJECT_0) { CloseHandle(Streams[i].thread); Streams[i].thread=NULL; }
        if(!Streams[i].thread) slot=&Streams[i];
    }
    if(!slot) return false;
    slot->socket=client; slot->result=nb::Result(); slot->error[0]=0;
    slot->phase=nb::Connecting; slot->bytes=0; slot->elapsed=0; slot->rate=0;
    EnterCriticalSection(&StateLock); ++ActiveStreams; State.streams=ActiveStreams; LeaveCriticalSection(&StateLock);
    slot->thread=CreateThread(NULL,64*1024,StreamThread,slot,CREATE_SUSPENDED,NULL);
    if(!slot->thread) {
        EnterCriticalSection(&StateLock); --ActiveStreams; State.streams=ActiveStreams; LeaveCriticalSection(&StateLock);
        return false;
    }
    if(CpuChoice) XSetThreadProcessor(slot->thread,CpuChoice-1);
    ResumeThread(slot->thread);
    return true;
}
static void DiskProgress(int phase,nb::U64 bytes,double elapsed) {
    EnterCriticalSection(&StateLock);
    State.diskPhase=phase; State.mebibytes=double(bytes)/1048576; State.elapsed=elapsed;
    State.rate=elapsed>0 ? State.mebibytes/elapsed : 0; State.diskTotal=DiskTestBytes/1048576;
    LeaveCriticalSection(&StateLock);
}
static void DiskFailure(const char* format,const char* drive,DWORD code) {
    char message[100]; _snprintf_s(message,sizeof(message),_TRUNCATE,format,drive,code);
    Failure(message);
    EnterCriticalSection(&StateLock); State.diskPhase=0; LeaveCriticalSection(&StateLock);
}
// Writes a 256 MiB pattern file in the selected block size and flushes it, then
// reads it back without the file cache and compares every byte. No network.
static DWORD WINAPI DiskThread(void*) {
    const DiskTarget& target=DiskTargets[DiskChoice];
    const DWORD block=DiskBlocks[DiskBlockChoice];
    bool linked=false;
    const char* path="game:\\NETBENCH360-DISKTEST.TMP";
    if(target.device) {
        LONG status=LinkDrive("\\??\\NBDISK:",target.device);
        if(status<0) { DiskFailure("%s NOT FOUND OR NOT MOUNTABLE (0X%08X)",target.name,DWORD(status)); return 0; }
        linked=true; path="NBDISK:\\NETBENCH360-DISKTEST.TMP";
    }
    unsigned char* buffer=static_cast<unsigned char*>(VirtualAlloc(NULL,block,MEM_COMMIT,PAGE_READWRITE));
    unsigned char* check=static_cast<unsigned char*>(VirtualAlloc(NULL,block,MEM_COMMIT,PAGE_READWRITE));
    double writeRate=0,readRate=0; bool ok=false;
    if(!buffer || !check) DiskFailure("COULD NOT ALLOCATE DISK TEST BUFFERS%s (%u)","",GetLastError());
    else {
        nb::Pattern(buffer,0x360D15C);
        for(DWORD at=nb::Chunk;at<block;at+=nb::Chunk) memcpy(buffer+at,buffer,nb::Chunk);
        HANDLE file=CreateFile(path,GENERIC_WRITE,0,NULL,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,NULL);
        if(file==INVALID_HANDLE_VALUE) DiskFailure("CANNOT CREATE TEST FILE ON %s (%u). FREE 256 MIB?",target.name,GetLastError());
        else {
            // Write phase: normal cached writes, like an FTP server; the final
            // flush is timed so data still in the cache is included.
            double begin=nb::Now(),update=begin; nb::U64 done=0; ok=true;
            while(done<DiskTestBytes) {
                if(IsCancelled(NULL)) { ok=false; break; }
                DWORD written=0;
                if(!WriteFile(file,buffer,block,&written,NULL) || written!=block) {
                    DiskFailure("WRITE FAILED ON %s (%u). DRIVE FULL?",target.name,GetLastError()); ok=false; break;
                }
                done+=block;
                if(nb::Now()-update>=0.25) { update=nb::Now(); DiskProgress(1,done,update-begin); }
            }
            if(ok && !FlushFileBuffers(file)) { DiskFailure("FLUSH FAILED ON %s (%u)",target.name,GetLastError()); ok=false; }
            CloseHandle(file);
            if(ok) { writeRate=double(DiskTestBytes)/1048576/(nb::Now()-begin); DiskProgress(1,done,nb::Now()-begin); }
        }
        if(ok) {
            // Read phase bypasses the file cache so the drive itself is measured.
            HANDLE file=CreateFile(path,GENERIC_READ,FILE_SHARE_READ,NULL,OPEN_EXISTING,FILE_FLAG_NO_BUFFERING|FILE_FLAG_SEQUENTIAL_SCAN,NULL);
            if(file==INVALID_HANDLE_VALUE) { DiskFailure("CANNOT REOPEN TEST FILE ON %s (%u)",target.name,GetLastError()); ok=false; }
            else {
                double begin=nb::Now(),update=begin; nb::U64 done=0;
                DiskProgress(2,0,0);
                while(done<DiskTestBytes) {
                    if(IsCancelled(NULL)) { ok=false; break; }
                    DWORD read=0;
                    if(!ReadFile(file,check,block,&read,NULL) || read!=block) {
                        DiskFailure("READ FAILED ON %s (%u)",target.name,GetLastError()); ok=false; break;
                    }
                    if(memcmp(check,buffer,block)) {
                        DiskFailure("READ-BACK DATA MISMATCH ON %s AT MIB %u. STOP USING THIS DRIVE.",target.name,DWORD(done/1048576));
                        ok=false; break;
                    }
                    done+=block;
                    if(nb::Now()-update>=0.25) { update=nb::Now(); DiskProgress(2,done,update-begin); }
                }
                CloseHandle(file);
                if(ok) readRate=double(DiskTestBytes)/1048576/(nb::Now()-begin);
            }
        }
        DeleteFile(path);
    }
    if(buffer) VirtualFree(buffer,0,MEM_RELEASE);
    if(check) VirtualFree(check,0,MEM_RELEASE);
    if(linked) UnlinkDrive("\\??\\NBDISK:");
    EnterCriticalSection(&StateLock);
    State.diskPhase=0;
    if(ok) {
        State.phase=nb::Idle;
        _snprintf_s(State.disk,sizeof(State.disk),_TRUNCATE,"DISK %s %u KIB: WRITE %.2f / READ %.2f MIB/S / ALL DATA CHECKED",
            target.name,block/1024,writeRate,readRate);
        strcpy_s(State.message,"DISK TEST PASSED. TEST FILE DELETED.");
    } else if(IsCancelled(NULL)) { State.phase=nb::Stopped; strcpy_s(State.message,"DISK TEST STOPPED. TEST FILE DELETED."); }
    LeaveCriticalSection(&StateLock);
    return 0;
}
static void FtpProgress(void*,const char* operation,const char*,nb::U64 bytes,double seconds,bool done,bool ok) {
    EnterCriticalSection(&StateLock);
    double mib=double(bytes)/1048576,rate=seconds>0 ? mib/seconds : 0;
    const char* kind=operation[0]=='S' ? "UPLOAD" : "DOWNLOAD";
    if(done) {
        State.ftpPhase=1;
        if(ok) _snprintf_s(State.ftp,sizeof(State.ftp),_TRUNCATE,"LAST %s %.1f MIB IN %.1f S / %.2f MIB/S",kind,mib,seconds,rate);
        else _snprintf_s(State.ftp,sizeof(State.ftp),_TRUNCATE,"LAST %s FAILED AFTER %.1f MIB",kind,mib);
    } else {
        State.ftpPhase=operation[0]=='S' ? 2 : 3; State.mebibytes=mib; State.elapsed=seconds; State.rate=rate;
    }
    LeaveCriticalSection(&StateLock);
}
// Passive-mode FTP server on port 21 until B. Folders: mounted drives that can
// be read, the app's own drive, and NULL (network only, no disk).
static DWORD WINAPI FtpThread(void*) {
    nb::FtpRoot roots[5]; int count=0; bool linked[3]={false,false,false};
    char folders[100]="FOLDERS:";
    for(int i=0;i<3;++i) {
        if(LinkDrive(FtpDrives[i].link,FtpDrives[i].device)<0) continue;
        char pattern[16]; _snprintf_s(pattern,sizeof(pattern),_TRUNCATE,"%s\\*",FtpDrives[i].path);
        WIN32_FIND_DATA entry; HANDLE find=FindFirstFile(pattern,&entry);
        bool readable=find!=INVALID_HANDLE_VALUE || GetLastError()==ERROR_FILE_NOT_FOUND;
        if(find!=INVALID_HANDLE_VALUE) FindClose(find);
        if(!readable) { UnlinkDrive(FtpDrives[i].link); continue; }
        linked[i]=true; roots[count].name=FtpDrives[i].name; roots[count].path=FtpDrives[i].path; ++count;
        strcat_s(folders," "); strcat_s(folders,FtpDrives[i].name);
    }
    roots[count].name="APP"; roots[count].path="game:"; ++count;
    roots[count].name="NULL"; roots[count].path=NULL; ++count;
    strcat_s(folders," APP NULL / PASSIVE MODE / B STOPS THE SERVER");
    char error[100]={0};
    SOCKET listener=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);
    bool ready=listener!=INVALID_SOCKET;
    if(!ready) _snprintf_s(error,sizeof(error),_TRUNCATE,"FTP SOCKET CREATE FAILED (%d)",WSAGetLastError());
    if(ready && BufferChoices[BufferChoice]) ready=nb::SetSocketBuffers(listener,BufferChoices[BufferChoice],error,sizeof(error));
    if(ready) ready=nb::PrepareListener(listener,error,sizeof(error));
    sockaddr_in address={0}; address.sin_family=AF_INET; address.sin_addr.s_addr=INADDR_ANY; address.sin_port=htons(nb::FtpPort);
    if(ready && bind(listener,reinterpret_cast<sockaddr*>(&address),sizeof(address))) {
        _snprintf_s(error,sizeof(error),_TRUNCATE,"BIND FTP PORT 21 FAILED (%d)",WSAGetLastError()); ready=false;
    }
    if(ready && listen(listener,8)) { _snprintf_s(error,sizeof(error),_TRUNCATE,"LISTEN FTP PORT 21 FAILED (%d)",WSAGetLastError()); ready=false; }
    if(ready) {
        XNADDR xnaddr={0}; XNetGetTitleXnAddr(&xnaddr);
        EnterCriticalSection(&StateLock);
        State.ftpPhase=1; State.phase=nb::Idle; State.ftp[0]=0; strcpy_s(State.message,folders);
        LeaveCriticalSection(&StateLock);
        nb::FtpObserver observer={NULL,IsCancelled,FtpProgress};
        if(!nb::ServeFtp(listener,roots,count,xnaddr.ina.s_addr,BufferChoices[BufferChoice],0,observer,error,sizeof(error))) ready=false;
    }
    if(listener!=INVALID_SOCKET) closesocket(listener);
    for(int i=0;i<3;++i) if(linked[i]) UnlinkDrive(FtpDrives[i].link);
    EnterCriticalSection(&StateLock); State.ftpPhase=0; State.armed=0; LeaveCriticalSection(&StateLock);
    if(!ready && !IsCancelled(NULL)) Failure(error[0] ? error : "FTP SERVER FAILED");
    else { EnterCriticalSection(&StateLock); State.phase=nb::Stopped; strcpy_s(State.message,"FTP SERVER STOPPED."); LeaveCriticalSection(&StateLock); }
    return 0;
}
static DWORD WINAPI ServerThread(void*) {
    char error[100]={0};
    SOCKET listener=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);
    bool ready=listener!=INVALID_SOCKET;
    if(!ready) _snprintf_s(error,sizeof(error),_TRUNCATE,"LISTENER SOCKET CREATE FAILED (%d)",WSAGetLastError());
    if(ready && BufferChoices[BufferChoice]) ready=nb::SetSocketBuffers(listener,BufferChoices[BufferChoice],error,sizeof(error));
    if(ready) ready=nb::PrepareListener(listener,error,sizeof(error));
    sockaddr_in address={0}; address.sin_family=AF_INET; address.sin_addr.s_addr=INADDR_ANY; address.sin_port=htons(nb::Port);
    if(ready && bind(listener,reinterpret_cast<sockaddr*>(&address),sizeof(address))) {
        int code=WSAGetLastError();
        _snprintf_s(error,sizeof(error),_TRUNCATE,"BIND PORT 36020 FAILED (%d)",code); ready=false;
    }
    if(ready && listen(listener,nb::MaxStreams)) {
        int code=WSAGetLastError();
        _snprintf_s(error,sizeof(error),_TRUNCATE,"LISTEN PORT 36020 FAILED (%d)",code); ready=false;
    }
    if(!ready) Failure(error[0]?error:"COULD NOT LISTEN ON PORT 36020");
    else {
        Progress(NULL,nb::Listening,0,0,0,0);
        while(!IsCancelled(NULL)) {
            fd_set read; FD_ZERO(&read); FD_SET(listener,&read); timeval timeout={0,100000};
            int selected=select(0,&read,NULL,NULL,&timeout);
            if(selected==0) continue;
            if(selected==SOCKET_ERROR) {
                _snprintf_s(error,sizeof(error),_TRUNCATE,"LISTENER WAIT FAILED (%d)",WSAGetLastError()); Failure(error); break;
            }
            SOCKET client=accept(listener,NULL,NULL);
            if(client==INVALID_SOCKET) continue;
            if(!StartStream(client)) closesocket(client);
        }
    }
    // Streams poll the same cancel flag; wait so no socket outlives the session.
    for(int i=0;i<nb::MaxStreams;++i) {
        if(!Streams[i].thread) continue;
        WaitForSingleObject(Streams[i].thread,INFINITE); CloseHandle(Streams[i].thread); Streams[i].thread=NULL;
    }
    if(listener!=INVALID_SOCKET) closesocket(listener);
    EnterCriticalSection(&StateLock); State.armed=0; State.streams=0;
    if(IsCancelled(NULL)) { State.phase=nb::Stopped; strcpy_s(State.message,"STOPPED. PRESS A TO LISTEN AGAIN."); }
    LeaveCriticalSection(&StateLock);
    return 0;
}
static bool InitGraphics() {
    IDirect3D9* d3d=Direct3DCreate9(D3D_SDK_VERSION);
    if(!d3d) return false;
    D3DPRESENT_PARAMETERS pp={0};
    pp.BackBufferWidth=1280; pp.BackBufferHeight=720; pp.BackBufferFormat=D3DFMT_A8R8G8B8;
    pp.FrontBufferFormat=D3DFMT_LE_X8R8G8B8; pp.BackBufferCount=1; pp.SwapEffect=D3DSWAPEFFECT_DISCARD;
    pp.PresentationInterval=D3DPRESENT_INTERVAL_ONE;
    HRESULT hr=d3d->CreateDevice(0,D3DDEVTYPE_HAL,NULL,D3DCREATE_HARDWARE_VERTEXPROCESSING,&pp,&Device);
    d3d->Release(); if(FAILED(hr)) return false;
    const char* vs="struct V { float2 p:POSITION0; float4 c:COLOR0; }; struct O { float4 p:POSITION0; float4 c:COLOR0; }; O main(V v) { O o; o.p=float4(v.p,0,1); o.c=v.c; return o; }";
    const char* ps="float4 main(float4 c:COLOR0):COLOR0 { return c; }";
    ID3DXBuffer* code=NULL; ID3DXBuffer* errors=NULL;
    hr=D3DXCompileShader(vs,UINT(strlen(vs)),NULL,NULL,"main","vs_2_0",0,&code,&errors,NULL);
    if(errors) errors->Release(); if(FAILED(hr)) return false;
    hr=Device->CreateVertexShader(reinterpret_cast<DWORD*>(code->GetBufferPointer()),&VertexShader);
    code->Release(); if(FAILED(hr)) return false;
    code=NULL; errors=NULL;
    hr=D3DXCompileShader(ps,UINT(strlen(ps)),NULL,NULL,"main","ps_2_0",0,&code,&errors,NULL);
    if(errors) errors->Release(); if(FAILED(hr)) return false;
    hr=Device->CreatePixelShader(reinterpret_cast<DWORD*>(code->GetBufferPointer()),&PixelShader);
    code->Release(); if(FAILED(hr)) return false;
    const D3DVERTEXELEMENT9 elements[]={
        {0,0,D3DDECLTYPE_FLOAT2,D3DDECLMETHOD_DEFAULT,D3DDECLUSAGE_POSITION,0},
        {0,8,D3DDECLTYPE_D3DCOLOR,D3DDECLMETHOD_DEFAULT,D3DDECLUSAGE_COLOR,0},D3DDECL_END()};
    return SUCCEEDED(Device->CreateVertexDeclaration(elements,&Declaration));
}
static void Rectangle(void*,int x,int y,int w,int h,unsigned int color) {
    if(VertexCount+6>sizeof(Vertices)/sizeof(Vertices[0])) return;
    float left=2.0f*x/1280-1,right=2.0f*(x+w)/1280-1,top=1-2.0f*y/720,bottom=1-2.0f*(y+h)/720;
    Vertex quad[6]={{left,top,color},{right,top,color},{left,bottom,color},{left,bottom,color},{right,top,color},{right,bottom,color}};
    memcpy(Vertices+VertexCount,quad,sizeof(quad)); VertexCount+=6;
}
static void Render(const nb::UiState& state) {
    VertexCount=0; nb::DrawUi(state,Rectangle,NULL);
    Device->Clear(0,NULL,D3DCLEAR_TARGET,0xFF0F1725,1,0);
    Device->SetRenderState(D3DRS_ZENABLE,FALSE); Device->SetRenderState(D3DRS_CULLMODE,D3DCULL_NONE);
    Device->SetRenderState(D3DRS_ALPHABLENDENABLE,FALSE);
    Device->SetVertexDeclaration(Declaration); Device->SetVertexShader(VertexShader); Device->SetPixelShader(PixelShader);
    Device->DrawPrimitiveUP(D3DPT_TRIANGLELIST,VertexCount/3,Vertices,sizeof(Vertex));
    Device->Present(NULL,NULL,NULL,NULL);
}
void __cdecl main() {
    InitializeCriticalSection(&StateLock);
    for(int i=0;i<nb::MaxStreams;++i) { Streams[i].thread=NULL; Streams[i].socket=INVALID_SOCKET; }
    DescribeSettings();
    if(!InitGraphics()) XLaunchNewImage(XLAUNCH_KEYWORD_DASH,0);
    GraphicsIdle=CreateEvent(NULL,TRUE,FALSE,NULL);
    XNetStartupParams network={0}; network.cfgSizeOfStruct=sizeof(network); network.cfgFlags=XNET_STARTUP_BYPASS_SECURITY;
    network.cfgSockDefaultRecvBufsizeInK=64; network.cfgSockDefaultSendBufsizeInK=64;
    bool xnet=XNetStartup(&network)==0; WSADATA wsa;
    NetworkReady=xnet && WSAStartup(MAKEWORD(2,2),&wsa)==0;
    if(!NetworkReady) Failure("NETWORK STARTUP FAILED. BACK TO EXIT.");
    WORD previous=0; double lastAddress=0; bool renderingPaused=false;
    for(;;) {
        if(Worker && WaitForSingleObject(Worker,0)==WAIT_OBJECT_0) { CloseHandle(Worker); Worker=NULL; }
        if(NetworkReady && nb::Now()-lastAddress>=1) {
            XNADDR address={0}; XNetGetTitleXnAddr(&address);
            EnterCriticalSection(&StateLock);
            const BYTE* octets=reinterpret_cast<const BYTE*>(&address.ina);
            if(address.ina.s_addr) _snprintf_s(State.ip,sizeof(State.ip),_TRUNCATE,"%u.%u.%u.%u",octets[0],octets[1],octets[2],octets[3]);
            else strcpy_s(State.ip,"WAITING FOR NETWORK");
            LeaveCriticalSection(&StateLock); lastAddress=nb::Now();
        }
        WORD buttons=0;
        for(DWORD pad=0;pad<4;++pad) { XINPUT_STATE input={0}; if(XInputGetState(pad,&input)==ERROR_SUCCESS) buttons|=input.Gamepad.wButtons; }
        WORD pressed=buttons & ~previous; previous=buttons;
        if((pressed&XINPUT_GAMEPAD_A) && NetworkReady && !Worker) {
            InterlockedExchange(&Cancel,0);
            EnterCriticalSection(&StateLock); State.armed=1; State.message[0]=0; LeaveCriticalSection(&StateLock);
            Worker=CreateThread(NULL,128*1024,ServerThread,NULL,0,NULL);
            if(!Worker) { Failure("COULD NOT START WORKER"); EnterCriticalSection(&StateLock); State.armed=0; LeaveCriticalSection(&StateLock); }
        }
        if((pressed&XINPUT_GAMEPAD_LEFT_SHOULDER) && NetworkReady && !Worker) {
            InterlockedExchange(&Cancel,0);
            EnterCriticalSection(&StateLock); State.armed=1; State.message[0]=0; LeaveCriticalSection(&StateLock);
            Worker=CreateThread(NULL,128*1024,FtpThread,NULL,0,NULL);
            if(!Worker) { EnterCriticalSection(&StateLock); State.armed=0; LeaveCriticalSection(&StateLock); Failure("COULD NOT START FTP SERVER"); }
        }
        if((pressed&XINPUT_GAMEPAD_X) && !Worker) {
            InterlockedExchange(&Cancel,0);
            EnterCriticalSection(&StateLock); State.message[0]=0; State.phase=nb::Idle; LeaveCriticalSection(&StateLock);
            DiskProgress(1,0,0);
            Worker=CreateThread(NULL,64*1024,DiskThread,NULL,0,NULL);
            if(!Worker) { EnterCriticalSection(&StateLock); State.diskPhase=0; LeaveCriticalSection(&StateLock); Failure("COULD NOT START DISK TEST"); }
        }
        if(pressed&XINPUT_GAMEPAD_B) InterlockedExchange(&Cancel,1);
        // Settings apply when listening starts, so they change only while stopped.
        if(!Worker && (pressed&(XINPUT_GAMEPAD_DPAD_UP|XINPUT_GAMEPAD_DPAD_DOWN|XINPUT_GAMEPAD_DPAD_LEFT|XINPUT_GAMEPAD_DPAD_RIGHT))) {
            const int buffers=sizeof(BufferChoices)/sizeof(BufferChoices[0]);
            if(pressed&XINPUT_GAMEPAD_DPAD_UP) BufferChoice=(BufferChoice+1)%buffers;
            if(pressed&XINPUT_GAMEPAD_DPAD_DOWN) BufferChoice=(BufferChoice+buffers-1)%buffers;
            if(pressed&XINPUT_GAMEPAD_DPAD_RIGHT) CpuChoice=(CpuChoice+1)%7;
            if(pressed&XINPUT_GAMEPAD_DPAD_LEFT) CpuChoice=(CpuChoice+6)%7;
            EnterCriticalSection(&StateLock); DescribeSettings(); LeaveCriticalSection(&StateLock);
        }
        if(!Worker && (pressed&(XINPUT_GAMEPAD_Y|XINPUT_GAMEPAD_RIGHT_SHOULDER))) {
            if(pressed&XINPUT_GAMEPAD_Y) DiskChoice=(DiskChoice+1)%(sizeof(DiskTargets)/sizeof(DiskTargets[0]));
            if(pressed&XINPUT_GAMEPAD_RIGHT_SHOULDER) DiskBlockChoice=(DiskBlockChoice+1)%(sizeof(DiskBlocks)/sizeof(DiskBlocks[0]));
            EnterCriticalSection(&StateLock); DescribeSettings(); LeaveCriticalSection(&StateLock);
        }
        if((pressed&XINPUT_GAMEPAD_BACK) && !Worker) break;
        EnterCriticalSection(&StateLock); nb::UiState snapshot=State; LeaveCriticalSection(&StateLock);
        if(snapshot.armed && (snapshot.options&nb::PauseDrawing)) {
            if(!renderingPaused) {
                strcpy_s(snapshot.message,"DRAWING PAUSED. USE PC FOR LIVE PROGRESS. B STILL STOPS.");
                Render(snapshot); Device->BlockUntilIdle(); renderingPaused=true;
            }
            if(GraphicsIdle) SetEvent(GraphicsIdle);
        } else { renderingPaused=false; Render(snapshot); }
        // Ten UI updates per second keep rendering work modest during a test.
        Sleep(100);
    }
    if(NetworkReady) WSACleanup(); if(xnet) XNetCleanup();
    if(GraphicsIdle) CloseHandle(GraphicsIdle);
    XLaunchNewImage(XLAUNCH_KEYWORD_DASH,0);
}
