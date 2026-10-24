// SPDX-License-Identifier: MIT
// Just enough of Win32 and Winsock to build shared/ftp.cpp, shared/bench.cpp
// and tests/ftp_host.cpp on Linux or macOS, so tests/ftp.py runs without
// Windows. Force-included with -include; the Xbox and Windows builds never
// see it. File calls take the server's backslash paths and use slashes.
#pragma once
#ifndef _WIN32
#include <arpa/inet.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <pthread.h>
#include <sched.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define __int64 long long
typedef unsigned int DWORD;
typedef int LONG;
typedef int BOOL;
typedef unsigned char BYTE;
typedef unsigned short WORD;
typedef unsigned short u_short;
typedef unsigned long u_long;
typedef int SOCKET;
typedef long long LONGLONG;
typedef union { LONGLONG QuadPart; } LARGE_INTEGER;
typedef struct { DWORD dwLowDateTime, dwHighDateTime; } FILETIME;
typedef struct { WORD wYear, wMonth, wDayOfWeek, wDay, wHour, wMinute, wSecond, wMilliseconds; } SYSTEMTIME;
typedef struct {
    DWORD dwFileAttributes; FILETIME ftCreationTime, ftLastAccessTime, ftLastWriteTime;
    DWORD nFileSizeHigh, nFileSizeLow; char cFileName[260];
} WIN32_FIND_DATA;
typedef struct { WORD wVersion; } WSADATA;
#define TRUE 1
#define FALSE 0
#define WINAPI
#define WSAAPI
#define INFINITE 0xFFFFFFFFu
#define WAIT_OBJECT_0 0u
#define WAIT_TIMEOUT 258u
#define INVALID_SOCKET (-1)
#define SOCKET_ERROR (-1)
#define SD_SEND SHUT_WR
#define WSAEWOULDBLOCK 10035
#define _TRUNCATE ((size_t)-1)
#define MAKEWORD(a,b) WORD((a)|((b)<<8))
#define GENERIC_READ 0x80000000u
#define GENERIC_WRITE 0x40000000u
#define FILE_SHARE_READ 1
#define CREATE_ALWAYS 2
#define OPEN_EXISTING 3
#define FILE_ATTRIBUTE_DIRECTORY 0x10
#define FILE_ATTRIBUTE_NORMAL 0x80
#define FILE_FLAG_SEQUENTIAL_SCAN 0x08000000
#define FILE_FLAG_NO_BUFFERING 0x20000000
#define FILE_END 2
#define INVALID_SET_FILE_POINTER ((DWORD)-1)
#define NO_ERROR 0
#define MEM_COMMIT 0x1000
#define MEM_RELEASE 0x8000
#define PAGE_READWRITE 0x04

// Every HANDLE points at one of these; INVALID_HANDLE_VALUE never does.
struct NbHandle {
    enum Kind { File, Event, Thread, Find } kind;
    int fd;
    pthread_mutex_t lock; pthread_cond_t changed; bool signaled, manual;
    pthread_t thread; DWORD (*start)(void*); void* argument;
    DIR* dir; char folder[600]; bool single;
    // Opened with FILE_FLAG_NO_BUFFERING: reads and writes must be aligned.
    bool unbuffered;
};
typedef NbHandle* HANDLE;
#define INVALID_HANDLE_VALUE ((HANDLE)(-1L))

