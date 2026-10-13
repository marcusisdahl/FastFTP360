// SPDX-License-Identifier: MIT
// Runs the SAME session engine on Windows loopback for protocol verification.
#include "../shared/bench.h"
#include <stdio.h>
#include <stdlib.h>
static double Started;
static bool Cancelled(void*) { return nb::Now()-Started>180; }
// Each connection is served on its own thread, like the Xbox app's streams.
static DWORD WINAPI ServeClient(void* context) {
    SOCKET client=(SOCKET)(UINT_PTR)context;
    nb::Observer observer={NULL,Cancelled,NULL,NULL}; nb::Result result; char error[120]={0};
    bool ok=nb::Serve(client,observer,result,error,sizeof(error)); closesocket(client);
    if(ok) printf("PASS direction=%lu bytes=%llu seconds=%.6f MiB/s=%.2f\n",result.direction,result.bytes,double(result.microseconds)/1000000,result.Rate());
    else printf("ERROR %s\n",error);
    fflush(stdout);
    return 0;
}
int main(int argc,char** argv) {
    WSADATA wsa; if(WSAStartup(MAKEWORD(2,2),&wsa)) return 1;
    int port=argc>1?atoi(argv[1]):36020, count=argc>2?atoi(argv[2]):1;
    SOCKET listener=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);
    // Model the Xbox app's 64 KiB startup defaults before bind/accept.
    int buffer=nb::Chunk;
    if(setsockopt(listener,SOL_SOCKET,SO_SNDBUF,reinterpret_cast<const char*>(&buffer),sizeof(buffer)) ||
       setsockopt(listener,SOL_SOCKET,SO_RCVBUF,reinterpret_cast<const char*>(&buffer),sizeof(buffer))) return 2;
    char setupError[120]={0};
    if(!nb::PrepareListener(listener,setupError,sizeof(setupError))) { printf("ERROR %s\n",setupError); return 2; }
    sockaddr_in address={0}; address.sin_family=AF_INET; address.sin_addr.s_addr=htonl(INADDR_LOOPBACK); address.sin_port=htons((u_short)port);
    if(bind(listener,reinterpret_cast<sockaddr*>(&address),sizeof(address)) || listen(listener,nb::MaxStreams)) return 2;
    printf("READY %d\n",port); fflush(stdout);
    Started=nb::Now();
    HANDLE threads[64]; int started=0;
    for(int i=0;i<count && i<64;++i) {
        SOCKET client=INVALID_SOCKET;
        double deadline=nb::Now()+30;
        while(client==INVALID_SOCKET && nb::Now()<deadline) {
            client=accept(listener,NULL,NULL);
            if(client==INVALID_SOCKET && WSAGetLastError()!=WSAEWOULDBLOCK) return 3;
            if(client==INVALID_SOCKET) Sleep(10);
        }
        if(client==INVALID_SOCKET) return 3;
        threads[started]=CreateThread(NULL,0,ServeClient,(void*)(UINT_PTR)client,0,NULL);
        if(!threads[started]) return 4;
        ++started;
    }
    for(int i=0;i<started;++i) { WaitForSingleObject(threads[i],INFINITE); CloseHandle(threads[i]); }
    closesocket(listener); WSACleanup(); return 0;
}
