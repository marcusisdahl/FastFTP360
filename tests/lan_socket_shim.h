// SPDX-License-Identifier: MIT
#pragma once
#include "../shared/bench.h"
int WSAAPI LanTestSetSockOpt(SOCKET s,int level,int option,const char* value,int size);
#define setsockopt LanTestSetSockOpt