static inline int WSAGetLastError() {
    if(errno==EWOULDBLOCK || errno==EAGAIN || errno==EINPROGRESS) return WSAEWOULDBLOCK;
    return errno ? 20000+errno : 0;
}
static inline DWORD GetLastError() { return DWORD(errno); }
static inline int WSAStartup(WORD,WSADATA*) { return 0; }
static inline int WSACleanup() { return 0; }
static inline int closesocket(SOCKET s) { return close(s); }
#define FIONBIO 0
static inline int ioctlsocket(SOCKET s,long,u_long* value) {
    int flags=fcntl(s,F_GETFL,0); if(flags<0) return -1;
    return fcntl(s,F_SETFL,*value ? flags|O_NONBLOCK : flags&~O_NONBLOCK)<0 ? -1 : 0;
}
// Winsock ignores select's first argument; POSIX needs the highest fd + 1.
static inline int nb_select(int,fd_set* r,fd_set* w,fd_set* e,struct timeval* t) { int rc=select(FD_SETSIZE,r,w,e,t); return rc<0 ? -1 : rc; }
#define select nb_select
static inline int nb_send(SOCKET s,const char* data,int count,int flags) { return int(send(s,data,size_t(count),flags|MSG_NOSIGNAL)); }
#define send nb_send
static inline int nb_recv(SOCKET s,char* data,int count,int flags) { return int(recv(s,data,size_t(count),flags)); }
#define recv nb_recv
static inline int nb_accept(SOCKET s,sockaddr* address,int* length) {
    socklen_t l=length ? socklen_t(*length) : 0; int rc=accept(s,address,length ? &l : NULL); if(length) *length=int(l); return rc;
}
#define accept nb_accept
static inline int nb_getpeername(SOCKET s,sockaddr* address,int* length) {
    socklen_t l=socklen_t(*length); int rc=getpeername(s,address,&l); *length=int(l); return rc;
}
#define getpeername nb_getpeername
static inline int nb_getsockname(SOCKET s,sockaddr* address,int* length) {
    socklen_t l=socklen_t(*length); int rc=getsockname(s,address,&l); *length=int(l); return rc;
}
#define getsockname nb_getsockname
static inline int nb_getsockopt(SOCKET s,int level,int name,char* value,int* length) {
    socklen_t l=socklen_t(*length); int rc=getsockopt(s,level,name,value,&l); *length=int(l); return rc;
}
#define getsockopt nb_getsockopt

static inline void Sleep(DWORD ms) { if(ms) usleep(ms*1000); else sched_yield(); }
static inline BOOL QueryPerformanceCounter(LARGE_INTEGER* c) {
    timespec t; clock_gettime(CLOCK_MONOTONIC,&t); c->QuadPart=LONGLONG(t.tv_sec)*1000000000LL+t.tv_nsec; return TRUE;
}
static inline BOOL QueryPerformanceFrequency(LARGE_INTEGER* f) { f->QuadPart=1000000000LL; return TRUE; }
static inline LONG InterlockedIncrement(volatile LONG* p) { return __sync_add_and_fetch(p,1); }
static inline LONG InterlockedDecrement(volatile LONG* p) { return __sync_sub_and_fetch(p,1); }
static inline LONG InterlockedExchange(volatile LONG* p,LONG v) { return __sync_lock_test_and_set(p,v); }
typedef pthread_mutex_t CRITICAL_SECTION;
static inline void InitializeCriticalSection(CRITICAL_SECTION* c) { pthread_mutex_init(c,NULL); }
static inline void DeleteCriticalSection(CRITICAL_SECTION* c) { pthread_mutex_destroy(c); }
static inline void EnterCriticalSection(CRITICAL_SECTION* c) { pthread_mutex_lock(c); }
static inline void LeaveCriticalSection(CRITICAL_SECTION* c) { pthread_mutex_unlock(c); }

