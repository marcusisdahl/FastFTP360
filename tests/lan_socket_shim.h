// SPDX-License-Identifier: MIT
#pragma once
#include "../shared/bench.h"
int WSAAPI LanTestSetSockOpt(SOCKET s,int level,int option,const char* value,int size);
int WSAAPI LanTestSend(SOCKET s,const char* buffer,int count,int flags);
#define setsockopt LanTestSetSockOpt
#define send LanTestSend
