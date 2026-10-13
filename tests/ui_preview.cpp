// SPDX-License-Identifier: MIT
// Rasterize the exact Xbox layout for visual review without the Xbox graphics API.
#include <windows.h>
#include <stdio.h>
#include "../shared/ui.h"
static unsigned int pixels[1280*720];
static void Rectangle(void*,int x,int y,int w,int h,unsigned int color) {
    for(int row=y;row<y+h;++row) for(int col=x;col<x+w;++col)
        if(row>=0 && row<720 && col>=0 && col<1280) pixels[row*1280+col]=color;
}
int main(int argc,char** argv) {
    nb::UiState state;
    strcpy_s(state.ip,"192.168.1.50"); state.armed=1;
    strcpy_s(state.settings,"XBOX BUFFERS: STARTUP 64 KIB / WORKER CPU: AUTO");
    if(argc>2) { state.phase=5; state.direction=1; state.rate=27.48; state.upload=27.34; state.download=24.16; state.elapsed=12.4; state.mebibytes=340.752; state.streams=2; }
    else state.phase=1;
    if(argc>3) { state.armed=0; state.phase=0; state.diskPhase=1; state.diskTotal=256; state.mebibytes=96; state.elapsed=3.1; state.rate=30.97;
        strcpy_s(state.settings,"XBOX BUFFERS: STARTUP 64 KIB / WORKER CPU: AUTO / DISK: HDD 1024 KIB");
        strcpy_s(state.disk,"DISK USB 0 64 KIB: WRITE 8.52 / READ 24.10 MIB/S / ALL DATA CHECKED"); }
    nb::DrawUi(state,Rectangle,NULL);
    BITMAPFILEHEADER file={0}; BITMAPINFOHEADER info={0};
    file.bfType=0x4d42; file.bfOffBits=sizeof(file)+sizeof(info); file.bfSize=file.bfOffBits+sizeof(pixels);
    info.biSize=sizeof(info); info.biWidth=1280; info.biHeight=-720; info.biPlanes=1; info.biBitCount=32; info.biCompression=BI_RGB;
    FILE* output=NULL; if(fopen_s(&output,argc>1?argv[1]:"preview.bmp","wb")) return 1;
    fwrite(&file,sizeof(file),1,output); fwrite(&info,sizeof(info),1,output); fwrite(pixels,sizeof(pixels),1,output); fclose(output);
    return 0;
}