// MSVC string functions. Unlike MSVC, the checked copies abort the test
// program when the destination is too small, which is what the Xbox CRT's
// invalid parameter handler does to the app.
static inline void nb_invalid(const char* function) { fprintf(stderr,"INVALID PARAMETER in %s\n",function); abort(); }
static inline int _vsnprintf_s(char* out,size_t capacity,size_t,const char* format,va_list a) {
    // MSVC's %I64u is %llu here.
    char converted[256]; size_t used=0;
    for(const char* f=format;*f && used+4<sizeof(converted);++f) {
        if(f[0]=='I' && f[1]=='6' && f[2]=='4') { converted[used++]='l'; converted[used++]='l'; f+=2; }
        else converted[used++]=*f;
    }
    converted[used]=0;
    if(!capacity) return -1;
    int n=vsnprintf(out,capacity,converted,a);
    return n<0 || size_t(n)>=capacity ? -1 : n;
}
static inline int _snprintf_s(char* out,size_t capacity,size_t count,const char* format,...) {
    va_list a; va_start(a,format); int n=_vsnprintf_s(out,capacity,count,format,a); va_end(a); return n;
}
static inline int strcpy_s(char* out,size_t capacity,const char* text) {
    if(strlen(text)>=capacity) nb_invalid("strcpy_s");
    memcpy(out,text,strlen(text)+1); return 0;
}
template<size_t N> static inline int strcpy_s(char (&out)[N],const char* text) { return strcpy_s(out,N,text); }
static inline int strcat_s(char* out,size_t capacity,const char* text) {
    if(strlen(out)+strlen(text)>=capacity) nb_invalid("strcat_s");
    strcat(out,text); return 0;
}
static inline int strncpy_s(char* out,size_t capacity,const char* text,size_t count) {
    size_t n=strlen(text); if(count!=_TRUNCATE && n>count) n=count;
    if(n>=capacity) { if(count!=_TRUNCATE) nb_invalid("strncpy_s"); n=capacity-1; }
    memcpy(out,text,n); out[n]=0; return 0;
}
template<size_t N> static inline int strncpy_s(char (&out)[N],const char* text,size_t count) { return strncpy_s(out,N,text,count); }
static inline int strncat_s(char* out,size_t capacity,const char* text,size_t count) {
    size_t used=strlen(out); return strncpy_s(out+used,capacity-used,text,count);
}
template<size_t N> static inline int strncat_s(char (&out)[N],const char* text,size_t count) { return strncat_s(out,N,text,count); }
static inline int fopen_s(FILE** file,const char* path,const char* mode) { *file=fopen(path,mode); return *file ? 0 : errno; }
#define _stricmp strcasecmp
#define _strnicmp strncasecmp

static inline HANDLE nb_new(NbHandle::Kind kind) {
    NbHandle* h=new NbHandle(); h->kind=kind; h->fd=-1;
    pthread_mutex_init(&h->lock,NULL); pthread_cond_init(&h->changed,NULL);
    return h;
}
static inline void nb_path(const char* in,char* out,size_t capacity) {
    size_t i=0; for(;in[i] && i+1<capacity;++i) out[i]=in[i]=='\\' ? '/' : in[i];
    out[i]=0;
}
static inline HANDLE CreateEvent(void*,BOOL manual,BOOL initial,void*) {
    HANDLE h=nb_new(NbHandle::Event); h->manual=manual!=0; h->signaled=initial!=0; return h;
}
static inline BOOL SetEvent(HANDLE h) {
    pthread_mutex_lock(&h->lock); h->signaled=true; pthread_cond_broadcast(&h->changed); pthread_mutex_unlock(&h->lock); return TRUE;
}
static inline void* nb_thread(void* context) {
    HANDLE h=static_cast<HANDLE>(context);
    h->start(h->argument);
    pthread_mutex_lock(&h->lock); h->signaled=true; h->manual=true; pthread_cond_broadcast(&h->changed); pthread_mutex_unlock(&h->lock);
    return NULL;
}
// Threads start at once; CREATE_SUSPENDED is accepted and ResumeThread does nothing.
#define CREATE_SUSPENDED 0x4u
static inline HANDLE CreateThread(void*,size_t,DWORD (*start)(void*),void* argument,DWORD,DWORD*) {
    HANDLE h=nb_new(NbHandle::Thread); h->start=start; h->argument=argument; h->manual=true;
    if(pthread_create(&h->thread,NULL,nb_thread,h)) { delete h; return NULL; }
    return h;
}
static inline DWORD ResumeThread(HANDLE) { return 0; }
static inline DWORD WaitForSingleObject(HANDLE h,DWORD milliseconds) {
    timespec until; clock_gettime(CLOCK_REALTIME,&until);
    until.tv_sec+=milliseconds/1000; until.tv_nsec+=long(milliseconds%1000)*1000000L;
    if(until.tv_nsec>=1000000000L) { until.tv_sec++; until.tv_nsec-=1000000000L; }
    pthread_mutex_lock(&h->lock);
    while(!h->signaled) {
        if(milliseconds==INFINITE) pthread_cond_wait(&h->changed,&h->lock);
        else if(pthread_cond_timedwait(&h->changed,&h->lock,&until)==ETIMEDOUT) break;
    }
    bool signaled=h->signaled;
    if(signaled && !h->manual) h->signaled=false;
    pthread_mutex_unlock(&h->lock);
    return signaled ? WAIT_OBJECT_0 : WAIT_TIMEOUT;
}
static inline BOOL CloseHandle(HANDLE h) {
    if(h->kind==NbHandle::File) close(h->fd);
    if(h->kind==NbHandle::Thread) pthread_join(h->thread,NULL);
    if(h->kind==NbHandle::Find && h->dir) closedir(h->dir);
    pthread_mutex_destroy(&h->lock); pthread_cond_destroy(&h->changed);
    delete h; return TRUE;
}

