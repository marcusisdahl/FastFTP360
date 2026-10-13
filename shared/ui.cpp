// SPDX-License-Identifier: MIT
// Xbox screen drawn with Dear ImGui. Everything is laid out for a 1280x720
// back buffer, inside the title-safe area (central 90%).
#include "ui.h"
#include "../third_party/imgui/imgui.h"
#include "../third_party/fonts/roboto_medium.h"
#include <stdio.h>
#include <string.h>
namespace nb {
UiState::UiState() : phase(0),direction(0),duration(10),armed(0),completed(0),streams(0),rateLimit(0),options(0),downloadLimit(0),diskPhase(0),ftpPhase(0),
    tab(0),focus(0),busy(0),keyboard(0),elapsed(0),rate(0),upload(0),download(0),mebibytes(0),diskTotal(0) {
    strcpy_s(ip,"Waiting for network"); message[0]=0; disk[0]=0; ftp[0]=0;
    buffers[0]=0; cpu[0]=0; diskTarget[0]=0; diskBlock[0]=0; drawing[0]=0; ftpUser[0]=0; ftpPassword[0]=0;
}
int SettingCount(int tab) { return tab==TabDisk ? 2 : 3; }

static ImFont* Small; static ImFont* Body; static ImFont* Heading; static ImFont* Hero;
enum { Left=64, Right=1216, Top=36, Bottom=684 };

static ImU32 Hex(unsigned int rgb,int alpha=255) { return IM_COL32((rgb>>16)&255,(rgb>>8)&255,rgb&255,alpha); }
static const unsigned int Background=0x0F1725,Card=0x192537,Raised=0x223349,White=0xE2EBF4,Muted=0x8FA6BA,Dim=0x51667A,
    Green=0x3ED9BC,Blue=0x73B8FF,Error=0xFFAC8B,ButtonA=0x5DC21E,ButtonB=0xE5413A,ButtonY=0xF2C232;

void InitUi() {
    ImGui::CreateContext();
    ImGuiIO& io=ImGui::GetIO();
    io.IniFilename=NULL; io.LogFilename=NULL; io.DisplaySize=ImVec2(1280,720);
    ImFontConfig config; config.FontDataOwnedByAtlas=false; config.OversampleH=2; config.OversampleV=1;
    void* data=const_cast<unsigned char*>(RobotoMedium); const int size=int(sizeof(RobotoMedium));
    static const ImWchar ascii[]={0x20,0x7E,0};
    // The hero font only ever shows rates such as "27.48" or "--.--".
    static const ImWchar digits[]={0x20,0x20,0x2D,0x39,0};
    Body=io.Fonts->AddFontFromMemoryTTF(data,size,26,&config,ascii);
    Small=io.Fonts->AddFontFromMemoryTTF(data,size,21,&config,ascii);
    Heading=io.Fonts->AddFontFromMemoryTTF(data,size,38,&config,ascii);
    Hero=io.Fonts->AddFontFromMemoryTTF(data,size,96,&config,digits);
    ImGuiStyle& style=ImGui::GetStyle();
    style.WindowPadding=ImVec2(0,0); style.WindowBorderSize=0; style.WindowRounding=0;
}

// Drawing helpers on the one full-screen window's draw list.
static ImDrawList* List;
static void Box(float x,float y,float w,float h,ImU32 color,float rounding=0) { List->AddRectFilled(ImVec2(x,y),ImVec2(x+w,y+h),color,rounding); }
static float Width(ImFont* font,const char* text) { return font->CalcTextSizeA(font->FontSize,10000,0,text).x; }
static void Text(ImFont* font,float x,float y,ImU32 color,const char* text) { List->AddText(font,font->FontSize,ImVec2(x,y),color,text); }
static void TextRight(ImFont* font,float right,float y,ImU32 color,const char* text) { Text(font,right-Width(font,text),y,color,text); }
// A face button (round) or bumper/other (rounded rectangle) with its label after it. Returns the next x.
static float Button(float x,float y,const char* name,unsigned int color,const char* label) {
    float w;
    if(strlen(name)==1) {
        w=32; List->AddCircleFilled(ImVec2(x+16,y+16),16,Hex(color),24);
        Text(Small,x+16-Width(Small,name)/2,y+4,Hex(0x0F1725),name);
    } else {
        w=Width(Small,name)+20; Box(x,y+2,w,28,Hex(Raised),8); Text(Small,x+10,y+4,Hex(White),name);
    }
    Text(Body,x+w+10,y+1,Hex(White),label);
    return x+w+10+Width(Body,label)+40;
}
static void Progress(float x,float y,float w,double fraction) {
    if(fraction<0) fraction=0; if(fraction>1) fraction=1;
    Box(x,y,w,10,Hex(Raised),5);
    if(fraction>0) Box(x,y,float(w*fraction)<10 ? 10 : float(w*fraction),10,Hex(Green),5);
}
static void Rate(float x,float y,double rate) {
    char text[32];
    if(rate>0) _snprintf_s(text,sizeof(text),_TRUNCATE,"%.2f",rate); else strcpy_s(text,"--.--");
    Text(Hero,x,y,Hex(White),text);
    Text(Heading,x+Width(Hero,text)+16,y+46,Hex(Muted),"MiB/s");
}
static const char* PhaseName(int phase) {
    static const char* names[]={"Ready","Waiting for PC","Connecting","Checking data","Warming up","Measuring","Complete","Error","Stopped"};
    return names[phase>=0 && phase<=8 ? phase : 7];
}

// Left card: what the selected tab is doing now. The latest message (result,
// error, FTP folders) replaces the description when there is one.
static void Status(const UiState& s,float x,float y,float w,float h) {
    char text[128]; Box(x,y,w,h,Hex(Card),14); x+=32; y+=26; w-=64;
    const char* title; const char* lines[3]={NULL,NULL,NULL}; double fraction=-1; text[0]=0;
    if(s.tab==TabDisk) {
        title=s.diskPhase==1 ? "Writing test file" : s.diskPhase==2 ? "Reading back and checking" : s.phase==7 ? "Error" : "Disk test";
        if(s.diskPhase) {
            fraction=s.diskTotal>0 ? s.mebibytes/s.diskTotal : 0;
            _snprintf_s(text,sizeof(text),_TRUNCATE,"%.0f of %.0f MiB  /  %.1f s",s.mebibytes,s.diskTotal,s.elapsed);
        }
        lines[0]="Writes a 256 MiB file, reads it back without the cache";
        lines[1]="and checks every byte. Needs 256 MiB free on the drive.";
        lines[2]="The test file is deleted afterwards.";
    } else if(s.tab==TabFtp) {
        static const char* names[]={"FTP server","FTP server on","Receiving upload","Sending download"};
        title=names[s.ftpPhase>=0 && s.ftpPhase<4 ? s.ftpPhase : 0];
        if(s.phase==7 && !s.ftpPhase) title="Error";
        if(s.ftpPhase>1) _snprintf_s(text,sizeof(text),_TRUNCATE,"%.1f MiB  /  %.1f s",s.mebibytes,s.elapsed);
        else if(s.ftpPhase==1) {
            _snprintf_s(text,sizeof(text),_TRUNCATE,"Connect to %s on port 21.",s.ip);
            static char login[80];
            if(s.ftpUser[0]) _snprintf_s(login,sizeof(login),_TRUNCATE,"Passive mode. User %s, password %s.",s.ftpUser,s.ftpPassword);
            else strcpy_s(login,"Passive mode, any user name and password.");
            lines[0]=text; lines[1]=login; lines[2]=s.message;
        } else {
            lines[0]="Lists the drives the way Aurora's FTP does:";
            lines[1]="Game, Hdd1, HddX, SysExt, System, Usb0, USBMU0...";
            lines[2]=s.ftpUser[0] ? "Not encrypted. Use it only on a trusted network." : "No password. Use it only on a trusted network.";
        }
    } else {
        title=PhaseName(s.armed || s.phase>=6 ? s.phase : 0);
        if(s.phase==5 || s.phase==6) {
            fraction=s.phase==6 ? 1.0 : s.duration>0 ? s.elapsed/s.duration : 0;
            const char* way=s.direction==1 ? "PC to Xbox" : "Xbox to PC";
            if(s.streams>1) _snprintf_s(text,sizeof(text),_TRUNCATE,"%s  /  %.1f s  /  %.1f MiB  /  %d streams",way,s.elapsed,s.mebibytes,s.streams);
            else _snprintf_s(text,sizeof(text),_TRUNCATE,"%s  /  %.1f s  /  %.1f MiB",way,s.elapsed,s.mebibytes);
        } else if(s.armed) {
            lines[0]="Listening. In NetBench360-PC.exe on the same network,";
            lines[1]="enter the Xbox IP shown above and start the test.";
        } else {
            lines[0]="1   Press A to listen.";
            lines[1]="2   Open NetBench360-PC.exe and enter the Xbox IP.";
            lines[2]="3   Start the test on the PC.";
        }
    }
    bool error=s.phase==7 && !s.busy;
    Text(Heading,x,y,Hex(error ? Error : Green),title);
    bool live=(s.tab==TabNetwork && (s.phase==5 || s.phase==6)) || (s.tab==TabDisk && s.diskPhase) || (s.tab==TabFtp && s.ftpPhase>1);
    if(live) {
        Rate(x-4,y+50,s.rate);
        Text(Body,x,y+h-112,Hex(Muted),(s.options&1) ? "Drawing paused during the transfer. B still stops." : text);
    }
    else if(s.message[0] && s.ftpPhase!=1) List->AddText(Body,Body->FontSize,ImVec2(x,y+60),Hex(error ? Error : White),s.message,NULL,w);
    else for(int i=0;i<3 && lines[i];++i) Text(Body,x,y+60+i*38,Hex(White),lines[i]);
    if(fraction>=0) Progress(x,y+h-72,w,fraction);
}

// Right card: settings for the selected tab. Locked while something runs.
static void Settings(const UiState& s,float x,float y,float w,float h) {
    Box(x,y,w,h,Hex(Card),14);
    Text(Small,x+28,y+24,Hex(Muted),s.busy ? "SETTINGS  (STOP TO CHANGE)" : "SETTINGS");
    const char* labels[3]; const char* values[3]; int rows=SettingCount(s.tab); char login[56];
    if(s.tab==TabDisk) { labels[0]="Drive"; values[0]=s.diskTarget; labels[1]="Block size"; values[1]=s.diskBlock; }
    else if(s.tab==TabFtp) {
        if(s.ftpUser[0]) _snprintf_s(login,sizeof(login),_TRUNCATE,"%s / %s",s.ftpUser,s.ftpPassword); else strcpy_s(login,"Off, any user");
        labels[0]="Xbox socket buffers"; values[0]=s.buffers; labels[1]="Login (user / password)"; values[1]=login;
        labels[2]="Drawing during transfers"; values[2]=s.drawing;
    }
    else { labels[0]="Xbox socket buffers"; values[0]=s.buffers; labels[1]="Worker CPU"; values[1]=s.cpu; labels[2]="Drawing during transfers"; values[2]=s.drawing; }
    for(int i=0;i<rows;++i) {
        float top=y+56+i*70; bool focused=!s.busy && i==s.focus;
        if(focused) { Box(x+16,top,w-32,64,Hex(Raised),10); Box(x+16,top+10,5,44,Hex(Green),2); }
        Text(Small,x+36,top+5,Hex(s.busy ? Dim : Muted),labels[i]);
        Text(Body,x+36,top+30,Hex(s.busy ? Dim : White),values[i]);
        if(focused) {
            TextRight(Heading,x+w-36,top+12,Hex(Green),">");
            TextRight(Heading,x+w-36-Width(Heading,">")-14,top+12,Hex(Green),"<");
        }
    }
}

// Bottom card: the latest completed results for the selected tab.
static void Results(const UiState& s,float x,float y,float w,float h) {
    char text[64]; Box(x,y,w,h,Hex(Card),14);
    Text(Small,x+28,y+18,Hex(Muted),"LAST RESULT");
    if(s.tab==TabNetwork) {
        float half=(w-56)/2;
        for(int i=0;i<2;++i) {
            double rate=i ? s.download : s.upload; float left=x+28+i*half;
            Text(Body,left,y+52,Hex(i ? Blue : Green),i ? "Xbox to PC" : "PC to Xbox");
            if(rate>0) _snprintf_s(text,sizeof(text),_TRUNCATE,"%.2f MiB/s",rate); else strcpy_s(text,"--.-- MiB/s");
            Text(Heading,left+170,y+44,Hex(White),text);
            if(i && s.downloadLimit) { _snprintf_s(text,sizeof(text),_TRUNCATE,"cap %d",s.downloadLimit); Text(Small,left+170+Width(Heading,"00.00 MiB/s")+16,y+56,Hex(Muted),text); }
        }
    } else {
        const char* line=s.tab==TabDisk ? s.disk : s.ftp;
        Text(Body,x+28,y+52,Hex(line[0] ? White : Dim),line[0] ? line : "No result yet.");
    }
}

static void Footer(const UiState& s,float y) {
    Box(Left,y,Right-Left,2,Hex(Card));
    float x=Left; y+=18;
    if(s.busy) x=Button(x,y,"B",ButtonB,s.tab==TabFtp ? "Stop server" : "Stop");
    else if(s.keyboard) Text(Body,x,y+1,Hex(Muted),"Type the FTP user name, then the password.");
    else {
        static const char* start[]={"Listen","Run disk test","Start FTP server"};
        x=Button(x,y,"A",ButtonA,start[s.tab]);
        if(s.tab==TabFtp) x=Button(x,y,"Y",ButtonY,"Set login");
        x=Button(x,y,"D-pad",0,"Settings");
        x=Button(x,y,"Back",0,"Exit");
    }
}

void DrawUi(const UiState& s) {
    ImGui::NewFrame();
    ImGui::SetNextWindowPos(ImVec2(0,0)); ImGui::SetNextWindowSize(ImVec2(1280,720));
    ImGui::Begin("NetBench360",NULL,ImGuiWindowFlags_NoDecoration|ImGuiWindowFlags_NoBackground|ImGuiWindowFlags_NoInputs|ImGuiWindowFlags_NoSavedSettings);
    List=ImGui::GetWindowDrawList();
    List->AddRectFilled(ImVec2(0,0),ImVec2(1280,720),Hex(Background));
    char text[128];

    // Header: name and version, then the address to type into the PC app.
    Box(Left,Top+6,6,40,Hex(Green),3);
    Text(Heading,Left+20,Top,Hex(White),"NetBench360");
    Text(Small,Left+20+Width(Heading,"NetBench360")+16,Top+14,Hex(Muted),"Network, disk and FTP diagnostics  v0.4.0");
    bool online=strchr(s.ip,'.')!=NULL;
    TextRight(Heading,Right,Top,Hex(online ? White : Muted),s.ip);
    _snprintf_s(text,sizeof(text),_TRUNCATE,"%s  /  port %d",online ? "Xbox IP" : "No address yet",s.tab==TabFtp ? 21 : 36020);
    TextRight(Small,Right,Top+46,Hex(Muted),text);
    List->AddCircleFilled(ImVec2(Right-Width(Small,text)-14,Top+57),5,Hex(online ? Green : ButtonY),12);

    // Tabs, switched with the bumpers while nothing runs.
    static const char* tabs[]={"Network","Disk","FTP server"};
    float x=Left,y=118;
    if(!s.busy) { Box(x,y+6,40,30,Hex(Raised),8); Text(Small,x+8,y+9,Hex(White),"LB"); x+=52; }
    for(int i=0;i<TabCount;++i) {
        float w=Width(Body,tabs[i])+48; bool on=i==s.tab;
        Box(x,y,w,42,Hex(on ? Green : Card),21);
        Text(Body,x+24,y+7,Hex(on ? Background : s.busy ? Dim : Muted),tabs[i]);
        x+=w+12;
    }
    if(!s.busy) { Box(x,y+6,40,30,Hex(Raised),8); Text(Small,x+8,y+9,Hex(White),"RB"); }

    Status(s,Left,184,744,280);
    Settings(s,Left+764,184,Right-Left-764,280);
    Results(s,Left,484,Right-Left,110);

    Footer(s,628);

    ImGui::End();
    ImGui::Render();
}
}
