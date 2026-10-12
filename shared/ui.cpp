// SPDX-License-Identifier: MIT
#include "ui.h"
#include <stdio.h>
#include <string.h>
#include <ctype.h>
namespace nb {
UiState::UiState() : phase(0),direction(0),duration(20),armed(0),completed(0),elapsed(0),rate(0),upload(0),download(0),mebibytes(0) {
    strcpy_s(ip,"WAITING FOR NETWORK"); message[0]=0;
}
// Original compact bitmap lettering. No external font or SDK media assets.
static const char Alphabet[]=" ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.:/-()%+?";
static const unsigned char Glyphs[][7]={
 {0,0,0,0,0,0,0}, {14,17,17,31,17,17,17}, {30,17,17,30,17,17,30},
 {14,17,16,16,16,17,14}, {30,17,17,17,17,17,30}, {31,16,16,30,16,16,31},
 {31,16,16,30,16,16,16}, {14,17,16,23,17,17,15}, {17,17,17,31,17,17,17},
 {31,4,4,4,4,4,31}, {7,2,2,2,18,18,12}, {17,18,20,24,20,18,17},
 {16,16,16,16,16,16,31}, {17,27,21,21,17,17,17}, {17,25,21,19,17,17,17},
 {14,17,17,17,17,17,14}, {30,17,17,30,16,16,16}, {14,17,17,17,21,18,13},
 {30,17,17,30,20,18,17}, {15,16,16,14,1,1,30}, {31,4,4,4,4,4,4},
 {17,17,17,17,17,17,14}, {17,17,17,17,17,10,4}, {17,17,17,21,21,27,17},
 {17,17,10,4,10,17,17}, {17,17,10,4,4,4,4}, {31,1,2,4,8,16,31},
 {14,17,19,21,25,17,14}, {4,12,4,4,4,4,14}, {14,17,1,2,4,8,31},
 {30,1,1,14,1,1,30}, {2,6,10,18,31,2,2}, {31,16,16,30,1,1,30},
 {14,16,16,30,17,17,14}, {31,1,2,4,8,8,8}, {14,17,17,14,17,17,14},
 {14,17,17,15,1,1,14}, {0,0,0,0,0,6,6}, {0,6,6,0,6,6,0},
 {1,2,2,4,8,8,16}, {0,0,0,31,0,0,0}, {2,4,8,8,8,4,2},
 {8,4,2,2,2,4,8}, {17,2,4,8,16,17,0}, {0,4,4,31,4,4,0}, {14,17,1,2,4,0,4}
};
class Painter {
    Rectangle draw; void* context;
public:
    Painter(Rectangle r,void* c):draw(r),context(c){}
    void Box(int x,int y,int w,int h,unsigned int color) { draw(context,x,y,w,h,color); }
    void Text(const char* text,int x,int y,int scale,unsigned int color,int maxChars=96) {
        for(int i=0;text[i] && i<maxChars;++i) {
            char ch=char(toupper(static_cast<unsigned char>(text[i]))); const char* at=strchr(Alphabet,ch);
            unsigned int glyph=at ? unsigned(at-Alphabet) : unsigned(sizeof(Alphabet)-2);
            for(int row=0;row<7;++row) {
                for(int col=0;col<5;) {
                    if(!(Glyphs[glyph][row] & (16>>col))) { ++col; continue; }
                    int begin=col; while(col<5 && (Glyphs[glyph][row]&(16>>col))) ++col;
                    Box(x+i*6*scale+begin*scale,y+row*scale,(col-begin)*scale,scale,color);
                }
            }
        }
    }
};
void DrawUi(const UiState& s,Rectangle rectangle,void* context) {
    const unsigned int bg=0xFF0F1725,card=0xFF192537,white=0xFFE2EBF4,muted=0xFF8FA6BA,green=0xFF3ED9BC,blue=0xFF73B8FF;
    Painter p(rectangle,context); char text[128];
    p.Box(0,0,1280,720,bg); p.Box(56,48,7,44,green);
    p.Text("NETBENCH360",80,52,5,white); p.Text("RAM NETWORK BENCHMARK / V0.1.1",80,105,2,muted);
    p.Text("XBOX IP",832,53,2,muted); p.Text(s.ip,832,78,2,white,30);
    p.Text("PORT 36020",832,107,2,muted);
    p.Box(56,163,564,132,card); p.Box(660,163,564,132,card);
    p.Text("PC TO XBOX",80,184,2,green); p.Text("XBOX TO PC",684,184,2,blue);
    if(s.upload>0) _snprintf_s(text,sizeof(text),_TRUNCATE,"%.2f",s.upload); else strcpy_s(text,"--.--");
    p.Text(text,80,220,6,white); p.Text("MIB/S",334,239,2,muted);
    if(s.download>0) _snprintf_s(text,sizeof(text),_TRUNCATE,"%.2f",s.download); else strcpy_s(text,"--.--");
    p.Text(text,684,220,6,white); p.Text("MIB/S",938,239,2,muted);
    static const char* phases[]={"READY", "WAITING FOR PC", "CONNECTING", "CHECKING DATA", "WARMING UP", "MEASURING", "COMPLETE", "ERROR", "STOPPED"};
    p.Text(phases[s.phase>=0 && s.phase<=8 ? s.phase : 7],56,336,3,s.phase==7?0xFFFFAC8B:green);
    if(s.direction) p.Text(s.direction==1 ? "PC TO XBOX" : "XBOX TO PC",880,342,2,muted);
    if(s.phase==5 || s.phase==6) {
        _snprintf_s(text,sizeof(text),_TRUNCATE,"%.2f MIB/S",s.rate); p.Text(text,56,382,4,white);
        _snprintf_s(text,sizeof(text),_TRUNCATE,"%.1f SEC / %.1f MIB",s.elapsed,s.mebibytes); p.Text(text,640,394,2,muted);
    } else {
        p.Text(s.armed ? "START THE TEST IN THE WINDOWS APP." : "PRESS A TO ALLOW A PC TO CONNECT.",56,391,2,white);
    }
    p.Box(56,447,1168,8,card);
    double fraction=s.phase==6 ? 1.0 : (s.phase==5 && s.duration>0 ? s.elapsed/s.duration : 0);
    if(fraction>1) fraction=1; if(fraction<0) fraction=0;
    if(fraction>0) p.Box(56,447,int(1168*fraction),8,green);
    if(s.message[0]) p.Text(s.message,56,489,2,s.phase==7?0xFFFFAC8B:white,94);
    else p.Text("ONE TCP STREAM / DATA CHECK / WARM-UP / TIMED TRANSFER",56,489,2,muted);
    p.Text("RESULTS ABOVE ARE THE LATEST COMPLETED RUN IN EACH DIRECTION.",56,532,2,muted);
    p.Text("SAVE REPEATS AND MEDIANS FROM THE WINDOWS APP.",56,563,2,muted);
    p.Box(56,620,1168,2,card);
    p.Text(s.armed ? "A  LISTENING" : "A  START",56,649,2,green);
    p.Text("B  STOP",380,649,2,white); p.Text("BACK  EXIT WHEN STOPPED",688,649,2,muted);
}
}