static inline HANDLE CreateFile(const char* path,DWORD access,DWORD,void*,DWORD disposition,DWORD attributes,void*) {
    char native[600]; nb_path(path,native,sizeof(native));
    int flags=(access&GENERIC_WRITE) ? O_WRONLY : O_RDONLY;
    if(disposition==CREATE_ALWAYS) flags|=O_CREAT|O_TRUNC;
    int fd=open(native,flags,0644); if(fd<0) return INVALID_HANDLE_VALUE;
    struct stat st; if(fstat(fd,&st) || S_ISDIR(st.st_mode)) { close(fd); errno=EISDIR; return INVALID_HANDLE_VALUE; }
    HANDLE h=nb_new(NbHandle::File); h->fd=fd; h->unbuffered=(attributes&FILE_FLAG_NO_BUFFERING)!=0; return h;
}
// Like Windows without the file cache: offset, length and buffer must be
// whole 512-byte sectors.
static inline bool nb_misaligned(HANDLE h,const void* data,DWORD count) {
    if(!h->unbuffered || (!(count%512) && !(size_t(data)%512) && !(lseek(h->fd,0,SEEK_CUR)%512))) return false;
    errno=EINVAL; return true;
}
static inline BOOL ReadFile(HANDLE h,void* data,DWORD count,DWORD* got,void*) {
    if(nb_misaligned(h,data,count)) { *got=0; return FALSE; }
    DWORD total=0;
    while(total<count) {
        ssize_t n=read(h->fd,static_cast<char*>(data)+total,count-total);
        if(n<0) { if(errno==EINTR) continue; *got=total; return FALSE; }
        if(n==0) break;
        total+=DWORD(n);
    }
    *got=total; return TRUE;
}
static inline BOOL WriteFile(HANDLE h,const void* data,DWORD count,DWORD* done,void*) {
    if(nb_misaligned(h,data,count)) { *done=0; return FALSE; }
    DWORD total=0;
    while(total<count) {
        ssize_t n=write(h->fd,static_cast<const char*>(data)+total,count-total);
        if(n<0) { if(errno==EINTR) continue; *done=total; return FALSE; }
        total+=DWORD(n);
    }
    *done=total; return TRUE;
}
static inline BOOL FlushFileBuffers(HANDLE h) { return fsync(h->fd)==0; }
static inline DWORD SetFilePointer(HANDLE h,LONG distance,LONG* high,DWORD method) {
    off_t at=lseek(h->fd,off_t(distance)+(high ? off_t(*high)*4294967296LL : 0),method==FILE_END ? SEEK_END : method==1 ? SEEK_CUR : SEEK_SET);
    if(at<0) return INVALID_SET_FILE_POINTER;
    errno=0; if(high) *high=LONG(at>>32);
    return DWORD(at);
}
static inline void* VirtualAlloc(void*,size_t size,DWORD,DWORD) { void* p=NULL; return posix_memalign(&p,4096,size) ? NULL : p; }
static inline BOOL VirtualFree(void* p,size_t,DWORD) { free(p); return TRUE; }
static inline DWORD GetFileAttributes(const char* path) {
    char native[600]; nb_path(path,native,sizeof(native));
    struct stat st; if(stat(native,&st)) return DWORD(-1);
    return S_ISDIR(st.st_mode) ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
}
static inline BOOL DeleteFile(const char* path) { char n[600]; nb_path(path,n,sizeof(n)); return unlink(n)==0; }
static inline BOOL RemoveDirectory(const char* path) { char n[600]; nb_path(path,n,sizeof(n)); return rmdir(n)==0; }
static inline BOOL CreateDirectory(const char* path,void*) { char n[600]; nb_path(path,n,sizeof(n)); return mkdir(n,0755)==0; }
// Like Windows, moving onto an existing name fails.
static inline BOOL MoveFile(const char* from,const char* to) {
    char a[600],b[600]; nb_path(from,a,sizeof(a)); nb_path(to,b,sizeof(b));
    struct stat st; if(!lstat(b,&st)) { errno=EEXIST; return FALSE; }
    return rename(a,b)==0;
}
static inline void nb_filetime(time_t when,FILETIME* out) {
    unsigned long long ticks=(unsigned long long)(when+11644473600LL)*10000000ULL;
    out->dwLowDateTime=DWORD(ticks); out->dwHighDateTime=DWORD(ticks>>32);
}
static inline BOOL FileTimeToSystemTime(const FILETIME* in,SYSTEMTIME* out) {
    unsigned long long ticks=((unsigned long long)in->dwHighDateTime<<32)|in->dwLowDateTime;
    time_t when=time_t(ticks/10000000ULL)-11644473600LL; struct tm t; gmtime_r(&when,&t);
    memset(out,0,sizeof(*out)); out->wYear=WORD(t.tm_year+1900); out->wMonth=WORD(t.tm_mon+1); out->wDay=WORD(t.tm_mday);
    out->wHour=WORD(t.tm_hour); out->wMinute=WORD(t.tm_min); out->wSecond=WORD(t.tm_sec); return TRUE;
}
static inline bool nb_describe(const char* path,const char* name,WIN32_FIND_DATA* entry) {
    struct stat st; if(stat(path,&st)) return false;
    memset(entry,0,sizeof(*entry));
    entry->dwFileAttributes=S_ISDIR(st.st_mode) ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
    entry->nFileSizeHigh=DWORD((unsigned long long)st.st_size>>32); entry->nFileSizeLow=DWORD(st.st_size);
    nb_filetime(st.st_mtime,&entry->ftLastWriteTime);
    snprintf(entry->cFileName,sizeof(entry->cFileName),"%s",name);
    return true;
}
static inline BOOL FindNextFile(HANDLE h,WIN32_FIND_DATA* entry) {
    if(h->single) return FALSE;
    while(dirent* d=readdir(h->dir)) {
        char full[900]; snprintf(full,sizeof(full),"%s/%s",h->folder,d->d_name);
        if(nb_describe(full,d->d_name,entry)) return TRUE;
    }
    return FALSE;
}
// Supports the two forms the server uses: "folder\*" and an exact path.
static inline HANDLE FindFirstFile(const char* pattern,WIN32_FIND_DATA* entry) {
    char native[600]; nb_path(pattern,native,sizeof(native));
    size_t length=strlen(native);
    HANDLE h=nb_new(NbHandle::Find);
    if(length>=2 && !strcmp(native+length-2,"/*")) {
        native[length-2]=0; snprintf(h->folder,sizeof(h->folder),"%s",native);
        h->dir=opendir(native);
        if(h->dir && FindNextFile(h,entry)) return h;
    } else {
        const char* slash=strrchr(native,'/'); h->single=true;
        if(nb_describe(native,slash ? slash+1 : native,entry)) return h;
    }
    CloseHandle(h); errno=ENOENT; return INVALID_HANDLE_VALUE;
}
static inline BOOL FindClose(HANDLE h) { return CloseHandle(h); }
#endif
