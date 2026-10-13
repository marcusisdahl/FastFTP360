// SPDX-License-Identifier: MIT
// Runs the SAME FTP server source on Windows loopback for protocol tests.
// Usage: ftp-host <port> <folder served as /HDD> <sessions> [user password]
#include "../shared/ftp.h"
#include <stdio.h>
#include <stdlib.h>
static double Started;
static bool Cancelled(void*) { return nb::Now()-Started>120; }
static void Progress(void*,const char* operation,const char* file,nb::U64 bytes,double seconds,bool done,bool ok) {
    if(done) { printf("%s %s %s bytes=%I64u seconds=%.3f\n",ok ? "DONE" : "FAILED",operation,file,bytes,seconds); fflush(stdout); }
}
int main(int argc,char** argv) {
    if(argc<4) return 1;
    WSADATA wsa; if(WSAStartup(MAKEWORD(2,2),&wsa)) return 1;
    SOCKET listener=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);
    char error[120]={0};
    if(!nb::PrepareListener(listener,error,sizeof(error))) { printf("ERROR %s\n",error); return 2; }
    sockaddr_in address={0}; address.sin_family=AF_INET; address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
    address.sin_port=htons(u_short(atoi(argv[1])));
    if(bind(listener,reinterpret_cast<sockaddr*>(&address),sizeof(address)) || listen(listener,8)) return 2;
    const nb::FtpRoot roots[]={{"HDD",argv[2]},{"NULL",NULL}};
    nb::FtpObserver observer={NULL,Cancelled,Progress};
    printf("READY %s\n",argv[1]); fflush(stdout);
    Started=nb::Now();
    bool ok=nb::ServeFtp(listener,roots,2,htonl(INADDR_LOOPBACK),0,atoi(argv[3]),argc>5 ? argv[4] : NULL,argc>5 ? argv[5] : NULL,observer,error,sizeof(error));
    printf(ok ? "STOPPED\n" : "ERROR %s\n",error);
    closesocket(listener); WSACleanup(); return ok ? 0 : 3;
}
