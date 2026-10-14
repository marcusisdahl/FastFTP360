// SPDX-License-Identifier: MIT
// Runs the SAME read benchmark engine as the Xbox app on a PC, for tests.
// Usage:
//   read-host file <path> <workload> <seconds> <seed> <verify 0|1> [cancel-after-s]
//   read-host netiso <ip> <port> <iso|-> <workload> <seconds> <seed> <verify 0|1> <timeout-s> [cancel-after-s] [repeat]
//   read-host list <ip> <port>
//   read-host fixture <path> <bytes> <seed>      writes a known-data fixture
//   read-host settings <file>                    parses and re-formats FastFTP.ini read settings
//   read-host percentiles <us> <us> ...          histogram self-check
// Prints one RESULT line per run; tests/read.py parses them.
#define _CRT_SECURE_NO_WARNINGS
#include "../shared/readbench.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <dirent.h>
#endif

static double CancelAt=0;
static bool Cancelled(void*) { return CancelAt>0 && nb::Now()>=CancelAt; }

static int OpenDescriptors() {
#ifndef _WIN32
    int count=0; DIR* d=opendir("/proc/self/fd");
    if(!d) return -1;
    while(readdir(d)) ++count;
    closedir(d); return count;
#else
    return -1;
#endif
}
static void Print(const nb::ReadResult& r) {
    printf("RESULT status=%s verify=%d bytes=" NB_U64 " ops=" NB_U64 " size=" NB_U64 " seconds=%.3f rate=%.3f p50=%.3f p99=%.3f max=%.3f "
           "failed=%u short=%u timeouts=%u integrity=%u wraps=" NB_U64 " error=%s\n",
        nb::ReadStatusName(r.status),r.verify,r.bytes,r.operations,r.fileSize,r.seconds,r.Rate(),r.latencyP50,r.latencyP99,r.latencyMax,
        r.failed,r.shortReads,r.timeouts,r.integrity,r.wraps,r.error[0] ? r.error : "-");
    fflush(stdout);
}
static unsigned char* Buffer() {
    static unsigned char* buffer=NULL;
    if(!buffer) buffer=static_cast<unsigned char*>(malloc(nb::ReadMaxRequest));
    return buffer;
}
static nb::ReadConfig Config(const char* workload,const char* seconds,const char* seed,const char* verify) {
    nb::ReadConfig c; c.workload=atoi(workload); c.seconds=DWORD(atoi(seconds)); c.seed=DWORD(strtoul(seed,NULL,0));
    c.verify=atoi(verify)!=0; c.warmup=1;
    return c;
}

