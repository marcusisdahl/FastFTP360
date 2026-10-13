// SPDX-License-Identifier: MIT
#include "lan_socket_shim.h"
#undef setsockopt
#undef send
// Model console-reported failures: LAN/buffer changes work before connection
// and fail on accepted sockets. Other socket options still use real Winsock.
int WSAAPI LanTestSetSockOpt(SOCKET s,int level,int option,const char* value,int size) {
    if(level==SOL_SOCKET && (option==0x5801 || option==0x5802 || option==SO_SNDBUF || option==SO_RCVBUF)) {
        sockaddr_in peer={0}; int length=sizeof(peer);
        if(getpeername(s,reinterpret_cast<sockaddr*>(&peer),&length)==0) {
            WSASetLastError(WSAEINVAL); return SOCKET_ERROR;
        }
        if(option==0x5801 || option==0x5802) return 0;
    }
    return setsockopt(s,level,option,value,size);
}
int WSAAPI LanTestSend(SOCKET s,const char* buffer,int count,int flags) {
    static unsigned calls=0;
    // Select can say writable immediately before send reports would-block.
    // Force that race regularly, exercising retry/yield and cancellation paths.
    if(++calls%7==0) { WSASetLastError(WSAEWOULDBLOCK); return SOCKET_ERROR; }
    return send(s,buffer,count,flags);
}
