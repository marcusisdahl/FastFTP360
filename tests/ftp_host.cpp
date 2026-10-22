// SPDX-License-Identifier: MIT
// Runs the SAME FTP server source on Windows loopback for protocol tests.
// Usage: ftp-host [--speed <MiB/s>] <port> <folder served as /HDD> <sessions> [user password]
//        ftp-host --login-file <path>   prints the login the Xbox would read from it
#include "../shared/ftp.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static double Started;
static bool Cancelled(void*) { return nb::Now()-Started>120; }
static void Progress(void*,const char* operation,const char* file,nb::U64 bytes,double seconds,bool done,bool ok) {
    if(done) { printf("%s %s %s bytes=%llu seconds=%.3f\n",ok ? "DONE" : "FAILED",operation,file,bytes,seconds); fflush(stdout); }
}
static void Log(void*,const char* line) { printf("LOG %s\n",line); fflush(stdout); }
static void Starting(void*,const char* operation,const char* file) { printf("START %s %s\n",operation,file); fflush(stdout); }
int main(int argc,char** argv) {
    if(argc==3 && !strcmp(argv[1],"--login-file")) {
        char text[512]={0}; FILE* file=NULL;
        if(fopen_s(&file,argv[2],"rb")==0) { text[fread(text,1,sizeof(text)-1,file)]=0; fclose(file); }
        bool required=true; char user[nb::FtpLoginMax+1]="xboxftp",password[nb::FtpLoginMax+1]="xboxftp";
        nb::ParseFtpLogin(text,required,user,password);
        char saved[160]; nb::FormatFtpLogin(saved,sizeof(saved),required,user,password);
        bool same=true; char user2[nb::FtpLoginMax+1]="x",password2[nb::FtpLoginMax+1]="x"; bool required2=!required;
        nb::ParseFtpLogin(saved,required2,user2,password2);
        same=required2==required && !strcmp(user2,user) && !strcmp(password2,password);
        printf("LOGIN %s %s %s %s\n",required ? "on" : "off",user,password,same ? "roundtrip" : "mismatch");
        return 0;
    }
    int speed=0;
    if(argc>=5 && !strcmp(argv[1],"--speed")) { speed=atoi(argv[2]); argv+=2; argc-=2; }
    if(argc<4) return 1;
    WSADATA wsa; if(WSAStartup(MAKEWORD(2,2),&wsa)) return 1;
    SOCKET listener=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);
    char error[120]={0};
    if(!nb::PrepareListener(listener,error,sizeof(error))) { printf("ERROR %s\n",error); return 2; }
    sockaddr_in address={0}; address.sin_family=AF_INET; address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
    address.sin_port=htons(u_short(atoi(argv[1])));
    if(bind(listener,reinterpret_cast<sockaddr*>(&address),sizeof(address)) || listen(listener,8)) return 2;
    const nb::FtpRoot roots[]={{"HDD",argv[2]},{"NULL",NULL}};
    nb::FtpObserver observer={NULL,Cancelled,Progress,Log,Starting};
    printf("READY %s\n",argv[1]); fflush(stdout);
    Started=nb::Now();
    // No fallback address: passive replies must use the address the client reached.
    bool ok=nb::ServeFtp(listener,roots,2,0,0,speed,-1,atoi(argv[3]),argc>5 ? argv[4] : NULL,argc>5 ? argv[5] : NULL,observer,error,sizeof(error));
    printf(ok ? "STOPPED\n" : "ERROR %s\n",error);
    closesocket(listener); WSACleanup(); return ok ? 0 : 3;
}
