// SPDX-License-Identifier: MIT
#pragma once
namespace nb {
// FTP server first: it is the tab the app opens on.
enum UiTab { TabFtp, TabNetwork, TabDisk, TabSettings, TabCount };
// One finished FTP transfer for the "last transfers" list.
struct UiTransfer { char name[64]; double mebibytes,rate; int upload,ok; };
enum { UiRecentTransfers = 5 };
struct UiState {
    char ip[48],message[100],disk[100],ftp[100];
    // Current setting values as shown; main.cpp owns the choices.
    char buffers[32],cpu[24],diskTarget[16],diskBlock[16],drawing[16],speed[24],diskIo[24];
    // FTP login; an empty user means any user name and password.
    char ftpUser[24],ftpPassword[24];
    // ftpActive counts FTP transfers moving data; drawing pauses while any is.
    int phase,direction,duration,armed,completed,streams,rateLimit,options,downloadLimit,diskPhase,ftpPhase,ftpActive;
    int tab,focus,busy,keyboard;
    // Newest first; recentCount of them are filled in.
    UiTransfer recent[UiRecentTransfers]; int recentCount;
    double elapsed,rate,upload,download,mebibytes,diskTotal;
    UiState();
};
// Number of settings rows on a tab; D-pad up/down moves between them.
int SettingCount(int tab);
// Creates the Dear ImGui context, style and fonts. The renderer builds the
// font texture from ImGui::GetIO().Fonts afterwards.
void InitUi();
// Builds one frame. The caller sets io.DeltaTime first and draws
// ImGui::GetDrawData() afterwards.
void DrawUi(const UiState& state);
}