int main(int argc,char** argv) {
    if(argc<2) return 1;
#ifdef _WIN32
    WSADATA wsa; if(WSAStartup(MAKEWORD(2,2),&wsa)) return 1;
#endif
    nb::ReadObserver observer={NULL,Cancelled,NULL};
    const char* mode=argv[1];
    if(!strcmp(mode,"fixture") && argc==5) {
        FILE* f=fopen(argv[2],"wb"); if(!f) return 2;
        nb::U64 size=strtoull(argv[3],NULL,0); DWORD seed=DWORD(strtoul(argv[4],NULL,0));
        for(nb::U64 at=0;at<size;) {
            DWORD n=size-at<nb::U64(nb::ReadMaxRequest) ? DWORD(size-at) : DWORD(nb::ReadMaxRequest);
            nb::FixtureFill(Buffer(),seed,at,n); fwrite(Buffer(),1,n,f); at+=n;
        }
        fclose(f); printf("FIXTURE " NB_U64 "\n",size); return 0;
    }
    if(!strcmp(mode,"settings") && argc==3) {
        char text[2048]={0}; FILE* f=fopen(argv[2],"rb");
        if(f) { text[fread(text,1,sizeof(text)-1,f)]=0; fclose(f); }
        nb::ReadSettings s; nb::ParseReadSettings(text,s);
        char out[512]; nb::FormatReadSettings(out,sizeof(out),s);
        nb::ReadSettings again; strcpy_s(again.folder,"x"); again.seed=0; nb::ParseReadSettings(out,again);
        bool same=!strcmp(again.folder,s.folder) && !strcmp(again.connectx,s.connectx) && again.server==s.server && again.port==s.port && again.seed==s.seed;
        char server[32]="-"; if(s.server) nb::FormatServer(server,sizeof(server),s.server,s.port);
        printf("SETTINGS folder=%s connectx=%s server=%s port=%d seed=%u %s\n",s.folder,s.connectx[0] ? s.connectx : "-",server,s.port,s.seed,same ? "roundtrip" : "mismatch");
        return 0;
    }
    if(!strcmp(mode,"percentiles")) {
        nb::LatencyHistogram h;
        for(int i=2;i<argc;++i) h.Add(strtoull(argv[i],NULL,10));
        printf("PERCENTILES count=" NB_U64 " mean=%.3f p50=" NB_U64 " p95=" NB_U64 " p99=" NB_U64 " max=" NB_U64 "\n",
            h.Count(),h.Mean(),h.Percentile(0.5),h.Percentile(0.95),h.Percentile(0.99),h.Maximum());
        return 0;
    }
    if(!strcmp(mode,"file") && argc>=7) {
        nb::ReadConfig c=Config(argv[3],argv[4],argv[5],argv[6]);
        const char* name=strrchr(argv[2],'/'); name=name ? name+1 : argv[2];
        c.fixture=nb::FixtureSeed(name,c.fixtureSeed);
        if(argc>7) CancelAt=nb::Now()+atof(argv[7]);
        nb::FileReadBackend file; nb::ReadResult r;
        if(!file.Open(argv[2],false,nb::WorkloadRandom(c.workload))) { strcpy_s(r.error,file.Error()); r.status=nb::ReadFailed; Print(r); return 0; }
        nb::RunReadBenchmark(file,c,observer,Buffer(),r);
        Print(r); return 0;
    }
    if((!strcmp(mode,"netiso") && argc>=10) || (!strcmp(mode,"list") && argc==4)) {
        DWORD address=0; int port=atoi(argv[3]);
        if(!nb::ParseServer(argv[2],address,port)) { printf("RESULT status=failed error=bad-address\n"); return 0; }
        const bool list=!strcmp(mode,"list");
        const int repeat=!list && argc>11 ? atoi(argv[11]) : 1;
        const int before=OpenDescriptors();
        static char names[nb::NetIsoMaxIsos][nb::ReadNameMax];
        for(int run=0;run<repeat;++run) {
            nb::NetIsoClient client(observer,list ? 5 : atof(argv[9]));
            nb::ReadResult r; int count=0;
            if(!list && argc>10 && atof(argv[10])>0) CancelAt=nb::Now()+atof(argv[10]);
            if(!client.Connect(address,port,5) || !client.PingServer() || !client.ListIsos(names,nb::NetIsoMaxIsos,count)) {
                strcpy_s(r.error,client.Error()); r.status=nb::ReadFailed; Print(r); continue;
            }
            if(list) { for(int i=0;i<count;++i) printf("ISO %s\n",names[i]); printf("LISTED %d\n",count); return 0; }
            const char* iso=strcmp(argv[4],"-") ? argv[4] : (count ? names[0] : "");
            nb::ReadConfig c=Config(argv[5],argv[6],argv[7],argv[8]);
            c.fixture=nb::FixtureSeed(iso,c.fixtureSeed);
            if(!client.Mount(iso,names,count)) { strcpy_s(r.error,client.Error()); r.status=nb::ReadFailed; Print(r); continue; }
            nb::RunReadBenchmark(client,c,observer,Buffer(),r);
            Print(r);
        }
        printf("DESCRIPTORS before=%d after=%d\n",before,OpenDescriptors());
        return 0;
    }
    return 1;
}
