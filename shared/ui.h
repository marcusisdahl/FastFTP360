// SPDX-License-Identifier: MIT
#pragma once
namespace nb {
struct UiState {
    char ip[48],message[100],settings[96],disk[100];
    int phase,direction,duration,armed,completed,streams,rateLimit,options,downloadLimit,diskPhase;
    double elapsed,rate,upload,download,mebibytes,diskTotal;
    UiState();
};
typedef void (*Rectangle)(void*,int,int,int,int,unsigned int);
void DrawUi(const UiState& state,Rectangle rectangle,void* context);
}
