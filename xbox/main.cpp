// SPDX-License-Identifier: MIT
#include <xtl.h>
#include <d3dx9.h>
#include <stdio.h>
#include <stdlib.h>
#include "../shared/bench.h"
#include "../shared/ui.h"
#include "../shared/ftp.h"
#include "../third_party/imgui/imgui.h"
#include "imgui_impl_xbox360.h"

static CRITICAL_SECTION StateLock;
static nb::UiState State;
static volatile LONG Cancel=0;
static HANDLE Worker=NULL;
static HANDLE GraphicsIdle=NULL;
static bool NetworkReady=false;
static IDirect3DDevice9* Device=NULL;

// Xbox socket buffer choices for the listener; accepted sockets inherit them.
// Zero keeps the 64 KiB XNet startup default.
static const int BufferChoices[]={0,131072,262144};
static int BufferChoice=0;
// Hardware thread for network test streams and FTP sessions: 0 lets the kernel
// choose, otherwise 1-6 selects hardware thread 0-5. USB completion DPCs run on
// hardware thread 2.
static int CpuChoice=0;
// FTP speed limit for all transfers together in MiB/s; 0 is no limit. One
// console with a stock 20 GB drive froze at 20+ MiB/s but not at 14.
static const int SpeedChoices[]={0,12,16,20};
static int SpeedChoice=0;
// Whether to keep drawing while data moves. Off by default: drawing during
// Xbox sends has preceded console crashes, so the screen is held instead.
static bool DrawDuringTransfers=false;
// FTP login. Required by default; switching it off lets any user name in.
// The user can change both with the on-screen keyboard (Y on the FTP tab).
// The login and the Settings tab are kept in FastFTP360.ini next to the XEX.
static bool FtpLoginRequired=true;
static char FtpUser[nb::FtpLoginMax+1]="xboxftp",FtpPassword[nb::FtpLoginMax+1]="xboxftp";
static const char SettingsFile[]="game:\\FastFTP360.ini";
// Read when FastFTP360.ini is missing, so a login saved before the rename carries over.
static const char OldLoginFile[]="game:\\FastFTP.ini";
// On-screen keyboard: 0 closed, 1 asking for the user name, 2 for the password.
static int KeyboardStep=0;
static DWORD KeyboardPad=0;
static XOVERLAPPED KeyboardOverlapped;
static WCHAR KeyboardText[nb::FtpLoginMax+2];
static char PendingUser[nb::FtpLoginMax+1];

