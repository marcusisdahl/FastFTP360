// SPDX-License-Identifier: MIT
// Rasterize the exact Xbox screen for visual review without the Xbox graphics API.
// Draws Dear ImGui's triangles in software and writes a 1280x720 BMP.
// Usage: ui-preview out.bmp [idle|run|disk|ftp|ftpon|error|net|diskidle|settings]
#define _CRT_SECURE_NO_WARNINGS
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "../shared/ui.h"
#include "../third_party/imgui/imgui.h"
static unsigned int pixels[1280*720];
static const unsigned char* Texture; static int TextureWidth,TextureHeight;

static float Sample(float u,float v) {
    float x=u*TextureWidth-0.5f,y=v*TextureHeight-0.5f;
    int x0=int(x<0 ? x-1 : x),y0=int(y<0 ? y-1 : y); float fx=x-x0,fy=y-y0,sum=0;
    for(int dy=0;dy<2;++dy) for(int dx=0;dx<2;++dx) {
        int tx=x0+dx,ty=y0+dy; if(tx<0) tx=0; if(ty<0) ty=0;
        if(tx>=TextureWidth) tx=TextureWidth-1; if(ty>=TextureHeight) ty=TextureHeight-1;
        sum+=Texture[ty*TextureWidth+tx]/255.0f*(dx ? fx : 1-fx)*(dy ? fy : 1-fy);
    }
    return sum;
}
static float Channel(ImU32 c,int shift) { return float((c>>shift)&255); }
// Same pipeline as the Xbox renderer: vertex colour times texture alpha,
// blended over the target, clipped to the command's rectangle.
static void Triangle(const ImDrawVert* v[3],const ImVec4& clip) {
    float minX=v[0]->pos.x,maxX=minX,minY=v[0]->pos.y,maxY=minY;
    for(int i=1;i<3;++i) {
        if(v[i]->pos.x<minX) minX=v[i]->pos.x; if(v[i]->pos.x>maxX) maxX=v[i]->pos.x;
        if(v[i]->pos.y<minY) minY=v[i]->pos.y; if(v[i]->pos.y>maxY) maxY=v[i]->pos.y;
    }
    if(minX<clip.x) minX=clip.x; if(minY<clip.y) minY=clip.y; if(maxX>clip.z) maxX=clip.z; if(maxY>clip.w) maxY=clip.w;
    float area=(v[1]->pos.x-v[0]->pos.x)*(v[2]->pos.y-v[0]->pos.y)-(v[1]->pos.y-v[0]->pos.y)*(v[2]->pos.x-v[0]->pos.x);
    if(area==0) return;
    for(int y=int(minY);y<int(maxY+1) && y<720;++y) for(int x=int(minX);x<int(maxX+1) && x<1280;++x) {
        if(x<0 || y<0 || x<clip.x || y<clip.y || x>=clip.z || y>=clip.w) continue;
        float px=x+0.5f,py=y+0.5f,w[3];
        for(int i=0;i<3;++i) {
            const ImDrawVert* a=v[(i+1)%3]; const ImDrawVert* b=v[(i+2)%3];
            w[i]=((b->pos.x-a->pos.x)*(py-a->pos.y)-(b->pos.y-a->pos.y)*(px-a->pos.x))/area;
        }
        if(w[0]<0 || w[1]<0 || w[2]<0) continue;
        float u=0,t=0,c[4]={0,0,0,0}; static const int shifts[4]={IM_COL32_R_SHIFT,IM_COL32_G_SHIFT,IM_COL32_B_SHIFT,IM_COL32_A_SHIFT};
        for(int i=0;i<3;++i) { u+=w[i]*v[i]->uv.x; t+=w[i]*v[i]->uv.y; for(int k=0;k<4;++k) c[k]+=w[i]*Channel(v[i]->col,shifts[k]); }
        float alpha=c[3]/255.0f*Sample(u,t);
        unsigned int& p=pixels[y*1280+x]; float d[3]={float((p>>16)&255),float((p>>8)&255),float(p&255)};
        unsigned int out=0xFF000000;
        for(int k=0;k<3;++k) out|=unsigned(c[k]*alpha+d[k]*(1-alpha)+0.5f)<<(16-8*k);
        p=out;
    }
}
static void Rasterize(const ImDrawData* data) {
    for(int n=0;n<data->CmdListsCount;++n) {
        const ImDrawList* list=data->CmdLists[n];
        for(int c=0;c<list->CmdBuffer.Size;++c) {
            const ImDrawCmd& cmd=list->CmdBuffer[c];
            for(unsigned int i=0;i<cmd.ElemCount;i+=3) {
                const ImDrawVert* v[3];
                for(int k=0;k<3;++k) v[k]=&list->VtxBuffer[cmd.VtxOffset+list->IdxBuffer[cmd.IdxOffset+i+k]];
                Triangle(v,cmd.ClipRect);
            }
        }
    }
}
static void Put16(unsigned char* p,unsigned int v) { p[0]=v&255; p[1]=(v>>8)&255; }
static void Put32(unsigned char* p,unsigned int v) { Put16(p,v); Put16(p+2,v>>16); }
int main(int argc,char** argv) {
    nb::InitUi();
    unsigned char* alpha=NULL; ImGui::GetIO().Fonts->GetTexDataAsAlpha8(&alpha,&TextureWidth,&TextureHeight); Texture=alpha;
    nb::UiState state; const char* scene=argc>2 ? argv[2] : "idle";
    strcpy_s(state.ip,"192.168.1.50");
    strcpy_s(state.buffers,"Startup default (64 KiB)"); strcpy_s(state.cpu,"Automatic");
    strcpy_s(state.diskTarget,"HDD"); strcpy_s(state.diskBlock,"1 MiB"); strcpy_s(state.drawing,"Paused"); strcpy_s(state.ftpUser,"xboxftp"); strcpy_s(state.ftpPassword,"xboxftp");
    state.upload=27.34; state.download=24.16;
    if(!strcmp(scene,"run")) {
        state.tab=nb::TabNetwork; state.armed=1; state.busy=1; state.phase=5; state.direction=1; state.rate=27.48; state.elapsed=6.4; state.mebibytes=175.9; state.streams=2;
    } else if(!strcmp(scene,"disk")) {
        state.tab=nb::TabDisk; state.busy=1; state.diskPhase=1; state.diskTotal=256; state.mebibytes=96; state.elapsed=3.1; state.rate=30.97;
        strcpy_s(state.disk,"USB 0, 64 KiB blocks: write 8.52, read 24.10 MiB/s, all data checked");
    } else if(!strcmp(scene,"ftp")) {
        state.tab=nb::TabFtp; state.armed=1; state.busy=1; state.ftpPhase=2; state.mebibytes=812.5; state.elapsed=31.2; state.rate=26.04;
        strcpy_s(state.message,"Folders: Game Hdd1 HddX SysExt System Usb0 USBMU0 USBMUCache0");
        strcpy_s(state.ftp,"Upload, 1024.0 MiB in 41.3 s: 24.79 MiB/s");
        static const nb::UiTransfer recent[]={{"Halo 3 - Disc 1 (Title Update 12, all DLC included).iso",7120.4,27.48,1,1},
            {"default.xex",14.2,21.37,0,1},{"FastFTP.log",0.006,0.9,0,1},{"backup.bin",388.0,0,1,0},{"Content.zip",1024.0,26.91,1,1}};
        memcpy(state.recent,recent,sizeof(recent)); state.recentCount=5;
    } else if(!strcmp(scene,"ftpon")) {
        state.tab=nb::TabFtp; state.armed=1; state.busy=1; state.ftpPhase=1; state.focus=0;
        strcpy_s(state.message,"Folders: Game Hdd1 HddX SysExt System Usb0 Usb1 USBMU0 USBMU1 USBMUCache0 USBMUCache1");
        static const nb::UiTransfer recent[]={{"default.xex",14.2,21.37,0,1},{"Content.zip",1024.0,26.91,1,1}};
        memcpy(state.recent,recent,sizeof(recent)); state.recentCount=2;
    } else if(!strcmp(scene,"error")) {
        state.tab=nb::TabNetwork; state.phase=7; state.focus=1;
        strcpy_s(state.message,"Receive failed during warm-up (10054). Check the cable and press A to listen again.");
    } else if(!strcmp(scene,"net")) {
        state.tab=nb::TabNetwork;
    } else if(!strcmp(scene,"diskidle")) {
        state.tab=nb::TabDisk; state.focus=0;
        strcpy_s(state.disk,"HDD, 1024 KiB blocks: write 57.52, read 57.21 MiB/s, all data checked");
    } else if(!strcmp(scene,"settings")) {
        state.tab=nb::TabSettings; state.focus=2;
    } else { state.phase=0; state.download=0; }
    ImGui::GetIO().DeltaTime=1.0f/60;
    nb::DrawUi(state);
    Rasterize(ImGui::GetDrawData());
    unsigned char header[54]={'B','M'};
    Put32(header+2,54+sizeof(pixels)); Put32(header+10,54); Put32(header+14,40); Put32(header+18,1280); Put32(header+22,unsigned(-720));
    Put16(header+26,1); Put16(header+28,32); Put32(header+34,sizeof(pixels));
    FILE* output=fopen(argc>1 ? argv[1] : "preview.bmp","wb"); if(!output) return 1;
    fwrite(header,sizeof(header),1,output);
    for(int i=0;i<1280*720;++i) { unsigned char p[4]; Put32(p,pixels[i]); fwrite(p,4,1,output); }
    fclose(output);
    ImGui::DestroyContext();
    return 0;
}
