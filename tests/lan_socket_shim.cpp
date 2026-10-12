// SPDX-License-Identifier: MIT
#include "lan_socket_shim.h"
#undef setsockopt
// Model the console-reported failure: LAN options work before connection and
// fail on accepted sockets. All normal socket options still use real Winsock.
int WSAAPI LanTestSetSockOpt(SOCKET s,int level,int option,const char* value,int size) {
    if(level==SOL_SOCKET && (option==0x5801 || option==0x5802)) {
        sockaddr_in peer={0}; int length=sizeof(peer);
        if(getpeername(s,reinterpret_cast<sockaddr*>(&peer),&length)==0) {
            WSASetLastError(WSAEINVAL); return SOCKET_ERROR;
        }
        return 0;
    }
    return setsockopt(s,level,option,value,size);
}