// Disk test targets. The app's own drive needs no mount; the others are linked
// to their kernel device for the test and unlinked afterwards.
struct DiskTarget { const char* name; const char* device; };
static const DiskTarget DiskTargets[]={{"App drive",NULL},{"HDD","\\Device\\Harddisk0\\Partition1"},
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
// FTP folders backed by drives, with the names Aurora's FTP gives them. Each is
// linked as \??\NB<name>: to the first device path whose root can be read;
// drives that are absent or unreadable are not listed.
struct FtpDrive { const char* name; const char* devices[2]; };
static const FtpDrive FtpDrives[]={
    {"Hdd1",{"\\Device\\Harddisk0\\Partition1",NULL}},
    {"HddX",{"\\Device\\Harddisk0\\SystemPartition",NULL}},
    {"SysExt",{"\\Device\\Harddisk0\\SystemExtPartition",NULL}},
    {"System",{"\\Device\\Harddisk0\\SystemAuxPartition",NULL}},
    {"Usb0",{"\\Device\\Mass0",NULL}},{"Usb1",{"\\Device\\Mass1",NULL}},{"Usb2",{"\\Device\\Mass2",NULL}},
    {"USBMU0",{"\\Device\\Mass0PartitionFile\\Storage",NULL}},
    {"USBMU1",{"\\Device\\Mass1PartitionFile\\Storage",NULL}},
    {"USBMU2",{"\\Device\\Mass2PartitionFile\\Storage",NULL}},
    {"USBMUCache0",{"\\Device\\Mass0PartitionFile\\StorageSystem","\\Device\\Mass0PartitionFile\\SystemCache"}},
    {"USBMUCache1",{"\\Device\\Mass1PartitionFile\\StorageSystem","\\Device\\Mass1PartitionFile\\SystemCache"}},
    {"USBMUCache2",{"\\Device\\Mass2PartitionFile\\StorageSystem","\\Device\\Mass2PartitionFile\\SystemCache"}}};
enum { FtpDriveCount=sizeof(FtpDrives)/sizeof(FtpDrives[0]) };
static char FtpLinks[FtpDriveCount][24],FtpPaths[FtpDriveCount][24];
static int CompareRoots(const void* a,const void* b) {
    return _stricmp(static_cast<const nb::FtpRoot*>(a)->name,static_cast<const nb::FtpRoot*>(b)->name);
}

// FTP log, next to the XEX. Every line starts with the local date and time.
// A log over 1 MiB is moved to FastFTP360.old.log when the server starts.
static const char LogFile[]="game:\\FastFTP360.log",OldLogFile[]="game:\\FastFTP360.old.log";
static CRITICAL_SECTION LogLock;
static void WriteLog(const char* text) {
    SYSTEMTIME now; GetLocalTime(&now);
    char line[800];
    int length=_snprintf_s(line,sizeof(line),_TRUNCATE,"%04u-%02u-%02u %02u:%02u:%02u  %s\r\n",
        now.wYear,now.wMonth,now.wDay,now.wHour,now.wMinute,now.wSecond,text);
    if(length<0) length=int(strlen(line));
    EnterCriticalSection(&LogLock);
    HANDLE file=CreateFile(LogFile,GENERIC_WRITE,FILE_SHARE_READ,NULL,OPEN_ALWAYS,FILE_ATTRIBUTE_NORMAL,NULL);
    if(file!=INVALID_HANDLE_VALUE) {
        SetFilePointer(file,0,NULL,FILE_END);
        DWORD written=0; WriteFile(file,line,DWORD(length),&written,NULL);
        CloseHandle(file);
    }
    LeaveCriticalSection(&LogLock);
}
static void FtpLog(void*,const char* line) { WriteLog(line); }
static void RotateLog() {
    WIN32_FILE_ATTRIBUTE_DATA info;
    if(!GetFileAttributesEx(LogFile,GetFileExInfoStandard,&info) || (!info.nFileSizeHigh && info.nFileSizeLow<1048576)) return;
    EnterCriticalSection(&LogLock);
    DeleteFile(OldLogFile); MoveFile(LogFile,OldLogFile);
    LeaveCriticalSection(&LogLock);
}

// The index of a whole decimal number in choices, or -1.
static int ChoiceOf(const int* choices,int count,const char* value) {
    if(!*value) return -1;
    for(const char* c=value;*c;++c) if(*c<'0' || *c>'9') return -1;
    for(int i=0;i<count;++i) if(choices[i]==atoi(value)) return i;
    return -1;
}
// Missing or unknown values keep the defaults.
static void LoadSettings() {
    HANDLE file=CreateFile(SettingsFile,GENERIC_READ,FILE_SHARE_READ,NULL,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,NULL);
    if(file==INVALID_HANDLE_VALUE) file=CreateFile(OldLoginFile,GENERIC_READ,FILE_SHARE_READ,NULL,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,NULL);
    if(file==INVALID_HANDLE_VALUE) return;
    char text[512],value[16]; DWORD read=0; int choice;
    if(ReadFile(file,text,sizeof(text)-1,&read,NULL)) {
        text[read]=0; nb::ParseFtpLogin(text,FtpLoginRequired,FtpUser,FtpPassword);
        if(nb::FindSetting(text,"buffers",value,sizeof(value))
            && (choice=ChoiceOf(BufferChoices,sizeof(BufferChoices)/sizeof(BufferChoices[0]),value))>=0) BufferChoice=choice;
        if(nb::FindSetting(text,"cpu",value,sizeof(value))) {
            if(!_stricmp(value,"auto")) CpuChoice=0;
            else if(value[0]>='0' && value[0]<='5' && !value[1]) CpuChoice=value[0]-'0'+1;
        }
        if(nb::FindSetting(text,"drawing",value,sizeof(value))) {
            if(!_stricmp(value,"live")) DrawDuringTransfers=true; else if(!_stricmp(value,"paused")) DrawDuringTransfers=false;
        }
        if(nb::FindSetting(text,"speed",value,sizeof(value))
            && (choice=ChoiceOf(SpeedChoices,sizeof(SpeedChoices)/sizeof(SpeedChoices[0]),value))>=0) SpeedChoice=choice;
    }
    CloseHandle(file);
}
static bool SaveSettings() {
    char text[320],cpu[8]; nb::FormatFtpLogin(text,sizeof(text),FtpLoginRequired,FtpUser,FtpPassword);
    if(CpuChoice) _snprintf_s(cpu,sizeof(cpu),_TRUNCATE,"%d",CpuChoice-1); else strcpy_s(cpu,"auto");
    const size_t login=strlen(text);
    _snprintf_s(text+login,sizeof(text)-login,_TRUNCATE,"buffers=%d\r\ncpu=%s\r\ndrawing=%s\r\nspeed=%d\r\n",
        BufferChoices[BufferChoice],cpu,DrawDuringTransfers ? "live" : "paused",SpeedChoices[SpeedChoice]);
    HANDLE file=CreateFile(SettingsFile,GENERIC_WRITE,0,NULL,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,NULL);
    if(file==INVALID_HANDLE_VALUE) return false;
    DWORD written=0; bool ok=WriteFile(file,text,DWORD(strlen(text)),&written,NULL) && written==strlen(text);
    CloseHandle(file);
    return ok;
}

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
    if(BufferChoices[BufferChoice]) _snprintf_s(State.buffers,sizeof(State.buffers),_TRUNCATE,"%d KiB",BufferChoices[BufferChoice]/1024);
    else strcpy_s(State.buffers,"Startup default (64 KiB)");
    if(CpuChoice) _snprintf_s(State.cpu,sizeof(State.cpu),_TRUNCATE,"Hardware thread %d",CpuChoice-1); else strcpy_s(State.cpu,"Automatic");
    if(SpeedChoices[SpeedChoice]) _snprintf_s(State.speed,sizeof(State.speed),_TRUNCATE,"%d MiB/s",SpeedChoices[SpeedChoice]);
    else strcpy_s(State.speed,"Off");
    strcpy_s(State.diskTarget,DiskTargets[DiskChoice].name);
    if(DiskBlocks[DiskBlockChoice]>=1048576) _snprintf_s(State.diskBlock,sizeof(State.diskBlock),_TRUNCATE,"%u MiB",DiskBlocks[DiskBlockChoice]/1048576);
    else _snprintf_s(State.diskBlock,sizeof(State.diskBlock),_TRUNCATE,"%u KiB",DiskBlocks[DiskBlockChoice]/1024);
    strcpy_s(State.drawing,DrawDuringTransfers ? "Live" : "Paused");
    strcpy_s(State.ftpUser,FtpLoginRequired ? FtpUser : ""); strcpy_s(State.ftpPassword,FtpLoginRequired ? FtpPassword : "");
}
// Steps the focused setting on the current tab by one choice (D-pad left/right).
static void ChangeSetting(int tab,int row,int step) {
    const int buffers=sizeof(BufferChoices)/sizeof(BufferChoices[0]),targets=sizeof(DiskTargets)/sizeof(DiskTargets[0]),
        blocks=sizeof(DiskBlocks)/sizeof(DiskBlocks[0]),speeds=sizeof(SpeedChoices)/sizeof(SpeedChoices[0]);
    if(tab==nb::TabFtp) { FtpLoginRequired=!FtpLoginRequired; SaveSettings(); }
    else if(tab==nb::TabDisk) {
        if(row==0) DiskChoice=(DiskChoice+targets+step)%targets; else DiskBlockChoice=(DiskBlockChoice+blocks+step)%blocks;
    }
    // Advanced settings, kept off the everyday tabs.
    else if(tab==nb::TabSettings) {
        if(row==0) BufferChoice=(BufferChoice+buffers+step)%buffers;
        else if(row==1) CpuChoice=(CpuChoice+7+step)%7;
        else if(row==2) DrawDuringTransfers=!DrawDuringTransfers;
        else SpeedChoice=(SpeedChoice+speeds+step)%speeds;
        SaveSettings();
    }
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
            if(BatchCount>1) _snprintf_s(State.message,sizeof(State.message),_TRUNCATE,"%d streams passed their data checks. Ready for the next PC test.",BatchCount);
            else strcpy_s(State.message,stream->result.verified ? "All data checked. Ready for the next PC test." : "Data check passed. Ready for the next PC test.");
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
    const char* path="game:\\FASTFTP360-DISKTEST.TMP";
    if(target.device) {
        LONG status=LinkDrive("\\??\\NBDISK:",target.device);
        if(status<0) { DiskFailure("%s not found or not mountable (0x%08X).",target.name,DWORD(status)); return 0; }
        linked=true; path="NBDISK:\\FASTFTP360-DISKTEST.TMP";
    }
    unsigned char* buffer=static_cast<unsigned char*>(VirtualAlloc(NULL,block,MEM_COMMIT,PAGE_READWRITE));
    unsigned char* check=static_cast<unsigned char*>(VirtualAlloc(NULL,block,MEM_COMMIT,PAGE_READWRITE));
    double writeRate=0,readRate=0; bool ok=false;
    if(!buffer || !check) DiskFailure("Could not allocate disk test buffers%s (%u).","",GetLastError());
    else {
        nb::Pattern(buffer,0x360D15C);
        for(DWORD at=nb::Chunk;at<block;at+=nb::Chunk) memcpy(buffer+at,buffer,nb::Chunk);
        HANDLE file=CreateFile(path,GENERIC_WRITE,0,NULL,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,NULL);
        if(file==INVALID_HANDLE_VALUE) DiskFailure("Cannot create the test file on %s (%u). Is 256 MiB free?",target.name,GetLastError());
        else {
            // Write phase: normal cached writes, like an FTP server; the final
            // flush is timed so data still in the cache is included.
            double begin=nb::Now(),update=begin; nb::U64 done=0; ok=true;
            while(done<DiskTestBytes) {
                if(IsCancelled(NULL)) { ok=false; break; }
                DWORD written=0;
                if(!WriteFile(file,buffer,block,&written,NULL) || written!=block) {
                    DiskFailure("Write failed on %s (%u). Is the drive full?",target.name,GetLastError()); ok=false; break;
                }
                done+=block;
                if(nb::Now()-update>=0.25) { update=nb::Now(); DiskProgress(1,done,update-begin); }
            }
            if(ok && !FlushFileBuffers(file)) { DiskFailure("Flush failed on %s (%u).",target.name,GetLastError()); ok=false; }
            CloseHandle(file);
            if(ok) { writeRate=double(DiskTestBytes)/1048576/(nb::Now()-begin); DiskProgress(1,done,nb::Now()-begin); }
        }
        if(ok) {
            // Read phase bypasses the file cache so the drive itself is measured.
            HANDLE file=CreateFile(path,GENERIC_READ,FILE_SHARE_READ,NULL,OPEN_EXISTING,FILE_FLAG_NO_BUFFERING|FILE_FLAG_SEQUENTIAL_SCAN,NULL);
            if(file==INVALID_HANDLE_VALUE) { DiskFailure("Cannot reopen the test file on %s (%u).",target.name,GetLastError()); ok=false; }
            else {
                double begin=nb::Now(),update=begin; nb::U64 done=0;
                DiskProgress(2,0,0);
                while(done<DiskTestBytes) {
                    if(IsCancelled(NULL)) { ok=false; break; }
                    DWORD read=0;
                    if(!ReadFile(file,check,block,&read,NULL) || read!=block) {
                        DiskFailure("Read failed on %s (%u).",target.name,GetLastError()); ok=false; break;
                    }
                    if(memcmp(check,buffer,block)) {
                        DiskFailure("Read-back data mismatch on %s at MiB %u. Stop using this drive.",target.name,DWORD(done/1048576));
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
        _snprintf_s(State.disk,sizeof(State.disk),_TRUNCATE,"%s, %u KiB blocks: write %.2f, read %.2f MiB/s, all data checked",
            target.name,block/1024,writeRate,readRate);
        strcpy_s(State.message,"Disk test passed. Test file deleted.");
    } else if(IsCancelled(NULL)) { State.phase=nb::Stopped; strcpy_s(State.message,"Disk test stopped. Test file deleted."); }
    LeaveCriticalSection(&StateLock);
    return 0;
}
// A transfer's data connection is open. Unless drawing stays live, wait until
// the main thread has drawn its last frame and the GPU is idle, as the network
// test does, so no data moves while the GPU is still busy.
static void FtpStarting(void*,const char* operation,const char*) {
    const bool pause=!DrawDuringTransfers && GraphicsIdle;
    if(pause) ResetEvent(GraphicsIdle);
    EnterCriticalSection(&StateLock);
    ++State.ftpActive; State.ftpPhase=operation[0]=='S' ? 2 : 3; State.mebibytes=0; State.elapsed=0; State.rate=0;
    LeaveCriticalSection(&StateLock);
    if(!pause) return;
    const double deadline=nb::Now()+5;
    while(!IsCancelled(NULL) && nb::Now()<deadline && WaitForSingleObject(GraphicsIdle,100)!=WAIT_OBJECT_0) {}
}
static void FtpProgress(void*,const char* operation,const char* file,nb::U64 bytes,double seconds,bool done,bool ok) {
    EnterCriticalSection(&StateLock);
    double mib=double(bytes)/1048576,rate=seconds>0 ? mib/seconds : 0;
    const char* kind=operation[0]=='S' ? "Upload" : "Download";
    if(done) {
        // Another session's transfer may still be running; drawing stays paused until none is.
        if(State.ftpActive>0) --State.ftpActive;
        if(!State.ftpActive) State.ftpPhase=1;
        if(ok) _snprintf_s(State.ftp,sizeof(State.ftp),_TRUNCATE,"%s, %.1f MiB in %.1f s: %.2f MiB/s",kind,mib,seconds,rate);
        else _snprintf_s(State.ftp,sizeof(State.ftp),_TRUNCATE,"%s failed after %.1f MiB",kind,mib);
        // Newest first in the last-transfers list.
        memmove(&State.recent[1],&State.recent[0],sizeof(State.recent[0])*(nb::UiRecentTransfers-1));
        nb::UiTransfer& entry=State.recent[0];
        strncpy_s(entry.name,sizeof(entry.name),file,_TRUNCATE); entry.mebibytes=mib; entry.rate=rate; entry.upload=operation[0]=='S'; entry.ok=ok;
        if(State.recentCount<nb::UiRecentTransfers) ++State.recentCount;
    } else {
        State.ftpPhase=operation[0]=='S' ? 2 : 3; State.mebibytes=mib; State.elapsed=seconds; State.rate=rate;
    }
    LeaveCriticalSection(&StateLock);
}
// Passive-mode FTP server on port 21 until B. The top level matches Aurora's
// FTP: the readable drives plus Game for the app's own drive, sorted by name.
static DWORD WINAPI FtpThread(void*) {
    RotateLog();
    nb::FtpRoot roots[FtpDriveCount+1]; int count=0; bool linked[FtpDriveCount]={false};
    for(int i=0;i<FtpDriveCount;++i) {
        _snprintf_s(FtpLinks[i],sizeof(FtpLinks[i]),_TRUNCATE,"\\??\\NB%s:",FtpDrives[i].name);
        _snprintf_s(FtpPaths[i],sizeof(FtpPaths[i]),_TRUNCATE,"NB%s:",FtpDrives[i].name);
        for(int d=0;d<2 && FtpDrives[i].devices[d] && !linked[i];++d) {
            if(LinkDrive(FtpLinks[i],FtpDrives[i].devices[d])<0) continue;
            char pattern[40]; _snprintf_s(pattern,sizeof(pattern),_TRUNCATE,"%s\\*",FtpPaths[i]);
            WIN32_FIND_DATA entry; HANDLE find=FindFirstFile(pattern,&entry);
            bool readable=find!=INVALID_HANDLE_VALUE || GetLastError()==ERROR_FILE_NOT_FOUND;
            if(find!=INVALID_HANDLE_VALUE) FindClose(find);
            if(readable) linked[i]=true; else UnlinkDrive(FtpLinks[i]);
        }
        if(linked[i]) { roots[count].name=FtpDrives[i].name; roots[count].path=FtpPaths[i]; ++count; }
    }
    roots[count].name="Game"; roots[count].path="game:"; ++count;
    qsort(roots,count,sizeof(roots[0]),CompareRoots);
    char folders[100]="Folders:";
    for(int i=0;i<count;++i) { strncat_s(folders," ",_TRUNCATE); strncat_s(folders,roots[i].name,_TRUNCATE); }
    char error[100]={0};
    SOCKET listener=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);
    bool ready=listener!=INVALID_SOCKET;
    if(!ready) _snprintf_s(error,sizeof(error),_TRUNCATE,"FTP socket create failed (%d).",WSAGetLastError());
    if(ready && BufferChoices[BufferChoice]) ready=nb::SetSocketBuffers(listener,BufferChoices[BufferChoice],error,sizeof(error));
    if(ready) ready=nb::PrepareListener(listener,error,sizeof(error));
    sockaddr_in address={0}; address.sin_family=AF_INET; address.sin_addr.s_addr=INADDR_ANY; address.sin_port=htons(nb::FtpPort);
    if(ready && bind(listener,reinterpret_cast<sockaddr*>(&address),sizeof(address))) {
        _snprintf_s(error,sizeof(error),_TRUNCATE,"Bind FTP port 21 failed (%d).",WSAGetLastError()); ready=false;
    }
    if(ready && listen(listener,8)) { _snprintf_s(error,sizeof(error),_TRUNCATE,"Listen on FTP port 21 failed (%d).",WSAGetLastError()); ready=false; }
    if(ready) {
        XNADDR xnaddr={0}; XNetGetTitleXnAddr(&xnaddr);
        EnterCriticalSection(&StateLock);
        State.ftpPhase=1; State.phase=nb::Idle; State.ftp[0]=0; strcpy_s(State.message,folders);
        LeaveCriticalSection(&StateLock);
        nb::FtpObserver observer={NULL,IsCancelled,FtpProgress,FtpLog,FtpStarting};
        char line[300];
        const BYTE* ip=reinterpret_cast<const BYTE*>(&xnaddr.ina);
        _snprintf_s(line,sizeof(line),_TRUNCATE,"FTP server started on %u.%u.%u.%u port 21, login %s, speed limit %s, worker CPU %s. %s",
            ip[0],ip[1],ip[2],ip[3],FtpLoginRequired ? FtpUser : "off",State.speed,State.cpu,folders);
        WriteLog(line);
        if(!nb::ServeFtp(listener,roots,count,xnaddr.ina.s_addr,BufferChoices[BufferChoice],SpeedChoices[SpeedChoice],CpuChoice-1,0,
            FtpLoginRequired ? FtpUser : NULL,FtpPassword,observer,error,sizeof(error))) ready=false;
    }
    if(listener!=INVALID_SOCKET) closesocket(listener);
    for(int i=0;i<FtpDriveCount;++i) if(linked[i]) UnlinkDrive(FtpLinks[i]);
    if(!ready && !IsCancelled(NULL)) { char line[160]; _snprintf_s(line,sizeof(line),_TRUNCATE,"FTP server error: %s",error[0] ? error : "unknown"); WriteLog(line); }
    else WriteLog("FTP server stopped");
    EnterCriticalSection(&StateLock); State.ftpPhase=0; State.ftpActive=0; State.armed=0; LeaveCriticalSection(&StateLock);
    if(!ready && !IsCancelled(NULL)) Failure(error[0] ? error : "FTP server failed.");
    else { EnterCriticalSection(&StateLock); State.phase=nb::Stopped; strcpy_s(State.message,"FTP server stopped."); LeaveCriticalSection(&StateLock); }
    return 0;
}
static DWORD WINAPI ServerThread(void*) {
    char error[100]={0};
    SOCKET listener=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);
    bool ready=listener!=INVALID_SOCKET;
    if(!ready) _snprintf_s(error,sizeof(error),_TRUNCATE,"Listener socket create failed (%d).",WSAGetLastError());
    if(ready && BufferChoices[BufferChoice]) ready=nb::SetSocketBuffers(listener,BufferChoices[BufferChoice],error,sizeof(error));
    if(ready) ready=nb::PrepareListener(listener,error,sizeof(error));
    sockaddr_in address={0}; address.sin_family=AF_INET; address.sin_addr.s_addr=INADDR_ANY; address.sin_port=htons(nb::Port);
    if(ready && bind(listener,reinterpret_cast<sockaddr*>(&address),sizeof(address))) {
        int code=WSAGetLastError();
        _snprintf_s(error,sizeof(error),_TRUNCATE,"Bind port 36020 failed (%d).",code); ready=false;
    }
    if(ready && listen(listener,nb::MaxStreams)) {
        int code=WSAGetLastError();
        _snprintf_s(error,sizeof(error),_TRUNCATE,"Listen on port 36020 failed (%d).",code); ready=false;
    }
    if(!ready) Failure(error[0]?error:"Could not listen on port 36020.");
    else {
        Progress(NULL,nb::Listening,0,0,0,0);
        while(!IsCancelled(NULL)) {
            fd_set read; FD_ZERO(&read); FD_SET(listener,&read); timeval timeout={0,100000};
            int selected=select(0,&read,NULL,NULL,&timeout);
            if(selected==0) continue;
            if(selected==SOCKET_ERROR) {
                _snprintf_s(error,sizeof(error),_TRUNCATE,"Listener wait failed (%d).",WSAGetLastError()); Failure(error); break;
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
    if(IsCancelled(NULL)) { State.phase=nb::Stopped; strcpy_s(State.message,"Stopped. Press A to listen again."); }
    LeaveCriticalSection(&StateLock);
    return 0;
}
static bool OpenKeyboard(DWORD pad,int step,const char* current) {
    WCHAR initial[nb::FtpLoginMax+1];
    for(int i=0;;++i) { initial[i]=WCHAR(current[i]); if(!current[i]) break; }
    memset(&KeyboardOverlapped,0,sizeof(KeyboardOverlapped)); KeyboardText[0]=0;
    DWORD result=XShowKeyboardUI(pad,VKBD_DEFAULT,initial,step==1 ? L"FTP user name" : L"FTP password",
        L"Up to 23 characters: letters, digits and symbols, no spaces.",KeyboardText,nb::FtpLoginMax+2,&KeyboardOverlapped);
    if(result!=ERROR_IO_PENDING && result!=ERROR_SUCCESS) return false;
    KeyboardStep=step; KeyboardPad=pad; return true;
}
static void LoginMessage(const char* text) {
    EnterCriticalSection(&StateLock); State.phase=nb::Idle; strcpy_s(State.message,text); LeaveCriticalSection(&StateLock);
}
// Called every frame while the keyboard is open: user name first, then password.
static void PollKeyboard() {
    if(!KeyboardStep || !XHasOverlappedIoCompleted(&KeyboardOverlapped)) return;
    const int step=KeyboardStep; KeyboardStep=0;
    if(XGetOverlappedResult(&KeyboardOverlapped,NULL,FALSE)!=ERROR_SUCCESS) { LoginMessage("Login unchanged."); return; }
    char text[nb::FtpLoginMax+2]; int i=0;
    for(;KeyboardText[i] && i<nb::FtpLoginMax+1;++i) text[i]=KeyboardText[i]<0x80 ? char(KeyboardText[i]) : '\x01';
    text[i]=0;
    if(!nb::ValidFtpLogin(text)) { LoginMessage("Login unchanged: use 1-23 letters, digits or symbols, no spaces."); return; }
    if(step==1) {
        strcpy_s(PendingUser,text);
        if(!OpenKeyboard(KeyboardPad,2,FtpPassword)) LoginMessage("Could not open the keyboard. Login unchanged.");
        return;
    }
    EnterCriticalSection(&StateLock);
    strcpy_s(FtpUser,PendingUser); strcpy_s(FtpPassword,text); FtpLoginRequired=true; DescribeSettings();
    LeaveCriticalSection(&StateLock);
    char line[80]; _snprintf_s(line,sizeof(line),_TRUNCATE,"FTP login changed on the Xbox (user %s)",PendingUser); WriteLog(line);
    LoginMessage(SaveSettings() ? "Login changed and saved in FastFTP360.ini." : "Login changed, but FastFTP360.ini could not be saved.");
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
    return ImGuiXbox360Init(Device);
}
static void Render(const nb::UiState& state,float seconds) {
    ImGui::GetIO().DeltaTime=seconds>0.0001f ? seconds : 0.0001f;
    nb::DrawUi(state);
    Device->Clear(0,NULL,D3DCLEAR_TARGET,0xFF0F1725,1,0);
    ImGuiXbox360Render(ImGui::GetDrawData());
    Device->Present(NULL,NULL,NULL,NULL);
}
void __cdecl main() {
    InitializeCriticalSection(&StateLock); InitializeCriticalSection(&LogLock);
    for(int i=0;i<nb::MaxStreams;++i) { Streams[i].thread=NULL; Streams[i].socket=INVALID_SOCKET; }
    LoadSettings();
    DescribeSettings();
    nb::InitUi();
    if(!InitGraphics()) XLaunchNewImage(XLAUNCH_KEYWORD_DASH,0);
    GraphicsIdle=CreateEvent(NULL,TRUE,FALSE,NULL);
    XNetStartupParams network={0}; network.cfgSizeOfStruct=sizeof(network); network.cfgFlags=XNET_STARTUP_BYPASS_SECURITY;
    network.cfgSockDefaultRecvBufsizeInK=64; network.cfgSockDefaultSendBufsizeInK=64;
    bool xnet=XNetStartup(&network)==0; WSADATA wsa;
    NetworkReady=xnet && WSAStartup(MAKEWORD(2,2),&wsa)==0;
    if(!NetworkReady) Failure("Network startup failed. Press Back to exit.");
    WORD previous=0; double lastAddress=0,lastFrame=nb::Now(); bool renderingPaused=false;
    for(;;) {
        if(Worker && WaitForSingleObject(Worker,0)==WAIT_OBJECT_0) { CloseHandle(Worker); Worker=NULL; }
        if(NetworkReady && nb::Now()-lastAddress>=1) {
            XNADDR address={0}; XNetGetTitleXnAddr(&address);
            EnterCriticalSection(&StateLock);
            const BYTE* octets=reinterpret_cast<const BYTE*>(&address.ina);
            if(address.ina.s_addr) _snprintf_s(State.ip,sizeof(State.ip),_TRUNCATE,"%u.%u.%u.%u",octets[0],octets[1],octets[2],octets[3]);
            else strcpy_s(State.ip,"Waiting for network");
            LeaveCriticalSection(&StateLock); lastAddress=nb::Now();
        }
        WORD buttons=0; DWORD keyboardPad=0;
        for(DWORD pad=0;pad<4;++pad) {
            XINPUT_STATE input={0};
            if(XInputGetState(pad,&input)!=ERROR_SUCCESS) continue;
            if(input.Gamepad.wButtons & ~previous & XINPUT_GAMEPAD_Y) keyboardPad=pad;
            buttons|=input.Gamepad.wButtons;
        }
        WORD pressed=buttons & ~previous; previous=buttons;
        // The keyboard takes the controller while it is open.
        if(KeyboardStep) { PollKeyboard(); pressed=0; }
        if((pressed&XINPUT_GAMEPAD_Y) && !Worker && State.tab==nb::TabFtp &&
            !OpenKeyboard(keyboardPad,1,FtpUser)) LoginMessage("Could not open the keyboard.");
        // A starts whatever the current tab is for; B stops it.
        if((pressed&XINPUT_GAMEPAD_A) && !Worker && State.tab==nb::TabNetwork && NetworkReady) {
            InterlockedExchange(&Cancel,0);
            EnterCriticalSection(&StateLock); State.armed=1; State.message[0]=0; LeaveCriticalSection(&StateLock);
            Worker=CreateThread(NULL,128*1024,ServerThread,NULL,0,NULL);
            if(!Worker) { Failure("Could not start the worker."); EnterCriticalSection(&StateLock); State.armed=0; LeaveCriticalSection(&StateLock); }
        }
        if((pressed&XINPUT_GAMEPAD_A) && !Worker && State.tab==nb::TabFtp && NetworkReady) {
            InterlockedExchange(&Cancel,0);
            EnterCriticalSection(&StateLock); State.armed=1; State.message[0]=0; LeaveCriticalSection(&StateLock);
            Worker=CreateThread(NULL,128*1024,FtpThread,NULL,0,NULL);
            if(!Worker) { EnterCriticalSection(&StateLock); State.armed=0; LeaveCriticalSection(&StateLock); Failure("Could not start the FTP server."); }
        }
        if((pressed&XINPUT_GAMEPAD_A) && !Worker && State.tab==nb::TabDisk) {
            InterlockedExchange(&Cancel,0);
            EnterCriticalSection(&StateLock); State.message[0]=0; State.phase=nb::Idle; LeaveCriticalSection(&StateLock);
            DiskProgress(1,0,0);
            Worker=CreateThread(NULL,64*1024,DiskThread,NULL,0,NULL);
            if(!Worker) { EnterCriticalSection(&StateLock); State.diskPhase=0; LeaveCriticalSection(&StateLock); Failure("Could not start the disk test."); }
        }
        if(pressed&XINPUT_GAMEPAD_B) InterlockedExchange(&Cancel,1);
        // Tabs and settings change only while stopped; settings apply when a test starts.
        if(!Worker && (pressed&(XINPUT_GAMEPAD_LEFT_SHOULDER|XINPUT_GAMEPAD_RIGHT_SHOULDER))) {
            EnterCriticalSection(&StateLock);
            State.tab=(State.tab+nb::TabCount+((pressed&XINPUT_GAMEPAD_RIGHT_SHOULDER) ? 1 : -1))%nb::TabCount;
            State.focus=0; State.message[0]=0; State.phase=nb::Idle;
            LeaveCriticalSection(&StateLock);
        }
        if(!Worker && (pressed&(XINPUT_GAMEPAD_DPAD_UP|XINPUT_GAMEPAD_DPAD_DOWN|XINPUT_GAMEPAD_DPAD_LEFT|XINPUT_GAMEPAD_DPAD_RIGHT))) {
            EnterCriticalSection(&StateLock);
            const int rows=nb::SettingCount(State.tab);
            if(rows) {
                if(pressed&XINPUT_GAMEPAD_DPAD_DOWN) State.focus=(State.focus+1)%rows;
                if(pressed&XINPUT_GAMEPAD_DPAD_UP) State.focus=(State.focus+rows-1)%rows;
                if(pressed&XINPUT_GAMEPAD_DPAD_RIGHT) ChangeSetting(State.tab,State.focus,1);
                if(pressed&XINPUT_GAMEPAD_DPAD_LEFT) ChangeSetting(State.tab,State.focus,-1);
                DescribeSettings();
            }
            LeaveCriticalSection(&StateLock);
        }
        if((pressed&XINPUT_GAMEPAD_BACK) && !Worker) break;
        EnterCriticalSection(&StateLock); nb::UiState snapshot=State; LeaveCriticalSection(&StateLock);
        snapshot.busy=Worker!=NULL; snapshot.keyboard=KeyboardStep!=0;
        const double now=nb::Now(); const float seconds=float(now-lastFrame); lastFrame=now;
        // Hold one static frame while data moves if the PC asked for it, or
        // unless drawing during transfers is switched on.
        const bool transfer=snapshot.ftpActive>0 || (snapshot.armed && !snapshot.ftpPhase && snapshot.phase>=nb::Connecting && snapshot.phase<=nb::Measuring);
        if(snapshot.armed && ((snapshot.options&nb::PauseDrawing) || (transfer && !DrawDuringTransfers))) {
            if(!renderingPaused) {
                snapshot.options|=nb::PauseDrawing;
                strcpy_s(snapshot.message,"Screen paused while data moves. The PC shows progress.");
                Render(snapshot,seconds); Device->BlockUntilIdle(); renderingPaused=true;
            }
            if(GraphicsIdle) SetEvent(GraphicsIdle);
        } else { renderingPaused=false; Render(snapshot,seconds); }
        // Ten UI updates per second keep rendering work modest during a test;
        // while stopped, run faster so the menus respond quickly.
        Sleep(Worker ? 100 : 16);
    }
    if(NetworkReady) WSACleanup(); if(xnet) XNetCleanup();
    if(GraphicsIdle) CloseHandle(GraphicsIdle);
    XLaunchNewImage(XLAUNCH_KEYWORD_DASH,0);
}
